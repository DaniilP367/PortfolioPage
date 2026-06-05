#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <ctype.h>
#include <math.h>

// forward decl: local case-insensitive substring search (portable for MinGW/MSVC)
static const char *clar_strcasestr_local(const char *haystack, const char *needle);
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <io.h>
#include <stdint.h>

#include "include/input.h"
#include "include/curl/curl.h"
#include "include/cJSON.h"
#include "include/edit_types.h"
#include "include/sqlite3.h"

#define GEMINI_MAX_RETRIES        3
#define GEMINI_RETRY_BACKOFF_MS   400  // базовая задержка между попытками
#define CLARIFY_HARD_MAX_ROUNDS   64
// Project S: deterministic DB diagnostics (no silent rc=0).
#define LEARN_DB_PREP_FAIL(TAG) \
    do { fprintf(stderr, "[SKYNET][STRUCTURIZE][LEARN] DB prepare failed (%s): %s\n", (TAG), sqlite3_errmsg(g_db_structurize)); } while(0)

#define LEARN_DB_STEP_FAIL(TAG) \
    do { fprintf(stderr, "[SKYNET][STRUCTURIZE][LEARN] DB step failed (%s): %s\n", (TAG), sqlite3_errmsg(g_db_structurize)); } while(0)
    
// --- Прототип генератора из llama_client.c ---
int llama_client_generate(void *model_handle,
                          const char *prompt,
                          char *out_buffer,
                          size_t buffer_size,
                          float temperature,
                          int max_tokens);

// Forward-declarations локальных хелперов, которые используются до определений
static void   clar_trim(char *s);
static void clar_escape_ep_text(const char *in, char *out, size_t out_sz);
static int    clar_is_russian(const char *text);
static void   clar_configure_curl_tls(CURL *curl);
static size_t clar_write_memory_callback(void *contents, size_t size, size_t nmemb, void *userp);


static int clar_structurize_task_description(const char *initial_desc,
                                             const char *qa_log,
                                             const char *syntax_hint,
                                             int ring_hint,
                                             int use_external_llm,
                                             char *out_struct,
                                             size_t out_struct_size);

// --- Глобальные модели / ключи (реально определены в других модулях) ---
extern void *g_llama_model_gen;       // большая генеративная модель (Qwen и т.п.)
extern const char *GEMINI_API_KEY;    // ключ Gemini, если используется внешняя LLM
extern int g_skynet_online_enabled;   // ONLINE ON/OFF (политика выбора внешней LLM)
extern int g_skynet_debug_enabled;

// Project S: per-module verbosity; do not inherit global debug (anti-noise).
static int structurize_debug_enabled(void) {
    if (g_skynet_debug_enabled) return 1; /* твой общий debug (F9) */
    const char *e = getenv("SKYNET_STRUCTURIZE_DEBUG");
    return (e && e[0] && e[0] != '0');    /* env var fallback */
}

// Отдельная БД Structurize ML (веса/примеры)
extern sqlite3 *g_db_structurize;

// ================================================================
// Project S (v2.0): no semantic hardcode. Markers/templates are DB-driven.
// "Система никогда не 'придумывает'... Обучение на дельтах..." (Concept v2.0)
// ================================================================

#define STRUCT_FORBID_MAX        64
#define STRUCT_FORBID_MARKER_MAX 64

// ============================================================================
// Structurize ML (веса/примеры) + Structurize без Clarify
// ============================================================================

// Простое 64-bit FNV-1a
static unsigned long long struct_ml_fnv1a64(const void *data, size_t len) {
    const unsigned char *p = (const unsigned char *)data;
    unsigned long long h = 1469598103934665603ULL;
    for (size_t i = 0; i < len; ++i) {
        h ^= (unsigned long long)p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static unsigned long long struct_ml_hash_str(const char *s) {
    if (!s) return 0ULL;
    return struct_ml_fnv1a64(s, strlen(s));
}

// ---- Structurize Transforms (deterministic, ML-selected) ----
// Forward decls (avoid implicit declarations before static definitions below)
static void structurize_delta_hex64(unsigned long long v, char out[32]);
static int hybrid_count_line_starts_with(const char *text, const char *hdr);
static int hybrid_validate_clarified_output(const char *out_struct);
static int clar_struct_has_header_loose(const char *spec, const char *want_key);
static int hybrid_extract_env_value(const char *clarified, const char *key, char *out_val, size_t out_val_sz);
static int tr_edit_has_env_keys(const char *s);

static void hybrid_debug_dump_reject_reasons(const char *s) {
    if (!s) s = "";

    int cs   = hybrid_count_line_starts_with(s, "CLARIFIED_SPEC:");
    int goal = hybrid_count_line_starts_with(s, "GOAL:");
    int req  = hybrid_count_line_starts_with(s, "REQUIREMENTS:");
    int in   = hybrid_count_line_starts_with(s, "INPUT:");
    int outp = hybrid_count_line_starts_with(s, "OUTPUT:");
    int cons = hybrid_count_line_starts_with(s, "CONSTRAINTS:");
    int env  = hybrid_count_line_starts_with(s, "ENVIRONMENT:");
    int edit = hybrid_count_line_starts_with(s, "EDIT_PARAMETERS:");
    int ng   = hybrid_count_line_starts_with(s, "NON_GOALS:");

    if (cs != 1) printf("[HYBRID_FIX][DBG] bad CLARIFIED_SPEC count=%d\n", cs);
    if (goal != 1) printf("[HYBRID_FIX][DBG] missing/dup GOAL count=%d\n", goal);
    if (req  != 1) printf("[HYBRID_FIX][DBG] missing/dup REQUIREMENTS count=%d\n", req);
    if (in   != 1) printf("[HYBRID_FIX][DBG] missing/dup INPUT count=%d\n", in);
    if (outp != 1) printf("[HYBRID_FIX][DBG] missing/dup OUTPUT count=%d\n", outp);
    if (cons != 1) printf("[HYBRID_FIX][DBG] missing/dup CONSTRAINTS count=%d\n", cons);
    if (env  != 1) printf("[HYBRID_FIX][DBG] missing/dup ENVIRONMENT count=%d\n", env);
    if (edit != 1) printf("[HYBRID_FIX][DBG] missing/dup EDIT_PARAMETERS count=%d\n", edit);
    if (ng   != 1) printf("[HYBRID_FIX][DBG] missing/dup NON_GOALS count=%d\n", ng);

    // order check (new contract)
    const char *h_goal = strstr(s, "\nGOAL:");            if (h_goal) h_goal++;
    const char *h_req  = strstr(s, "\nREQUIREMENTS:");    if (h_req)  h_req++;
    const char *h_in   = strstr(s, "\nINPUT:");           if (h_in)   h_in++;
    const char *h_out  = strstr(s, "\nOUTPUT:");          if (h_out)  h_out++;
    const char *h_cons = strstr(s, "\nCONSTRAINTS:");     if (h_cons) h_cons++;
    const char *h_env  = strstr(s, "\nENVIRONMENT:");     if (h_env)  h_env++;
    const char *h_edit = strstr(s, "\nEDIT_PARAMETERS:"); if (h_edit) h_edit++;
    const char *h_ng   = strstr(s, "\nNON_GOALS:");       if (h_ng)   h_ng++;

    if (!(h_goal && h_req && h_in && h_out && h_cons && h_env && h_edit && h_ng)) return;
    if (!(h_goal < h_req && h_req < h_in && h_in < h_out && h_out < h_cons && h_cons < h_env && h_env < h_edit && h_edit < h_ng)) {
        printf("[HYBRID_FIX][DBG] order mismatch (expected GOAL->REQUIREMENTS->INPUT->OUTPUT->CONSTRAINTS->ENVIRONMENT->EDIT_PARAMETERS->NON_GOALS)\n");
    }
}

#define TR_T_CANON_HEADERS    "T_CANON_HEADERS"
#define TR_T_REMOVE_RING_HINT "T_REMOVE_RING_HINT"
#define TR_T_STRIP_BULLETS    "T_STRIP_BULLETS"
#define TR_T_DEDUP_HEADERS    "T_DEDUP_HEADERS"
#define TR_T_ENV_LITERAL      "T_ENV_LITERAL"

// Forward declarations for deterministic transforms (avoid implicit declarations)
static void tr_apply_strip_bullets(char *s, size_t sz);
static void tr_apply_env_literal(char *s, size_t sz);
static void tr_apply_canon_headers(char *s, size_t sz);

static int tr_line_is_header(const char *s) {
    return s && (
        !strncmp(s, "CLARIFIED_SPEC:", 14) ||
        !strncmp(s, "GOAL:", 5) ||
        !strncmp(s, "REQUIREMENTS:", 13) ||
        !strncmp(s, "INPUT:", 6) ||
        !strncmp(s, "OUTPUT:", 7) ||
		!strncmp(s, "CONSTRAINTS:", 12) ||
		!strncmp(s, "CONSTRAINT:", 11) ||
		!strncmp(s, "ENVIRONMENT:", 12) ||
		!strncmp(s, "ENV:", 4) ||
		!strncmp(s, "EDIT_PARAMETERS:", 16) ||
		!strncmp(s, "EDIT PARAMETERS:", 16) ||
        !strncmp(s, "NON_GOALS:", 9)
    );
}

// ---- Canonical sections (Project S: single source of truth + legacy compatibility) ----
// New contract: GOAL + REQUIREMENTS, legacy: TASK.
#define STRUCT_HDR_GOAL         "GOAL:"
#define STRUCT_HDR_REQUIREMENTS "REQUIREMENTS:"
// Header aliases accepted on input (deterministic normalization)
#define HDR_ARRLEN(a) ((int)(sizeof(a)/sizeof((a)[0])))

static const char *HDR_ENV[]  = { "ENVIRONMENT:",     "ENV:" };
static const char *HDR_EDIT[] = { "EDIT_PARAMETERS:", "EDIT PARAMETERS:" };
static const char *HDR_CONS[] = { "CONSTRAINTS:",     "CONSTRAINT:" };

// forward decl (used in helpers before definition)
static void tr_copy_section_payload(const char *s, const char *hdr, char *out, size_t out_sz);

static void structurize_copy_goal_requirements_compat(const char *s,
                                                 char *out_goal, size_t out_goal_sz,
                                                 char *out_req,  size_t out_req_sz) {
    if (!out_goal || out_goal_sz == 0 || !out_req || out_req_sz == 0) return;

    out_goal[0] = '\0';
    out_req[0]  = '\0';

    if (!s || !s[0]) {
        strncpy(out_goal, "NONE", out_goal_sz - 1); out_goal[out_goal_sz - 1] = '\0';
        strncpy(out_req,  "NONE", out_req_sz  - 1); out_req[out_req_sz  - 1] = '\0';
        return;
    }

    // New contract: GOAL + REQUIREMENTS only (TASK is not recognized).
    tr_copy_section_payload(s, "GOAL:", out_goal, out_goal_sz);
    tr_copy_section_payload(s, "REQUIREMENTS:", out_req, out_req_sz);

    if (!out_goal[0]) { strncpy(out_goal, "NONE", out_goal_sz - 1); out_goal[out_goal_sz - 1] = '\0'; }
    if (!out_req[0])  { strncpy(out_req,  "NONE", out_req_sz  - 1); out_req[out_req_sz  - 1] = '\0'; }
}


static const char *HDR_NG[]   = {"NON_GOALS:", "NON GOALS:"};

static int tr_has_bullet_lines(const char *s) {
    if (!s) return 0;
    const char *p = s;
    while (*p) {
        const char *ls = p;
        while (*p && *p != '\n') p++;
        const char *le = p;
        if (*p == '\n') p++;

        while (ls < le && (*ls == ' ' || *ls == '\t' || *ls == '\r')) ls++;
        if (ls < le && (*ls == '*' || *ls == '-' || (unsigned char)*ls == 0x95 /* rare bullet */)) {
            return 1;
        }
        // '•' in UTF-8: we just treat any non-ascii bullet as potential bullet if first byte matches 0xE2
        if (ls + 2 < le && (unsigned char)ls[0] == 0xE2) return 1;
    }
    return 0;
}

static void tr_apply_strip_bullets(char *s, size_t sz) {
    if (!s || sz == 0) return;
    char tmp[65536];
    size_t w = 0;
    const char *p = s;

    while (*p && w + 1 < sizeof(tmp)) {
        const char *ls = p;
        while (*p && *p != '\n') p++;
        const char *le = p;
        if (*p == '\n') p++;

        // copy line with bullet stripped
        const char *q = ls;
        while (q < le && (*q == ' ' || *q == '\t' || *q == '\r')) {
            if (w + 1 < sizeof(tmp)) tmp[w++] = *q;
            q++;
        }

        // strip bullet marker + optional single space
        if (q < le && (*q == '*' || *q == '-')) {
            q++;
            if (q < le && (*q == ' ' || *q == '\t')) q++;
        } else if (q + 2 < le && (unsigned char)q[0] == 0xE2) {
            // UTF-8 bullet; drop leading 3 bytes if present
            q += 3;
            if (q < le && (*q == ' ' || *q == '\t')) q++;
        }

        while (q < le && w + 1 < sizeof(tmp)) tmp[w++] = *q++;
        if (w + 1 < sizeof(tmp)) tmp[w++] = '\n';
    }
    tmp[w] = '\0';

    strncpy(s, tmp, sz - 1);
    s[sz - 1] = '\0';
}

static int tr_edit_has_env_keys(const char *s) {
    if (!s) return 0;
    const char *e = strstr(s, "\nEDIT_PARAMETERS:");
    if (!e) {
        if (!strncmp(s, "EDIT_PARAMETERS:", 16)) e = s;
        else return 0;
    } else {
        e += 1;
    }
    const char *start = strchr(e, '\n');
    if (!start) return 0;
    start++;

    const char *end = strstr(start, "\nNON_GOALS:");
    if (!end) end = s + strlen(s);

    const char *p = start;
    while (p < end) {
        const char *le = strchr(p, '\n');
        if (!le || le > end) le = end;

        const char *t = p;
        while (t < le && (*t == ' ' || *t == '\t' || *t == '\r')) t++;

        if ((le - t) >= 12 && !_strnicmp(t, "ENVIRONMENT.", 12)) return 1;
        if ((le - t) >= 7  && !_strnicmp(t, "SYNTAX:", 7)) return 1;
        if ((le - t) >= 5  && !_strnicmp(t, "RING:", 5)) return 1;
        if ((le - t) >= 3  && !_strnicmp(t, "OS:", 3)) return 1;
        if ((le - t) >= 5  && !_strnicmp(t, "ARCH:", 5)) return 1;

        if (le == end) break;
        p = le + 1;
    }
    return 0;
}

static void tr_apply_env_literal(char *s, size_t sz) {
    // minimal safe fix: delete ENVIRONMENT.* and SYNTAX/RING/OS/ARCH lines from EDIT_PARAMETERS (ENVIRONMENT must hold them)
    if (!s || sz == 0) return;

    const char *e = strstr(s, "\nEDIT_PARAMETERS:");
    if (!e) {
        if (!strncmp(s, "EDIT_PARAMETERS:", 16)) e = s;
        else return;
    } else {
        e += 1;
    }
    const char *start = strchr(e, '\n');
    if (!start) return;
    start++;

    const char *end = strstr(start, "\nNON_GOALS:");
    if (!end) end = s + strlen(s);

    char tmp[65536];
    size_t w = 0;

    // copy up to EDIT payload start
    const char *src = s;
    while (src < start && w + 1 < sizeof(tmp)) tmp[w++] = *src++;

    // filter lines inside EDIT payload
    const char *p = start;
    while (p < end && w + 1 < sizeof(tmp)) {
        const char *le = strchr(p, '\n');
        if (!le || le > end) le = end;

        const char *t = p;
        while (t < le && (*t == ' ' || *t == '\t' || *t == '\r')) t++;

        int drop = 0;
        if ((le - t) >= 12 && !_strnicmp(t, "ENVIRONMENT.", 12)) drop = 1;
        if ((le - t) >= 7  && !_strnicmp(t, "SYNTAX:", 7)) drop = 1;
        if ((le - t) >= 5  && !_strnicmp(t, "RING:", 5)) drop = 1;
        if ((le - t) >= 3  && !_strnicmp(t, "OS:", 3)) drop = 1;
        if ((le - t) >= 5  && !_strnicmp(t, "ARCH:", 5)) drop = 1;

        if (!drop) {
            const char *q = p;
            while (q < le && w + 1 < sizeof(tmp)) tmp[w++] = *q++;
            if (w + 1 < sizeof(tmp)) tmp[w++] = '\n';
        }

        if (le == end) break;
        p = le + 1;
    }

    // copy rest (from end to end of struct)
    const char *rest = end;
    while (*rest && w + 1 < sizeof(tmp)) tmp[w++] = *rest++;
    tmp[w] = '\0';

    strncpy(s, tmp, sz - 1);
    s[sz - 1] = '\0';
}

// Canon headers rebuild: preserves payload of first occurrence of each section; ensures order; inserts NONE if empty/missing.
static const char *tr_find_header(const char *s, const char *hdr) {
    if (!s || !hdr) return NULL;
    size_t L = strlen(hdr);
    if (!strncmp(s, hdr, L)) return s;
    char pat[64];
    _snprintf_s(pat, sizeof(pat), _TRUNCATE, "\n%s", hdr);
    const char *p = strstr(s, pat);
    if (p) return p + 1;
    return NULL;
}

static const char *tr_find_header_any(const char *s, const char * const *hdrs, int n) {
    const char *best = NULL;
    for (int i = 0; i < n; i++) {
        const char *p = tr_find_header(s, hdrs[i]);
        if (p && (!best || p < best)) best = p;
    }
    return best;
}

static void tr_copy_section_payload_any(const char *s, const char * const *hdrs, int n,
                                        char *out, size_t out_sz) {
    if (!out || out_sz == 0) return;
    out[0] = 0;

    const char *p = tr_find_header_any(s, hdrs, n);
    if (!p) {
        _snprintf_s(out, out_sz, _TRUNCATE, "NONE\n");
        return;
    }

    const char *nl = strchr(p, '\n');
    const char *cur = nl ? (nl + 1) : (p + strlen(p));
    const char *end = cur;

    while (*end) {
        const char *line_end = strchr(end, '\n');
        size_t len = line_end ? (size_t)(line_end - end) : strlen(end);
        char tmp[256];
        if (len >= sizeof(tmp)) len = sizeof(tmp) - 1;
        memcpy(tmp, end, len);
        tmp[len] = 0;
        if (tr_line_is_header(tmp)) break;
        end = line_end ? (line_end + 1) : (end + strlen(end));
    }

    if (end <= cur) {
        _snprintf_s(out, out_sz, _TRUNCATE, "NONE\n");
        return;
    }

    size_t payload_len = (size_t)(end - cur);
    if (payload_len >= out_sz) payload_len = out_sz - 1;
    memcpy(out, cur, payload_len);
    out[payload_len] = 0;
    if (payload_len && out[payload_len - 1] != '\n') {
        if (payload_len + 1 < out_sz) {
            out[payload_len] = '\n';
            out[payload_len + 1] = 0;
        }
    }
}

static void tr_copy_section_payload(const char *s, const char *hdr, char *out, size_t out_sz) {
    if (!out || out_sz == 0) return;
    out[0] = '\0';
    const char *h = tr_find_header(s, hdr);
    if (!h) return;
    const char *p = strchr(h, '\n');
    if (!p) return;
    p++;

    // end at next known header
    const char *end = NULL;
    const char *scan = p;
    while (*scan) {
        if (*scan == '\n' && tr_line_is_header(scan + 1)) { end = scan + 1; break; }
        scan++;
    }
    if (!end) end = s + strlen(s);

    // trim right
    while (end > p && (end[-1] == '\n' || end[-1] == '\r')) end--;

    size_t n = (size_t)(end - p);
    if (n >= out_sz) n = out_sz - 1;
    memcpy(out, p, n);
    out[n] = '\0';
    clar_trim(out);
}

// Copy payload using the first matching header alias.
// Project S: deterministic parsing, but tolerant to spelling variants (underscore vs space, short aliases).
// Returns 1 if a non-empty payload was copied, otherwise 0.

static void tr_apply_canon_headers(char *s, size_t sz) {
    if (!s || sz == 0) return;

    char goal[16384], req[16384], in[16384], outp[16384], cons[16384], env[16384], edit[16384], ng[16384];
    goal[0]=req[0]=in[0]=outp[0]=cons[0]=env[0]=edit[0]=ng[0]='\0';

    structurize_copy_goal_requirements_compat(s, goal, sizeof(goal), req, sizeof(req));

    tr_copy_section_payload(s, "INPUT:", in, sizeof(in));
    tr_copy_section_payload(s, "OUTPUT:", outp, sizeof(outp));
	(void)tr_copy_section_payload_any(s, HDR_CONS, 2, cons, sizeof(cons));
	(void)tr_copy_section_payload_any(s, HDR_ENV,  2, env,  sizeof(env));
	(void)tr_copy_section_payload_any(s, HDR_EDIT, 2, edit, sizeof(edit));
	(void)tr_copy_section_payload_any(s, HDR_NG,   2, ng,   sizeof(ng));

    if (!goal[0]) strncpy(goal, "NONE", sizeof(goal)-1);
    if (!req[0])  strncpy(req,  "NONE", sizeof(req)-1);
    if (!in[0])   strncpy(in,   "NONE", sizeof(in)-1);
    if (!outp[0]) strncpy(outp, "NONE", sizeof(outp)-1);
    if (!cons[0]) strncpy(cons, "NONE", sizeof(cons)-1);
    if (!env[0])  strncpy(env,  "NONE", sizeof(env)-1);
    if (!edit[0]) strncpy(edit, "NONE", sizeof(edit)-1);
    if (!ng[0])   strncpy(ng,   "NONE", sizeof(ng)-1);

    goal[sizeof(goal)-1] = '\0';
    req[sizeof(req)-1]   = '\0';
    in[sizeof(in)-1]     = '\0';
    outp[sizeof(outp)-1] = '\0';
    cons[sizeof(cons)-1] = '\0';
    env[sizeof(env)-1]   = '\0';
    edit[sizeof(edit)-1] = '\0';
    ng[sizeof(ng)-1]     = '\0';

    char tmp[65536];
    _snprintf_s(tmp, sizeof(tmp), _TRUNCATE,
        "CLARIFIED_SPEC:\n"
        "GOAL:\n%s\n\n"
        "REQUIREMENTS:\n%s\n\n"
        "INPUT:\n%s\n\n"
        "OUTPUT:\n%s\n\n"
        "CONSTRAINTS:\n%s\n\n"
        "ENVIRONMENT:\n%s\n\n"
        "EDIT_PARAMETERS:\n%s\n\n"
        "NON_GOALS:\n%s\n",
        goal, req, in, outp, cons, env, edit, ng
    );

    strncpy(s, tmp, sz - 1);
    s[sz - 1] = '\0';
}

static int tr_has_duplicate_headers(const char *s) {
    if (!s) return 0;
    int cs   = hybrid_count_line_starts_with(s, "CLARIFIED_SPEC:");
    int goal = hybrid_count_line_starts_with(s, "GOAL:");
    int req  = hybrid_count_line_starts_with(s, "REQUIREMENTS:");
    int in   = hybrid_count_line_starts_with(s, "INPUT:");
    int outp = hybrid_count_line_starts_with(s, "OUTPUT:");
    int cons = hybrid_count_line_starts_with(s, "CONSTRAINTS:");
    int env  = hybrid_count_line_starts_with(s, "ENVIRONMENT:");
    int edit = hybrid_count_line_starts_with(s, "EDIT_PARAMETERS:");
    int ng   = hybrid_count_line_starts_with(s, "NON_GOALS:");

    if (cs > 1 || goal > 1 || req > 1 || in > 1 || outp > 1 || cons > 1 || env > 1 || edit > 1 || ng > 1) return 1;
    return 0;
}

// Project S Structurize: deterministic operations chosen by similarity-weighted retrieval.
// Concept quote (v2.0): "ML/Embeddings не правит текст, а выбирает/взвешивает набор фиксированных детерминированных трансформов".

// ---------- OP_* (deterministic operations) ----------
#define OP_MOVE_OS_TO_ENV				 "OP_MOVE_OS_TO_ENV"
#define OP_OUTPUT_FROM_TASK_TEMPLATE	 "OP_OUTPUT_FROM_TASK_TEMPLATE"
#define OP_FORBIDDEN_ONLY_IN_CONSTRAINTS "OP_FORBIDDEN_ONLY_IN_CONSTRAINTS"
#define OP_EXTRACT_EDIT_PARAMS_BY_TYPES  "OP_EXTRACT_EDIT_PARAMS_BY_TYPES"

// Project S: compat helper (TASK -> GOAL+REQUIREMENTS). Declared here to avoid order issues.
static void structurize_copy_goal_requirements_compat(const char *spec,
                                                      char *out_goal, size_t out_goal_sz,
                                                      char *out_req,  size_t out_req_sz);

// ---------- Embedding retrieval + OP scoring ----------

extern void *g_llama_model_en;
extern void *g_llama_model_ru;
int llama_client_get_embedding_dim(void *model_handle);
int llama_client_get_embeddings(void *model_handle, const char *text, float *out_embeddings);

#define STRUCTURIZE_EMB_TAG "struct-v1"

// --- DB schema migration (backward compatible) ---
// Deterministic: old DBs must be migrated, not rejected.

static int structurize_ops_table_has_column(sqlite3 *db, const char *table, const char *col) {
    if (!db || !table || !col) return 0;

    char sql[256];
    // table is internal constant (not user input)
    _snprintf_s(sql, sizeof(sql), _TRUNCATE, "PRAGMA table_info(%s);", table);

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return 0;

    int has = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *name = sqlite3_column_text(st, 1); // column name
        if (name && _stricmp((const char *)name, col) == 0) { has = 1; break; }
    }
    sqlite3_finalize(st);
    return has;
}

static int structurize_ops_exec_sql(sqlite3 *db, const char *sql, const char *tag) {
    char *err = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
        if (err) {
            fprintf(stderr, "[SKYNET][STRUCTURIZE][DB] ERROR: %s: %s\n", tag ? tag : "sql", err);
            sqlite3_free(err);
        }
        return 0;
    }
    return 1;
}

static int structurize_ops_migrate_schema(sqlite3 *db) {
    if (!db) return 0;

    // Old DB: struct_example_embeddings had no 'model'
    if (!structurize_ops_table_has_column(db, "struct_example_embeddings", "model")) {
        if (!structurize_ops_exec_sql(
                db,
                "ALTER TABLE struct_example_embeddings ADD COLUMN model TEXT NOT NULL DEFAULT '';",
                "migrate add column model")) {
            return 0;
        }
    }

    // Some old variants might miss updated_at
    if (!structurize_ops_table_has_column(db, "struct_example_embeddings", "updated_at")) {
        if (!structurize_ops_exec_sql(
                db,
                "ALTER TABLE struct_example_embeddings ADD COLUMN updated_at TEXT DEFAULT CURRENT_TIMESTAMP;",
                "migrate add column updated_at")) {
            return 0;
        }
    }
    // Legacy schema: struct_example_embeddings has embed_dim (NOT NULL) which breaks INSERT into (dim).
    // Project S: deterministic migration to authoritative schema => rebuild table WITHOUT embed_dim.
    if (structurize_ops_table_has_column(db, "struct_example_embeddings", "embed_dim")) {
        int has_model       = structurize_ops_table_has_column(db, "struct_example_embeddings", "model");
        int has_embed_model = structurize_ops_table_has_column(db, "struct_example_embeddings", "embed_model");
        int has_dim         = structurize_ops_table_has_column(db, "struct_example_embeddings", "dim");
        int has_updated_at  = structurize_ops_table_has_column(db, "struct_example_embeddings", "updated_at");
        int has_embedding   = structurize_ops_table_has_column(db, "struct_example_embeddings", "embedding");
        int has_example_id  = structurize_ops_table_has_column(db, "struct_example_embeddings", "example_id");

        const char *model_expr   = has_model ? "model" : (has_embed_model ? "embed_model" : "''");
        const char *dim_expr     = has_dim ? "CASE WHEN dim IS NOT NULL AND dim>0 THEN dim ELSE embed_dim END" : "embed_dim";
        const char *updated_expr = has_updated_at ? "updated_at" : "CURRENT_TIMESTAMP";

        if (structurize_debug_enabled()) {
            fprintf(stderr, "[SKYNET][STRUCTURIZE][DB][DBG] migrate embeddings: embed_dim detected -> rebuild table\n");
        }

        // best-effort cleanup if previous attempt left temp table
        {
            char *err = NULL;
            (void)sqlite3_exec(db, "DROP TABLE IF EXISTS struct_example_embeddings__new;", NULL, NULL, &err);
            if (err) sqlite3_free(err);
        }

        {
            char *err = NULL;
            if (sqlite3_exec(db, "BEGIN IMMEDIATE;", NULL, NULL, &err) != SQLITE_OK) {
                if (err) {
                    fprintf(stderr, "[SKYNET][STRUCTURIZE][DB] ERROR: migrate emb begin: %s\n", err);
                    sqlite3_free(err);
                }
                return 0;
            }
        }

        // Create authoritative table
        {
            char *err = NULL;
            const char *sql_new =
                "CREATE TABLE struct_example_embeddings__new("
                "  example_id INTEGER PRIMARY KEY,"
                "  model TEXT NOT NULL DEFAULT '',"
                "  dim INTEGER NOT NULL DEFAULT 0,"
                "  embedding BLOB NOT NULL,"
                "  updated_at TEXT DEFAULT CURRENT_TIMESTAMP"
                ");";
            if (sqlite3_exec(db, sql_new, NULL, NULL, &err) != SQLITE_OK) {
                if (err) {
                    fprintf(stderr, "[SKYNET][STRUCTURIZE][DB] ERROR: migrate emb create new: %s\n", err);
                    sqlite3_free(err);
                }
                (void)sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
                return 0;
            }
        }

        // Copy data when possible (otherwise rebuild empty, which is acceptable for early bootstrap)
        if (has_example_id && has_embedding) {
            char sql_copy[1600];
            snprintf(sql_copy, sizeof(sql_copy),
                "INSERT INTO struct_example_embeddings__new(example_id,model,dim,embedding,updated_at) "
                "SELECT example_id, %s, %s, embedding, %s "
                "FROM struct_example_embeddings "
                "WHERE embedding IS NOT NULL;",
                model_expr, dim_expr, updated_expr);

            char *err = NULL;
            if (sqlite3_exec(db, sql_copy, NULL, NULL, &err) != SQLITE_OK) {
                if (err) {
                    fprintf(stderr, "[SKYNET][STRUCTURIZE][DB] ERROR: migrate emb copy: %s\n", err);
                    sqlite3_free(err);
                }
                (void)sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
                return 0;
            }
        } else if (structurize_debug_enabled()) {
            fprintf(stderr, "[SKYNET][STRUCTURIZE][DB][DBG] migrate embeddings: copy skipped (legacy columns missing)\n");
        }

        // Replace old table
        {
            char *err = NULL;
            if (sqlite3_exec(db, "DROP TABLE struct_example_embeddings;", NULL, NULL, &err) != SQLITE_OK) {
                if (err) {
                    fprintf(stderr, "[SKYNET][STRUCTURIZE][DB] ERROR: migrate emb drop old: %s\n", err);
                    sqlite3_free(err);
                }
                (void)sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
                return 0;
            }
        }
        {
            char *err = NULL;
            if (sqlite3_exec(db,
                    "ALTER TABLE struct_example_embeddings__new RENAME TO struct_example_embeddings;",
                    NULL, NULL, &err) != SQLITE_OK) {
                if (err) {
                    fprintf(stderr, "[SKYNET][STRUCTURIZE][DB] ERROR: migrate emb rename: %s\n", err);
                    sqlite3_free(err);
                }
                (void)sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
                return 0;
            }
        }

        // Commit
        {
            char *err = NULL;
            if (sqlite3_exec(db, "COMMIT;", NULL, NULL, &err) != SQLITE_OK) {
                if (err) {
                    fprintf(stderr, "[SKYNET][STRUCTURIZE][DB] ERROR: migrate emb commit: %s\n", err);
                    sqlite3_free(err);
                }
                (void)sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
                return 0;
            }
        }
    }
    // Index is safe only AFTER model exists
    (void)structurize_ops_exec_sql(
        db,
        "CREATE INDEX IF NOT EXISTS idx_struct_example_embeddings_model_dim "
        "ON struct_example_embeddings(model, dim);",
        "migrate index model_dim"
    );
	    // ---- migrate legacy struct_fix_pairs: older schema may lack before_text/after_text ----
    if (structurize_ops_exec_sql(db,
            "SELECT 1 FROM sqlite_master WHERE type='table' AND name='struct_fix_pairs' LIMIT 1;",
            NULL)) {
        // add missing columns (safe on existing DB)
		if (!structurize_ops_table_has_column(db, "struct_fix_pairs", "before_struct")) {
			if (!structurize_ops_exec_sql(
					db,
					"ALTER TABLE struct_fix_pairs ADD COLUMN before_struct TEXT NOT NULL DEFAULT '';",
					"migrate struct_fix_pairs add before_struct")) return 0;
		}
		if (!structurize_ops_table_has_column(db, "struct_fix_pairs", "after_struct")) {
			if (!structurize_ops_exec_sql(
					db,
					"ALTER TABLE struct_fix_pairs ADD COLUMN after_struct TEXT NOT NULL DEFAULT '';",
					"migrate struct_fix_pairs add after_struct")) return 0;
		}
        // best-effort backfill from legacy columns if they exist
        if (structurize_ops_table_has_column(db, "struct_fix_pairs", "before")) {
            (void)structurize_ops_exec_sql(
                db,
                "UPDATE struct_fix_pairs "
                "SET before_struct = CASE WHEN before_struct='' THEN before ELSE before_struct END "
                "WHERE before IS NOT NULL;",
                "migrate struct_fix_pairs backfill before_text"
            );
        }
        if (structurize_ops_table_has_column(db, "struct_fix_pairs", "after")) {
            (void)structurize_ops_exec_sql(
                db,
                "UPDATE struct_fix_pairs "
                "SET after_struct = CASE WHEN after_struct='' THEN after ELSE after_struct END "
                "WHERE after IS NOT NULL;",
                "migrate struct_fix_pairs backfill after_text"
            );
        }
    }
    // ---- migrate legacy struct_examples: add output_text and other required columns ----
{
    // Ensure required columns exist (safe for legacy DBs).
    if (!structurize_ops_table_has_column(db, "struct_examples", "input_text")) {
        if (!structurize_ops_exec_sql(
                db,
                "ALTER TABLE struct_examples ADD COLUMN input_text TEXT NOT NULL DEFAULT '';",
                "migrate struct_examples add input_text")) return 0;
    }
    if (!structurize_ops_table_has_column(db, "struct_examples", "output_text")) {
        if (!structurize_ops_exec_sql(
                db,
                "ALTER TABLE struct_examples ADD COLUMN output_text TEXT NOT NULL DEFAULT '';",
                "migrate struct_examples add output_text")) return 0;
    }
    if (!structurize_ops_table_has_column(db, "struct_examples", "syntax_hint")) {
        if (!structurize_ops_exec_sql(
                db,
                "ALTER TABLE struct_examples ADD COLUMN syntax_hint TEXT NOT NULL DEFAULT '';",
                "migrate struct_examples add syntax_hint")) return 0;
    }
    if (!structurize_ops_table_has_column(db, "struct_examples", "ring_hint")) {
        if (!structurize_ops_exec_sql(
                db,
                "ALTER TABLE struct_examples ADD COLUMN ring_hint INTEGER NOT NULL DEFAULT 0;",
                "migrate struct_examples add ring_hint")) return 0;
    }
    if (!structurize_ops_table_has_column(db, "struct_examples", "approved")) {
        if (!structurize_ops_exec_sql(
                db,
                "ALTER TABLE struct_examples ADD COLUMN approved INTEGER NOT NULL DEFAULT 0;",
                "migrate struct_examples add approved")) return 0;
    }
    if (!structurize_ops_table_has_column(db, "struct_examples", "version")) {
        if (!structurize_ops_exec_sql(
                db,
                "ALTER TABLE struct_examples ADD COLUMN version INTEGER NOT NULL DEFAULT 1;",
                "migrate struct_examples add version")) return 0;
    }
    if (!structurize_ops_table_has_column(db, "struct_examples", "created_at")) {
        if (!structurize_ops_exec_sql(
                db,
                "ALTER TABLE struct_examples ADD COLUMN created_at TEXT DEFAULT CURRENT_TIMESTAMP;",
                "migrate struct_examples add created_at")) return 0;
    }

    // Best-effort backfill for output_text from legacy column names (only if they exist).
    if (structurize_ops_table_has_column(db, "struct_examples", "output") &&
        structurize_ops_table_has_column(db, "struct_examples", "output_text")) {
        (void)structurize_ops_exec_sql(
            db,
            "UPDATE struct_examples "
            "SET output_text = CASE WHEN (output_text IS NULL OR output_text='') THEN output ELSE output_text END "
            "WHERE output IS NOT NULL;",
            "migrate struct_examples backfill output_text from output"
        );
    }
    // Project S (dev bootstrap): if we already have examples but none approved, promote them.
	// This avoids "topk=0 join_n=0" dead-start on legacy DBs during early bring-up.
	(void)structurize_ops_exec_sql(
		db,
		"UPDATE struct_examples SET approved=1 WHERE approved=0;",
		"migrate backfill struct_examples.approved"
	);
}
    // ---- migrate Project S v2.2: struct_delta_ops token-span columns ----
    if (structurize_ops_exec_sql(db,
            "SELECT 1 FROM sqlite_master WHERE type='table' AND name='struct_delta_ops' LIMIT 1;",
            NULL)) {

        if (!structurize_ops_table_has_column(db, "struct_delta_ops", "src_tok_start")) {
            if (!structurize_ops_exec_sql(
                    db,
                    "ALTER TABLE struct_delta_ops ADD COLUMN src_tok_start INTEGER NOT NULL DEFAULT 0;",
                    "migrate struct_delta_ops add src_tok_start")) return 0;
        }
        if (!structurize_ops_table_has_column(db, "struct_delta_ops", "src_tok_len")) {
            if (!structurize_ops_exec_sql(
                    db,
                    "ALTER TABLE struct_delta_ops ADD COLUMN src_tok_len INTEGER NOT NULL DEFAULT 0;",
                    "migrate struct_delta_ops add src_tok_len")) return 0;
        }
        if (!structurize_ops_table_has_column(db, "struct_delta_ops", "dst_anchor_hash")) {
            if (!structurize_ops_exec_sql(
                    db,
                    "ALTER TABLE struct_delta_ops ADD COLUMN dst_anchor_hash INTEGER NOT NULL DEFAULT 0;",
                    "migrate struct_delta_ops add dst_anchor_hash")) return 0;
        }
        if (!structurize_ops_table_has_column(db, "struct_delta_ops", "dst_tok_offset")) {
            if (!structurize_ops_exec_sql(
                    db,
                    "ALTER TABLE struct_delta_ops ADD COLUMN dst_tok_offset INTEGER NOT NULL DEFAULT 0;",
                    "migrate struct_delta_ops add dst_tok_offset")) return 0;
        }
        if (!structurize_ops_table_has_column(db, "struct_delta_ops", "payload_hash")) {
            if (!structurize_ops_exec_sql(
                    db,
                    "ALTER TABLE struct_delta_ops ADD COLUMN payload_hash INTEGER NOT NULL DEFAULT 0;",
                    "migrate struct_delta_ops add payload_hash")) return 0;
        }
        if (!structurize_ops_table_has_column(db, "struct_delta_ops", "guards_mask")) {
            if (!structurize_ops_exec_sql(
                    db,
                    "ALTER TABLE struct_delta_ops ADD COLUMN guards_mask INTEGER NOT NULL DEFAULT 0;",
                    "migrate struct_delta_ops add guards_mask")) return 0;
        }
    }

    return 1;
}

static int structurize_ops_ensure_schema(sqlite3 *db) {
    if (!db) return 0;

       /* таблицы (Project S: единая авторитетная схема + совместимость со старыми БД) */
    const char *sql_tables =
        "CREATE TABLE IF NOT EXISTS struct_model_meta("
        "  id INTEGER PRIMARY KEY CHECK(id=1),"
        "  current_version INTEGER NOT NULL DEFAULT 1,"
        "  lr REAL NOT NULL DEFAULT 0.10,"
        "  threshold REAL NOT NULL DEFAULT 0.50"
        ");"
        "INSERT OR IGNORE INTO struct_model_meta(id,current_version,lr,threshold) VALUES(1,1,0.10,0.50);"

        "CREATE TABLE IF NOT EXISTS struct_labels("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  name TEXT NOT NULL UNIQUE"
        ");"

		"CREATE TABLE IF NOT EXISTS struct_examples("
		"  id INTEGER PRIMARY KEY AUTOINCREMENT,"
		"  input_text TEXT NOT NULL,"
		"  output_text TEXT NOT NULL,"
		"  final_struct TEXT NOT NULL DEFAULT '',"
		"  syntax_hint TEXT NOT NULL DEFAULT '',"
		"  ring_hint INTEGER NOT NULL DEFAULT 0,"
		"  approved INTEGER NOT NULL DEFAULT 0,"
		"  version INTEGER NOT NULL DEFAULT 1,"
		"  created_at TEXT DEFAULT CURRENT_TIMESTAMP"
		");"
		
        "CREATE TABLE IF NOT EXISTS struct_fix_pairs("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  input_text TEXT NOT NULL,"
        "  before_struct TEXT NOT NULL DEFAULT '',"
        "  after_struct TEXT NOT NULL DEFAULT '',"
        "  syntax_hint TEXT NOT NULL DEFAULT '',"
        "  ring_hint INTEGER NOT NULL DEFAULT 0,"
        "  created_at TEXT DEFAULT CURRENT_TIMESTAMP"
        ");"

        "CREATE TABLE IF NOT EXISTS struct_fix_pair_labels("
        "  pair_id INTEGER NOT NULL,"
        "  label_id INTEGER NOT NULL,"
        "  PRIMARY KEY(pair_id,label_id)"
        ");"

        "CREATE TABLE IF NOT EXISTS struct_example_embeddings("
        "  example_id INTEGER PRIMARY KEY,"
        "  model TEXT NOT NULL,"
        "  dim INTEGER NOT NULL,"
        "  embedding BLOB NOT NULL,"
        "  updated_at TEXT DEFAULT CURRENT_TIMESTAMP"
        ");"

        "CREATE TABLE IF NOT EXISTS struct_example_ops("
        "  example_id INTEGER NOT NULL,"
        "  label_id INTEGER NOT NULL,"
        "  PRIMARY KEY(example_id,label_id)"
        ");"

		"CREATE TABLE IF NOT EXISTS struct_op_weights("
		"  label_id INTEGER PRIMARY KEY,"
		"  weight REAL NOT NULL DEFAULT 1.0,"
		"  updates INTEGER NOT NULL DEFAULT 0,"
		"  updated_at TEXT DEFAULT CURRENT_TIMESTAMP"
		");"
		
		"CREATE TABLE IF NOT EXISTS struct_key_types("
		" key TEXT PRIMARY KEY,"
		" scalar_hits INTEGER NOT NULL DEFAULT 0,"
		" string_hits INTEGER NOT NULL DEFAULT 0,"
		" updated_at TEXT DEFAULT (CURRENT_TIMESTAMP)"
		");"
		
        // === Project S v2.0: Transfer Templates (повторяемые переносы из GOAL) ===
        "CREATE TABLE IF NOT EXISTS struct_transfer_templates("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  pair_id INTEGER NOT NULL DEFAULT 0,"
        "  successes INTEGER NOT NULL DEFAULT 0,"
        "  failures INTEGER NOT NULL DEFAULT 0,"
        "  created_at TEXT DEFAULT CURRENT_TIMESTAMP"
        ");"

        "CREATE TABLE IF NOT EXISTS struct_transfer_rules("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  template_id INTEGER NOT NULL,"
        "  ordinal INTEGER NOT NULL,"
        "  from_section TEXT NOT NULL DEFAULT 'GOAL',"
        "  to_section TEXT NOT NULL,"
        "  needle TEXT NOT NULL,"
        "  left_ctx TEXT NOT NULL DEFAULT '',"
        "  right_ctx TEXT NOT NULL DEFAULT '',"
        "  created_at TEXT DEFAULT CURRENT_TIMESTAMP,"
        "  FOREIGN KEY(template_id) REFERENCES struct_transfer_templates(id) ON DELETE CASCADE"
                ");"

        // === Project S v2.1: Delta Programs (AUTO->FIX operations) ===
        "CREATE TABLE IF NOT EXISTS struct_delta_examples("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  pair_id INTEGER NOT NULL,"
        "  input_fingerprint TEXT NOT NULL,"
        "  input_text TEXT NOT NULL,"
        "  syntax_hint TEXT NOT NULL DEFAULT '',"
        "  ring_hint INTEGER NOT NULL DEFAULT 0,"
        "  embed_model TEXT NOT NULL,"
        "  embed_dim INTEGER NOT NULL,"
        "  embedding BLOB NOT NULL,"
        "  created_at TEXT DEFAULT CURRENT_TIMESTAMP"
        ");"

        "CREATE TABLE IF NOT EXISTS struct_delta_programs("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  pair_id INTEGER NOT NULL,"
        "  example_id INTEGER NOT NULL,"
        "  syntax_hint TEXT NOT NULL DEFAULT '',"
        "  ring_hint INTEGER NOT NULL DEFAULT 0,"
        "  embed_model TEXT NOT NULL,"
        "  embed_dim INTEGER NOT NULL,"
        "  successes INTEGER NOT NULL DEFAULT 0,"
        "  failures INTEGER NOT NULL DEFAULT 0,"
        "  created_at TEXT DEFAULT CURRENT_TIMESTAMP,"
        "  updated_at TEXT DEFAULT CURRENT_TIMESTAMP"
        ");"

        "CREATE TABLE IF NOT EXISTS struct_delta_ops("
        "  program_id INTEGER NOT NULL,"
        "  op_order INTEGER NOT NULL,"
        "  op_type INTEGER NOT NULL,"
        "  src_section TEXT NOT NULL DEFAULT '',"
        "  dst_section TEXT NOT NULL DEFAULT '',"
        "  match_before TEXT NOT NULL DEFAULT '',"
        "  match_after TEXT NOT NULL DEFAULT '',"
        "  guard INTEGER NOT NULL DEFAULT 0,"
        "  flags INTEGER NOT NULL DEFAULT 0,"
        "  created_at TEXT DEFAULT CURRENT_TIMESTAMP,"
        "  PRIMARY KEY(program_id, op_order)"
        ");"
        
                // === Step2: word-context rules ===
        "CREATE TABLE IF NOT EXISTS struct_word_change_rules("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  src_word TEXT NOT NULL COLLATE NOCASE,"
        "  dst_word TEXT NOT NULL,"
        "  section TEXT NOT NULL DEFAULT 'GOAL',"
        "  lev_dist INTEGER NOT NULL DEFAULT 0,"
        "  embed_model TEXT NOT NULL,"
        "  embed_dim INTEGER NOT NULL,"
        "  ctx_seq TEXT NOT NULL,"
        "  embedding BLOB NOT NULL,"
        "  successes INTEGER NOT NULL DEFAULT 0,"
        "  failures INTEGER NOT NULL DEFAULT 0,"
        "  created_at TEXT DEFAULT CURRENT_TIMESTAMP,"
        "  UNIQUE(src_word, dst_word, section, ctx_seq, embed_model, embed_dim)"
        ");"

        // === Step2: value rules (ALUE) ===
        "CREATE TABLE IF NOT EXISTS struct_value_rules("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  key TEXT NOT NULL,"
        "  section TEXT NOT NULL DEFAULT 'GOAL',"
        "  embed_model TEXT NOT NULL,"
        "  embed_dim INTEGER NOT NULL,"
        "  ctx_seq TEXT NOT NULL,"
        "  embedding BLOB NOT NULL,"
        "  successes INTEGER NOT NULL DEFAULT 0,"
        "  failures INTEGER NOT NULL DEFAULT 0,"
        "  created_at TEXT DEFAULT CURRENT_TIMESTAMP,"
        "  UNIQUE(key, section, ctx_seq, embed_model, embed_dim)"
        ");"

        // === Step2: apply logs (for penalty-after-fix) ===
        "CREATE TABLE IF NOT EXISTS struct_delta_apply_logs("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  input_fingerprint TEXT NOT NULL,"
        "  embed_model TEXT NOT NULL,"
        "  embed_dim INTEGER NOT NULL,"
        "  syntax_hint TEXT NOT NULL DEFAULT '',"
        "  ring_hint INTEGER NOT NULL DEFAULT 0,"
        "  created_at TEXT DEFAULT CURRENT_TIMESTAMP"
        ");"
        "CREATE TABLE IF NOT EXISTS struct_delta_apply_log_ops("
        "  log_id INTEGER NOT NULL,"
        "  ordinal INTEGER NOT NULL,"
        "  section TEXT NOT NULL,"
        "  section_fp TEXT NOT NULL,"
        "  op_key TEXT NOT NULL,"
        "  op_type INTEGER NOT NULL,"
        "  match_before TEXT NOT NULL DEFAULT '',"
        "  match_after TEXT NOT NULL DEFAULT '',"
        "  dst_section TEXT NOT NULL DEFAULT '',"
        "  meta TEXT NOT NULL DEFAULT '',"
        "  PRIMARY KEY(log_id, ordinal)"
        ");"

        // === Step2: penalty 0% per (section_fp, op_key) ===
        "CREATE TABLE IF NOT EXISTS struct_delta_op_penalties("
        "  section_fp TEXT NOT NULL,"
        "  section TEXT NOT NULL,"
        "  op_key TEXT NOT NULL,"
        "  disabled INTEGER NOT NULL DEFAULT 0,"
        "  updated_at TEXT DEFAULT CURRENT_TIMESTAMP,"
        "  PRIMARY KEY(section_fp, section, op_key)"
        ");"
        
                // === Step1.4: embeddings artifacts per AUTO->FIX pair ===
        "CREATE TABLE IF NOT EXISTS struct_fix_pair_embeddings("
        "  pair_id INTEGER NOT NULL,"
        "  kind TEXT NOT NULL,"                 // INPUT_CANON, AFTER:GOAL, AFTER:REQUIREMENTS, ...
        "  embed_model TEXT NOT NULL,"
        "  embed_dim INTEGER NOT NULL,"
        "  embedding BLOB NOT NULL,"
        "  created_at TEXT DEFAULT CURRENT_TIMESTAMP,"
        "  PRIMARY KEY(pair_id, kind, embed_model, embed_dim)"
        ");"

        "CREATE TABLE IF NOT EXISTS struct_fix_pair_span_embeddings("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  pair_id INTEGER NOT NULL,"
        "  op_type INTEGER NOT NULL,"           // SD_OP_MOVE_TOKENS / SD_OP_INSERT_TOKENS
        "  ordinal INTEGER NOT NULL,"           // index in derived list
        "  src_section TEXT NOT NULL DEFAULT '',"
        "  dst_section TEXT NOT NULL DEFAULT '',"
        "  anchor_hash TEXT NOT NULL DEFAULT '',"
        "  payload_hash TEXT NOT NULL DEFAULT '',"
        "  offset INTEGER NOT NULL DEFAULT 0,"
        "  which INTEGER NOT NULL DEFAULT 0,"   // 0=before_ctx, 1=after_ctx
        "  ctx_seq TEXT NOT NULL,"
        "  embed_model TEXT NOT NULL,"
        "  embed_dim INTEGER NOT NULL,"
        "  embedding BLOB NOT NULL,"
        "  created_at TEXT DEFAULT CURRENT_TIMESTAMP"
        ");";
	
       /* индексы */
	const char *sql_indexes =
		"CREATE INDEX IF NOT EXISTS idx_struct_example_ops_label ON struct_example_ops(label_id);"
		"CREATE INDEX IF NOT EXISTS idx_struct_fix_pair_labels_label ON struct_fix_pair_labels(label_id);"
		"CREATE INDEX IF NOT EXISTS idx_struct_examples_hint ON struct_examples(approved, syntax_hint, ring_hint);"
		"CREATE INDEX IF NOT EXISTS idx_struct_example_embeddings_model_dim ON struct_example_embeddings(model, dim);"
		"CREATE INDEX IF NOT EXISTS idx_struct_transfer_rules_tid_ord ON struct_transfer_rules(template_id, ordinal);"
		"CREATE INDEX IF NOT EXISTS idx_struct_delta_examples_scope ON struct_delta_examples(embed_model, embed_dim, syntax_hint, ring_hint);"
		"CREATE INDEX IF NOT EXISTS idx_struct_delta_programs_scope ON struct_delta_programs(embed_model, embed_dim, syntax_hint, ring_hint);"
		"CREATE INDEX IF NOT EXISTS idx_struct_delta_ops_pid_ord ON struct_delta_ops(program_id, op_order);"
		
		"CREATE INDEX IF NOT EXISTS idx_word_change_src ON struct_word_change_rules(src_word, section);"
        "CREATE INDEX IF NOT EXISTS idx_word_change_scope ON struct_word_change_rules(embed_model, embed_dim, section);"
        "CREATE INDEX IF NOT EXISTS idx_value_rules_scope ON struct_value_rules(embed_model, embed_dim, section);"
        "CREATE INDEX IF NOT EXISTS idx_value_rules_key ON struct_value_rules(key, section);"
        "CREATE INDEX IF NOT EXISTS idx_apply_logs_fp ON struct_delta_apply_logs(input_fingerprint, id);"
		"CREATE INDEX IF NOT EXISTS idx_key_types_updated ON struct_key_types(updated_at);"
        "CREATE INDEX IF NOT EXISTS idx_apply_log_ops_log ON struct_delta_apply_log_ops(log_id, ordinal);"
        "CREATE INDEX IF NOT EXISTS idx_penalty_lookup ON struct_delta_op_penalties(section_fp, section, op_key);"
		
		"CREATE INDEX IF NOT EXISTS idx_fix_pair_embeddings_pair ON struct_fix_pair_embeddings(pair_id);"
        "CREATE INDEX IF NOT EXISTS idx_fix_pair_embeddings_kind ON struct_fix_pair_embeddings(kind);"
        "CREATE UNIQUE INDEX IF NOT EXISTS uq_fix_pair_span_key ON struct_fix_pair_span_embeddings(pair_id, op_type, ordinal, which, embed_model, embed_dim);"
        "CREATE INDEX IF NOT EXISTS idx_fix_pair_span_pair ON struct_fix_pair_span_embeddings(pair_id);";

    char *err = NULL;

	if (sqlite3_exec(db, sql_tables, NULL, NULL, &err) != SQLITE_OK) {
		if (err) {
			fprintf(stderr, "[SKYNET][STRUCTURIZE][DB] ERROR: ensure schema failed: %s\n", err);
			sqlite3_free(err);
		}
		return 0;
	}

    /* миграция старых БД (если таблица была создана ранее без model/dim/updated_at) */
    if (!structurize_ops_migrate_schema(db)) {
        return 0;
    }

    if (sqlite3_exec(db, sql_indexes, NULL, NULL, &err) != SQLITE_OK) {
        if (err) {
            fprintf(stderr, "[SKYNET][STRUCTURIZE][DB] ERROR: ensure schema failed: %s\n", err);
            sqlite3_free(err);
        }
        return 0;
    }

    return 1;
}

typedef struct {
    sqlite3_int64 example_id;
    double sim;
} StructK;

// -------------------- Patch-programs (LLM FIX -> reuse) --------------------
// Project S: no semantic hardcode. Program is purely textual, derived from before/after.

typedef struct {
    sqlite3_int64 id;
    double sim;
    double score;
    double rel;
    int kind;
    int successes;
    int failures;
    char *left;
    char *right;
    char *repl;
} StructProgK;

// ============================================================================
// Project S v2.0: Transfer Templates (повторяемые переносы из GOAL)
// Логика:
// - Learn: из (before GOAL-only) и (after structured) делаем список переносов.
// - Apply: на новой задаче сначала всё в GOAL, затем пробуем повторить переносы.
// - Если needle не найден точно -> не трогаем, оставляем в GOAL.
// - После применения: validate; успех/провал записываем в successes/failures.
// ============================================================================

typedef struct TT_Cand {
    sqlite3_int64 id;
    int successes;
    int failures;
    double rel;       // (succ+1)/(succ+fail+2)
    int matchable;    // сколько переносов можно применить сейчас
} TT_Cand;

static void tt_free_lines(char **lines, int n) {
    if (!lines) return;
    for (int i = 0; i < n; ++i) {
        if (lines[i]) free(lines[i]);
    }
    free(lines);
}

static int tt_is_none_text(const char *s) {
    if (!s) return 1;
    while (*s && (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')) s++;
    if (!*s) return 1;
    // точное NONE (после trim)
    if (s[0]=='N' && s[1]=='O' && s[2]=='N' && s[3]=='E' && (s[4]==0 || s[4]=='\r' || s[4]=='\n')) return 1;
    return 0;
}

static int tt_split_lines(const char *payload, char ***out_lines, int *out_n) {
    if (!out_lines || !out_n) return 0;
    *out_lines = NULL;
    *out_n = 0;

    if (!payload) return 1;

    char *buf = _strdup(payload);
    if (!buf) return 0;

    clar_trim(buf);
    if (tt_is_none_text(buf)) {
        free(buf);
        return 1;
    }

    int cap = 32;
    char **arr = (char **)calloc((size_t)cap, sizeof(char *));
    if (!arr) { free(buf); return 0; }

    int n = 0;
    char *p = buf;
    while (*p) {
        // line start
        char *e = p;
        while (*e && *e != '\n' && *e != '\r') e++;

        char save = *e;
        *e = '\0';

        clar_trim(p);
        if (p[0] && strcmp(p, "NONE") != 0) {
            if (n >= cap) {
                cap *= 2;
                char **tmp = (char **)realloc(arr, (size_t)cap * sizeof(char *));
                if (!tmp) {
                    *e = save;
                    tt_free_lines(arr, n);
                    free(buf);
                    return 0;
                }
                arr = tmp;
            }
            arr[n] = _strdup(p);
            if (!arr[n]) {
                *e = save;
                tt_free_lines(arr, n);
                free(buf);
                return 0;
            }
            n++;
        }

        *e = save;

        // advance to next line
        while (*e && (*e == '\n' || *e == '\r')) e++;
        p = e;
    }

    free(buf);
    *out_lines = arr;
    *out_n = n;
    return 1;
}

// find unique line index in GOAL (ignoring used[])
static int tt_find_unique_line(char **goal_lines, int goal_n, const char *line, const int *used)
{
    if (!goal_lines || goal_n <= 0 || !line || !line[0]) return -1;

    int found = -1;
    int hits = 0;

    for (int i = 0; i < goal_n; ++i) {
        if (used && used[i]) continue;
        if (!goal_lines[i]) continue;
        if (strcmp(goal_lines[i], line) != 0) continue;

        found = i;
        hits++;
        if (hits > 1) return -2; // ambiguous
    }

    return (hits == 1) ? found : -1;
}

// get first and last non-empty line from a block text (needle may contain '\n')
static void tt_block_first_last(const char *block, char *out_first, size_t out_first_sz,
                                char *out_last, size_t out_last_sz)
{
    if (out_first && out_first_sz) out_first[0] = '\0';
    if (out_last  && out_last_sz)  out_last[0]  = '\0';
    if (!block || !block[0]) return;

    const char *p = block;
    char first[1024]; first[0] = '\0';
    char last[1024];  last[0]  = '\0';

    while (*p) {
        const char *e = p;
        while (*e && *e != '\n' && *e != '\r') e++;

        size_t len = (size_t)(e - p);
        if (len > 0 && len < sizeof(first)) {
            char tmp[1024];
            memcpy(tmp, p, len);
            tmp[len] = '\0';
            clar_trim(tmp);

            if (tmp[0] && strcmp(tmp, "NONE") != 0) {
				if (!first[0]) {
					size_t n = strlen(tmp);
					if (n >= sizeof(first)) n = sizeof(first) - 1;
					memcpy(first, tmp, n);
					first[n] = '\0';
				}

				{
					size_t n = strlen(tmp);
					if (n >= sizeof(last)) n = sizeof(last) - 1;
					memcpy(last, tmp, n);
					last[n] = '\0';
				}
            }
        }

        while (*e && (*e == '\n' || *e == '\r')) e++;
        p = e;
    }

    if (out_first && out_first_sz && first[0]) {
        strncpy(out_first, first, out_first_sz - 1);
        out_first[out_first_sz - 1] = '\0';
    }
    if (out_last && out_last_sz && last[0]) {
        strncpy(out_last, last, out_last_sz - 1);
        out_last[out_last_sz - 1] = '\0';
    }
}

static int tt_append_block(char *dst, size_t dst_sz, const char *block_text)
{
    if (!dst || dst_sz == 0 || !block_text || !block_text[0]) return 0;

    if (tt_is_none_text(dst)) dst[0] = '\0';

    size_t cur = strlen(dst);
    size_t add = strlen(block_text);

    size_t need = cur + (cur ? 1 : 0) + add + 1;
    if (need > dst_sz) return 0;

    if (cur) {
        dst[cur] = '\n';
        dst[cur + 1] = '\0';
        cur++;
    }

    memcpy(dst + cur, block_text, add);
    dst[cur + add] = '\0';
    return 1;
}

static int tt_append_line(char *dst, size_t dst_sz, const char *line) {
    if (!dst || dst_sz == 0 || !line || !line[0]) return 0;

    // treat "NONE" as empty
    if (tt_is_none_text(dst)) dst[0] = '\0';

    size_t cur = strlen(dst);
    size_t add = strlen(line);

    size_t need = cur + add + (cur ? 1 : 0) + 1;
    if (need > dst_sz) return 0;

    if (cur) {
        dst[cur] = '\n';
        dst[cur + 1] = '\0';
        cur++;
    }
    memcpy(dst + cur, line, add);
    dst[cur + add] = '\0';
    return 1;
}

static void tt_rebuild_struct(char *io_struct, size_t io_sz,
                              const char *goal,
                              const char *reqs,
                              const char *inp,
                              const char *outp,
                              const char *cons,
                              const char *env,
                              const char *edit,
                              const char *ng)
{
    if (!io_struct || io_sz == 0) return;

    const char *G  = (goal && goal[0]) ? goal : "NONE";
    const char *R  = (reqs && reqs[0]) ? reqs : "NONE";
    const char *I  = (inp  && inp[0])  ? inp  : "NONE";
    const char *O  = (outp && outp[0]) ? outp : "NONE";
    const char *C  = (cons && cons[0]) ? cons : "NONE";
    const char *E  = (env  && env[0])  ? env  : "NONE";
    const char *EP = (edit && edit[0]) ? edit : "NONE";
    const char *NG = (ng   && ng[0])   ? ng   : "NONE";

    _snprintf_s(io_struct, io_sz, _TRUNCATE,
        "CLARIFIED_SPEC:\n"
        "GOAL:\n%s\n\n"
        "REQUIREMENTS:\n%s\n\n"
        "INPUT:\n%s\n\n"
        "OUTPUT:\n%s\n\n"
        "CONSTRAINTS:\n%s\n\n"
        "ENVIRONMENT:\n%s\n\n"
        "EDIT_PARAMETERS:\n%s\n\n"
        "NON_GOALS:\n%s\n",
        G, R, I, O, C, E, EP, NG
    );
}

static sqlite3_int64 structurize_tt_learn_from_pair(sqlite3 *db,
                                                    sqlite3_int64 pair_id,
                                                    const char *before_struct,
                                                    const char *after_struct)
{
    if (!db || pair_id <= 0 || !before_struct || !after_struct) return 0;

    // ---- Extract GOAL before/after ----
    char before_goal[65536]; before_goal[0] = '\0';
    char after_goal[65536];  after_goal[0]  = '\0';

    tr_copy_section_payload(before_struct, "GOAL:", before_goal, sizeof(before_goal));
    tr_copy_section_payload(after_struct,  "GOAL:", after_goal,  sizeof(after_goal));

    clar_trim(before_goal);
    clar_trim(after_goal);

    if (!before_goal[0] || strcmp(before_goal, "NONE") == 0) return 0;

    // ---- Split BEFORE GOAL lines ----
    char **b_lines = NULL;
    int b_n = 0;
    if (!tt_split_lines(before_goal, &b_lines, &b_n)) return 0;

    int *used = NULL;
    if (b_n > 0) used = (int *)calloc((size_t)b_n, sizeof(int));

    // ---- Cache AFTER sections ----
    struct SecCache { const char *sec; const char *hdr; char payload[65536]; char **lines; int n; } secs[] = {
        { "REQUIREMENTS",    "REQUIREMENTS:",    {0}, NULL, 0 },
        { "INPUT",           "INPUT:",           {0}, NULL, 0 },
        { "OUTPUT",          "OUTPUT:",          {0}, NULL, 0 },
        { "CONSTRAINTS",     "CONSTRAINTS:",     {0}, NULL, 0 },
        { "ENVIRONMENT",     "ENVIRONMENT:",     {0}, NULL, 0 },
        { "EDIT_PARAMETERS", "EDIT_PARAMETERS:", {0}, NULL, 0 },
        { "NON_GOALS",       "NON_GOALS:",       {0}, NULL, 0 }
    };

    for (int si = 0; si < (int)(sizeof(secs)/sizeof(secs[0])); ++si) {
        secs[si].payload[0] = '\0';
        tr_copy_section_payload(after_struct, secs[si].hdr, secs[si].payload, sizeof(secs[si].payload));
        clar_trim(secs[si].payload);
        secs[si].lines = NULL;
        secs[si].n = 0;
        if (secs[si].payload[0] && strcmp(secs[si].payload, "NONE") != 0) {
            tt_split_lines(secs[si].payload, &secs[si].lines, &secs[si].n);
        }
    }

    // ---- Create template row ----
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
        "INSERT INTO struct_transfer_templates(pair_id, successes, failures) VALUES(?, 0, 0);",
        -1, &st, NULL) != SQLITE_OK)
    {
        if (used) free(used);
        tt_free_lines(b_lines, b_n);
        for (int si = 0; si < (int)(sizeof(secs)/sizeof(secs[0])); ++si) tt_free_lines(secs[si].lines, secs[si].n);
        return 0;
    }

    sqlite3_bind_int64(st, 1, pair_id);

    if (sqlite3_step(st) != SQLITE_DONE) {
        sqlite3_finalize(st);
        if (used) free(used);
        tt_free_lines(b_lines, b_n);
        for (int si = 0; si < (int)(sizeof(secs)/sizeof(secs[0])); ++si) tt_free_lines(secs[si].lines, secs[si].n);
        return 0;
    }
    sqlite3_finalize(st);

    sqlite3_int64 tpl_id = sqlite3_last_insert_rowid(db);
    if (tpl_id <= 0) {
        if (used) free(used);
        tt_free_lines(b_lines, b_n);
        for (int si = 0; si < (int)(sizeof(secs)/sizeof(secs[0])); ++si) tt_free_lines(secs[si].lines, secs[si].n);
        return 0;
    }

    const char *sql_ins_rule =
        "INSERT INTO struct_transfer_rules(template_id, ordinal, from_section, to_section, needle, left_ctx, right_ctx) "
        "VALUES(?, ?, 'GOAL', ?, ?, ?, ?);";

    int ordinal = 0;
    int rules_added = 0;

    // ============================================================
    // PASS 1 (preferred): learn from AFTER sections by (first,last) in BEFORE GOAL
    // ============================================================
    for (int si = 0; si < (int)(sizeof(secs)/sizeof(secs[0])); ++si) {
        if (!secs[si].payload[0] || strcmp(secs[si].payload, "NONE") == 0) continue;

        char first[1024], last[1024];
        tt_block_first_last(secs[si].payload, first, sizeof(first), last, sizeof(last));
        if (!first[0] || !last[0]) continue;

        int fi = tt_find_unique_line(b_lines, b_n, first, used);
        int li = tt_find_unique_line(b_lines, b_n, last,  used);

        if (fi < 0 || li < 0) continue;
        if (fi > li) continue;

        int ok = 1;
        for (int k = fi; k <= li; ++k) {
            if (used && used[k]) { ok = 0; break; }
        }
        if (!ok) continue;

        const char *left_anchor  = (fi > 0 && b_lines[fi - 1]) ? b_lines[fi - 1] : "";
        const char *right_anchor = (li + 1 < b_n && b_lines[li + 1]) ? b_lines[li + 1] : "";

        if ((!left_anchor || !left_anchor[0]) && (!right_anchor || !right_anchor[0])) continue;

        char needle[65536];
        needle[0] = '\0';
        for (int k = fi; k <= li; ++k) {
            if (!b_lines[k] || !b_lines[k][0]) continue;
            if (!tt_append_line(needle, sizeof(needle), b_lines[k])) { needle[0] = '\0'; break; }
        }
        if (!needle[0]) continue;

        sqlite3_stmt *rst = NULL;
        if (sqlite3_prepare_v2(db, sql_ins_rule, -1, &rst, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(rst, 1, tpl_id);
            sqlite3_bind_int(rst, 2, ++ordinal);
            sqlite3_bind_text(rst, 3, secs[si].sec, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(rst, 4, needle, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(rst, 5, left_anchor, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(rst, 6, right_anchor, -1, SQLITE_TRANSIENT);

            if (sqlite3_step(rst) == SQLITE_DONE) {
                rules_added++;
                if (used) {
                    for (int k = fi; k <= li; ++k) used[k] = 1;
                }
            }
            sqlite3_finalize(rst);
        }
    }

    // ============================================================
    // PASS 2 (fallback): diff GOAL(before)->GOAL(after) spans, map by overlap with AFTER sections
    // ============================================================
    if (!after_goal[0]) {
        strncpy(after_goal, "NONE", sizeof(after_goal) - 1);
        after_goal[sizeof(after_goal) - 1] = '\0';
    }

    char **a_lines = NULL;
    int a_n = 0;
    if (strcmp(after_goal, "NONE") != 0) {
        tt_split_lines(after_goal, &a_lines, &a_n);
    }

    int *keep = NULL;
    if (b_n > 0) keep = (int *)calloc((size_t)b_n, sizeof(int));

    // greedy subsequence match: AFTER_GOAL lines inside BEFORE_GOAL, skipping already-used lines
    int cursor = 0;
    for (int ai = 0; ai < a_n; ++ai) {
        const char *L = a_lines[ai];
        if (!L || !L[0]) continue;

        int found = -1;
        for (int bj = cursor; bj < b_n; ++bj) {
            if (used && used[bj]) continue;
            if (keep && keep[bj]) continue;
            if (!b_lines[bj]) continue;
            if (strcmp(b_lines[bj], L) == 0) { found = bj; break; }
        }
        if (found >= 0) {
            if (keep) keep[found] = 1;
            cursor = found + 1;
        }
    }

    // scan removed spans = lines not used and not kept
    for (int i = 0; i < b_n; ) {
        if ((used && used[i]) || (keep && keep[i])) { i++; continue; }

        int a = i;
        int b = i;
        while (b + 1 < b_n && !(used && used[b + 1]) && !(keep && keep[b + 1])) b++;

        // score overlap with sections
        int best_si = -1;
        int best_score = 0;
        int tie = 0;

        for (int si = 0; si < (int)(sizeof(secs)/sizeof(secs[0])); ++si) {
            if (!secs[si].lines || secs[si].n <= 0) continue;

            int sc = 0;
            for (int k = a; k <= b; ++k) {
                const char *s = b_lines[k];
                if (!s || !s[0]) continue;
                for (int j = 0; j < secs[si].n; ++j) {
                    const char *t = secs[si].lines[j];
                    if (!t || !t[0]) continue;
                    if (strcmp(s, t) == 0) { sc++; break; }
                }
            }

            if (sc > best_score) { best_score = sc; best_si = si; tie = 0; }
            else if (sc > 0 && sc == best_score) { tie = 1; }
        }

        if (best_score <= 0 || best_si < 0 || tie) {
            i = b + 1;
            continue;
        }

        // anchors: nearest kept (not used) line around span
        const char *left_anchor = "";
        for (int k = a - 1; k >= 0; --k) {
            if (used && used[k]) continue;
            if (keep && keep[k] && b_lines[k] && b_lines[k][0]) { left_anchor = b_lines[k]; break; }
        }

        const char *right_anchor = "";
        for (int k = b + 1; k < b_n; ++k) {
            if (used && used[k]) continue;
            if (keep && keep[k] && b_lines[k] && b_lines[k][0]) { right_anchor = b_lines[k]; break; }
        }

        if ((!left_anchor || !left_anchor[0]) && (!right_anchor || !right_anchor[0])) {
            i = b + 1;
            continue;
        }

        char needle[65536];
        needle[0] = '\0';
        for (int k = a; k <= b; ++k) {
            if (!b_lines[k] || !b_lines[k][0]) continue;
            if (!tt_append_line(needle, sizeof(needle), b_lines[k])) { needle[0] = '\0'; break; }
        }
        if (!needle[0]) { i = b + 1; continue; }

        sqlite3_stmt *rst = NULL;
        if (sqlite3_prepare_v2(db, sql_ins_rule, -1, &rst, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(rst, 1, tpl_id);
            sqlite3_bind_int(rst, 2, ++ordinal);
            sqlite3_bind_text(rst, 3, secs[best_si].sec, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(rst, 4, needle, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(rst, 5, left_anchor, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(rst, 6, right_anchor, -1, SQLITE_TRANSIENT);

            if (sqlite3_step(rst) == SQLITE_DONE) {
                rules_added++;
                if (used) {
                    for (int k = a; k <= b; ++k) used[k] = 1;
                }
            }
            sqlite3_finalize(rst);
        }

        i = b + 1;
    }

    if (keep) free(keep);
    tt_free_lines(a_lines, a_n);

    // ---- cleanup section caches + goal lines ----
    if (used) free(used);
    tt_free_lines(b_lines, b_n);
    for (int si = 0; si < (int)(sizeof(secs)/sizeof(secs[0])); ++si) tt_free_lines(secs[si].lines, secs[si].n);

    // ---- delete empty template ----
    if (rules_added <= 0) {
        sqlite3_stmt *dst = NULL;
        if (sqlite3_prepare_v2(db, "DELETE FROM struct_transfer_templates WHERE id=?;", -1, &dst, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(dst, 1, tpl_id);
            sqlite3_step(dst);
            sqlite3_finalize(dst);
        }
        return 0;
    }

    return tpl_id;
}

static int structurize_tt_count_matches(sqlite3 *db, sqlite3_int64 tpl_id, char **goal_lines, int goal_n)
{
    if (!db || tpl_id <= 0 || !goal_lines || goal_n <= 0) return 0;

    sqlite3_stmt *st = NULL;
    const char *sql =
        "SELECT needle, left_ctx, right_ctx FROM struct_transfer_rules "
        "WHERE template_id=? ORDER BY ordinal ASC;";

    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int64(st, 1, tpl_id);

    int *used = (int *)calloc((size_t)goal_n, sizeof(int));
    int matched = 0;

    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *needle = (const char *)sqlite3_column_text(st, 0);
        const char *lctx   = (const char *)sqlite3_column_text(st, 1);
        const char *rctx   = (const char *)sqlite3_column_text(st, 2);

        char first[1024], last[1024];
        tt_block_first_last(needle ? needle : "", first, sizeof(first), last, sizeof(last));

        int li = (lctx && lctx[0]) ? tt_find_unique_line(goal_lines, goal_n, lctx, used) : -1;
        int ri = (rctx && rctx[0]) ? tt_find_unique_line(goal_lines, goal_n, rctx, used) : -1;

        int a = -1, b = -1;

        if (li >= 0 && ri >= 0) {
            if (li + 1 >= ri) continue;
            a = li + 1;
            b = ri - 1;
        } else if (ri >= 0 && first[0]) {
            int fi = tt_find_unique_line(goal_lines, goal_n, first, used);
            if (fi < 0 || fi >= ri) continue;
            a = fi;
            b = ri - 1;
        } else if (li >= 0 && last[0]) {
            int la = tt_find_unique_line(goal_lines, goal_n, last, used);
            if (la < 0 || la <= li) continue;
            a = li + 1;
            b = la;
        } else {
            continue;
        }

        // ensure span is clean
        int ok = 1;
        for (int k = a; k <= b; ++k) {
            if (used[k]) { ok = 0; break; }
        }
        if (!ok) continue;

        for (int k = a; k <= b; ++k) used[k] = 1;
        matched++;
    }

    if (used) free(used);
    sqlite3_finalize(st);
    return matched;
}

static void structurize_tt_mark(sqlite3 *db, sqlite3_int64 tpl_id, int success) {
    if (!db || tpl_id <= 0) return;
    const char *sql_ok = "UPDATE struct_transfer_templates SET successes=successes+1 WHERE id=?;";
    const char *sql_no = "UPDATE struct_transfer_templates SET failures=failures+1 WHERE id=?;";
    const char *sql = success ? sql_ok : sql_no;

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return;
    sqlite3_bind_int64(st, 1, tpl_id);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

static int structurize_tt_apply_template(sqlite3 *db, sqlite3_int64 tpl_id, char *io_struct, size_t io_sz, int *out_moved)
{
    if (out_moved) *out_moved = 0;
    if (!db || tpl_id <= 0 || !io_struct || io_sz == 0) return 0;

    // extract current sections
    char goal[65536], reqs[65536], inp[65536], outp[65536], cons[65536], env[65536], edit[65536], ng[65536];
    goal[0]=reqs[0]=inp[0]=outp[0]=cons[0]=env[0]=edit[0]=ng[0]='\0';

    structurize_copy_goal_requirements_compat(io_struct, goal, sizeof(goal), reqs, sizeof(reqs));
    tr_copy_section_payload(io_struct, "INPUT:", inp, sizeof(inp));
    tr_copy_section_payload(io_struct, "OUTPUT:", outp, sizeof(outp));
    tr_copy_section_payload(io_struct, "CONSTRAINTS:", cons, sizeof(cons));
    tr_copy_section_payload(io_struct, "ENVIRONMENT:", env, sizeof(env));
    tr_copy_section_payload(io_struct, "EDIT_PARAMETERS:", edit, sizeof(edit));
    tr_copy_section_payload(io_struct, "NON_GOALS:", ng, sizeof(ng));

    // split goal lines
    char **goal_lines = NULL;
    int goal_n = 0;
    if (!tt_split_lines(goal, &goal_lines, &goal_n)) return 0;

    int *used = NULL;
    if (goal_n > 0) used = (int *)calloc((size_t)goal_n, sizeof(int));

    sqlite3_stmt *st = NULL;
    const char *sql =
        "SELECT to_section, needle, left_ctx, right_ctx "
        "FROM struct_transfer_rules WHERE template_id=? ORDER BY ordinal ASC;";

    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        if (used) free(used);
        tt_free_lines(goal_lines, goal_n);
        return 0;
    }
    sqlite3_bind_int64(st, 1, tpl_id);

    int moved = 0;

    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *to_sec = (const char *)sqlite3_column_text(st, 0);
        const char *needle = (const char *)sqlite3_column_text(st, 1);
        const char *lctx   = (const char *)sqlite3_column_text(st, 2);
        const char *rctx   = (const char *)sqlite3_column_text(st, 3);

        if (!to_sec) continue;

        char first[1024], last[1024];
        tt_block_first_last(needle ? needle : "", first, sizeof(first), last, sizeof(last));

        int li = (lctx && lctx[0]) ? tt_find_unique_line(goal_lines, goal_n, lctx, used) : -1;
        int ri = (rctx && rctx[0]) ? tt_find_unique_line(goal_lines, goal_n, rctx, used) : -1;

        int a = -1, b = -1;

        // Preferred: both anchors
        if (li >= 0 && ri >= 0) {
            if (li + 1 >= ri) continue;
            a = li + 1;
            b = ri - 1;
        }
        // Start-anchored: use example first line + right anchor
        else if (ri >= 0 && first[0]) {
            int fi = tt_find_unique_line(goal_lines, goal_n, first, used);
            if (fi < 0 || fi >= ri) continue;
            a = fi;
            b = ri - 1;
        }
        // End-anchored: use left anchor + example last line
        else if (li >= 0 && last[0]) {
            int la = tt_find_unique_line(goal_lines, goal_n, last, used);
            if (la < 0 || la <= li) continue;
            a = li + 1;
            b = la;
        }
        else {
            continue;
        }

        // ensure span is clean
        int ok = 1;
        for (int k = a; k <= b; ++k) {
            if (used && used[k]) { ok = 0; break; }
        }
        if (!ok) continue;

        // build current block text from GOAL lines [a..b]
        char block[65536];
        block[0] = '\0';

        for (int k = a; k <= b; ++k) {
            if (!goal_lines[k] || !goal_lines[k][0]) continue;
            if (!tt_append_line(block, sizeof(block), goal_lines[k])) { ok = 0; break; }
        }
        if (!ok || !block[0]) continue;

        // choose destination buffer
        char *dst = NULL;
        size_t dst_sz = 0;

        if (strcmp(to_sec, "REQUIREMENTS") == 0) { dst = reqs; dst_sz = sizeof(reqs); }
        else if (strcmp(to_sec, "INPUT") == 0) { dst = inp; dst_sz = sizeof(inp); }
        else if (strcmp(to_sec, "OUTPUT") == 0) { dst = outp; dst_sz = sizeof(outp); }
        else if (strcmp(to_sec, "CONSTRAINTS") == 0) { dst = cons; dst_sz = sizeof(cons); }
        else if (strcmp(to_sec, "ENVIRONMENT") == 0) { dst = env; dst_sz = sizeof(env); }
        else if (strcmp(to_sec, "EDIT_PARAMETERS") == 0) { dst = edit; dst_sz = sizeof(edit); }
        else if (strcmp(to_sec, "NON_GOALS") == 0) { dst = ng; dst_sz = sizeof(ng); }
        else continue;

        if (!tt_append_block(dst, dst_sz, block)) continue;

        // mark removed from GOAL
        for (int k = a; k <= b; ++k) {
            if (used) used[k] = 1;
        }
        moved++;
    }

    sqlite3_finalize(st);

    // rebuild GOAL from non-removed lines
    char new_goal[65536];
    new_goal[0] = '\0';

    for (int i = 0; i < goal_n; ++i) {
        if (used && used[i]) continue;
        if (!goal_lines[i] || !goal_lines[i][0]) continue;
        if (!tt_append_line(new_goal, sizeof(new_goal), goal_lines[i])) break;
    }

    if (!new_goal[0]) {
        strncpy(new_goal, "NONE", sizeof(new_goal) - 1);
        new_goal[sizeof(new_goal) - 1] = '\0';
    }

    if (used) free(used);
    tt_free_lines(goal_lines, goal_n);

    // rebuild full struct
    tt_rebuild_struct(io_struct, io_sz, new_goal, reqs, inp, outp, cons, env, edit, ng);

    if (out_moved) *out_moved = moved;
    return 1;
}

static sqlite3_int64 structurize_try_apply_transfer_templates(sqlite3 *db, char *io_struct, size_t io_sz) {
    if (!db || !io_struct || io_sz == 0) return 0;

    // current goal lines for match counting
    char goal[65536], reqs[64];
    goal[0] = '\0';
    reqs[0] = '\0';
    structurize_copy_goal_requirements_compat(io_struct, goal, sizeof(goal), reqs, sizeof(reqs));

    char **goal_lines = NULL;
    int goal_n = 0;
    if (!tt_split_lines(goal, &goal_lines, &goal_n)) return 0;

    // read candidate templates
    sqlite3_stmt *st = NULL;
    const char *sql =
        "SELECT id, successes, failures FROM struct_transfer_templates "
        "ORDER BY (successes + 1.0) / (successes + failures + 2.0) DESC, "
        "(successes + failures) DESC, id DESC "
        "LIMIT 12;";

    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        tt_free_lines(goal_lines, goal_n);
        return 0;
    }

    TT_Cand cand[12];
    int cand_n = 0;
    memset(cand, 0, sizeof(cand));

    while (sqlite3_step(st) == SQLITE_ROW) {
        if (cand_n >= (int)(sizeof(cand)/sizeof(cand[0]))) break;
        cand[cand_n].id = sqlite3_column_int64(st, 0);
        cand[cand_n].successes = sqlite3_column_int(st, 1);
        cand[cand_n].failures  = sqlite3_column_int(st, 2);
        cand[cand_n].rel = (cand[cand_n].successes + 1.0) / (cand[cand_n].successes + cand[cand_n].failures + 2.0);
        cand[cand_n].matchable = structurize_tt_count_matches(db, cand[cand_n].id, goal_lines, goal_n);
        cand_n++;
    }
    sqlite3_finalize(st);

    tt_free_lines(goal_lines, goal_n);

    if (cand_n <= 0) return 0;

    // sort: matchable desc, rel desc
    for (int i = 0; i < cand_n; ++i) {
        int best = i;
        for (int j = i + 1; j < cand_n; ++j) {
            if (cand[j].matchable > cand[best].matchable) best = j;
            else if (cand[j].matchable == cand[best].matchable && cand[j].rel > cand[best].rel) best = j;
        }
        if (best != i) {
            TT_Cand t = cand[i]; cand[i] = cand[best]; cand[best] = t;
        }
    }

    // ============================================================
    // v1: Multi-template apply (combo of 2 templates, no overlap risk)
    // Strategy:
    //   - pick top candidates with matchable>0
    //   - try combos (A->B) and (B->A) for top pairs
    //   - commit only if validate passes
    // ============================================================

    int best[4];
    int best_n = 0;
    for (int i = 0; i < cand_n && best_n < (int)(sizeof(best)/sizeof(best[0])); ++i) {
        if (cand[i].matchable <= 0) continue;
        best[best_n++] = i;
    }

    // Try combo on top pairs (up to first 3 candidates)
    for (int ai = 0; ai < best_n && ai < 3; ++ai) {
        for (int bi = ai + 1; bi < best_n && bi < 3; ++bi) {
            const int ia = best[ai];
            const int ib = best[bi];

            const sqlite3_int64 ida = cand[ia].id;
            const sqlite3_int64 idb = cand[ib].id;

            // pass=0: A->B, pass=1: B->A
            for (int pass = 0; pass < 2; ++pass) {
                const sqlite3_int64 first  = (pass == 0) ? ida : idb;
                const sqlite3_int64 second = (pass == 0) ? idb : ida;

                const double rel1 = (pass == 0) ? cand[ia].rel : cand[ib].rel;
                const double rel2 = (pass == 0) ? cand[ib].rel : cand[ia].rel;

                char tmp[65536];
                strncpy(tmp, io_struct, sizeof(tmp) - 1);
                tmp[sizeof(tmp) - 1] = '\0';

                int moved1 = 0;
                int moved2 = 0;

                if (!structurize_tt_apply_template(db, first, tmp, sizeof(tmp), &moved1)) {
                    structurize_tt_mark(db, first, 0);
                    continue;
                }

                if (!structurize_tt_apply_template(db, second, tmp, sizeof(tmp), &moved2)) {
                    structurize_tt_mark(db, second, 0);
                    continue;
                }

                const int moved_total = moved1 + moved2;
                if (moved_total <= 0) continue;

                if (!hybrid_validate_clarified_output(tmp)) {
                    if (moved1 > 0) structurize_tt_mark(db, first, 0);
                    if (moved2 > 0) structurize_tt_mark(db, second, 0);
                    continue;
                }

                // success: commit
                strncpy(io_struct, tmp, io_sz - 1);
                io_struct[io_sz - 1] = '\0';

                if (moved1 > 0) structurize_tt_mark(db, first, 1);
                if (moved2 > 0) structurize_tt_mark(db, second, 1);

                fprintf(stderr,
                        "[SKYNET][STRUCTURIZE][TT] applied combo first=%lld second=%lld moved=%d (m1=%d m2=%d) rel1=%.3f rel2=%.3f\n",
                        (long long)first, (long long)second,
                        moved_total, moved1, moved2, rel1, rel2);

                return first;
            }
        }
    }

    // Fallback: try single templates (as before)
    for (int i = 0, tried = 0; i < cand_n && tried < 3; ++i) {
        if (cand[i].matchable <= 0) continue;
        tried++;

        char tmp[65536];
        strncpy(tmp, io_struct, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = '\0';

        int moved = 0;
        if (!structurize_tt_apply_template(db, cand[i].id, tmp, sizeof(tmp), &moved)) {
            structurize_tt_mark(db, cand[i].id, 0);
            continue;
        }
        if (moved <= 0) continue;

        if (!hybrid_validate_clarified_output(tmp)) {
            structurize_tt_mark(db, cand[i].id, 0);
            continue;
        }

        strncpy(io_struct, tmp, io_sz - 1);
        io_struct[io_sz - 1] = '\0';
        structurize_tt_mark(db, cand[i].id, 1);

        fprintf(stderr, "[SKYNET][STRUCTURIZE][TT] applied template id=%lld moved=%d rel=%.3f\n",
                (long long)cand[i].id, moved, cand[i].rel);

        return cand[i].id;
    }

    return 0;
}

// ============================================================================
// Project S v2.1: Delta Programs (AUTO -> FIX) with strict guards
// - Embedding is only for candidate selection.
// - Actual edits are only recorded ops with guards.
// - If guard not satisfied => skip op (neutral).
// - After apply => validate(), else rollback and count failure.
// ============================================================================

// Project S v2.2: Delta Programs (AUTO -> FIX) token-level ops + legacy compatibility
#define SD_OP_MOVE_LINE        1   // legacy
#define SD_OP_INSERT_LINE      2   // legacy
#define SD_OP_REPLACE_SECTION  3   // legacy
#define SD_OP_DELETE_LINE      4   // legacy
// New token-level ops (learn/apply)
#define SD_OP_MOVE_TOKENS      11
#define SD_OP_DELETE_TOKENS    12
#define SD_OP_CHANGE_WORD      13  // learn+apply support
#define SD_OP_INSERT_TOKENS    14

#define SD_GUARD_NONE                 0
#define SD_GUARD_DST_MATCH_BEFORE     1   // legacy
#define SD_GUARD_DST_IS_NONE          2   // legacy
#define SD_GUARD_SRC_HAS_UNIQUE_LINE  3   // legacy
#define SD_GUARD_SRC_HAS_UNIQUE_TOKSEQ 10  // src must contain exactly one match of token-seq

#define SD_FLAG_GENERATIVE   (1 << 0)
#define SD_FLAG_CHG_ENDING           (1 << 1)  // Levenshtein <= 2 (окончания/опечатки)
#define SD_FLAG_CHG_REPLACE          (1 << 2)  // сильная замена слова
#define SD_FLAG_CHG_CENTER_SHIFT     8
#define SD_FLAG_CHG_CENTER_MASK      (0xFF << SD_FLAG_CHG_CENTER_SHIFT)

#define SD_RETRIEVE_LIMIT 256

#define SD_TOPK              40      // твой topK=40
#define SD_TRY_N             3       // legacy fallback (не используем в новом apply)
#define SD_MIN_SIM           0.75
#define SD_MIN_SCORE         0.42
#define SD_ALPHA             0.15f

// === Project S: Delta Token Ops helpers (canonize+tokenize+spans+move/delete+anchor) ===

static int sd_is_punct_char(unsigned char c) {
    switch (c) {
        case '.': case ',': case ';': case ':': case '!': case '?':
        case '(': case ')': case '[': case ']': case '{': case '}':
        case '<': case '>':
        case '"': case '\'':
        case '/': case '\\':
        case '|': case '+': case '*': case '=':
        case '@': case '#': case '$': case '%': case '^': case '&':
        case '~': case '`':
        case '-':
            return 1;
        default:
            return 0;
    }
}

// 2) твоя функция: перед эмбеддингами — trim spaces, lowercase, punctuation split
static int sd_canonize_for_embedding(const char *in, char *out, size_t out_sz) {
    if (!out || out_sz == 0) return 0;
    out[0] = '\0';
    if (!in) return 1;

    size_t w = 0;
    int prev_space = 1;

    for (size_t i = 0; in[i] != '\0'; ++i) {
        unsigned char c = (unsigned char)in[i];

        if (c == '\r' || c == '\n' || c == '\t' || c == ' ' || c == '\v' || c == '\f') {
            if (!prev_space) {
                if (w + 1 >= out_sz) break;
                out[w++] = ' ';
                prev_space = 1;
            }
            continue;
        }

        if (sd_is_punct_char(c)) {
            if (!prev_space) {
                if (w + 1 >= out_sz) break;
                out[w++] = ' ';
            }
            if (w + 1 >= out_sz) break;
            out[w++] = (char)tolower((int)c);
            if (w + 1 >= out_sz) break;
            out[w++] = ' ';
            prev_space = 1;
            continue;
        }

        if (w + 1 >= out_sz) break;
        out[w++] = (char)tolower((int)c);
        prev_space = 0;
    }

    while (w > 0 && out[w - 1] == ' ') w--;
    out[w] = '\0';
    return 1;
}

static char *sd_canonize_alloc(const char *in)
{
    if (!in) in = "";
    size_t n = strlen(in);
    // worst-case expansion: punctuation gets spaces around it => ~3x + slack
    size_t cap = n * 3u + 64u;
    if (cap < 256u) cap = 256u;
    // hard cap to avoid pathological allocations
    if (cap > 1024u * 1024u) cap = 1024u * 1024u;

    char *out = (char*)malloc(cap);
    if (!out) return NULL;
    if (!sd_canonize_for_embedding(in, out, cap)) {
        free(out);
        return NULL;
    }
    return out;
}

typedef struct SdTokList {
    char **t;
    int n;
    int cap;
} SdTokList;

static void sd_toklist_init(SdTokList *L) {
    if (!L) return;
    L->t = NULL;
    L->n = 0;
    L->cap = 0;
}

static void sd_toklist_free(SdTokList *L) {
    if (!L) return;
    for (int i = 0; i < L->n; ++i) {
        free(L->t[i]);
    }
    free(L->t);
    L->t = NULL;
    L->n = 0;
    L->cap = 0;
}

static int sd_toklist_reserve(SdTokList *L, int need) {
    if (!L) return 0;
    if (need <= L->cap) return 1;
    int nc = (L->cap == 0) ? 16 : L->cap;
    while (nc < need) nc *= 2;
    char **nt = (char**)realloc(L->t, (size_t)nc * sizeof(char*));
    if (!nt) return 0;
    L->t = nt;
    L->cap = nc;
    return 1;
}

static int sd_toklist_push_copy_len(SdTokList *L, const char *s, int len) {
    if (!L || !s || len < 0) return 0;
    if (!sd_toklist_reserve(L, L->n + 1)) return 0;
    char *p = (char*)malloc((size_t)len + 1);
    if (!p) return 0;
    memcpy(p, s, (size_t)len);
    p[len] = '\0';
    L->t[L->n++] = p;
    return 1;
}

static int sd_toklist_push_copy(SdTokList *L, const char *s) {
    return sd_toklist_push_copy_len(L, s, (int)strlen(s));
}

static int sd_tok_is_nl(const char *tok) {
    return (tok && tok[0] == '\\' && tok[1] == 'n' && tok[2] == '\0');
}

static int sd_tok_eq_ci(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        unsigned char ca = (unsigned char)*a;
        unsigned char cb = (unsigned char)*b;
        if ((unsigned char)tolower((int)ca) != (unsigned char)tolower((int)cb)) return 0;
        ++a; ++b;
    }
    return (*a == '\0' && *b == '\0');
}

static int sd_is_sentence_delim(const char *tok) {
    if (!tok) return 0;
    if (sd_tok_is_nl(tok)) return 1;
    if (tok[0] && tok[1] == '\0') {
        if (tok[0] == '.' || tok[0] == '?' || tok[0] == '!') return 1;
    }
    return 0;
}

static int sd_is_right_punct_tok(const char *tok) {
    if (!tok || tok[1] != '\0') return 0;
    switch (tok[0]) {
		case '.': case ',': case ';': case ':': case '!': case '?':
		case '\"': case '#':
		case ')': case ']': case '}':
			return 1;
        default: return 0;
    }
}

static int sd_is_left_punct_tok(const char *tok) {
    if (!tok || tok[1] != '\0') return 0;
    switch (tok[0]) {
		case '(': case '[': case '{':
        case '\"':
        case '#':
            return 1;
        default: return 0;
    }
}

static int sd_is_ascii_ident_tok(const char *tok) {
    if (!tok || !tok[0]) return 0;
    for (const unsigned char *p = (const unsigned char*)tok; *p; ++p) {
        const unsigned char c = *p;
        const int ok = ((c >= 'a' && c <= 'z') ||
                        (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') ||
                        (c == '_'));
        if (!ok) return 0;
    }
    return 1;
}

// Tokenize payload: split punctuation, keep "\\n" tokens, keep original case for reconstruction.
// Match is case-insensitive, so learned sequences can be lower-case.
static int sd_tokenize_payload(const char *text, SdTokList *out) {
    sd_toklist_init(out);
    if (!text || !text[0] || strcmp(text, "NONE") == 0) return 1;

    const char *p = text;
    while (*p) {
        unsigned char c = (unsigned char)*p;

        if (c == '\r') { p++; continue; }

        if (c == '\n') {
            if (!sd_toklist_push_copy(out, "\\n")) return 0;
            p++;
            continue;
        }

        if (c == ' ' || c == '\t' || c == '\v' || c == '\f') {
            p++;
            continue;
        }

        if (sd_is_punct_char(c)) {
            char tmp[2] = { (char)c, '\0' };
            if (!sd_toklist_push_copy(out, tmp)) return 0;
            p++;
            continue;
        }

        const char *s = p;
        while (*p) {
            unsigned char cc = (unsigned char)*p;
            if (cc == '\r' || cc == '\n' || cc == ' ' || cc == '\t' || cc == '\v' || cc == '\f') break;
            if (sd_is_punct_char(cc)) break;
            p++;
        }
        int len = (int)(p - s);
        if (len > 0) {
            if (!sd_toklist_push_copy_len(out, s, len)) return 0;
        }
    }
    return 1;
}

// Parse stored token-seq (space separated, "\\n" as 2 chars)
static int sd_parse_token_seq(const char *seq, SdTokList *out) {
    sd_toklist_init(out);
    if (!seq || !seq[0]) return 1;

    const char *p = seq;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;

        const char *s = p;
        while (*p && *p != ' ') p++;
        int len = (int)(p - s);
        if (len <= 0) break;

        if (len == 2 && s[0] == '\\' && s[1] == 'n') {
            if (!sd_toklist_push_copy(out, "\\n")) return 0;
        } else {
            if (!sd_toklist_push_copy_len(out, s, len)) return 0;
        }
    }
    return 1;
}

static int sd_find_subseq_ci(const SdTokList *hay, const SdTokList *needle, int *out_first, int *out_count) {
    if (out_first) *out_first = -1;
    if (out_count) *out_count = 0;
    if (!hay || !needle || needle->n <= 0) return 0;
    if (hay->n < needle->n) return 0;

    int first = -1;
    int cnt = 0;

    for (int i = 0; i <= hay->n - needle->n; ++i) {
        int ok = 1;
        for (int j = 0; j < needle->n; ++j) {
            if (!sd_tok_eq_ci(hay->t[i + j], needle->t[j])) { ok = 0; break; }
        }
        if (ok) {
            if (first < 0) first = i;
            cnt++;
        }
    }

    if (out_first) *out_first = first;
    if (out_count) *out_count = cnt;
    return (cnt > 0);
}

typedef struct SdOccSpan { int s; int e; } SdOccSpan;
typedef struct SdOccList { SdOccSpan v[256]; int n; } SdOccList;
typedef struct SdSectionLocks { SdOccList occ[8]; } SdSectionLocks;

static void sd_occ_init(SdOccList *L);
static int  sd_occ_overlap(const SdOccList *L, int s, int e);
static void sd_occ_add(SdOccList *L, int s, int e);

static void sd_locks_init(SdSectionLocks *L)
{
    if (!L) return;
    for (int i = 0; i < 8; ++i) sd_occ_init(&L->occ[i]);
}

static int sd_section_index(const char *sec)
{
    if (!sec || !sec[0]) return -1;
    if (strcmp(sec, "GOAL") == 0) return 0;
    if (strcmp(sec, "REQUIREMENTS") == 0) return 1;
    if (strcmp(sec, "INPUT") == 0) return 2;
    if (strcmp(sec, "OUTPUT") == 0) return 3;
    if (strcmp(sec, "CONSTRAINTS") == 0) return 4;
    if (strcmp(sec, "ENVIRONMENT") == 0) return 5;
    if (strcmp(sec, "EDIT_PARAMETERS") == 0) return 6;
    if (strcmp(sec, "NON_GOALS") == 0) return 7;
    return -1;
}

static void sd_occ_init(SdOccList *L)
{
    if (!L) return;
    L->n = 0;
}

static int sd_occ_overlap(const SdOccList *L, int s, int e)
{
    if (!L) return 0;
    if (s < 0) s = 0;
    if (e < s) e = s;

    for (int i = 0; i < L->n; ++i) {
        const int os = L->v[i].s;
        const int oe = L->v[i].e;
        if (oe <= os) continue;

        /* overlap for half-open ranges [s,e) */
        if (s < oe && e > os) return 1;
    }
    return 0;
}

static void sd_occ_add(SdOccList *L, int s, int e)
{
    if (!L) return;
    if (s < 0) s = 0;
    if (e < s) e = s;

    const int cap = (int)(sizeof(L->v) / sizeof(L->v[0]));
    if (L->n >= cap) return;

    L->v[L->n].s = s;
    L->v[L->n].e = e;
    L->n++;
}

static void sd_occ_shift_from(SdOccList *L, int from, int delta)
{
    if (!L || delta == 0) return;

    for (int i = 0; i < L->n; ++i) {
        if (L->v[i].s >= from) L->v[i].s += delta;
        if (L->v[i].e >= from) L->v[i].e += delta;

        if (L->v[i].s < 0) L->v[i].s = 0;
        if (L->v[i].e < 0) L->v[i].e = 0;
        if (L->v[i].e < L->v[i].s) L->v[i].e = L->v[i].s;
    }
}

static void sd_occ_delete_adjust(SdOccList *L, int del_s, int del_e)
{
    if (!L) return;
    if (del_e < del_s) { int t = del_s; del_s = del_e; del_e = t; }

    const int len = del_e - del_s;
    if (len <= 0) return;

    /* safety: drop any span that intersects deletion (shouldn't happen if we block overlaps) */
    int w = 0;
    for (int i = 0; i < L->n; ++i) {
        const int s = L->v[i].s;
        const int e = L->v[i].e;
        if (e <= s) continue;

        if (s < del_e && e > del_s) {
            continue; /* drop */
        }
        L->v[w++] = L->v[i];
    }
    L->n = w;

    /* shift spans after deleted range */
    sd_occ_shift_from(L, del_e, -len);
}

static unsigned long long sd_hash_tokens_range_ci(const SdTokList *L, int start, int len) {
    unsigned long long h = 1469598103934665603ULL;
    const unsigned long long prime = 1099511628211ULL;

    int end = start + len;
    if (!L || start < 0 || len < 0 || end > L->n) return 0ULL;

    for (int i = start; i < end; ++i) {
        const char *t = L->t[i];
        if (!t) continue;
        for (const unsigned char *q = (const unsigned char*)t; *q; ++q) {
            unsigned char cc = (unsigned char)tolower((int)*q);
            h ^= (unsigned long long)cc;
            h *= prime;
        }
        // delimiter
        h ^= (unsigned long long)' ';
        h *= prime;
    }
    return h;
}

static char *sd_tokens_range_to_seq_lower(const SdTokList *L, int start, int len) {
    if (!L || start < 0 || len <= 0 || start + len > L->n) return NULL;

    size_t cap = 0;
    for (int i = start; i < start + len; ++i) {
        cap += strlen(L->t[i]) + 1;
    }
    if (cap < 2) cap = 2;

    char *out = (char*)malloc(cap);
    if (!out) return NULL;

    size_t w = 0;
    for (int i = start; i < start + len; ++i) {
        const char *t = L->t[i];
        if (!t) continue;
        if (w && w + 1 < cap) out[w++] = ' ';
        for (const unsigned char *q = (const unsigned char*)t; *q && w + 1 < cap; ++q) {
            out[w++] = (char)tolower((int)*q);
        }
    }
    out[w] = '\0';
    return out;
}

static int sd_tok_is_wordlike(const char *t);

static int sd_tok_is_ascii_ext_token(const char *tok); /* defined later */

static int sd_tok_is_ascii_identlike_token(const char *tok)
{
    if (!tok || !tok[0]) return 0;
    if (strcmp(tok, "\\n") == 0) return 0;
    for (const unsigned char *p = (const unsigned char*)tok; *p; ++p) {
        if (*p >= 0x80) return 0;
        if (!(isalnum((int)*p) || *p == '_')) return 0;
    }
    return 1;
}

typedef struct SdSpan { int start; int len; } SdSpan;

// Build sentence spans from token list (delims: . ? ! \\n). Delim token is included in span.
static int sd_collect_sentence_spans(const SdTokList *L, SdSpan **out_spans, int *out_n) {
    if (out_spans) *out_spans = NULL;
    if (out_n) *out_n = 0;
    if (!L || L->n <= 0) return 1;

    SdSpan *sp = NULL;
    int sn = 0, sc = 0;

    int s0 = 0;
    for (int i = 0; i < L->n; ++i) {
        int is_delim = sd_is_sentence_delim(L->t[i]);

	/* '.' внутри идентификаторов (window.width, 127.0.0.1, {{scope.key}}) НЕ должен резать предложение */
	if (is_delim && L->t[i] && L->t[i][0] == '.' && L->t[i][1] == '\0') {
		const char *prev = (i > 0) ? L->t[i - 1] : NULL;
		const char *next = ((i + 1) < L->n) ? L->t[i + 1] : NULL;

		/* Ограничиваем правило ТОЛЬКО ASCII-идентификаторами. */
		if (prev && next &&
			sd_tok_is_ascii_identlike_token(prev) &&
			sd_tok_is_ascii_identlike_token(next))
		{
			/* Не склеивать "file.ext. Next": это ВТОРАЯ точка после расширения.
			   Но домены / a.b.c оставляем как идентификатор (next тоже ext). */
			if (i >= 2 &&
				L->t[i - 2] && L->t[i - 2][0] == '.' && L->t[i - 2][1] == '\0' &&
				sd_tok_is_ascii_ext_token(prev) &&
				!sd_tok_is_ascii_ext_token(next))
			{
				/* keep delimiter */
			} else {
				is_delim = 0;
			}
		}
	}

        if (is_delim) {
            int e = i;
            int ln = (e - s0 + 1);
            if (ln > 0) {
                if (sn + 1 > sc) {
                    int nc = (sc == 0) ? 16 : sc * 2;
                    SdSpan *np = (SdSpan*)realloc(sp, (size_t)nc * sizeof(SdSpan));
                    if (!np) { free(sp); return 0; }
                    sp = np;
                    sc = nc;
                }
                sp[sn].start = s0;
                sp[sn].len = ln;
                sn++;
            }
            s0 = i + 1;
        }
    }
    if (s0 < L->n) {
        int ln = (L->n - s0);
        if (ln > 0) {
            if (sn + 1 > sc) {
                int nc = (sc == 0) ? 16 : sc * 2;
                SdSpan *np = (SdSpan*)realloc(sp, (size_t)nc * sizeof(SdSpan));
                if (!np) { free(sp); return 0; }
                sp = np;
                sc = nc;
            }
            sp[sn].start = s0;
            sp[sn].len = ln;
            sn++;
        }
    }

    if (out_spans) *out_spans = sp;
    else free(sp);

    if (out_n) *out_n = sn;
    return 1;
}

// splice-out (move ownership of token pointers)
static int sd_toklist_splice_out(SdTokList *src, int start, int len, SdTokList *out_span) {
    if (!src || start < 0 || len <= 0 || start + len > src->n) return 0;
    sd_toklist_init(out_span);
    out_span->t = (char**)malloc((size_t)len * sizeof(char*));
    if (!out_span->t) return 0;
    out_span->cap = len;
    out_span->n = len;

    for (int i = 0; i < len; ++i) {
        out_span->t[i] = src->t[start + i];
        src->t[start + i] = NULL;
    }

    memmove(&src->t[start], &src->t[start + len], (size_t)(src->n - (start + len)) * sizeof(char*));
    src->n -= len;
    return 1;
}

static int sd_toklist_insert_owned(SdTokList *dst, int idx, SdTokList *span) {
    if (!dst || !span || span->n <= 0) return 0;
    if (idx < 0) idx = 0;
    if (idx > dst->n) idx = dst->n;

    if (!sd_toklist_reserve(dst, dst->n + span->n)) return 0;

    memmove(&dst->t[idx + span->n], &dst->t[idx], (size_t)(dst->n - idx) * sizeof(char*));
    for (int i = 0; i < span->n; ++i) {
        dst->t[idx + i] = span->t[i];
        span->t[i] = NULL;
    }
    dst->n += span->n;

    free(span->t);
    span->t = NULL;
    span->n = 0;
    span->cap = 0;
    return 1;
}

static int sd_move_meta_make(char out[128], unsigned long long anchor_hash, int offset, unsigned long long payload_hash) {
    if (!out) return 0;
    _snprintf_s(out, 128, _TRUNCATE, "A=%016llx O=%d P=%016llx", anchor_hash, offset, payload_hash);
    return 1;
}

static int sd_move_meta_parse(const char *s, unsigned long long *out_a, int *out_o, unsigned long long *out_p) {
    if (out_a) *out_a = 0ULL;
    if (out_o) *out_o = 0;
    if (out_p) *out_p = 0ULL;
    if (!s || !s[0]) return 0;

    unsigned long long a = 0ULL, p = 0ULL;
    int o = 0;
    int n = sscanf(s, "A=%llx O=%d P=%llx", &a, &o, &p);
    if (n != 3) n = sscanf(s, "A=%llx|O=%d|P=%llx", &a, &o, &p);
    if (n != 3) return 0;

    if (out_a) *out_a = a;
    if (out_o) *out_o = o;
    if (out_p) *out_p = p;
    return 1;
}

static float sd_prob_from_similarity(float sim) {
    // min similarity - 0.75, 1.0..0.9 => 99%, 0.9..0.75 => 99%..0%
    if (sim >= 0.90f) return 0.99f;
    if (sim <= 0.75f) return 0.0f;
    float t = (sim - 0.75f) / (0.90f - 0.75f);
    if (t < 0.f) t = 0.f;
    if (t > 1.f) t = 1.f;
    return 0.99f * t;
}

static int sd_priority_for_op(int op_type) {
    // Concept-safe order: MOVE/INSERT/CHANGE first, DELETE last.
    if (op_type == SD_OP_MOVE_TOKENS   || op_type == SD_OP_MOVE_LINE)   return 0;
    if (op_type == SD_OP_INSERT_TOKENS || op_type == SD_OP_INSERT_LINE) return 1;
    if (op_type == SD_OP_CHANGE_WORD)                                  return 2;
    if (op_type == SD_OP_DELETE_TOKENS || op_type == SD_OP_DELETE_LINE) return 3;
    return 4;
}

// deterministic coin flip (AUTO can be nondeterministic, but this is stable for debugging)
static int sd_should_apply(const char *input_fp, const char *op_key, float p) {
    if (p <= 0.0f) return 0;
    if (p >= 0.999f) return 1;

    char buf[2048];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%s|%s", input_fp ? input_fp : "", op_key ? op_key : "");
    unsigned long long h = struct_ml_hash_str(buf);
    double u = (double)(h & 0xffffffffULL) / (double)0xffffffffULL;
    return (u < (double)p) ? 1 : 0;
}

// -------------------- Step3: penalty + apply-logs --------------------

static unsigned long long sd_hash_toklist_ci(const SdTokList *L)
{
    if (!L || L->n <= 0) return 0ULL;
    char *seq = sd_tokens_range_to_seq_lower(L, 0, L->n);
    if (!seq) return 0ULL;
    unsigned long long h = struct_ml_hash_str(seq);
    free(seq);
    return h;
}

static int structurize_penalty_is_disabled(sqlite3 *db, const char *section_fp, const char *section, const char *op_key)
{
    if (!db || !section_fp || !section_fp[0] || !section || !section[0] || !op_key || !op_key[0]) return 0;

    const char *sql =
        "SELECT disabled FROM struct_delta_op_penalties "
        "WHERE section_fp=? AND section=? AND op_key=? LIMIT 1;";

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return 0;

    sqlite3_bind_text(st, 1, section_fp, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, section,    -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, op_key,     -1, SQLITE_TRANSIENT);

    int disabled = 0;
    if (sqlite3_step(st) == SQLITE_ROW) {
        disabled = sqlite3_column_int(st, 0) ? 1 : 0;
    }
    sqlite3_finalize(st);
    return disabled;
}

static void structurize_penalty_set_disabled(sqlite3 *db, const char *section_fp, const char *section, const char *op_key)
{
    if (!db || !section_fp || !section_fp[0] || !section || !section[0] || !op_key || !op_key[0]) return;

    const char *sql =
        "INSERT OR REPLACE INTO struct_delta_op_penalties(section_fp,section,op_key,disabled,updated_at) "
        "VALUES(?,?,?,1,CURRENT_TIMESTAMP);";

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return;

    sqlite3_bind_text(st, 1, section_fp, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, section,    -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, op_key,     -1, SQLITE_TRANSIENT);

    (void)sqlite3_step(st);
    sqlite3_finalize(st);
}

static sqlite3_int64 structurize_apply_log_begin(sqlite3 *db,
                                                const char *input_fp,
                                                const char *embed_model,
                                                int embed_dim,
                                                const char *syntax_hint,
                                                int ring_hint)
{
    if (!db || !input_fp || !input_fp[0]) return 0;

    const char *sql =
        "INSERT INTO struct_delta_apply_logs(input_fingerprint,embed_model,embed_dim,syntax_hint,ring_hint,created_at) "
        "VALUES(?,?,?,?,?,CURRENT_TIMESTAMP);";

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return 0;

    sqlite3_bind_text(st, 1, input_fp,                 -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, embed_model ? embed_model : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int (st, 3, embed_dim);
    sqlite3_bind_text(st, 4, syntax_hint ? syntax_hint : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int (st, 5, ring_hint);

    if (sqlite3_step(st) != SQLITE_DONE) {
        sqlite3_finalize(st);
        return 0;
    }
    sqlite3_finalize(st);
    return sqlite3_last_insert_rowid(db);
}

static sqlite3_int64 structurize_apply_log_get_latest_and_next_ord(sqlite3 *db,
                                                                   const char *input_text,
                                                                   const char *syntax_hint,
                                                                   int ring_hint,
                                                                   int *out_next_ord)
{
    if (out_next_ord) *out_next_ord = 0;
    if (!db || !input_text || !input_text[0]) return 0;

    char *canon_in = sd_canonize_alloc(input_text);
    if (!canon_in) canon_in = _strdup(input_text);
    if (!canon_in) return 0;

    char fp[32];
    structurize_delta_hex64(struct_ml_hash_str(canon_in), fp);
    free(canon_in);

    sqlite3_int64 log_id = 0;

    const char *sql_log =
        "SELECT id FROM struct_delta_apply_logs "
        "WHERE input_fingerprint=? AND syntax_hint=? AND ring_hint=? "
        "ORDER BY id DESC LIMIT 1;";

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql_log, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, fp, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, syntax_hint ? syntax_hint : "", -1, SQLITE_TRANSIENT);
        sqlite3_bind_int (st, 3, ring_hint);
        if (sqlite3_step(st) == SQLITE_ROW) log_id = sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);

    if (log_id <= 0) return 0;

    int next_ord = 0;
    const char *sql_ord =
        "SELECT COALESCE(MAX(ordinal)+1,0) FROM struct_delta_apply_log_ops WHERE log_id=?;";

    if (sqlite3_prepare_v2(db, sql_ord, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, log_id);
        if (sqlite3_step(st) == SQLITE_ROW) next_ord = sqlite3_column_int(st, 0);
    }
    sqlite3_finalize(st);

    if (out_next_ord) *out_next_ord = next_ord;
    return log_id;
}

static void structurize_apply_log_add_op(sqlite3 *db,
                                        sqlite3_int64 log_id,
                                        int ordinal,
                                        const char *section,
                                        const char *section_fp,
                                        const char *op_key,
                                        int op_type,
                                        const char *match_before,
                                        const char *match_after,
                                        const char *dst_section,
                                        const char *meta)
{
    if (!db || log_id <= 0) return;

    const char *sql =
        "INSERT INTO struct_delta_apply_log_ops(log_id,ordinal,section,section_fp,op_key,op_type,match_before,match_after,dst_section,meta) "
        "VALUES(?,?,?,?,?,?,?,?,?,?);";

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return;

    sqlite3_bind_int64(st, 1, log_id);
    sqlite3_bind_int  (st, 2, ordinal);
    sqlite3_bind_text (st, 3, section ? section : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (st, 4, section_fp ? section_fp : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (st, 5, op_key ? op_key : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int  (st, 6, op_type);
    sqlite3_bind_text (st, 7, match_before ? match_before : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (st, 8, match_after  ? match_after  : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (st, 9, dst_section  ? dst_section  : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (st,10, meta ? meta : "", -1, SQLITE_TRANSIENT);

    (void)sqlite3_step(st);
    sqlite3_finalize(st);
}

static const char *sd_section_hdr_by_name(const char *sec)
{
    if (!sec || !sec[0]) return NULL;
    if (strcmp(sec, "GOAL") == 0) return "GOAL:";
    if (strcmp(sec, "REQUIREMENTS") == 0) return "REQUIREMENTS:";
    if (strcmp(sec, "INPUT") == 0) return "INPUT:";
    if (strcmp(sec, "OUTPUT") == 0) return "OUTPUT:";
    if (strcmp(sec, "CONSTRAINTS") == 0) return "CONSTRAINTS:";
    if (strcmp(sec, "ENVIRONMENT") == 0) return "ENVIRONMENT:";
    if (strcmp(sec, "EDIT_PARAMETERS") == 0) return "EDIT_PARAMETERS:";
    if (strcmp(sec, "NON_GOALS") == 0) return "NON_GOALS:";
    return NULL;
}

static int sd_tokseq_present_in_section(const char *full_struct, const char *section_name, const char *tokseq)
{
    if (!full_struct || !section_name || !tokseq || !tokseq[0]) return 0;

    const char *hdr = sd_section_hdr_by_name(section_name);
    if (!hdr) return 0;

    char payload[65536]; payload[0] = 0;
    tr_copy_section_payload(full_struct, hdr, payload, sizeof(payload));

    SdTokList H, N;
    if (!sd_tokenize_payload(payload, &H)) return 0;
    if (!sd_parse_token_seq(tokseq, &N)) {
        sd_toklist_free(&H);
        sd_toklist_free(&N);
        return 0;
    }

    int first = -1, cnt = 0;
    sd_find_subseq_ci(&H, &N, &first, &cnt);

    sd_toklist_free(&H);
    sd_toklist_free(&N);

    return (cnt >= 1 && first >= 0) ? 1 : 0;
}

static void structurize_penalty_update_from_last_log(sqlite3 *db,
                                                     const char *input_text,
                                                     const char *syntax_hint,
                                                     int ring_hint,
                                                     const char *after_struct)
{
    if (!db || !input_text || !input_text[0] || !after_struct || !after_struct[0]) return;

    char *canon_in = sd_canonize_alloc(input_text);
    if (!canon_in) canon_in = _strdup(input_text);
    if (!canon_in) return;

    char fp[32];
    structurize_delta_hex64(struct_ml_hash_str(canon_in), fp);

    // find latest log for this input_fp + hints
    const char *sql_log =
        "SELECT id FROM struct_delta_apply_logs "
        "WHERE input_fingerprint=? AND syntax_hint=? AND ring_hint=? "
        "ORDER BY id DESC LIMIT 1;";

    sqlite3_stmt *st = NULL;
    sqlite3_int64 log_id = 0;

    if (sqlite3_prepare_v2(db, sql_log, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, fp, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, syntax_hint ? syntax_hint : "", -1, SQLITE_TRANSIENT);
        sqlite3_bind_int (st, 3, ring_hint);
        if (sqlite3_step(st) == SQLITE_ROW) log_id = sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);
    free(canon_in);

    if (log_id <= 0) return;

    const char *sql_ops =
        "SELECT section,section_fp,op_key,op_type,match_before,match_after,dst_section "
        "FROM struct_delta_apply_log_ops WHERE log_id=? ORDER BY ordinal ASC;";

    if (sqlite3_prepare_v2(db, sql_ops, -1, &st, NULL) != SQLITE_OK) return;
    sqlite3_bind_int64(st, 1, log_id);

    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *section    = (const char*)sqlite3_column_text(st, 0);
        const char *section_fp = (const char*)sqlite3_column_text(st, 1);
        const char *op_key     = (const char*)sqlite3_column_text(st, 2);
        int op_type            = sqlite3_column_int(st, 3);
        const char *mb         = (const char*)sqlite3_column_text(st, 4);
        const char *ma         = (const char*)sqlite3_column_text(st, 5);
        const char *dst_sec    = (const char*)sqlite3_column_text(st, 6);

        int corrected = 0;

        // критерий "полностью исправили" (минимально достаточный)
        if (op_type == SD_OP_INSERT_TOKENS) {
            // вставка исправлена, если вставленного больше нет
            corrected = (sd_tokseq_present_in_section(after_struct, section, mb) ? 0 : 1);
        } else if (op_type == SD_OP_DELETE_TOKENS) {
            // удаление исправлено, если удалённое снова появилось
            corrected = (sd_tokseq_present_in_section(after_struct, section, mb) ? 1 : 0);
        } else if (op_type == SD_OP_MOVE_TOKENS) {
            // move исправлен, если в dst (мы логируем dst как section) оно не осталось
            corrected = (sd_tokseq_present_in_section(after_struct, section, mb) ? 0 : 1);
        } else if (op_type == SD_OP_CHANGE_WORD) {
            // change исправлен, если "после" не осталось (используем match_after как needle)
            const char *needle = (ma && ma[0]) ? ma : mb;
            corrected = (sd_tokseq_present_in_section(after_struct, section, needle) ? 0 : 1);
        } else {
            // неизвестные опы пока не штрафуем
            corrected = 0;
        }

        if (corrected) {
            structurize_penalty_set_disabled(db,
                                             section_fp ? section_fp : "",
                                             section ? section : "",
                                             op_key ? op_key : "");
        }

        (void)dst_sec;
    }

    sqlite3_finalize(st);
}

static int sd_find_anchor_insert_index(const SdTokList *dst, unsigned long long anchor_hash, int offset) {
    const unsigned long long H_BEGIN = struct_ml_hash_str("__BEGIN__");
    const unsigned long long H_END   = struct_ml_hash_str("__END__");

    if (!dst || dst->n <= 0) return 0;
    if (anchor_hash == H_BEGIN) return 0;
    if (anchor_hash == H_END) return dst->n;

    SdSpan *sp = NULL;
    int sn = 0;
    if (!sd_collect_sentence_spans(dst, &sp, &sn)) return dst->n;

    for (int i = 0; i < sn; ++i) {
        unsigned long long h = sd_hash_tokens_range_ci(dst, sp[i].start, sp[i].len);
        if (h == anchor_hash) {
            int idx0 = sp[i].start;
            int idx1 = sp[i].start + sp[i].len;
            free(sp);
            if (offset <= 0) return idx0;
            return idx1;
        }
    }
    free(sp);
    return dst->n; // anchor not found => append
}

static void sd_toklist_remove_span_free(SdTokList *src, int start, int len) {
    if (!src || start < 0 || len <= 0 || start + len > src->n) return;
    for (int i = 0; i < len; ++i) {
        free(src->t[start + i]);
        src->t[start + i] = NULL;
    }
    memmove(&src->t[start], &src->t[start + len], (size_t)(src->n - (start + len)) * sizeof(char*));
    src->n -= len;
}

static void sd_toklist_to_payload(char *out, size_t cap, const SdTokList *L) {
    if (!out || cap == 0) return;
    out[0] = '\0';

    if (!L || L->n <= 0) {
        strncpy(out, "NONE", cap - 1);
        out[cap - 1] = '\0';
        return;
    }

    size_t w = 0;
    const char *prev = NULL;
	const char *prev2 = NULL;
	
    for (int i = 0; i < L->n; ++i) {
        const char *tok = L->t[i];
        if (!tok) continue;

        if (sd_tok_is_nl(tok)) {
            if (w + 1 < cap) out[w++] = '\n';
            prev = tok;
            continue;
        }

        int need_space = 0;
        if (w > 0) {
            if (prev && sd_tok_is_nl(prev)) need_space = 0;
				else if (prev && sd_is_left_punct_tok(prev)) need_space = 0;
				else if (prev && prev[0] == '\"' && prev[1] == '\0') need_space = 0; /* после открывающей " */
                else if (prev && prev[0] == '#'  && prev[1] == '\0') need_space = 0; /* после # */
				else if (sd_is_right_punct_tok(tok)) need_space = 0;
				
				else if (tok && tok[0] == '\"' && tok[1] == '\0') {
                    /* закрывающая кавычка: если следующий токен НЕ wordlike — не ставим пробел перед " */
                    int next_is_word = 0;
                    if ((i + 1) < L->n && sd_tok_is_wordlike(L->t[i + 1])) next_is_word = 1;
                    if (!next_is_word) need_space = 0;
                    else need_space = 1; /* это открывающая кавычка — пробел перед ней пусть решает общий код */
                }
                else need_space = 1;

				/* NEW: пробел перед началом плейсхолдера "{{" (чтобы не было "размером{{window.width}}") */
				const char *next = (i + 1 < L->n) ? L->t[i + 1] : NULL;
				if (need_space == 0 &&
					tok[0] == '{' && tok[1] == '\0' &&
					next && next[0] == '{' && next[1] == '\0' &&
					prev && sd_tok_is_wordlike(prev))
				{
					need_space = 1;
				}
				
				/* НОВОЕ: склейка идентификаторов: window . width => window.width */
				if (need_space && prev && prev[0] == '.' && prev[1] == '\0' &&
					prev2 && sd_tok_is_wordlike(prev2) && sd_tok_is_wordlike(tok)) {
					need_space = 0;
				}
        
			if (need_space && prev && prev[1] == '\0') {
				// glue: window . width -> window.width
				if (prev[0] == '.' && prev2 && sd_is_ascii_ident_tok(prev2) && sd_is_ascii_ident_tok(tok)) {
					need_space = 0;
				}
				// glue: # 00FF00 -> #00FF00
				if (prev[0] == '#' && sd_is_ascii_ident_tok(tok)) {
					need_space = 0;
				}
			}
		}
        if (need_space) {
            if (w + 1 < cap) out[w++] = ' ';
        }

        size_t tl = strlen(tok);
        if (w + tl >= cap) tl = cap - 1 - w;
        if (tl > 0) {
            memcpy(out + w, tok, tl);
            w += tl;
        }
		prev2 = prev;
        prev = tok;
        if (w >= cap - 1) break;
    }

    out[w] = '\0';
}

typedef struct SdMove {
    const char *dst_section;     // points to sec_names[] entry
    char *tokseq_lower;          // owned lower-case token sequence (space-separated, "\\n" for line breaks)
    unsigned long long anchor_hash;
    int offset;
    unsigned long long payload_hash;
} SdMove;

typedef struct SdInsert {
    const char *dst_section;     // points to sec_names[] entry
    char *tokseq_lower;          // owned lower-case token sequence
    unsigned long long anchor_hash;
    int offset;
    unsigned long long payload_hash;
} SdInsert;

static int sd_derive_inserts(
    const SdTokList *B_goal,
    const SdTokList **A_secs,
    const char **sec_names,
    int sec_n,
    SdInsert **out_ins,
    int *out_in)
{
    if (out_ins) *out_ins = NULL;
    if (out_in) *out_in = 0;

    if (!B_goal || !A_secs || !sec_names || sec_n <= 0) return 0;

    SdInsert *ins = NULL;
    int in = 0, ic = 0;

    for (int s = 0; s < sec_n; ++s) {
        const SdTokList *A = A_secs[s];
        if (!A || A->n <= 0) continue;

        SdSpan *asp = NULL;
        int asn = 0;
        if (!sd_collect_sentence_spans(A, &asp, &asn) || asn <= 0) {
            if (asp) free(asp);
            continue;
        }

        for (int si = 0; si < asn; ++si) {
            const int start = asp[si].start;
            const int len   = asp[si].len;
            if (len <= 0) continue;

            char *tokseq = sd_tokens_range_to_seq_lower(A, start, len);
            if (!tokseq || !tokseq[0]) { if (tokseq) free(tokseq); continue; }

            SdTokList needle;
            if (!sd_parse_token_seq(tokseq, &needle) || needle.n <= 0) {
                sd_toklist_free(&needle);
                free(tokseq);
                continue;
            }

            // must be unique in this destination section (avoid ambiguous inserts)
            int first = -1, cnt = 0;
            sd_find_subseq_ci(A, &needle, &first, &cnt);
            if (cnt != 1) {
                sd_toklist_free(&needle);
                free(tokseq);
                continue;
            }

            // must NOT exist in baseline GOAL (this is true "addition")
            first = -1; cnt = 0;
            sd_find_subseq_ci(B_goal, &needle, &first, &cnt);
            if (cnt > 0) {
                sd_toklist_free(&needle);
                free(tokseq);
                continue;
            }

            // anchor: prev sentence if possible else next else END
            unsigned long long anchor = struct_ml_hash_str("__END__");
            int off = 1;

            if (si > 0) {
                anchor = sd_hash_tokens_range_ci(A, asp[si - 1].start, asp[si - 1].len);
                off = 1;
            } else if (si + 1 < asn) {
                anchor = sd_hash_tokens_range_ci(A, asp[si + 1].start, asp[si + 1].len);
                off = 0;
            } else {
                anchor = struct_ml_hash_str("__END__");
                off = 1;
            }

            if (in + 1 > ic) {
                int nc = (ic == 0) ? 16 : (ic * 2);
                SdInsert *ni = (SdInsert*)realloc(ins, sizeof(SdInsert) * (size_t)nc);
                if (!ni) {
                    sd_toklist_free(&needle);
                    free(tokseq);
                    continue;
                }
                ins = ni;
                ic = nc;
            }

            ins[in].dst_section = sec_names[s];
            ins[in].tokseq_lower = tokseq;
            ins[in].anchor_hash = anchor;
            ins[in].offset = off;
            ins[in].payload_hash = sd_hash_tokens_range_ci(A, start, len);
            in++;

            sd_toklist_free(&needle);
        }

        free(asp);
    }

    if (out_ins) *out_ins = ins;
    else {
        for (int i = 0; i < in; ++i) free(ins[i].tokseq_lower);
        free(ins);
    }

    if (out_in) *out_in = in;
    return 1;
}

static int sd_derive_moves_deletes(
    const SdTokList *B_goal,
    const SdTokList *A_goal,
    const SdTokList *secs,
    const char **sec_names,
    int sec_n,
    SdMove **out_moves,
    int *out_moves_n,
    char ***out_dels,
    int *out_dels_n)
{
    if (out_moves) *out_moves = NULL;
    if (out_moves_n) *out_moves_n = 0;
    if (out_dels) *out_dels = NULL;
    if (out_dels_n) *out_dels_n = 0;

    if (!B_goal || !A_goal || !secs || !sec_names || sec_n <= 0) return 0;

    SdSpan *sp = NULL; int sp_n = 0;
    if (!sd_collect_sentence_spans(B_goal, &sp, &sp_n) || sp_n <= 0) {
        if (sp) free(sp);
        return 1; // nothing to learn
    }

    SdMove *moves = NULL; int mn = 0; int mc = 0;
    char **dels = NULL; int dn = 0; int dc = 0;

    for (int si = 0; si < sp_n; ++si) {
        const int start = sp[si].start;
        const int len   = sp[si].len;
        if (len <= 0) continue;

        char *tokseq = sd_tokens_range_to_seq_lower(B_goal, start, len);
        if (!tokseq || !tokseq[0]) { if (tokseq) free(tokseq); continue; }

        SdTokList needle;
        if (!sd_parse_token_seq(tokseq, &needle) || needle.n <= 0) {
            sd_toklist_free(&needle);
            free(tokseq);
            continue;
        }

        int first = -1, cnt = 0;
        (void)sd_find_subseq_ci(B_goal, &needle, &first, &cnt);
        if (cnt != 1) {
            sd_toklist_free(&needle);
            free(tokseq);
            continue;
        }

        // if still present in GOAL after fix => not a move/delete
        first = -1; cnt = 0;
        (void)sd_find_subseq_ci(A_goal, &needle, &first, &cnt);
        if (cnt > 0) {
            sd_toklist_free(&needle);
            free(tokseq);
            continue;
        }

        // search destination sections
        int best_sec = -1;
        unsigned long long best_anchor = struct_ml_hash_str("__END__");
        int best_off = 1;

        for (int sidx = 0; sidx < sec_n; ++sidx) {
            int sfirst = -1, scnt = 0;
            (void)sd_find_subseq_ci(&secs[sidx], &needle, &sfirst, &scnt);
            if (scnt != 1) continue;

            // choose anchor as neighbor sentence (prev if possible, else next, else END)
            SdSpan *dsp = NULL; int dsp_n = 0;
            if (sd_collect_sentence_spans(&secs[sidx], &dsp, &dsp_n) && dsp_n > 0) {
                int k = -1;
                for (int j = 0; j < dsp_n; ++j) {
                    if (sfirst >= dsp[j].start && sfirst < (dsp[j].start + dsp[j].len)) {
                        k = j;
                        break;
                    }
                }

                if (k > 0) {
                    best_anchor = sd_hash_tokens_range_ci(&secs[sidx], dsp[k-1].start, dsp[k-1].len);
                    best_off = 1;
                } else if (k >= 0 && (k + 1) < dsp_n) {
                    best_anchor = sd_hash_tokens_range_ci(&secs[sidx], dsp[k+1].start, dsp[k+1].len);
                    best_off = 0;
                } else {
                    best_anchor = struct_ml_hash_str("__END__");
                    best_off = 1;
                }
            }
            if (dsp) free(dsp);

            best_sec = sidx;
            break; // deterministic: first section with unique match wins
        }

        if (best_sec >= 0) {
            if (mn + 1 > mc) {
                int nc = (mc == 0) ? 16 : (mc * 2);
                SdMove *nmv = (SdMove*)realloc(moves, sizeof(SdMove) * (size_t)nc);
                if (!nmv) {
                    sd_toklist_free(&needle);
                    free(tokseq);
                    continue;
                }
                moves = nmv;
                mc = nc;
            }

            moves[mn].dst_section = sec_names[best_sec];
            moves[mn].tokseq_lower = tokseq; // keep ownership
            moves[mn].anchor_hash = best_anchor;
            moves[mn].offset = best_off;
            moves[mn].payload_hash = sd_hash_tokens_range_ci(B_goal, start, len);
            mn++;

        } else {
            if (dn + 1 > dc) {
                int nc = (dc == 0) ? 16 : (dc * 2);
                char **nd = (char**)realloc(dels, sizeof(char*) * (size_t)nc);
                if (!nd) {
                    sd_toklist_free(&needle);
                    free(tokseq);
                    continue;
                }
                dels = nd;
                dc = nc;
            }

            dels[dn++] = tokseq; // keep ownership
        }

        sd_toklist_free(&needle);
    }

    free(sp);

    if (out_moves) *out_moves = moves;
    if (out_moves_n) *out_moves_n = mn;
    if (out_dels) *out_dels = dels;
    if (out_dels_n) *out_dels_n = dn;
    return 1;
}

/* ===== Project S v2.1: CHANGE_WORD learning (deterministic, Levenshtein + local context) ===== */

#define SD_CHG_FLAG_ENDING   1
#define SD_CHG_FLAG_REPLACE  2

typedef struct SdChangeWord {
    char *before_tok; /* owned */
    char *after_tok;  /* owned */
    int flags;
} SdChangeWord;

static void sd_changeword_free(SdChangeWord *c) {
    if (!c) return;
    free(c->before_tok);
    free(c->after_tok);
    c->before_tok = NULL;
    c->after_tok = NULL;
    c->flags = 0;
}

/* bounded Levenshtein, case-insensitive; returns maxd+1 if distance > maxd */
static int sd_levenshtein_bounded_ci(const char *a, const char *b, int maxd) {
    if (!a || !b) return maxd + 1;

    const size_t la = strlen(a);
    const size_t lb = strlen(b);

    if ((int)la == 0) return ((int)lb <= maxd) ? (int)lb : (maxd + 1);
    if ((int)lb == 0) return ((int)la <= maxd) ? (int)la : (maxd + 1);

    if (abs((int)la - (int)lb) > maxd) return maxd + 1;

    int *v0 = (int*)malloc(sizeof(int) * (lb + 1));
    int *v1 = (int*)malloc(sizeof(int) * (lb + 1));
    if (!v0 || !v1) {
        free(v0);
        free(v1);
        return maxd + 1;
    }

    for (size_t j = 0; j <= lb; ++j) v0[j] = (int)j;

    for (size_t i = 0; i < la; ++i) {
        v1[0] = (int)(i + 1);
        int row_min = v1[0];

        const unsigned char ca = (unsigned char)a[i];
        const int a_ci = tolower((int)ca);

        for (size_t j = 0; j < lb; ++j) {
            const unsigned char cb = (unsigned char)b[j];
            const int b_ci = tolower((int)cb);

            const int cost = (a_ci == b_ci) ? 0 : 1;

            const int del = v0[j + 1] + 1;
            const int ins = v1[j] + 1;
            const int sub = v0[j] + cost;

            int best = del;
            if (ins < best) best = ins;
            if (sub < best) best = sub;

            v1[j + 1] = best;
            if (best < row_min) row_min = best;
        }

        if (row_min > maxd) {
            free(v0);
            free(v1);
            return maxd + 1;
        }

        int *tmp = v0; v0 = v1; v1 = tmp;
    }

    const int dist = v0[lb];
    free(v0);
    free(v1);
    return (dist <= maxd) ? dist : (maxd + 1);
}

static int sd_tok_is_wordlike(const char *t) {
    if (!t || !t[0]) return 0;
    if (strcmp(t, "\\n") == 0) return 0;

    const unsigned char c0 = (unsigned char)t[0];

    /* UTF-8: treat any non-ASCII leading byte as wordlike to support RU text */
    if (c0 >= 0x80) return 1;

    return (isalnum((int)c0) || c0 == '_') ? 1 : 0;
}

typedef struct SdValueItem {
    int start_tok;
    int end_tok;
    long long value;     // только для APPLY (для LEARN = 0)
    char key[128];       // только для LEARN (для APPLY пусто)
} SdValueItem;

typedef struct SdValueGroup {
    int first_item;
    int count;
} SdValueGroup;

static int sd_tok_is_int_token(const char *tok, long long *out_val)
{
    if (out_val) *out_val = 0;
    if (!tok || !tok[0]) return 0;
    if (strcmp(tok, "\\n") == 0) return 0;

    const unsigned char *p = (const unsigned char *)tok;
    if (!isdigit((int)*p)) return 0;

    long long v = 0;
    while (*p) {
        if (!isdigit((int)*p)) return 0;
        v = v * 10 + (long long)(*p - '0');
        p++;
    }
    if (out_val) *out_val = v;
    return 1;
}

static int sd_tok_parse_int_prefix_unit_token(const char *tok, long long *out_val)
{
    if (out_val) *out_val = 0;
    if (!tok || !tok[0]) return 0;
    if (strcmp(tok, "\\n") == 0) return 0;

    const unsigned char *p = (const unsigned char *)tok;
    if (!isdigit((int)*p)) return 0;

    long long v = 0;
    while (*p && isdigit((int)*p)) {
        v = v * 10 + (long long)(*p - '0');
        ++p;
    }

    /* pure int */
    if (!*p) {
        if (out_val) *out_val = v;
        return 1;
    }

    /* accept unit suffix only if all remaining chars are ASCII letters */
    for (const unsigned char *q = p; *q; ++q) {
        if (!((*q >= 'A' && *q <= 'Z') || (*q >= 'a' && *q <= 'z'))) return 0;
    }

    if (out_val) *out_val = v;
    return 1;
}

static int sd_tok_parse_int_x_int_token(const char *tok, long long *out_a, long long *out_b)
{
    if (out_a) *out_a = 0;
    if (out_b) *out_b = 0;
    if (!tok || !tok[0]) return 0;
    if (strcmp(tok, "\\n") == 0) return 0;

    const unsigned char *p = (const unsigned char *)tok;

    if (!isdigit((int)*p)) return 0;

    long long a = 0;
    while (*p && isdigit((int)*p)) {
        a = a * 10 + (long long)(*p - '0');
        ++p;
    }

    if (!*p) return 0;

    if (*p != 'x' && *p != 'X') return 0;
    ++p;

    if (!*p || !isdigit((int)*p)) return 0;

    long long b = 0;
    while (*p && isdigit((int)*p)) {
        b = b * 10 + (long long)(*p - '0');
        ++p;
    }

    if (*p) return 0;

    if (out_a) *out_a = a;
    if (out_b) *out_b = b;
    return 1;
}

static int sd_tok_is_hex6_prefix(const char *tok)
{
    if (!tok) return 0;
    for (int i = 0; i < 6; ++i) {
        unsigned char c = (unsigned char)tok[i];
        if (!c || !isxdigit((int)c)) return 0;
    }
    return 1;
}

static int sd_tok_is_value_literal_token(const char *tok)
{
    long long v=0,a=0,b=0;
    if (!tok || !tok[0]) return 0;
    if (sd_tok_is_int_token(tok, &v)) return 1;
    if (sd_tok_parse_int_x_int_token(tok, &a, &b)) return 1;
    if (tok[0] == '#' && sd_tok_is_hex6_prefix(tok + 1)) return 1; /* "#RRGGBB..." */
    return 0;
}

static int sd_toklist_contains_value_literal(const SdTokList *L)
{
    if (!L || L->n <= 0) return 0;
    for (int i = 0; i < L->n; ++i) {
        const char *tok = L->t[i];
        if (sd_tok_is_value_literal_token(tok)) return 1;

        /* "#", "RRGGBB..." as 2 tokens */
        if (tok && tok[0] == '#' && tok[1] == '\0') {
            if ((i + 1) < L->n && sd_tok_is_hex6_prefix(L->t[i + 1])) return 1;
        }
    }
    return 0;
}

static int sd_span_extract_first_hex_color(const SdTokList *T, int start, int end, char hex_out[16])
{
    if (hex_out) hex_out[0] = '\0';
    if (!T || start < 0 || end > T->n || start >= end) return 0;

    for (int i = start; i < end; ++i) {
        const char *tok = T->t[i];
        if (!tok) continue;

        /* "#RRGGBB..." in one token */
        if (tok[0] == '#' && sd_tok_is_hex6_prefix(tok + 1)) {
            if (hex_out) _snprintf_s(hex_out, 16, _TRUNCATE, "#%.6s", tok + 1);
            return 1;
        }

        /* "#", "RRGGBB..." */
        if (tok[0] == '#' && tok[1] == '\0') {
            if ((i + 1) < end && sd_tok_is_hex6_prefix(T->t[i + 1])) {
                if (hex_out) _snprintf_s(hex_out, 16, _TRUNCATE, "#%.6s", T->t[i + 1]);
                return 1;
            }
        }
    }
    return 0;
}

static int sd_span_extract_first_hex_color_or_bare(const SdTokList *T, int start, int end, int allow_bare, char hex_out[16])
{
    if (hex_out) hex_out[0] = '\0';
    if (!T || start < 0 || end > T->n || start >= end) return 0;

    if (sd_span_extract_first_hex_color(T, start, end, hex_out)) return 1;
    if (!allow_bare) return 0;

    for (int i = start; i < end; ++i) {
        const char *tok = T->t[i];
        if (!tok) continue;

        /* accept only exact 6-char token, must contain A-F to avoid confusing with pure numbers */
        if ((int)strlen(tok) != 6) continue;

        int ok = 1;
        int has_alpha = 0;
        for (int k = 0; k < 6; ++k) {
            const unsigned char c = (unsigned char)tok[k];
            if (!isxdigit((int)c)) { ok = 0; break; }
            if ((c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) has_alpha = 1;
        }
        if (ok && has_alpha) {
            if (hex_out) _snprintf_s(hex_out, 16, _TRUNCATE, "#%.6s", tok);
            return 1;
        }
    }

    return 0;
}

static int sd_tok_is_upper_token(const char *tok)
{
    if (!tok) return 0;

    int has_alpha = 0;
    int n = 0;

    for (const unsigned char *p = (const unsigned char*)tok; *p; ++p) {
        unsigned char c = *p;
        n++;

        if (!(isalnum((int)c) || c == '_')) return 0;

        if (c >= 'a' && c <= 'z') return 0;
        if (c >= 'A' && c <= 'Z') has_alpha = 1;
    }

    /* avoid matching drive-letter "C" */
    if (n < 3) return 0;

    return has_alpha ? 1 : 0;
}

static int sd_span_extract_first_upper_token(const SdTokList *T, int start, int end, char *out, size_t cap)
{
    if (out && cap) out[0] = '\0';
    if (!T || start < 0 || end > T->n || start >= end || !out || cap == 0) return 0;

    for (int i = start; i < end; ++i) {
        const char *tok = T->t[i];
        if (!tok || !tok[0]) continue;
        if (strcmp(tok, "\\n") == 0) continue;
        if (tok[1] == '\0' && sd_is_punct_char((unsigned char)tok[0])) continue;

        if (sd_tok_is_upper_token(tok)) {
            _snprintf_s(out, cap, _TRUNCATE, "%s", tok);
            return 1;
        }
    }
    return 0;
}

static void sd_rstrip_path_punct(char *s)
{
    if (!s) return;
    size_t n = strlen(s);
    while (n > 0) {
        char c = s[n - 1];
        if (c == '.' || c == ',' || c == ';' || c == ':' || c == '!' || c == '?' ||
            c == ')' || c == '"' || c == '\'') {
            s[n - 1] = '\0';
            n--;
            continue;
        }
        break;
    }
    clar_trim(s);
}

static int sd_tok_is_ascii_ext_token(const char *tok)
{
    if (!tok || !tok[0]) return 0;
    if (strcmp(tok, "\\n") == 0) return 0;

    int n = 0;
    for (const unsigned char *p = (const unsigned char*)tok; *p; ++p) {
        if (*p >= 0x80) return 0;
        if (!(isalnum((int)*p) || *p == '_')) return 0;
        n++;
        if (n > 8) return 0;
    }
    return (n > 0) ? 1 : 0;
}

static int sd_span_extract_first_win_path(const SdTokList *T, int start, int end, char *out, size_t cap)
{
    if (out && cap) out[0] = '\0';
    if (!T || start < 0 || end > T->n || start >= end || !out || cap == 0) return 0;

    for (int i = start; i + 2 < end; ++i) {
        const char *t0 = T->t[i];
        const char *t1 = T->t[i + 1];
        const char *t2 = T->t[i + 2];

        if (!t0 || !t1 || !t2) continue;

        /* Drive-letter path: C : \ ...  OR  C : / ... */
        if (t0[0] && !t0[1] && isalpha((int)(unsigned char)t0[0]) &&
            t1[0] == ':' && !t1[1] &&
            ((t2[0] == '\\' && !t2[1]) || (t2[0] == '/' && !t2[1])))
        {
            size_t w = 0;

            for (int k = i; k < end; ++k) {
                const char *tk = T->t[k];
                if (!tk || !tk[0]) break;
                if (strcmp(tk, "\\n") == 0) break;

                /* terminate on obvious sentence separators */
                if (tk[1] == '\0' && (tk[0] == ',' || tk[0] == ';' || tk[0] == '!' || tk[0] == '?' || tk[0] == ')')) {
                    break;
                }
                if (tk[1] == '\0' && (tk[0] == '"' || tk[0] == '\'')) {
                    break;
                }

				/* '.' is part of extension only if followed by an ASCII extension token */
				if (tk[1] == '\0' && tk[0] == '.') {
					if ((k + 1) < end && sd_tok_is_ascii_ext_token(T->t[k + 1])) {
						if (w + 1 < cap) out[w++] = '.';
						continue;
					}
					break;
				}

                for (size_t z = 0; tk[z] && (w + 1) < cap; ++z) {
                    out[w++] = tk[z];
                }
            }

            out[(w < cap) ? w : (cap - 1)] = '\0';
            sd_rstrip_path_punct(out);
            return out[0] ? 1 : 0;
        }
    }

    return 0;
}

static int sd_key_count_pipe(const char *key)
{
    if (!key || !key[0]) return 0;
    int c = 1;
    for (const char *p = key; *p; ++p) if (*p == '|') c++;
    return c;
}

static int sd_count_word_tokens_between(const SdTokList *L, int a, int b)
{
    if (!L) return 0;
    if (a < 0) a = 0;
    if (b > L->n) b = L->n;
    int wc = 0;
    for (int i = a; i < b; ++i) {
        if (sd_tok_is_wordlike(L->t[i])) wc++;
    }
    return wc;
}

static int sd_extract_placeholder_key(
    const SdTokList *L,
    int i,
    int sent_end,
    int *out_close_end,      // индекс последнего токена закрывающей пары '}}'
    char *out_key,
    size_t out_key_sz)
{
    if (out_close_end) *out_close_end = -1;
    if (out_key && out_key_sz) out_key[0] = 0;

    if (!L || i < 0 || i + 1 >= sent_end) return 0;
    if (strcmp(L->t[i], "{") != 0) return 0;
    if (strcmp(L->t[i + 1], "{") != 0) return 0;

    int j = i + 2;
    int close_j = -1;
    for (; j + 1 < sent_end; ++j) {
        if (strcmp(L->t[j], "}") == 0 && strcmp(L->t[j + 1], "}") == 0) {
            close_j = j;
            break;
        }
    }
    if (close_j < 0) return 0;

    // собрать key = конкат токенов между '{{' и '}}' без пробелов
    if (!out_key || out_key_sz == 0) return 0;
    size_t w = 0;
    for (int k = i + 2; k < close_j; ++k) {
        const char *t = L->t[k];
        if (!t || !t[0]) return 0;
        if (strcmp(t, "\\n") == 0) return 0;
        for (const char *p = t; *p; ++p) {
            const unsigned char c = (unsigned char)*p;
            // разрешаем только [A-Za-z0-9_ .]
            if (!(isalnum((int)c) || c == '_' || c == '.')) return 0;
            if (w + 1 >= out_key_sz) return 0;
            out_key[w++] = (char)c;
        }
    }
    if (w == 0) return 0;
    out_key[w] = 0;

    if (out_close_end) *out_close_end = close_j + 1; // последний токен из '}}'
    return 1;
}

static int sd_collect_value_items_learn(
    const SdTokList *L,
    int sent_start,
    int sent_len,
    SdValueItem **out_items,
    int *out_n)
{
    if (out_items) *out_items = NULL;
    if (out_n) *out_n = 0;
    if (!L || sent_len <= 0) return 0;

    int sent_end = sent_start + sent_len;
    int cap = 0, n = 0;
    SdValueItem *items = NULL;

    for (int i = sent_start; i < sent_end; ++i) {
        if (strcmp(L->t[i], "{") == 0 && (i + 1) < sent_end && strcmp(L->t[i + 1], "{") == 0) {
            int close_end = -1;
            char key[128];
            if (sd_extract_placeholder_key(L, i, sent_end, &close_end, key, sizeof(key))) {
                if (n + 1 > cap) {
                    int ncap = cap ? cap * 2 : 8;
                    SdValueItem *tmp = (SdValueItem *)realloc(items, (size_t)ncap * sizeof(SdValueItem));
                    if (!tmp) { free(items); return 0; }
                    items = tmp;
                    cap = ncap;
                }
                SdValueItem it;
                memset(&it, 0, sizeof(it));
                it.start_tok = i;
                it.end_tok   = close_end;
                it.value     = 0;
                _snprintf_s(it.key, sizeof(it.key), _TRUNCATE, "%s", key);
                items[n++] = it;
                i = close_end; // пропускаем до конца '}}'
            }
        }
    }

    if (!items || n == 0) { free(items); return 0; }
    if (out_items) *out_items = items;
    if (out_n) *out_n = n;
    return 1;
}

static int sd_collect_value_items_apply(
    const SdTokList *L,
    int sent_start,
    int sent_len,
    SdValueItem **out_items,
    int *out_n)
{
    if (out_items) *out_items = NULL;
    if (out_n) *out_n = 0;
    if (!L || sent_len <= 0) return 0;

    int sent_end = sent_start + sent_len;
    int cap = 0, n = 0;
    SdValueItem *items = NULL;

    for (int i = sent_start; i < sent_end; ++i) {
        // если встретили '{{...}}' — пропускаем целиком, чтобы не взять число внутри как raw-int
        if (strcmp(L->t[i], "{") == 0 && (i + 1) < sent_end && strcmp(L->t[i + 1], "{") == 0) {
            int close_end = -1;
            char key[128];
            if (sd_extract_placeholder_key(L, i, sent_end, &close_end, key, sizeof(key))) {
                i = close_end;
                continue;
            }
        }

        long long v = 0;
                /* do not treat hex color digits as integer values: "#101010" -> skip "101010" */
        if (i > sent_start) {
            const char *prv = L->t[i - 1];
            const char *cur = L->t[i];
            if (prv && prv[0] == '#' && prv[1] == '\0' && cur && strlen(cur) == 6) {
                continue;
            }
        }
        if (sd_tok_is_int_token(L->t[i], &v)) {
            if (n + 1 > cap) {
                int ncap = cap ? cap * 2 : 8;
                SdValueItem *tmp = (SdValueItem *)realloc(items, (size_t)ncap * sizeof(SdValueItem));
                if (!tmp) { free(items); return 0; }
                items = tmp;
                cap = ncap;
            }
            SdValueItem it;
            memset(&it, 0, sizeof(it));
            it.start_tok = i;
            it.end_tok   = i;
            it.value     = v;
            it.key[0]    = 0;
            items[n++] = it;
        }
		else {
            long long a = 0, b = 0;
            if (sd_tok_parse_int_x_int_token(L->t[i], &a, &b)) {
                if (n + 2 > cap) {
                    int ncap = cap ? cap * 2 : 8;
                    while (n + 2 > ncap) ncap *= 2;
                    SdValueItem *tmp = (SdValueItem *)realloc(items, (size_t)ncap * sizeof(SdValueItem));
                    if (!tmp) { free(items); return 0; }
                    items = tmp;
                    cap = ncap;
                }

                SdValueItem it1;
                memset(&it1, 0, sizeof(it1));
                it1.start_tok = i;
                it1.end_tok   = i;
                it1.value     = a;
                it1.key[0]    = 0;
                items[n++] = it1;

                SdValueItem it2;
                memset(&it2, 0, sizeof(it2));
                it2.start_tok = i;
                it2.end_tok   = i;
                it2.value     = b;
                it2.key[0]    = 0;
                items[n++] = it2;
            }
        }

    }

    if (!items || n == 0) { free(items); return 0; }
    if (out_items) *out_items = items;
    if (out_n) *out_n = n;
    return 1;
}

static int sd_build_value_groups(
    const SdTokList *L,
    const SdValueItem *items,
    int n_items,
    SdValueGroup **out_groups,
    int *out_gn)
{
    if (out_groups) *out_groups = NULL;
    if (out_gn) *out_gn = 0;
    if (!L || !items || n_items <= 0) return 0;

    SdValueGroup *G = (SdValueGroup *)malloc((size_t)n_items * sizeof(SdValueGroup));
    if (!G) return 0;

    int gn = 0;
    G[gn].first_item = 0;
    G[gn].count = 1;
    gn++;

    for (int i = 1; i < n_items; ++i) {
        const SdValueItem *prev = &items[i - 1];
        const SdValueItem *cur  = &items[i];

        int wc = 0;
        if (cur->start_tok == prev->start_tok) {
            wc = 1; /* два значения из одного токена типа 100x400 */
        } else {
            wc = sd_count_word_tokens_between(L, prev->end_tok + 1, cur->start_tok);
        }

        if (wc == 1) {
            G[gn - 1].count++;
        } else {
            G[gn].first_item = i;
            G[gn].count = 1;
            gn++;
        }
    }

    if (out_groups) *out_groups = G;
    if (out_gn) *out_gn = gn;
    return 1;
}

static char *sd_build_value_ctx_seq_generic(
    const SdTokList *L,
    int sent_start,
    int sent_len,
    const SdValueItem *items,
    int first_item,
    int item_count)
{
    if (!L || sent_len <= 0 || !items || item_count <= 0) return NULL;

    SdTokList tmp;
    sd_toklist_init(&tmp);

    const int sent_end = sent_start + sent_len;
    int cur = sent_start;

	for (int k = 0; k < item_count; ++k) {
		const SdValueItem *it = &items[first_item + k];

		/* special: токен вида "100x400" — один токен, но ctx_seq должен быть "{{VALUE}} x {{VALUE}}" */
		if ((k + 1) < item_count) {
			const SdValueItem *it2 = &items[first_item + k + 1];
			if (it2->start_tok == it->start_tok && it2->end_tok == it->end_tok) {
				long long a = 0, b = 0;
				if (it->start_tok >= sent_start && it->start_tok < sent_end &&
					sd_tok_parse_int_x_int_token(L->t[it->start_tok], &a, &b)) {

					for (int i = cur; i < it->start_tok; ++i) {
						(void)sd_toklist_push_copy(&tmp, L->t[i]);
					}
					(void)sd_toklist_push_copy(&tmp, "{{VALUE}}");
					(void)sd_toklist_push_copy(&tmp, "x");
					(void)sd_toklist_push_copy(&tmp, "{{VALUE}}");
					cur = it->end_tok + 1;

					++k; /* съели второй value из того же токена */
					continue;
				}
			}
		}

		for (int i = cur; i < it->start_tok; ++i) {
			(void)sd_toklist_push_copy(&tmp, L->t[i]);
		}
		(void)sd_toklist_push_copy(&tmp, "{{VALUE}}");
		cur = it->end_tok + 1;
	}

    for (int i = cur; i < sent_end; ++i) {
        (void)sd_toklist_push_copy(&tmp, L->t[i]);
    }

    char *seq = sd_tokens_range_to_seq_lower(&tmp, 0, tmp.n);
    sd_toklist_free(&tmp);
    return seq;
}

static float sd_value_prob_from_similarity(float sim)
{
    if (sim >= 0.9f) return 0.99f;
    if (sim <= 0.75f) return 0.0f;
    // линейно: 0.75..0.90 => 0..0.99
    const float t = (sim - 0.75f) / (0.90f - 0.75f);
    return 0.99f * t;
}

static void sd_editparams_make_skeleton(char *out, size_t cap)
{
    if (!out || cap == 0) return;
    _snprintf_s(out, cap, _TRUNCATE,
        "SCALAR:\n"
        "NONE\n"
        "\n"
        "STRING:\n"
        "NONE\n"
        "\n"
        "HARDWARE:\n"
        "NONE\n"
        "\n"
        "NETWORK:\n"
        "NONE\n"
        "\n"
        "DOMAIN:\n"
        "NONE\n"
    );
}

// UPDATE/INSERT в SCALAR-группу: "SCALAR: <key>: <value>"
static int sd_editparams_set_scalar_int(char *edit_payload, size_t cap, const char *key, long long value)
{
    if (!edit_payload || cap == 0 || !key || !key[0]) return 0;

    char trimmed[64];
    _snprintf_s(trimmed, sizeof(trimmed), _TRUNCATE, "%s", edit_payload);
    clar_trim(trimmed);

    if (tt_is_none_text(trimmed) || !edit_payload[0]) {
        sd_editparams_make_skeleton(edit_payload, cap);
    }

	char *p_scalar = NULL;
	char *p_string_hdr = NULL;
	int repaired = 0;

	retry_headers:
		p_scalar = (char*)clar_strcasestr_local(edit_payload, "SCALAR:");
		p_string_hdr = NULL;
		if (p_scalar) {
			const char *scan = p_scalar + 1;
			for (;;) {
				char *hit = (char*)clar_strcasestr_local(scan, "STRING:");
				if (!hit) break;
				if ((hit == edit_payload || hit[-1] == '\n') && hit > p_scalar) {
					p_string_hdr = hit;
					break;
				}
				scan = hit + 1;
			}
		}

		if (!p_scalar || !p_string_hdr || p_string_hdr <= p_scalar) {
			if (!repaired) {
				repaired = 1;
				if (structurize_debug_enabled()) {
					fprintf(stderr, "[SKYNET][STRUCTURIZE][EDITP] repair skeleton (missing SCALAR/STRING) key=%s\n", key);
				}
				sd_editparams_make_skeleton(edit_payload, cap);
				goto retry_headers;
			}
			if (structurize_debug_enabled()) {
				fprintf(stderr, "[SKYNET][STRUCTURIZE][EDITP] still missing SCALAR/STRING after repair key=%s\n", key);
			}
			return 0;
		}

	char *scalar_line_end = strchr(p_scalar, '\n');
	if (!scalar_line_end) return 0;
	char *scalar_body = scalar_line_end + 1;
	char *scalar_end = p_string_hdr;

    size_t prefix_len = (size_t)(scalar_body - edit_payload);
    size_t suffix_len = strlen(scalar_end);

    // Разбираем существующие строки SCALAR
    char *block = (char *)malloc((size_t)(scalar_end - scalar_body) + 1);
    if (!block) return 0;
    memcpy(block, scalar_body, (size_t)(scalar_end - scalar_body));
    block[(size_t)(scalar_end - scalar_body)] = 0;

    char new_block[4096];
    new_block[0] = 0;

    int found = 0;
    int any = 0;

    // построчно
    char *ctx = NULL;
    for (char *line = strtok_s(block, "\n", &ctx); line; line = strtok_s(NULL, "\n", &ctx)) {
        char ltmp[512];
        _snprintf_s(ltmp, sizeof(ltmp), _TRUNCATE, "%s", line);
        clar_trim(ltmp);
        if (!ltmp[0]) continue;
        if (tt_is_none_text(ltmp)) continue;

        // формат: "INT: key: value" (тип может быть не INT, но ключ после первого ':' и до следующего ':')
        const char *s = ltmp;
        const char *c1 = strchr(s, ':');
        if (!c1) continue;
        c1++; while (*c1 == ' ') c1++;
        const char *c2 = strchr(c1, ':');
        if (!c2) continue;

        size_t klen = (size_t)(c2 - c1);
        if (klen > 127) klen = 127;
        char kbuf[128];
        memcpy(kbuf, c1, klen);
        kbuf[klen] = 0;
        clar_trim(kbuf);

        if (_stricmp(kbuf, key) == 0) {
            char row[256];
            _snprintf_s(row, sizeof(row), _TRUNCATE, "SCALAR: %s: %lld\n", key, (long long)value);
            strncat_s(new_block, sizeof(new_block), row, _TRUNCATE);
            found = 1;
            any = 1;
        } else {
            char row[512];
            _snprintf_s(row, sizeof(row), _TRUNCATE, "%s\n", ltmp);
            strncat_s(new_block, sizeof(new_block), row, _TRUNCATE);
            any = 1;
        }
    }

    free(block);

    if (!found) {
        char row[256];
        _snprintf_s(row, sizeof(row), _TRUNCATE, "SCALAR: %s: %lld\n", key, (long long)value);
        strncat_s(new_block, sizeof(new_block), row, _TRUNCATE);
        any = 1;
    }

    if (!any) {
        _snprintf_s(new_block, sizeof(new_block), _TRUNCATE, "NONE\n");
    }

    // Нормализуем: один пустой line после SCALAR-блока (перед STRING:)
    size_t nb_len = strlen(new_block);
    if (nb_len == 0) {
        _snprintf_s(new_block, sizeof(new_block), _TRUNCATE, "NONE\n");
        nb_len = strlen(new_block);
    }
    if (nb_len >= 1 && new_block[nb_len - 1] != '\n') {
        strncat_s(new_block, sizeof(new_block), "\n", _TRUNCATE);
        nb_len = strlen(new_block);
    }
    // добавить ещё один '\n' чтобы был пустой ряд
    if (nb_len < 2 || new_block[nb_len - 1] != '\n' || new_block[nb_len - 2] != '\n') {
        strncat_s(new_block, sizeof(new_block), "\n", _TRUNCATE);
        nb_len = strlen(new_block);
    }

    size_t need = prefix_len + nb_len + suffix_len + 1;
    if (need > cap) return 0;

    char *tmp = (char *)malloc(cap);
    if (!tmp) return 0;

    memcpy(tmp, edit_payload, prefix_len);
    memcpy(tmp + prefix_len, new_block, nb_len);
    memcpy(tmp + prefix_len + nb_len, scalar_end, suffix_len + 1);

    memcpy(edit_payload, tmp, need);
    free(tmp);
    return 1;
}

static int sd_editparams_get_scalar_int(const char *edit_payload, const char *key, long long *out_value)
{
    if (!edit_payload || !key || !key[0] || !out_value) return 0;

    const char *p_scalar = clar_strcasestr_local(edit_payload, "SCALAR:");
    if (!p_scalar) return 0;

    const char *p_string_hdr = NULL;
    {
        const char *scan = p_scalar + 1;
        for (;;) {
            const char *hit = clar_strcasestr_local(scan, "STRING:");
            if (!hit) break;
            if ((hit == edit_payload || hit[-1] == '\n') && hit > p_scalar) {
                p_string_hdr = hit;
                break;
            }
            scan = hit + 1;
        }
    }
    if (!p_string_hdr || p_string_hdr <= p_scalar) return 0;

    const char *scalar_line_end = strchr(p_scalar, '\n');
    if (!scalar_line_end) return 0;

    const char *p = scalar_line_end + 1;
    const char *end = p_string_hdr;

    while (p < end) {
        const char *nl = (const char*)memchr(p, '\n', (size_t)(end - p));
        const char *line_end = nl ? nl : end;
        size_t len = (size_t)(line_end - p);
        if (len > 0) {
            char ltmp[512];
            if (len >= sizeof(ltmp)) len = sizeof(ltmp) - 1;
            memcpy(ltmp, p, len);
            ltmp[len] = 0;
            clar_trim(ltmp);
            if (ltmp[0] && !tt_is_none_text(ltmp)) {
                const char *s = ltmp;
                const char *c1 = strchr(s, ':');
                if (c1) {
                    c1++; while (*c1 == ' ') c1++;
                    const char *c2 = strchr(c1, ':');
                    if (c2) {
                        size_t klen = (size_t)(c2 - c1);
                        if (klen > 127) klen = 127;
                        char kbuf[128];
                        memcpy(kbuf, c1, klen);
                        kbuf[klen] = 0;
                        clar_trim(kbuf);

                        if (_stricmp(kbuf, key) == 0) {
                            const char *v = c2 + 1;
                            while (*v == ' ') v++;
                            *out_value = (long long)strtoll(v, NULL, 10);
                            return 1;
                        }
                    }
                }
            }
        }
        p = nl ? (nl + 1) : end;
    }

    return 0;
}

static int sd_editparams_has_string_key(const char *edit_payload, const char *key)
{
    if (!edit_payload || !key || !key[0]) return 0;

    const char *p_str = clar_strcasestr_local(edit_payload, "STRING:");
    if (!p_str) return 0;

    const char *p_after = strchr(p_str, '\n');
    if (!p_after) return 0;
    p_after++;

    const char *end_payload = edit_payload + strlen(edit_payload);

    /* find end of STRING section */
    const char *p_end = end_payload;
    {
        const char *hdrs[] = { "\nHARDWARE:", "\nNETWORK:", "\nDOMAIN:" };
        for (int i = 0; i < (int)(sizeof(hdrs)/sizeof(hdrs[0])); ++i) {
            const char *h = clar_strcasestr_local(p_str, hdrs[i]);
            if (h && h > p_str && h < p_end) p_end = h;
        }
    }

    const char *p = p_after;
    while (p < p_end) {
        const char *nl = (const char*)memchr(p, '\n', (size_t)(p_end - p));
        const char *le = nl ? nl : p_end;

        char line[512];
        size_t ln = (size_t)(le - p);
        if (ln >= sizeof(line)) ln = sizeof(line) - 1;
        memcpy(line, p, ln);
        line[ln] = '\0';
        clar_trim(line);

        if (!line[0]) break;
        if (tt_is_none_text(line)) { p = nl ? (nl + 1) : p_end; continue; }

        if (!_strnicmp(line, "STRING:", 7)) {
            const char *ks = line + 7;
            while (*ks == ' ' || *ks == '\t') ks++;
            const char *kc = strchr(ks, ':');
            if (kc) {
                while (kc > ks && (kc[-1] == ' ' || kc[-1] == '\t')) kc--;
                char kbuf[128];
                size_t klen = (size_t)(kc - ks);
                if (klen >= sizeof(kbuf)) klen = sizeof(kbuf) - 1;
                memcpy(kbuf, ks, klen);
                kbuf[klen] = '\0';
                clar_trim(kbuf);

                if (_stricmp(kbuf, key) == 0) return 1;
            }
        }

        p = nl ? (nl + 1) : p_end;
    }

    return 0;
}

static int sd_editparams_delete_scalar_key(char *edit_payload, size_t cap, const char *key)
{
    if (!edit_payload || cap == 0 || !key || !key[0]) return 0;

    char *p_scalar = (char*)clar_strcasestr_local(edit_payload, "SCALAR:");
    if (!p_scalar) return 0;

    char *p_string_hdr = NULL;
    {
        const char *scan = p_scalar + 1;
        for (;;) {
            char *hit = (char*)clar_strcasestr_local(scan, "STRING:");
            if (!hit) break;
            if ((hit == edit_payload || hit[-1] == '\n') && hit > p_scalar) { p_string_hdr = hit; break; }
            scan = hit + 1;
        }
    }
    if (!p_string_hdr || p_string_hdr <= p_scalar) return 0;

    char *scalar_line_end = strchr(p_scalar, '\n');
    if (!scalar_line_end) return 0;

    char *scalar_body = scalar_line_end + 1;
    char *scalar_end = p_string_hdr;

    char *block = (char *)malloc((size_t)(scalar_end - scalar_body) + 1);
    if (!block) return 0;
    memcpy(block, scalar_body, (size_t)(scalar_end - scalar_body));
    block[(size_t)(scalar_end - scalar_body)] = 0;

    char new_block[8192];
    new_block[0] = 0;

    int any = 0;
    int removed = 0;

    char *ctx = NULL;
    for (char *line = strtok_s(block, "\n", &ctx); line; line = strtok_s(NULL, "\n", &ctx)) {
        char ltmp[512];
        _snprintf_s(ltmp, sizeof(ltmp), _TRUNCATE, "%s", line);
        clar_trim(ltmp);
        if (!ltmp[0]) continue;
        if (tt_is_none_text(ltmp)) continue;

        const char *s = ltmp;
        const char *c1 = strchr(s, ':');
        if (!c1) continue;
        c1++; while (*c1 == ' ') c1++;
        const char *c2 = strchr(c1, ':');
        if (!c2) continue;

        size_t klen = (size_t)(c2 - c1);
        if (klen > 127) klen = 127;
        char kbuf[128];
        memcpy(kbuf, c1, klen);
        kbuf[klen] = 0;
        clar_trim(kbuf);

        if (_stricmp(kbuf, key) == 0) {
            removed = 1;
            continue;
        }

        {
            char row[512];
            _snprintf_s(row, sizeof(row), _TRUNCATE, "%s\n", ltmp);
            strncat_s(new_block, sizeof(new_block), row, _TRUNCATE);
            any = 1;
        }
    }

    free(block);

    if (!removed) return 0;

    if (!any) {
        _snprintf_s(new_block, sizeof(new_block), _TRUNCATE, "NONE\n");
    }

    size_t nb_len = strlen(new_block);
    if (nb_len == 0) { _snprintf_s(new_block, sizeof(new_block), _TRUNCATE, "NONE\n"); nb_len = strlen(new_block); }
    if (nb_len >= 1 && new_block[nb_len - 1] != '\n') { strncat_s(new_block, sizeof(new_block), "\n", _TRUNCATE); nb_len = strlen(new_block); }
    if (nb_len < 2 || new_block[nb_len - 1] != '\n' || new_block[nb_len - 2] != '\n') {
        strncat_s(new_block, sizeof(new_block), "\n", _TRUNCATE);
        nb_len = strlen(new_block);
    }

    size_t prefix_len = (size_t)(scalar_body - edit_payload);
    size_t suffix_len = strlen(scalar_end);

    if (prefix_len + nb_len + suffix_len + 1 > cap) return 0;

    char *tmp = (char *)malloc(cap);
    if (!tmp) return 0;

    memcpy(tmp, edit_payload, prefix_len);
    memcpy(tmp + prefix_len, new_block, nb_len);
    memcpy(tmp + prefix_len + nb_len, scalar_end, suffix_len + 1);

    memcpy(edit_payload, tmp, prefix_len + nb_len + suffix_len + 1);
    free(tmp);
    return 1;
}

static int sd_toklist_count_ci(const SdTokList *L, const char *tok) {
    if (!L || !tok || !tok[0]) return 0;
    int c = 0;
    for (int i = 0; i < L->n; ++i) {
        if (sd_tok_eq_ci(L->t[i], tok)) c++;
    }
    return c;
}

static int sd_chg_find_by_before(const SdChangeWord *C, int cn, const char *before_tok) {
    if (!C || cn <= 0 || !before_tok) return -1;
    for (int i = 0; i < cn; ++i) {
        if (C[i].before_tok && sd_tok_eq_ci(C[i].before_tok, before_tok)) return i;
    }
    return -1;
}

/* Learn CHANGE_WORD for GOAL only:
   - context match: prev token and next token must match (case-insensitive)
   - before_tok must be absent in A_goal (otherwise apply would over-replace)
   - after_tok must be absent in B_goal (avoid global false-positive)
   - distance <= 2 => ENDING, else (<= 6) => REPLACE
*/
static int sd_derive_change_words_goal(
    const SdTokList *B_goal,
    const SdTokList *A_goal,
    SdChangeWord **out_chg,
    int *out_cn)
{
    if (out_chg) *out_chg = NULL;
    if (out_cn) *out_cn = 0;

    if (!B_goal || !A_goal || B_goal->n <= 0 || A_goal->n <= 0) return 1;

    SdChangeWord *C = NULL;
    int cn = 0, cc = 0;

    for (int i = 0; i < B_goal->n; ++i) {
        const char *oldtok = B_goal->t[i];
        if (!sd_tok_is_wordlike(oldtok)) continue;

        if (sd_toklist_count_ci(A_goal, oldtok) > 0) continue;

        const char *prev = (i > 0) ? B_goal->t[i - 1] : NULL;
        const char *next = (i + 1 < B_goal->n) ? B_goal->t[i + 1] : NULL;

        const char *best_new = NULL;
        int best_dist = 999;
        int best_j = -1;
        int ambiguous = 0;

        for (int j = 0; j < A_goal->n; ++j) {
            const char *newtok = A_goal->t[j];
            if (!sd_tok_is_wordlike(newtok)) continue;
            if (sd_tok_eq_ci(newtok, oldtok)) continue;

            if (prev) {
                if (j <= 0) continue;
                if (!sd_tok_eq_ci(A_goal->t[j - 1], prev)) continue;
            } else {
                if (j != 0) continue;
            }

            if (next) {
                if (j + 1 >= A_goal->n) continue;
                if (!sd_tok_eq_ci(A_goal->t[j + 1], next)) continue;
            } else {
                if (j != A_goal->n - 1) continue;
            }

            if (sd_toklist_count_ci(B_goal, newtok) > 0) continue;

            const int dist = sd_levenshtein_bounded_ci(oldtok, newtok, 6);
            if (dist > 6) continue;

            if (dist < best_dist) {
                best_dist = dist;
                best_new = newtok;
                best_j = j;
                ambiguous = 0;
            } else if (dist == best_dist && best_new && !sd_tok_eq_ci(best_new, newtok)) {
                ambiguous = 1;
            }
        }

        if (best_j < 0 || !best_new || ambiguous) continue;

        int flags = (best_dist <= 2) ? SD_CHG_FLAG_ENDING : SD_CHG_FLAG_REPLACE;

        const int idx = sd_chg_find_by_before(C, cn, oldtok);
        if (idx >= 0) {
            if (!sd_tok_eq_ci(C[idx].after_tok, best_new)) {
                /* conflict for same before_tok -> drop this mapping (too risky) */
                sd_changeword_free(&C[idx]);
                C[idx].before_tok = _strdup("__CONFLICT__");
                C[idx].after_tok  = _strdup("__CONFLICT__");
                C[idx].flags = 0;
            }
            continue;
        }

        if (cn + 1 > cc) {
            int nc = (cc == 0) ? 16 : (cc * 2);
            SdChangeWord *nC = (SdChangeWord*)realloc(C, sizeof(SdChangeWord) * (size_t)nc);
            if (!nC) continue;
            C = nC;
            cc = nc;
        }

        C[cn].before_tok = _strdup(oldtok);
        C[cn].after_tok  = _strdup(best_new);
        C[cn].flags      = flags;

        if (!C[cn].before_tok || !C[cn].after_tok) {
            sd_changeword_free(&C[cn]);
            continue;
        }

        cn++;
    }

    /* compact out conflicts */
    int w = 0;
    for (int i = 0; i < cn; ++i) {
        if (!C[i].before_tok || strcmp(C[i].before_tok, "__CONFLICT__") == 0) {
            sd_changeword_free(&C[i]);
            continue;
        }
        if (w != i) C[w] = C[i];
        w++;
    }
    cn = w;

    if (out_chg) *out_chg = C;
    if (out_cn) *out_cn = cn;
    return 1;
}

typedef struct SdWordCtxRule {
    char *src_word;   // owned
    char *dst_word;   // owned
    char *ctx_seq;    // owned (lowercase context 4+4 around the source token)
} SdWordCtxRule;

static void sd_wordctxrule_free(SdWordCtxRule *r)
{
    if (!r) return;
    if (r->src_word) free(r->src_word);
    if (r->dst_word) free(r->dst_word);
    if (r->ctx_seq)  free(r->ctx_seq);
    r->src_word = r->dst_word = r->ctx_seq = NULL;
}

static char *sd_build_word_ctx_seq_4_4(const SdTokList *L, int pos)
{
    if (!L || pos < 0 || pos >= L->n) return NULL;

    const char *left[4]  = {0};
    const char *right[4] = {0};
    int ln = 0, rn = 0;

    const char *VAL = "VAL";

    for (int i = pos - 1; i >= 0 && ln < 4; --i) {
        const char *tok = L->t[i];
        if (!tok || !tok[0]) continue;

        /* treat {{...}} as single VAL when scanning left */
        if (tok[0] == '}' && tok[1] == '\0' && (i - 1) >= 0) {
            const char *p2 = L->t[i - 1];
            if (p2 && p2[0] == '}' && p2[1] == '\0') {
                int s = -1;
                for (int k = i - 2; k >= 1; --k) {
                    const char *a = L->t[k - 1];
                    const char *b = L->t[k];
                    if (a && b && a[0] == '{' && a[1] == '\0' && b[0] == '{' && b[1] == '\0') { s = k - 1; break; }
                }
                left[ln++] = VAL;
                if (s >= 0) i = s;
                continue;
            }
        }

        if (sd_tok_is_wordlike(tok)) left[ln++] = tok;
    }

    for (int i = pos + 1; i < L->n && rn < 4; ++i) {
        const char *tok = L->t[i];
        if (!tok || !tok[0]) continue;

        /* treat {{...}} as single VAL when scanning right */
        if (tok[0] == '{' && tok[1] == '\0' && (i + 1) < L->n) {
            const char *n2 = L->t[i + 1];
            if (n2 && n2[0] == '{' && n2[1] == '\0') {
                int e = -1;
                for (int k = i + 2; k + 1 < L->n; ++k) {
                    const char *a = L->t[k];
                    const char *b = L->t[k + 1];
                    if (a && b && a[0] == '}' && a[1] == '\0' && b[0] == '}' && b[1] == '\0') { e = k + 1; break; }
                }
                right[rn++] = VAL;
                if (e >= 0) i = e;
                continue;
            }
        }

        if (sd_tok_is_wordlike(tok)) right[rn++] = tok;
    }

    size_t cap = 32;
    for (int i = 0; i < ln; ++i) cap += strlen(left[i]) + 1;
    for (int i = 0; i < rn; ++i) cap += strlen(right[i]) + 1;

    char *out = (char*)malloc(cap);
    if (!out) return NULL;

    size_t w = 0;
    #define APPEND_LIT(lit) do { \
        const char *_p = (lit); \
        while (*_p && w + 1 < cap) out[w++] = *_p++; \
    } while (0)

    #define APPEND_WORD_LOWER(word) do { \
        const char *_p = (word); \
        while (*_p && w + 1 < cap) { \
            char c = *_p++; \
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a'); \
            out[w++] = c; \
        } \
    } while (0)

    APPEND_LIT("L:");

    for (int i = ln - 1; i >= 0; --i) {
        if (w + 2 >= cap) break;
        out[w++] = ' ';
        APPEND_WORD_LOWER(left[i]);
    }

    APPEND_LIT(" R:");

    for (int i = 0; i < rn; ++i) {
        if (w + 2 >= cap) break;
        out[w++] = ' ';
        APPEND_WORD_LOWER(right[i]);
    }

    out[w] = '\0';
    #undef APPEND_LIT
    #undef APPEND_WORD_LOWER
    return out;
}

static int sd_derive_word_ctx_rules_section(
    const SdTokList *B,
    const SdTokList *A,
    SdWordCtxRule **out_rules,
    int *out_n);

static int sd_derive_word_ctx_rules_goal(const SdTokList *B_goal, const SdTokList *A_goal, SdWordCtxRule **out_R, int *out_n)
{
    if (out_R) *out_R = NULL;
    if (out_n) *out_n = 0;
    if (!B_goal || !A_goal || !out_R || !out_n) return 0;

    const int BN = B_goal->n;
    const int AN = A_goal->n;
    if (BN <= 0 || AN <= 0) return 1;

    SdWordCtxRule *R = NULL;
    int rn = 0, rcap = 0;

    for (int i = 0; i < BN; ++i) {
        const char *oldtok = B_goal->t[i];
        if (!sd_tok_is_wordlike(oldtok)) continue;
        if (sd_toklist_count_ci(A_goal, oldtok) > 0) continue;

        const char *prev = (i > 0) ? B_goal->t[i - 1] : NULL;
        const char *next = (i + 1 < BN) ? B_goal->t[i + 1] : NULL;
        const int need_prev = (prev && prev[0]) ? 1 : 0;
        const int need_next = (next && next[0]) ? 1 : 0;

        const char *best_new = NULL;
        int best_j = -1;
        int best_dist = 999;

        for (int j = 0; j < AN; ++j) {
            const char *cand = A_goal->t[j];
            if (!sd_tok_is_wordlike(cand)) continue;
            if (sd_toklist_count_ci(B_goal, cand) > 0) continue;

            if (need_prev) {
                int ok_prev = 0;
                if (j > 0 && A_goal->t[j - 1] && _stricmp(A_goal->t[j - 1], prev) == 0) ok_prev = 1;
                if (!ok_prev) continue;
            }
            if (need_next) {
                int ok_next = 0;
                if (j + 1 < AN && A_goal->t[j + 1] && _stricmp(A_goal->t[j + 1], next) == 0) ok_next = 1;
                if (!ok_next) continue;
            }

            int d = sd_levenshtein_bounded_ci(oldtok, cand, 8);
            if (d < 0) d = 999;
            if (d < best_dist) {
                best_dist = d;
                best_new = cand;
                best_j = j;
            }
        }

        if (best_new && best_j >= 0) {
            char *ctx_seq = sd_build_word_ctx_seq_4_4(B_goal, i);
            if (!ctx_seq || !ctx_seq[0]) {
                if (ctx_seq) free(ctx_seq);
                continue;
            }

            if (rn >= rcap) {
                int ncap = (rcap == 0) ? 8 : (rcap * 2);
                SdWordCtxRule *tmp = (SdWordCtxRule*)realloc(R, sizeof(SdWordCtxRule) * ncap);
                if (!tmp) {
                    free(ctx_seq);
                    break;
                }
                R = tmp;
                for (int k = rcap; k < ncap; ++k) {
                    R[k].src_word = NULL;
                    R[k].dst_word = NULL;
                    R[k].ctx_seq  = NULL;
                }
                rcap = ncap;
            }

            R[rn].src_word = _strdup(oldtok);
            R[rn].dst_word = _strdup(best_new);
            R[rn].ctx_seq  = ctx_seq;

            if (!R[rn].src_word || !R[rn].dst_word || !R[rn].ctx_seq) {
                sd_wordctxrule_free(&R[rn]);
                continue;
            }
            rn++;
        }
    }

    *out_R = R;
    *out_n = rn;
    return 1;
}

static int sd_derive_word_ctx_rules_section(const SdTokList *B, const SdTokList *A, SdWordCtxRule **out_rules, int *out_n)
{
    /* переиспользуем тот же алгоритм, что и для GOAL, но для любой секции */
    return sd_derive_word_ctx_rules_goal(B, A, out_rules, out_n);
}

typedef struct SdCand {
    sqlite3_int64 program_id;
    int successes;
    int failures;
    float sim;
    float rel;
    float score;
} SdCand;

static float structurize_delta_cosine_sim(const float *a, const float *b, int dim)
{
    if (!a || !b || dim <= 0) return -1.0f;
    double dot = 0.0, na = 0.0, nb = 0.0;
    for (int i = 0; i < dim; ++i) {
        const double da = (double)a[i];
        const double db = (double)b[i];
        dot += da * db;
        na  += da * da;
        nb  += db * db;
    }
    if (na <= 1e-18 || nb <= 1e-18) return -1.0f;
    return (float)(dot / (sqrt(na) * sqrt(nb)));
}

static void structurize_delta_hex64(unsigned long long v, char out[32])
{
    if (!out) return;
    _snprintf_s(out, 32, _TRUNCATE, "%016llx", (unsigned long long)v);
}

static void sd_trunc_str(const char *in, char *out, size_t cap)
{
    if (!out || cap == 0) return;
    if (!in) { out[0] = '\0'; return; }

    size_t n = strlen(in);
    if (n < cap) {
        memcpy(out, in, n + 1);
        return;
    }
    if (cap < 5) { out[0] = '\0'; return; }

    memcpy(out, in, cap - 4);
    out[cap - 4] = '.';
    out[cap - 3] = '.';
    out[cap - 2] = '.';
    out[cap - 1] = '\0';
}

static int structurize_delta_select_embed_model(
    const char *text,
    void **out_model,
    const char **out_model_name,
    int *out_dim)
{
    if (out_model) *out_model = NULL;
    if (out_model_name) *out_model_name = "";
    if (out_dim) *out_dim = 0;

    const int is_ru = (text && text[0]) ? (clar_is_russian(text) ? 1 : 0) : 0;
    void *m = is_ru ? g_llama_model_ru : g_llama_model_en;
    const char *name = is_ru ? "struct-v1-ru" : "struct-v1-en";

    // fallback if selected not available
    if (!m) {
        if (g_llama_model_en) { m = g_llama_model_en; name = "struct-v1-en"; }
        else if (g_llama_model_ru) { m = g_llama_model_ru; name = "struct-v1-ru"; }
    }
    if (!m) return 0;

    const int dim = llama_client_get_embedding_dim(m);
    if (dim <= 0) return 0;

    if (out_model) *out_model = m;
    if (out_model_name) *out_model_name = name;
    if (out_dim) *out_dim = dim;
    return 1;
}

// ===== Step1.4: embeddings artifacts for AUTO->FIX pair =====

static void sd_make_section_embed_text(const char *section, const char *payload, char *out, size_t cap)
{
    if (!out || cap == 0) return;
    if (!section) section = "";
    if (!payload) payload = "";
    _snprintf_s(out, cap, _TRUNCATE, "SECTION=%s\n%s", section, payload);
}

static int structurize_fix_pair_upsert_embedding(
    sqlite3 *db,
    sqlite3_int64 pair_id,
    const char *kind,
    const char *text)
{
    if (!db || pair_id <= 0) return 0;
    if (!kind || !kind[0]) return 0;
    if (!text) text = "";

    void *m = NULL;
    const char *mname = NULL;
    int dim = 0;

    if (!structurize_delta_select_embed_model(text, &m, &mname, &dim) || !m || dim <= 0) return 0;

    float *vec = (float*)malloc(sizeof(float) * (size_t)dim);
    if (!vec) return 0;

    if (!llama_client_get_embeddings(m, text, vec)) {
        free(vec);
        return 0;
    }

    const char *sql =
        "INSERT OR REPLACE INTO struct_fix_pair_embeddings(pair_id,kind,embed_model,embed_dim,embedding,created_at) "
        "VALUES(?,?,?,?,?,CURRENT_TIMESTAMP);";

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        free(vec);
        return 0;
    }

    sqlite3_bind_int64(st, 1, pair_id);
    sqlite3_bind_text(st,  2, kind, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st,  3, mname ? mname : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st,   4, dim);
    sqlite3_bind_blob(st,  5, vec, (int)(sizeof(float) * (size_t)dim), SQLITE_TRANSIENT);

    const int ok = (sqlite3_step(st) == SQLITE_DONE) ? 1 : 0;
    sqlite3_finalize(st);
    free(vec);
    return ok;
}

static void sd_append_tok_lower(char *buf, size_t cap, size_t *io_w, const char *tok)
{
    if (!buf || cap == 0 || !io_w || !tok || !tok[0]) return;

    // prepend space if needed
    if (*io_w > 0 && *io_w + 1 < cap) buf[(*io_w)++] = ' ';

    for (const unsigned char *p = (const unsigned char*)tok; *p; ++p) {
        if (*io_w + 1 >= cap) break;
        buf[(*io_w)++] = (char)tolower((int)*p);
    }
    if (*io_w < cap) buf[*io_w] = '\0';
}

static char *sd_build_span_ctx_seq_4_4(const SdTokList *L, int start, int len, int include_payload)
{
    if (!L || L->n <= 0) return NULL;
    if (start < 0 || start >= L->n) return NULL;
    if (len <= 0) return NULL;

    const int end = start + len;
    if (end > L->n) return NULL;

    char buf[4096];
    buf[0] = '\0';
    size_t w = 0;

    // left 4 wordlike
    int left_idx[4];
    int ln = 0;
    for (int i = start - 1; i >= 0 && ln < 4; --i) {
        if (sd_tok_is_wordlike(L->t[i])) left_idx[ln++] = i;
    }

    // "L:"
    sd_append_tok_lower(buf, sizeof(buf), &w, "L:");
    for (int k = ln - 1; k >= 0; --k) {
        const char *t = L->t[left_idx[k]];
        if (t && t[0] && strcmp(t, "\\n") != 0) sd_append_tok_lower(buf, sizeof(buf), &w, t);
    }

    if (include_payload) {
        // "P:" + payload (bounded to 32 tokens)
        sd_append_tok_lower(buf, sizeof(buf), &w, "P:");
        int pc = 0;
        for (int i = start; i < end && pc < 32; ++i, ++pc) {
            const char *t = L->t[i];
            if (t && t[0] && strcmp(t, "\\n") != 0) sd_append_tok_lower(buf, sizeof(buf), &w, t);
        }
    }

    // right 4 wordlike
    sd_append_tok_lower(buf, sizeof(buf), &w, "R:");
    int rc = 0;
    for (int i = end; i < L->n && rc < 4; ++i) {
        if (sd_tok_is_wordlike(L->t[i])) {
            const char *t = L->t[i];
            if (t && t[0] && strcmp(t, "\\n") != 0) sd_append_tok_lower(buf, sizeof(buf), &w, t);
            rc++;
        }
    }

    if (!buf[0]) return NULL;
    return _strdup(buf);
}

static int structurize_fix_pair_insert_span_embedding(
    sqlite3 *db,
    sqlite3_int64 pair_id,
    int op_type,
    int ordinal,
    const char *src_section,
    const char *dst_section,
    unsigned long long anchor_hash,
    unsigned long long payload_hash,
    int offset,
    int which,
    const char *ctx_seq)
{
    if (!db || pair_id <= 0) return 0;
    if (!ctx_seq || !ctx_seq[0]) return 0;

    void *m = NULL;
    const char *mname = NULL;
    int dim = 0;

    if (!structurize_delta_select_embed_model(ctx_seq, &m, &mname, &dim) || !m || dim <= 0) return 0;

    float *vec = (float*)malloc(sizeof(float) * (size_t)dim);
    if (!vec) return 0;

    if (!llama_client_get_embeddings(m, ctx_seq, vec)) {
        free(vec);
        return 0;
    }

    char anchor_hex[32];
    char payload_hex[32];
    structurize_delta_hex64(anchor_hash, anchor_hex);
    structurize_delta_hex64(payload_hash, payload_hex);

    const char *sql =
        "INSERT OR REPLACE INTO struct_fix_pair_span_embeddings("
        "pair_id,op_type,ordinal,src_section,dst_section,anchor_hash,payload_hash,offset,which,ctx_seq,embed_model,embed_dim,embedding,created_at"
        ") VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,CURRENT_TIMESTAMP);";

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        free(vec);
        return 0;
    }

    sqlite3_bind_int64(st, 1, pair_id);
    sqlite3_bind_int(st,   2, op_type);
    sqlite3_bind_int(st,   3, ordinal);
    sqlite3_bind_text(st,  4, src_section ? src_section : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st,  5, dst_section ? dst_section : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st,  6, anchor_hex, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st,  7, payload_hex, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st,   8, offset);
    sqlite3_bind_int(st,   9, which);
    sqlite3_bind_text(st, 10, ctx_seq, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 11, mname ? mname : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st,  12, dim);
    sqlite3_bind_blob(st, 13, vec, (int)(sizeof(float) * (size_t)dim), SQLITE_TRANSIENT);

    const int ok = (sqlite3_step(st) == SQLITE_DONE) ? 1 : 0;
    sqlite3_finalize(st);
    free(vec);
    return ok;
}

static void structurize_fix_pair_save_basic_embeddings(
    sqlite3 *db,
    sqlite3_int64 pair_id,
    const char *input_text,
    const char *after_struct)
{
    if (!db || pair_id <= 0) return;
    if (!input_text) input_text = "";
    if (!after_struct) after_struct = "";

    // INPUT_CANON
    char *canon = sd_canonize_alloc(input_text);
    if (!canon) canon = _strdup(input_text);

    {
        char embed_text[65536];
        sd_make_section_embed_text("INPUT_CANON", canon ? canon : input_text, embed_text, sizeof(embed_text));
        (void)structurize_fix_pair_upsert_embedding(db, pair_id, "INPUT_CANON", embed_text);
    }

    if (canon) free(canon);

    // AFTER:<SECTION>
    const char *sec_keys[] = {
        "GOAL:",
        "REQUIREMENTS:",
        "INPUT:",
        "OUTPUT:",
        "CONSTRAINTS:",
        "ENVIRONMENT:",
        "EDIT_PARAMETERS:",
        "NON_GOALS:"
    };
    const char *sec_names[] = {
        "GOAL",
        "REQUIREMENTS",
        "INPUT",
        "OUTPUT",
        "CONSTRAINTS",
        "ENVIRONMENT",
        "EDIT_PARAMETERS",
        "NON_GOALS"
    };

    for (int i = 0; i < 8; ++i) {
        char payload[65536];
        payload[0] = '\0';
        tr_copy_section_payload(after_struct, sec_keys[i], payload, sizeof(payload));

        char embed_text[65536];
        sd_make_section_embed_text(sec_names[i], payload, embed_text, sizeof(embed_text));

        char kind[64];
        _snprintf_s(kind, sizeof(kind), _TRUNCATE, "AFTER:%s", sec_names[i]);

        (void)structurize_fix_pair_upsert_embedding(db, pair_id, kind, embed_text);
    }
}

static void structurize_fix_pair_save_span_embeddings_from_pair(
    sqlite3 *db,
    sqlite3_int64 pair_id,
    const char *before_struct,
    const char *after_struct)
{
    if (!db || pair_id <= 0) return;
    if (!before_struct) before_struct = "";
    if (!after_struct) after_struct = "";
	
	enum { SD_SPAN_SEC_CAP = 65536 };
    char *span_blk = (char*)calloc(9, (size_t)SD_SPAN_SEC_CAP);
    if (!span_blk) return;

    char *b_goal = span_blk + 0 * SD_SPAN_SEC_CAP;
    char *a_goal = span_blk + 1 * SD_SPAN_SEC_CAP;
    char *a_req  = span_blk + 2 * SD_SPAN_SEC_CAP;
    char *a_inp  = span_blk + 3 * SD_SPAN_SEC_CAP;
    char *a_outp = span_blk + 4 * SD_SPAN_SEC_CAP;
    char *a_cons = span_blk + 5 * SD_SPAN_SEC_CAP;
    char *a_env  = span_blk + 6 * SD_SPAN_SEC_CAP;
    char *a_edit = span_blk + 7 * SD_SPAN_SEC_CAP;
    char *a_ng   = span_blk + 8 * SD_SPAN_SEC_CAP;

    tr_copy_section_payload(before_struct, "GOAL:", b_goal, SD_SPAN_SEC_CAP);
    tr_copy_section_payload(after_struct,  "GOAL:", a_goal, SD_SPAN_SEC_CAP);

    SdTokList T_b_goal, T_a_goal;
    if (!sd_tokenize_payload(b_goal, &T_b_goal) || !sd_tokenize_payload(a_goal, &T_a_goal)) {
        sd_toklist_free(&T_b_goal);
        sd_toklist_free(&T_a_goal);
        free(span_blk);
        return;
    }

    tr_copy_section_payload(after_struct, "REQUIREMENTS:",	  a_req,  SD_SPAN_SEC_CAP);
    tr_copy_section_payload(after_struct, "INPUT:",			  a_inp,  SD_SPAN_SEC_CAP);
    tr_copy_section_payload(after_struct, "OUTPUT:",		  a_outp, SD_SPAN_SEC_CAP);
    tr_copy_section_payload(after_struct, "CONSTRAINTS:",	  a_cons, SD_SPAN_SEC_CAP);
    tr_copy_section_payload(after_struct, "ENVIRONMENT:",	  a_env,  SD_SPAN_SEC_CAP);
    tr_copy_section_payload(after_struct, "EDIT_PARAMETERS:", a_edit, SD_SPAN_SEC_CAP);
    tr_copy_section_payload(after_struct, "NON_GOALS:",		  a_ng,   SD_SPAN_SEC_CAP);

    SdTokList T_secs[7];
    for (int i = 0; i < 7; ++i) sd_toklist_init(&T_secs[i]);

    if (!sd_tokenize_payload(a_req,  &T_secs[0]) ||
        !sd_tokenize_payload(a_inp,  &T_secs[1]) ||
        !sd_tokenize_payload(a_outp, &T_secs[2]) ||
        !sd_tokenize_payload(a_cons, &T_secs[3]) ||
        !sd_tokenize_payload(a_env,  &T_secs[4]) ||
        !sd_tokenize_payload(a_edit, &T_secs[5]) ||
        !sd_tokenize_payload(a_ng,   &T_secs[6]))
    {
        sd_toklist_free(&T_b_goal);
        sd_toklist_free(&T_a_goal);
        for (int i = 0; i < 7; ++i) sd_toklist_free(&T_secs[i]);
        free(span_blk);
        return;
    }

    const char *sec_names[7] = {
        "REQUIREMENTS",
        "INPUT",
        "OUTPUT",
        "CONSTRAINTS",
        "ENVIRONMENT",
        "EDIT_PARAMETERS",
        "NON_GOALS"
    };

    // derive moves/deletes
    SdMove *moves = NULL; int mn = 0;
    char **deletes = NULL; int dn = 0;
    (void)sd_derive_moves_deletes(&T_b_goal, &T_a_goal, T_secs, sec_names, 7, &moves, &mn, &deletes, &dn);

    // derive inserts
    SdInsert *ins = NULL; int in = 0;
    const SdTokList *A_ins_secs[8] = { &T_a_goal, &T_secs[0], &T_secs[1], &T_secs[2], &T_secs[3], &T_secs[4], &T_secs[5], &T_secs[6] };
    const char *A_ins_names[8] = { "GOAL", "REQUIREMENTS", "INPUT", "OUTPUT", "CONSTRAINTS", "ENVIRONMENT", "EDIT_PARAMETERS", "NON_GOALS" };
    (void)sd_derive_inserts(&T_b_goal, A_ins_secs, A_ins_names, 8, &ins, &in);

    // save MOVE spans: ctx_before(source includes payload) + ctx_after(dest includes payload)
    for (int i = 0; i < mn; ++i) {
        if (!moves[i].dst_section || !moves[i].tokseq_lower || !moves[i].tokseq_lower[0]) continue;

        SdTokList needle;
        if (!sd_parse_token_seq(moves[i].tokseq_lower, &needle) || needle.n <= 0) {
            sd_toklist_free(&needle);
            continue;
        }

        int sfirst = -1, scnt = 0;
        sd_find_subseq_ci(&T_b_goal, &needle, &sfirst, &scnt);
        if (scnt != 1 || sfirst < 0) {
            sd_toklist_free(&needle);
            continue;
        }

        int dst_idx = -1;
        for (int s = 0; s < 7; ++s) {
            if (sec_names[s] && moves[i].dst_section && strcmp(sec_names[s], moves[i].dst_section) == 0) { dst_idx = s; break; }
        }
        if (dst_idx < 0) {
            sd_toklist_free(&needle);
            continue;
        }

        int dfirst = -1, dcnt = 0;
        sd_find_subseq_ci(&T_secs[dst_idx], &needle, &dfirst, &dcnt);
        if (dcnt != 1 || dfirst < 0) {
            sd_toklist_free(&needle);
            continue;
        }

        char *ctx_before = sd_build_span_ctx_seq_4_4(&T_b_goal, sfirst, needle.n, 1);
        char *ctx_after  = sd_build_span_ctx_seq_4_4(&T_secs[dst_idx], dfirst, needle.n, 1);

        if (ctx_before) {
            (void)structurize_fix_pair_insert_span_embedding(
                db, pair_id,
                SD_OP_MOVE_TOKENS, i,
                "GOAL",
                moves[i].dst_section,
                moves[i].anchor_hash,
                moves[i].payload_hash,
                moves[i].offset,
                0,
                ctx_before);
            free(ctx_before);
        }
        if (ctx_after) {
            (void)structurize_fix_pair_insert_span_embedding(
                db, pair_id,
                SD_OP_MOVE_TOKENS, i,
                "GOAL",
                moves[i].dst_section,
                moves[i].anchor_hash,
                moves[i].payload_hash,
                moves[i].offset,
                1,
                ctx_after);
            free(ctx_after);
        }

        sd_toklist_free(&needle);
    }

    // save INSERT spans: ctx_before(dest without payload) + ctx_after(dest with payload)
    for (int i = 0; i < in; ++i) {
        if (!ins[i].dst_section || !ins[i].tokseq_lower || !ins[i].tokseq_lower[0]) continue;

        SdTokList needle;
        if (!sd_parse_token_seq(ins[i].tokseq_lower, &needle) || needle.n <= 0) {
            sd_toklist_free(&needle);
            continue;
        }

        int dst_idx = -1;
        for (int s = 0; s < 8; ++s) {
            if (A_ins_names[s] && ins[i].dst_section && strcmp(A_ins_names[s], ins[i].dst_section) == 0) { dst_idx = s; break; }
        }
        if (dst_idx < 0) {
            sd_toklist_free(&needle);
            continue;
        }

        const SdTokList *DstL = A_ins_secs[dst_idx];
        if (!DstL) {
            sd_toklist_free(&needle);
            continue;
        }

        int dfirst = -1, dcnt = 0;
        sd_find_subseq_ci(DstL, &needle, &dfirst, &dcnt);
        if (dcnt != 1 || dfirst < 0) {
            sd_toklist_free(&needle);
            continue;
        }

        char *ctx_before = sd_build_span_ctx_seq_4_4(DstL, dfirst, needle.n, 0);
        char *ctx_after  = sd_build_span_ctx_seq_4_4(DstL, dfirst, needle.n, 1);

        if (ctx_before) {
            (void)structurize_fix_pair_insert_span_embedding(
                db, pair_id,
                SD_OP_INSERT_TOKENS, i,
                "",
                ins[i].dst_section,
                ins[i].anchor_hash,
                ins[i].payload_hash,
                ins[i].offset,
                0,
                ctx_before);
            free(ctx_before);
        }
        if (ctx_after) {
            (void)structurize_fix_pair_insert_span_embedding(
                db, pair_id,
                SD_OP_INSERT_TOKENS, i,
                "",
                ins[i].dst_section,
                ins[i].anchor_hash,
                ins[i].payload_hash,
                ins[i].offset,
                1,
                ctx_after);
            free(ctx_after);
        }

        sd_toklist_free(&needle);
    }

    // cleanup
    for (int i = 0; i < dn; ++i) free(deletes[i]);
    free(deletes);

    if (moves) {
        for (int i = 0; i < mn; ++i) free(moves[i].tokseq_lower);
        free(moves);
    }
    if (ins) {
        for (int i = 0; i < in; ++i) free(ins[i].tokseq_lower);
        free(ins);
    }

    sd_toklist_free(&T_b_goal);
    sd_toklist_free(&T_a_goal);
    for (int i = 0; i < 7; ++i) sd_toklist_free(&T_secs[i]);
    free(span_blk);
}

static void structurize_delta_mark_program(sqlite3 *db, sqlite3_int64 program_id, int success)
{
    if (!db || program_id <= 0) return;

    const char *sql =
        success
        ? "UPDATE struct_delta_programs SET successes=successes+1, updated_at=CURRENT_TIMESTAMP WHERE id=?;"
        : "UPDATE struct_delta_programs SET failures=failures+1, updated_at=CURRENT_TIMESTAMP WHERE id=?;";

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return;
    sqlite3_bind_int64(st, 1, program_id);
    (void)sqlite3_step(st);
    sqlite3_finalize(st);
}

static void sd_trunc(const char *in, char *out, size_t cap)
{
    if (!out || cap == 0) return;
    if (!in) in = "";
    const size_t in_len = strlen(in);
    if (in_len < cap) {
        memcpy(out, in, in_len + 1);
        return;
    }
    if (cap <= 4) { out[0] = '\0'; return; }
    memcpy(out, in, cap - 4);
    memcpy(out + (cap - 4), "...", 4);
}

int rag_structurize_delta_debug_dump_last(int max_ops)
{
    if (!g_db_structurize) {
        fprintf(stderr, "[SKYNET][STRUCTURIZE][DELTA][DUMP] DB not open.\n");
        return 0;
    }
    if (!structurize_ops_ensure_schema(g_db_structurize)) {
        fprintf(stderr, "[SKYNET][STRUCTURIZE][DELTA][DUMP] ensure_schema failed.\n");
        return 0;
    }

    if (max_ops <= 0) max_ops = 128;

    sqlite3_stmt *st = NULL;
    const char *sqlp =
        "SELECT id, pair_id, syntax_hint, ring_hint, embed_model, embed_dim, successes, failures, created_at, updated_at "
        "FROM struct_delta_programs ORDER BY id DESC LIMIT 1;";

    if (sqlite3_prepare_v2(g_db_structurize, sqlp, -1, &st, NULL) != SQLITE_OK) {
        fprintf(stderr, "[SKYNET][STRUCTURIZE][DELTA][DUMP] prepare failed.\n");
        return 0;
    }

    sqlite3_int64 program_id = 0;
    sqlite3_int64 pair_id = 0;

    if (sqlite3_step(st) == SQLITE_ROW) {
        program_id = sqlite3_column_int64(st, 0);
        pair_id    = sqlite3_column_int64(st, 1);
        const char *syn = (const char*)sqlite3_column_text(st, 2);
        int ring = sqlite3_column_int(st, 3);
        const char *m = (const char*)sqlite3_column_text(st, 4);
        int dim = sqlite3_column_int(st, 5);
        int succ = sqlite3_column_int(st, 6);
        int fail = sqlite3_column_int(st, 7);

        fprintf(stderr,
            "[SKYNET][STRUCTURIZE][DELTA][DUMP] last program=%lld pair=%lld scope=(%s,%d) model=%s dim=%d succ=%d fail=%d\n",
            (long long)program_id, (long long)pair_id,
            (syn ? syn : ""), ring,
            (m ? m : ""), dim, succ, fail);
    } else {
        sqlite3_finalize(st);
        fprintf(stderr, "[SKYNET][STRUCTURIZE][DELTA][DUMP] no programs.\n");
        return 0;
    }
    sqlite3_finalize(st);

    const char *sqlo =
        "SELECT op_order, op_type, src_section, dst_section, guard, flags, match_before, match_after "
        "FROM struct_delta_ops WHERE program_id=? ORDER BY op_order ASC LIMIT ?;";

    if (sqlite3_prepare_v2(g_db_structurize, sqlo, -1, &st, NULL) != SQLITE_OK) {
        fprintf(stderr, "[SKYNET][STRUCTURIZE][DELTA][DUMP] prepare ops failed.\n");
        return 0;
    }

    sqlite3_bind_int64(st, 1, program_id);
    sqlite3_bind_int(st, 2, max_ops);

    fprintf(stderr, "[SKYNET][STRUCTURIZE][DELTA][DUMP] ops:\n");
    while (sqlite3_step(st) == SQLITE_ROW) {
        int ord = sqlite3_column_int(st, 0);
        int typ = sqlite3_column_int(st, 1);
        const char *src = (const char*)sqlite3_column_text(st, 2);
        const char *dst = (const char*)sqlite3_column_text(st, 3);
        int guard = sqlite3_column_int(st, 4);
        int flags = sqlite3_column_int(st, 5);
        const char *mb = (const char*)sqlite3_column_text(st, 6);
        const char *ma = (const char*)sqlite3_column_text(st, 7);

        char mbuf[121], abuf[121];
        sd_trunc(mb ? mb : "", mbuf, sizeof(mbuf));
        sd_trunc(ma ? ma : "", abuf, sizeof(abuf));

        fprintf(stderr, "  #%d type=%d src=%s dst=%s guard=%d flags=%d before='%s' after='%s'\n",
            ord, typ,
            (src ? src : ""), (dst ? dst : ""),
            guard, flags, mbuf, abuf);
    }

    sqlite3_finalize(st);
    return 1;
}

static int structurize_delta_collect_candidates(
    sqlite3 *db,
    const char *embed_model,
    int embed_dim,
    const char *syntax_hint,
    int ring_hint,
    const float *qvec,
    int strict_scope,
    SdCand *out_cand,
    int out_cap)
{
    if (!db || !embed_model || embed_dim <= 0 || !qvec || !out_cand || out_cap <= 0) return 0;

    const char *sql_strict =
        "SELECT p.id, p.successes, p.failures, e.embedding "
        "FROM struct_delta_programs p "
        "JOIN struct_delta_examples e ON e.id=p.example_id "
        "WHERE p.embed_model=? AND p.embed_dim=? AND p.syntax_hint=? AND p.ring_hint=?;";

    const char *sql_relaxed =
        "SELECT p.id, p.successes, p.failures, e.embedding "
        "FROM struct_delta_programs p "
        "JOIN struct_delta_examples e ON e.id=p.example_id "
        "WHERE p.embed_model=? AND p.embed_dim=?;";

    const char *sql = strict_scope ? sql_strict : sql_relaxed;

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return 0;

    int b = 1;
    sqlite3_bind_text(st, b++, embed_model, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st,  b++, embed_dim);

    if (strict_scope) {
        sqlite3_bind_text(st, b++, (syntax_hint ? syntax_hint : ""), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st,  b++, ring_hint);
    }

    int out_n = 0;

    while (sqlite3_step(st) == SQLITE_ROW) {
        sqlite3_int64 pid = sqlite3_column_int64(st, 0);
        int succ = sqlite3_column_int(st, 1);
        int fail = sqlite3_column_int(st, 2);

        const void *blob = sqlite3_column_blob(st, 3);
        int bytes = sqlite3_column_bytes(st, 3);
        if (!blob || bytes != (int)(sizeof(float) * (size_t)embed_dim)) continue;

        const float *avec = (const float*)blob;

        float sim = structurize_delta_cosine_sim(qvec, avec, embed_dim);
        if (sim < SD_MIN_SIM) continue;

        float rel = (float)((double)(succ + 1) / (double)(succ + fail + 2));
        float score = sim + (SD_ALPHA * rel);

        SdCand c;
        c.program_id = pid;
        c.successes = succ;
        c.failures  = fail;
        c.sim = sim;
        c.rel = rel;
        c.score = score;

        if (out_n < out_cap) {
            int j = out_n;
            while (j > 0 && c.score > out_cand[j - 1].score) {
                out_cand[j] = out_cand[j - 1];
                j--;
            }
            out_cand[j] = c;
            out_n++;
        } else {
            if (c.score <= out_cand[out_cap - 1].score) continue;

            int j = out_cap - 1;
            while (j > 0 && c.score > out_cand[j - 1].score) {
                out_cand[j] = out_cand[j - 1];
                j--;
            }
            out_cand[j] = c;
        }
    }

    sqlite3_finalize(st);
    return out_n;
}

static int sd_collect_sentence_hashset_ci(const SdTokList *T, int start, int len,
                                         unsigned long long **out_arr, int *out_n);

static int sd_insert_supported_by_input_sentences(const SdTokList *needle,
                                                  unsigned long long **in_set,
                                                  const int *in_n,
                                                  int in_sn);

static int sd_collect_sentence_hashset_ci_fuzzy_no_placeholders(const SdTokList *T, int start, int len,
                                                                unsigned long long **out_arr, int *out_n);                                                  
static sqlite3_int64 structurize_delta_try_apply_programs(
    sqlite3 *db,
    const char *input_text,
    const char *syntax_hint,
    int ring_hint,
    char *io_struct,
    size_t io_sz,
    int do_log,
    SdSectionLocks *locks)
{
    if (!db || !io_struct || io_sz == 0) return 0;
    if (!input_text || !input_text[0]) return 0;

    // canonize before embeddings + fingerprint (Project S)
    char *canon_in = sd_canonize_alloc(input_text);
    if (!canon_in) canon_in = _strdup(input_text ? input_text : "");
    if (!canon_in) return 0;

    void *model_handle = NULL;
    const char *model_name = NULL;
    int dim = 0;
	if (!structurize_delta_select_embed_model(canon_in, &model_handle, &model_name, &dim) || !model_handle || dim <= 0) {
		if (structurize_debug_enabled()) {
			char pv[256];
			sd_trunc_str(canon_in, pv, sizeof(pv));
			fprintf(stderr,
				"[SKYNET][STRUCTURIZE][AUTO] embed_model unavailable (en=%s ru=%s) pv='%s'\n",
				g_llama_model_en ? "loaded" : "NULL",
				g_llama_model_ru ? "loaded" : "NULL",
				pv
			);
		}
		free(canon_in);
		return 0;
	}

    float *qvec = (float*)malloc(sizeof(float) * (size_t)dim);
    if (!qvec) {
        free(canon_in);
        return 0;
    }

	if (!llama_client_get_embeddings(model_handle, canon_in, qvec)) {
		if (structurize_debug_enabled()) {
			char pv[256];
			sd_trunc_str(canon_in, pv, sizeof(pv));
			fprintf(stderr,
				"[SKYNET][STRUCTURIZE][AUTO] llama_client_get_embeddings failed (model=%s dim=%d) pv='%s'\n",
				model_name ? model_name : "",
				dim,
				pv
			);
		}
		free(qvec);
		free(canon_in);
		return 0;
	}

    SdCand cand[SD_TOPK];
    int n = structurize_delta_collect_candidates(db, model_name, dim, syntax_hint, ring_hint, qvec, 1, cand, SD_TOPK);

    if (n > 0 && n < SD_TOPK) {
        SdCand extra[SD_TOPK];
        int en = structurize_delta_collect_candidates(db, model_name, dim, syntax_hint, ring_hint, qvec, 0, extra, SD_TOPK);

        for (int i = 0; i < en; ++i) {
            int dup = 0;
            for (int k = 0; k < n; ++k) {
                if (cand[k].program_id == extra[i].program_id) { dup = 1; break; }
            }
            if (dup) continue;

            if (n < SD_TOPK) {
                int j = n;
                while (j > 0 && extra[i].score > cand[j - 1].score) {
                    cand[j] = cand[j - 1];
                    j--;
                }
                cand[j] = extra[i];
                n++;
            } else {
                if (extra[i].score <= cand[SD_TOPK - 1].score) break;

                int j = SD_TOPK - 1;
                while (j > 0 && extra[i].score > cand[j - 1].score) {
                    cand[j] = cand[j - 1];
                    j--;
                }
                cand[j] = extra[i];
            }

            if (n >= SD_TOPK && i + 1 < en) {
                if (extra[i + 1].score <= cand[SD_TOPK - 1].score) break;
            }
        }
    }

	if (n <= 0) {
		if (structurize_debug_enabled()) {
			char pv[256];
			sd_trunc_str(canon_in, pv, sizeof(pv));
			fprintf(stderr,
				"[SKYNET][STRUCTURIZE][AUTO] no delta candidates (model=%s dim=%d syntax='%s' ring=%d) pv='%s'\n",
				model_name ? model_name : "",
				dim,
				(syntax_hint ? syntax_hint : ""),
				ring_hint,
				pv
			);
		}
		free(qvec);
		free(canon_in);
		return 0;
	}

    char fp[32];
    structurize_delta_hex64(struct_ml_hash_str(canon_in), fp);
	
	sqlite3_int64 apply_log_id = 0;
	if (do_log) {
		apply_log_id = structurize_apply_log_begin(db, fp, model_name, dim, syntax_hint, ring_hint);
	}
    int apply_log_ord = 0;

    // ---- group identical ops across candidates (average probability) ----
    typedef struct {
        int op_type;
        char src_section[32];
        char dst_section[32];
        char *match_before;
        char *match_after;
        int guard;
        int flags;
        double p_sum;
        int p_n;
        float best_sim;
        sqlite3_int64 best_prog;
    } SdOpGroup;

    SdOpGroup *G = NULL;
    int gn = 0, gc = 0;

    const char *sql_ops =
        "SELECT op_type,src_section,dst_section,match_before,match_after,guard,flags"
        " FROM struct_delta_ops WHERE program_id=? ORDER BY op_order ASC;";

    sqlite3_stmt *st_ops = NULL;
    if (sqlite3_prepare_v2(db, sql_ops, -1, &st_ops, NULL) != SQLITE_OK) {
        free(qvec);
        free(canon_in);
        return 0;
    }

    for (int i = 0; i < n; ++i) {
        float p = sd_prob_from_similarity(cand[i].sim);
        if (p <= 0.0f) continue;

        sqlite3_reset(st_ops);
        sqlite3_clear_bindings(st_ops);
        sqlite3_bind_int64(st_ops, 1, cand[i].program_id);

        while (sqlite3_step(st_ops) == SQLITE_ROW) {
            int op_type = sqlite3_column_int(st_ops, 0);
            const char *src = (const char*)sqlite3_column_text(st_ops, 1);
            const char *dst = (const char*)sqlite3_column_text(st_ops, 2);
            const char *mb  = (const char*)sqlite3_column_text(st_ops, 3);
            const char *ma  = (const char*)sqlite3_column_text(st_ops, 4);
            int guard = sqlite3_column_int(st_ops, 5);
            int flags = sqlite3_column_int(st_ops, 6);

			if (op_type != SD_OP_DELETE_TOKENS && op_type != SD_OP_MOVE_TOKENS && op_type != SD_OP_INSERT_TOKENS && op_type != SD_OP_CHANGE_WORD)
				continue;

            // guards: we currently enforce uniqueness for token-seq ops
            (void)guard;

            // find existing group
            int idx = -1;
            for (int k = 0; k < gn; ++k) {
                if (G[k].op_type != op_type) continue;
                if (strcmp(G[k].src_section, src ? src : "") != 0) continue;
                if (strcmp(G[k].dst_section, dst ? dst : "") != 0) continue;
                if (strcmp(G[k].match_before, mb ? mb : "") != 0) continue;
                if (strcmp(G[k].match_after,  ma ? ma : "") != 0) continue;
                if (G[k].flags != flags) continue;
                idx = k;
                break;
            }

            if (idx < 0) {
                if (gn + 1 > gc) {
                    int nc = (gc == 0) ? 16 : (gc * 2);
                    SdOpGroup *ngp = (SdOpGroup*)realloc(G, sizeof(SdOpGroup) * (size_t)nc);
                    if (!ngp) continue;
                    G = ngp;
                    gc = nc;
                }
                idx = gn++;
                memset(&G[idx], 0, sizeof(G[idx]));
                G[idx].op_type = op_type;
                sd_trunc_str(src ? src : "", G[idx].src_section, sizeof(G[idx].src_section));
                sd_trunc_str(dst ? dst : "", G[idx].dst_section, sizeof(G[idx].dst_section));
                G[idx].match_before = _strdup(mb ? mb : "");
                G[idx].match_after  = _strdup(ma ? ma : "");
                G[idx].flags = flags;
                G[idx].best_sim = -1.0f;
                G[idx].best_prog = 0;
                G[idx].p_sum = 0.0;
                G[idx].p_n = 0;
            }

            G[idx].p_sum += (double)p;
            G[idx].p_n += 1;
            if (cand[i].sim > G[idx].best_sim) {
                G[idx].best_sim = cand[i].sim;
                G[idx].best_prog = cand[i].program_id;
            }
        }
    }

    sqlite3_finalize(st_ops);
    free(qvec);

    if (gn <= 0) {
        free(canon_in);
        return 0;
    }

    // sort by priority then by similarity
    for (int i = 0; i < gn; ++i) {
        for (int j = i + 1; j < gn; ++j) {
            int pi = sd_priority_for_op(G[i].op_type);
            int pj = sd_priority_for_op(G[j].op_type);
            int swap = 0;
            if (pj < pi) swap = 1;
            else if (pj == pi && G[j].best_sim > G[i].best_sim) swap = 1;
            if (swap) {
                SdOpGroup tmp = G[i];
                G[i] = G[j];
                G[j] = tmp;
            }
        }
    }

    // tokenize current struct sections (heap to avoid stack overflow)
    const size_t sec_buf_sz = 65536;

    char *goal = (char*)calloc(1, sec_buf_sz);
    char *req  = (char*)calloc(1, sec_buf_sz);
    char *inp  = (char*)calloc(1, sec_buf_sz);
    char *outp = (char*)calloc(1, sec_buf_sz);
    char *cons = (char*)calloc(1, sec_buf_sz);
    char *env  = (char*)calloc(1, sec_buf_sz);
    char *edit = (char*)calloc(1, sec_buf_sz);
    char *ng   = (char*)calloc(1, sec_buf_sz);

    if (!goal || !req || !inp || !outp || !cons || !env || !edit || !ng) {
        free(goal); free(req); free(inp); free(outp); free(cons); free(env); free(edit); free(ng);
        for (int i = 0; i < gn; ++i) { free(G[i].match_before); free(G[i].match_after); }
        free(G);
        free(canon_in);
        return 0;
    }

    tr_copy_section_payload(io_struct, "GOAL:", goal, sec_buf_sz);
    tr_copy_section_payload(io_struct, "REQUIREMENTS:", req, sec_buf_sz);
    tr_copy_section_payload(io_struct, "INPUT:", inp, sec_buf_sz);
    tr_copy_section_payload(io_struct, "OUTPUT:", outp, sec_buf_sz);
    tr_copy_section_payload(io_struct, "CONSTRAINTS:", cons, sec_buf_sz);
    tr_copy_section_payload(io_struct, "ENVIRONMENT:", env, sec_buf_sz);
    tr_copy_section_payload(io_struct, "EDIT_PARAMETERS:", edit, sec_buf_sz);
    tr_copy_section_payload(io_struct, "NON_GOALS:", ng, sec_buf_sz);

    SdTokList T_goal, T_req, T_inp, T_outp, T_cons, T_env, T_edit, T_ng;
    if (!sd_tokenize_payload(goal, &T_goal) ||
        !sd_tokenize_payload(req,  &T_req)  ||
        !sd_tokenize_payload(inp,  &T_inp)  ||
        !sd_tokenize_payload(outp, &T_outp) ||
        !sd_tokenize_payload(cons, &T_cons) ||
        !sd_tokenize_payload(env,  &T_env)  ||
        !sd_tokenize_payload(edit, &T_edit) ||
        !sd_tokenize_payload(ng,   &T_ng))
    {
        sd_toklist_free(&T_goal); sd_toklist_free(&T_req); sd_toklist_free(&T_inp); sd_toklist_free(&T_outp);
        sd_toklist_free(&T_cons); sd_toklist_free(&T_env); sd_toklist_free(&T_edit); sd_toklist_free(&T_ng);

        free(goal); free(req); free(inp); free(outp); free(cons); free(env); free(edit); free(ng);

        for (int i = 0; i < gn; ++i) { free(G[i].match_before); free(G[i].match_after); }
        free(G);
        free(canon_in);
        return 0;
    }

    // section mapping by name
    struct SecRef { const char *name; SdTokList *L; };
    struct SecRef secs[] = {
        {"GOAL", &T_goal}, {"REQUIREMENTS", &T_req}, {"INPUT", &T_inp}, {"OUTPUT", &T_outp},
        {"CONSTRAINTS", &T_cons}, {"ENVIRONMENT", &T_env}, {"EDIT_PARAMETERS", &T_edit}, {"NON_GOALS", &T_ng}
    };
	
	/* Build sentence hashsets for current input_text (canon_in), for INSERT "fits current text" gate */
    SdTokList T_intext;
    sd_toklist_init(&T_intext);

    SdSpan *in_sp = NULL;
    int in_sn = 0;

    unsigned long long *in_set[64];
    int in_n[64];
    int in_max = 0;

    for (int i = 0; i < 64; ++i) { in_set[i] = NULL; in_n[i] = 0; }

    if (sd_tokenize_payload(canon_in, &T_intext)) {
        sd_collect_sentence_spans(&T_intext, &in_sp, &in_sn);
        if (in_sp && in_sn > 0) {
            in_max = (in_sn > 64) ? 64 : in_sn;
            for (int i = 0; i < in_max; ++i) {
                (void)sd_collect_sentence_hashset_ci_fuzzy_no_placeholders(&T_intext, in_sp[i].start, in_sp[i].len, &in_set[i], &in_n[i]);
            }
        }
    }
    
    int applied_any = 0;
	
	SdOccList local_occ[8];
	SdOccList *occ = local_occ;
	if (locks) occ = locks->occ;
	for (int si = 0; si < 8; ++si) sd_occ_init(&occ[si]);

	for (int i = 0; i < gn; ++i) {
		float p_avg = (G[i].p_n > 0) ? (float)(G[i].p_sum / (double)G[i].p_n) : 0.0f;

    char op_key[512];
    _snprintf_s(op_key, sizeof(op_key), _TRUNCATE,
                "%d|%s|%s|%s|%s|%d",
                G[i].op_type,
                G[i].src_section ? G[i].src_section : "",
                G[i].dst_section ? G[i].dst_section : "",
                G[i].match_before ? G[i].match_before : "",
                G[i].match_after ? G[i].match_after : "",
                G[i].flags);

    if (!sd_should_apply(fp, op_key, p_avg)) continue;

    SdTokList needle;
    sd_toklist_init(&needle);
    if (!sd_parse_token_seq(G[i].match_before, &needle)) {
        sd_toklist_free(&needle);
        continue;
    }

	SdTokList *srcL = NULL;
	SdTokList *dstL = NULL;
	int src_si = -1;
	int dst_si = -1;

	for (int si = 0; si < (int)(sizeof(secs) / sizeof(secs[0])); ++si) {
		if (G[i].src_section && strcmp(secs[si].name, G[i].src_section) == 0) { srcL = secs[si].L; src_si = si; }
		if (G[i].dst_section && strcmp(secs[si].name, G[i].dst_section) == 0) { dstL = secs[si].L; dst_si = si; }
	}

    if ((G[i].op_type == SD_OP_DELETE_TOKENS) || (G[i].op_type == SD_OP_CHANGE_WORD)) {
        if (!srcL) { sd_toklist_free(&needle); continue; }
    } else if (G[i].op_type == SD_OP_MOVE_TOKENS) {
        if (!srcL || !dstL) { sd_toklist_free(&needle); continue; }
    } else if (G[i].op_type == SD_OP_INSERT_TOKENS) {
        if (!dstL) { sd_toklist_free(&needle); continue; }
    }
	
	/* Guard: APPLY-дельты не должны “портить” EDIT_PARAMETERS.
	   Значения обновляет value-transformer (иначе остаются старые 200/200/1000 из прошлой задачи). */
	if ((G[i].src_section && strcmp(G[i].src_section, "EDIT_PARAMETERS") == 0) ||
		(G[i].dst_section && strcmp(G[i].dst_section, "EDIT_PARAMETERS") == 0))
	{
		sd_toklist_free(&needle);
		continue;
	}
	
	/* Guard: APPLY word-ops must not bring literal values from past tasks into other sections.
	Values must come from current input via value-transformer/prefill. */
	if (G[i].op_type == SD_OP_INSERT_TOKENS && sd_toklist_contains_value_literal(&needle)) {
		sd_toklist_free(&needle);
		continue;
	}
	
	/* v2.2 APPLY rule (p.95): INSERT must fit current input_text, otherwise skip silently */
	if (G[i].op_type == SD_OP_INSERT_TOKENS &&
    G[i].dst_section && strcmp(G[i].dst_section, "INPUT") == 0) {
		const char *ds = G[i].dst_section ? G[i].dst_section : "";
		const int skip_gate =
			(strcmp(ds, "ENVIRONMENT") == 0) ||
			(strcmp(ds, "HARDWARE") == 0) ||
			(strcmp(ds, "NETWORK") == 0) ||
			(strcmp(ds, "DOMAIN") == 0);

		if (!skip_gate) {
			if (!sd_insert_supported_by_input_sentences(&needle, in_set, in_n, in_max)) {
				if (structurize_debug_enabled()) {
					fprintf(stderr, "[SKYNET][STRUCTURIZE][APPLY][GATE] skip INSERT (not supported by current input) op_key=%s\n", op_key);
				}
				sd_toklist_free(&needle);
				continue;
			}
		}
	}
    
	if (G[i].op_type == SD_OP_CHANGE_WORD && G[i].match_after && sd_tok_is_value_literal_token(G[i].match_after)) {
		sd_toklist_free(&needle);
		continue;
	}

    /* penalty-check: считаем fingerprint той секции, где оп реально “работает” */
    SdTokList *penL = NULL;
    const char *pen_section = NULL;

	char pen_section_fp[32];
    pen_section_fp[0] = '\0';

    if (G[i].op_type == SD_OP_DELETE_TOKENS || G[i].op_type == SD_OP_CHANGE_WORD) {
        penL = srcL;
        pen_section = G[i].src_section;
    } else {
        penL = dstL;
        pen_section = G[i].dst_section;
    }

    if (penL && pen_section && pen_section[0]) {
        structurize_delta_hex64(sd_hash_toklist_ci(penL), pen_section_fp);

        if (structurize_penalty_is_disabled(db, pen_section_fp, pen_section, op_key)) {
            sd_toklist_free(&needle);
            continue;
        }
    }

    int applied_this = 0;

	if (G[i].op_type == SD_OP_DELETE_TOKENS) {
		int first = -1, cnt = 0;
		sd_find_subseq_ci(srcL, &needle, &first, &cnt);
		if (cnt == 1 && first >= 0) {
			const int s = first;
			const int e = first + needle.n;

			if (src_si >= 0 && sd_occ_overlap(&occ[src_si], s, e)) {
				fprintf(stderr, "[SKYNET][STRUCTURIZE][APPLY][CONFLICT] skip DELETE op_key=%s section=%s span=[%d,%d)\n",
						op_key, secs[src_si].name, s, e);
			} else {
				/* Guard (v2.2 semantics): if this DELETE is a cleanup for a MOVE from GOAL,
				   do not delete unless the same token-seq already exists in some other section.
				   This prevents losing required content when MOVE was skipped due to conflicts. */
				int is_cleanup = 0;
				if (G[i].src_section && strcmp(G[i].src_section, "GOAL") == 0) {
					for (int j = 0; j < gn; ++j) {
						if (G[j].op_type != SD_OP_MOVE_TOKENS) continue;
						if (!G[j].src_section || strcmp(G[j].src_section, "GOAL") != 0) continue;
						if (!G[j].match_before || !G[i].match_before) continue;
						if (strcmp(G[j].match_before, G[i].match_before) == 0) { is_cleanup = 1; break; }
					}
				}

				if (is_cleanup) {
					int found_elsewhere = 0;
					for (int sj = 0; sj < (int)(sizeof(secs) / sizeof(secs[0])); ++sj) {
						if (!secs[sj].L || secs[sj].L == srcL) continue;
						int f2 = -1, c2 = 0;
						sd_find_subseq_ci(secs[sj].L, &needle, &f2, &c2);
						if (c2 > 0) { found_elsewhere = 1; break; }
					}

					if (!found_elsewhere) {
						if (structurize_debug_enabled()) {
							fprintf(stderr, "[SKYNET][STRUCTURIZE][APPLY][GUARD] skip cleanup DELETE (MOVE not materialized) op_key=%s\n", op_key);
						}
						/* Do not delete; keep content in GOAL to avoid loss. */
					} else {
						sd_toklist_remove_span_free(srcL, first, needle.n);
						if (src_si >= 0) sd_occ_delete_adjust(&occ[src_si], s, e);
						applied_this = 1;
					}
				} else {
					/* non-cleanup DELETE (semantic deletion): keep legacy behavior */
					sd_toklist_remove_span_free(srcL, first, needle.n);
					if (src_si >= 0) sd_occ_delete_adjust(&occ[src_si], s, e);
					applied_this = 1;
				}
			}
		}
	}
    else if (G[i].op_type == SD_OP_MOVE_TOKENS) {
        unsigned long long a_hash = 0, p_hash = 0;
        int off = 0;

        if (!sd_move_meta_parse(G[i].match_after, &a_hash, &off, &p_hash)) {
            sd_toklist_free(&needle);
            continue;
        }

        int first = -1, cnt = 0;
        sd_find_subseq_ci(srcL, &needle, &first, &cnt);
        if (cnt == 1 && first >= 0) {
			const int s = first;
			const int e = first + needle.n;

			if (src_si >= 0 && sd_occ_overlap(&occ[src_si], s, e)) {
				fprintf(stderr, "[SKYNET][STRUCTURIZE][APPLY][CONFLICT] skip MOVE(src) op_key=%s section=%s span=[%d,%d)\n",
						op_key, secs[src_si].name, s, e);
				sd_toklist_free(&needle);
				continue;
			}
            SdTokList span;
            sd_toklist_init(&span);

            if (sd_toklist_splice_out(srcL, first, needle.n, &span)) {
                int idx = sd_find_anchor_insert_index(dstL, a_hash, off);
					const int ins_s = idx;
					const int ins_e = idx + needle.n;

					if (dst_si >= 0 && sd_occ_overlap(&occ[dst_si], ins_s, ins_e)) {
						fprintf(stderr, "[SKYNET][STRUCTURIZE][APPLY][CONFLICT] skip MOVE(dst) op_key=%s section=%s span=[%d,%d)\n",
								op_key, secs[dst_si].name, ins_s, ins_e);

						/* rollback: вернём обратно */
						sd_toklist_insert_owned(srcL, first, &span);
						sd_toklist_free(&needle);
						continue;
					}
                if (sd_toklist_insert_owned(dstL, idx, &span)) {
                    applied_this = 1;
                    /* update occupied spans (source removal + destination insertion) */
					if (src_si >= 0) sd_occ_delete_adjust(&occ[src_si], s, e);

					if (dst_si >= 0) {
						sd_occ_shift_from(&occ[dst_si], idx, needle.n);
						sd_occ_add(&occ[dst_si], idx, idx + needle.n);
					}
                } else {
                    /* rollback: вернём обратно */
                    sd_toklist_insert_owned(srcL, first, &span);
                }
            } else {
                sd_toklist_free(&span);
            }
        }
    }
    else if (G[i].op_type == SD_OP_INSERT_TOKENS) {
        int idx = dstL->n;

        /* если meta есть — пытаемся вставлять около якоря, иначе просто в конец */
        if (G[i].match_after && G[i].match_after[0]) {
            unsigned long long a_hash = 0, p_hash = 0;
            int off = 0;

            if (!sd_move_meta_parse(G[i].match_after, &a_hash, &off, &p_hash)) {
                sd_toklist_free(&needle);
                continue;
            }

            idx = sd_find_anchor_insert_index(dstL, a_hash, off);
        }

        SdTokList span;
        sd_toklist_init(&span);
        
		int already_first = -1, already_cnt = 0;
        sd_find_subseq_ci(dstL, &needle, &already_first, &already_cnt);
        (void)already_first;

        for (int k = 0; k < needle.n; ++k) {
            if (!sd_toklist_push_copy(&span, needle.t[k])) {
                sd_toklist_free(&span);
                span.n = 0;
                break;
            }
        }

        if (span.n == needle.n && already_cnt == 0) {
			{
				const int ins_s = idx;
				const int ins_e = idx + needle.n;

				if (dst_si >= 0 && sd_occ_overlap(&occ[dst_si], ins_s, ins_e)) {
					fprintf(stderr, "[SKYNET][STRUCTURIZE][APPLY][CONFLICT] skip INSERT op_key=%s section=%s span=[%d,%d)\n",
							op_key, secs[dst_si].name, ins_s, ins_e);
					sd_toklist_free(&span);
				} else {
					if (sd_toklist_insert_owned(dstL, idx, &span)) {
						if (dst_si >= 0) {
							sd_occ_shift_from(&occ[dst_si], idx, needle.n);
							sd_occ_add(&occ[dst_si], idx, idx + needle.n);
						}
						applied_this = 1;
					} else {
						sd_toklist_free(&span);
					}
				}
			}
        } else {
            sd_toklist_free(&span);
        }
    }
    else if (G[i].op_type == SD_OP_CHANGE_WORD) {
        if (needle.n > 0 && G[i].match_after && G[i].match_after[0]) {
            int first = -1, cnt = 0;
            sd_find_subseq_ci(srcL, &needle, &first, &cnt);

            if (cnt == 1 && first >= 0) {
				const int s = first;
				const int e = first + needle.n;

				if (src_si >= 0 && sd_occ_overlap(&occ[src_si], s, e)) {
					fprintf(stderr, "[SKYNET][STRUCTURIZE][APPLY][CONFLICT] skip CHANGE_WORD op_key=%s section=%s span=[%d,%d)\n",
							op_key, secs[src_si].name, s, e);
					sd_toklist_free(&needle);
					continue;
				}
                int center = (G[i].flags & SD_FLAG_CHG_CENTER_MASK) >> SD_FLAG_CHG_CENTER_SHIFT;
                if (center < 0 || center >= needle.n) center = needle.n / 2;

                int pos = first + center;
                if (pos >= 0 && pos < srcL->n) {
                    free(srcL->t[pos]);
                    srcL->t[pos] = _strdup(G[i].match_after);
                    if (srcL->t[pos]) applied_this = 1;
                    if (src_si >= 0) sd_occ_add(&occ[src_si], s, e);
                }
            }
        }
    }
	if (applied_this && apply_log_id > 0) {
        structurize_apply_log_add_op(
            db,
            apply_log_id,
            apply_log_ord++,
            pen_section ? pen_section : "",
            pen_section_fp,
            op_key,
            G[i].op_type,
            G[i].match_before ? G[i].match_before : "",
            G[i].match_after  ? G[i].match_after  : "",
            G[i].dst_section  ? G[i].dst_section  : "",
            ""
        );
    }

    if (applied_this) applied_any = 1;

    sd_toklist_free(&needle);
}
    // rebuild + validate
    sqlite3_int64 applied_prog = 0;

    if (applied_any) {
        const size_t sec_buf_sz = 65536;
        const size_t rebuilt_sz = 65536 * 2;

        char *new_goal = (char*)calloc(1, sec_buf_sz);
        char *new_req  = (char*)calloc(1, sec_buf_sz);
        char *new_inp  = (char*)calloc(1, sec_buf_sz);
        char *new_outp = (char*)calloc(1, sec_buf_sz);
        char *new_cons = (char*)calloc(1, sec_buf_sz);
        char *new_env  = (char*)calloc(1, sec_buf_sz);
        char *new_edit = (char*)calloc(1, sec_buf_sz);
        char *new_ng   = (char*)calloc(1, sec_buf_sz);
        char *rebuilt  = (char*)calloc(1, rebuilt_sz);

        if (!new_goal || !new_req || !new_inp || !new_outp || !new_cons || !new_env || !new_edit || !new_ng || !rebuilt) {
            free(new_goal); free(new_req); free(new_inp); free(new_outp); free(new_cons); free(new_env); free(new_edit); free(new_ng); free(rebuilt);
            applied_prog = 0;
        } else {
            sd_toklist_to_payload(new_goal, sec_buf_sz, &T_goal);
            sd_toklist_to_payload(new_req,  sec_buf_sz, &T_req);
            sd_toklist_to_payload(new_inp,  sec_buf_sz, &T_inp);
            sd_toklist_to_payload(new_outp, sec_buf_sz, &T_outp);
            sd_toklist_to_payload(new_cons, sec_buf_sz, &T_cons);
            sd_toklist_to_payload(new_env,  sec_buf_sz, &T_env);
            sd_toklist_to_payload(new_edit, sec_buf_sz, &T_edit);
            sd_toklist_to_payload(new_ng,   sec_buf_sz, &T_ng);

            tt_rebuild_struct(rebuilt, rebuilt_sz, new_goal, new_req, new_inp, new_outp, new_cons, new_env, new_edit, new_ng);

            if (hybrid_validate_clarified_output(rebuilt)) {
                sd_trunc_str(rebuilt, io_struct, io_sz);
                // mark success for best programs of groups that contributed
                for (int i = 0; i < gn; ++i) {
                    if (G[i].best_prog > 0) structurize_delta_mark_program(db, G[i].best_prog, 1);
                    if (G[i].best_prog > 0 && applied_prog == 0) applied_prog = G[i].best_prog;
                }
            } else {
                // mark failure for the most similar candidate
                if (cand[0].program_id > 0) structurize_delta_mark_program(db, cand[0].program_id, 0);
                applied_prog = 0;
            }

            free(new_goal); free(new_req); free(new_inp); free(new_outp); free(new_cons); free(new_env); free(new_edit); free(new_ng); free(rebuilt);
        }
    }
	free(goal); free(req); free(inp); free(outp); free(cons); free(env); free(edit); free(ng);
    sd_toklist_free(&T_goal); sd_toklist_free(&T_req); sd_toklist_free(&T_inp); sd_toklist_free(&T_outp);
    sd_toklist_free(&T_cons); sd_toklist_free(&T_env); sd_toklist_free(&T_edit); sd_toklist_free(&T_ng);
	
	    for (int i = 0; i < in_max; ++i) {
			if (in_set[i]) free(in_set[i]);
			in_set[i] = NULL;
			in_n[i] = 0;
		}
		if (in_sp) free(in_sp);
		in_sp = NULL;
		sd_toklist_free(&T_intext);
		
    for (int i = 0; i < gn; ++i) { free(G[i].match_before); free(G[i].match_after); }
    free(G);
    free(canon_in);

    return applied_prog;
}

static int structurize_word_rule_upsert(sqlite3 *db,
                                       const char *src_word,
                                       const char *dst_word,
                                       const char *section,
                                       int lev_dist,
                                       const char *embed_model,
                                       int embed_dim,
                                       const char *ctx_seq,
                                       const float *vec);

static sqlite3_int64 structurize_delta_learn_program_from_pair(
    sqlite3 *db,
    sqlite3_int64 pair_id,
    const char *input_text,
    const char *syntax_hint,
    int ring_hint,
    const char *before_struct,
    const char *after_struct,
    int *out_ops_count)
{
    if (out_ops_count) *out_ops_count = 0;
    if (!db || pair_id <= 0) return 0;
    if (!input_text) input_text = "";
    if (!syntax_hint) syntax_hint = "";
    if (!before_struct) before_struct = "";
    if (!after_struct) after_struct = "";

    char *canon_in = sd_canonize_alloc(input_text);
    if (!canon_in) canon_in = _strdup(input_text);
    if (!canon_in) return 0;

    void *model_handle = NULL;
    const char *model_name = NULL;
    int dim = 0;

    if (!structurize_delta_select_embed_model(canon_in, &model_handle, &model_name, &dim) || !model_handle || dim <= 0) {
        free(canon_in);
        return 0;
    }

    float *vec = (float*)malloc(sizeof(float) * (size_t)dim);
    if (!vec) {
        free(canon_in);
        return 0;
    }

    if (!llama_client_get_embeddings(model_handle, canon_in, vec)) {
        free(vec);
        free(canon_in);
        return 0;
    }

    char fp[32];
    structurize_delta_hex64(struct_ml_hash_str(canon_in), fp);

    /* tokenize GOAL before/after */
    char b_goal[65536] = {0};
    char a_goal[65536] = {0};
    tr_copy_section_payload(before_struct, "GOAL:", b_goal, sizeof(b_goal));
    tr_copy_section_payload(after_struct,  "GOAL:", a_goal, sizeof(a_goal));

    SdTokList T_b_goal, T_a_goal;
    if (!sd_tokenize_payload(b_goal, &T_b_goal) || !sd_tokenize_payload(a_goal, &T_a_goal)) {
        sd_toklist_free(&T_b_goal);
        sd_toklist_free(&T_a_goal);
        free(vec);
        free(canon_in);
        return 0;
    }

    /* tokenize AFTER target sections (the destinations) */
    char a_req[65536] = {0}, a_inp[65536] = {0}, a_outp[65536] = {0}, a_cons[65536] = {0}, a_env[65536] = {0}, a_edit[65536] = {0}, a_ng[65536] = {0};

    tr_copy_section_payload(after_struct, "REQUIREMENTS:", a_req, sizeof(a_req));
    tr_copy_section_payload(after_struct, "INPUT:",        a_inp, sizeof(a_inp));
    tr_copy_section_payload(after_struct, "OUTPUT:",       a_outp, sizeof(a_outp));
    tr_copy_section_payload(after_struct, "CONSTRAINTS:",  a_cons, sizeof(a_cons));
    tr_copy_section_payload(after_struct, "ENVIRONMENT:",  a_env, sizeof(a_env));
    tr_copy_section_payload(after_struct, "EDIT_PARAMETERS:", a_edit, sizeof(a_edit));
    tr_copy_section_payload(after_struct, "NON_GOALS:",    a_ng, sizeof(a_ng));

    SdTokList T_secs[7];
    const char *sec_names[7] = {
        "REQUIREMENTS",
        "INPUT",
        "OUTPUT",
        "CONSTRAINTS",
        "ENVIRONMENT",
        "EDIT_PARAMETERS",
        "NON_GOALS"
    };

    for (int i = 0; i < 7; ++i) sd_toklist_init(&T_secs[i]);

    if (!sd_tokenize_payload(a_req,  &T_secs[0]) ||
        !sd_tokenize_payload(a_inp,  &T_secs[1]) ||
        !sd_tokenize_payload(a_outp, &T_secs[2]) ||
        !sd_tokenize_payload(a_cons, &T_secs[3]) ||
        !sd_tokenize_payload(a_env,  &T_secs[4]) ||
        !sd_tokenize_payload(a_edit, &T_secs[5]) ||
        !sd_tokenize_payload(a_ng,   &T_secs[6]))
    {
        sd_toklist_free(&T_b_goal);
        sd_toklist_free(&T_a_goal);
        for (int i = 0; i < 7; ++i) sd_toklist_free(&T_secs[i]);
        free(vec);
        free(canon_in);
        return 0;
    }

    /* derive MOVE/DELETE from (before GOAL) -> (after destinations) */
    SdMove *moves = NULL; int mn = 0;
    char **deletes = NULL; int dn = 0;

    if (!sd_derive_moves_deletes(&T_b_goal, &T_a_goal, T_secs, sec_names, 7, &moves, &mn, &deletes, &dn)) {
        sd_toklist_free(&T_b_goal);
        sd_toklist_free(&T_a_goal);
        for (int i = 0; i < 7; ++i) sd_toklist_free(&T_secs[i]);
        free(vec);
        free(canon_in);
        return 0;
    }

    /* derive CHANGE_WORD from GOAL before/after (context+Levenshtein) */
    SdChangeWord *chg = NULL; int cn = 0;
    (void)sd_derive_change_words_goal(&T_b_goal, &T_a_goal, &chg, &cn);
	
	/* Step 2.3: Learn word-change context rules (src->dst + ctx(4+4) embedding) */
	SdWordCtxRule *wcr = NULL; int wn = 0;
	(void)sd_derive_word_ctx_rules_goal(&T_b_goal, &T_a_goal, &wcr, &wn);

	for (int wi = 0; wi < wn; ++wi) {
		if (!wcr[wi].src_word || !wcr[wi].dst_word || !wcr[wi].ctx_seq) continue;

		int lev = sd_levenshtein_bounded_ci(wcr[wi].src_word, wcr[wi].dst_word, 32);
		if (lev < 0) lev = 999;

		void *ctx_model = NULL;
		const char *ctx_model_name = NULL;
		int ctx_dim = 0;

		if (!structurize_delta_select_embed_model(wcr[wi].ctx_seq, &ctx_model, &ctx_model_name, &ctx_dim) ||
			!ctx_model || !ctx_model_name || ctx_dim <= 0) {
			continue;
		}

		float *wvec = (float*)malloc(sizeof(float) * ctx_dim);
		if (!wvec) continue;

		llama_client_get_embeddings(ctx_model, wcr[wi].ctx_seq, wvec);

		(void)structurize_word_rule_upsert(db,
										  wcr[wi].src_word,
										  wcr[wi].dst_word,
										  "GOAL",
										  lev,
										  ctx_model_name,
										  ctx_dim,
										  wcr[wi].ctx_seq,
										  wvec);

		free(wvec);
	}

	if (wcr) {
		for (int wi = 0; wi < wn; ++wi) sd_wordctxrule_free(&wcr[wi]);
		free(wcr);
	}
	
	/* Step 2.3b: Learn word-change context rules for other sections (before->after) */
	char b_req[65536] = {0}, b_inp[65536] = {0}, b_outp[65536] = {0}, b_cons[65536] = {0}, b_env[65536] = {0}, b_edit[65536] = {0}, b_ng[65536] = {0};

	tr_copy_section_payload(before_struct, "REQUIREMENTS:",     b_req,  sizeof(b_req));
	tr_copy_section_payload(before_struct, "INPUT:",            b_inp,  sizeof(b_inp));
	tr_copy_section_payload(before_struct, "OUTPUT:",           b_outp, sizeof(b_outp));
	tr_copy_section_payload(before_struct, "CONSTRAINTS:",      b_cons, sizeof(b_cons));
	tr_copy_section_payload(before_struct, "ENVIRONMENT:",      b_env,  sizeof(b_env));
	tr_copy_section_payload(before_struct, "EDIT_PARAMETERS:",  b_edit, sizeof(b_edit));
	tr_copy_section_payload(before_struct, "NON_GOALS:",        b_ng,   sizeof(b_ng));

	SdTokList T_b_secs[7];
	for (int si = 0; si < 7; ++si) sd_toklist_init(&T_b_secs[si]);

	(void)sd_tokenize_payload(b_req,  &T_b_secs[0]);
	(void)sd_tokenize_payload(b_inp,  &T_b_secs[1]);
	(void)sd_tokenize_payload(b_outp, &T_b_secs[2]);
	(void)sd_tokenize_payload(b_cons, &T_b_secs[3]);
	(void)sd_tokenize_payload(b_env,  &T_b_secs[4]);
	(void)sd_tokenize_payload(b_edit, &T_b_secs[5]);
	(void)sd_tokenize_payload(b_ng,   &T_b_secs[6]);

	for (int si = 0; si < 7; ++si) {
			if (_stricmp(sec_names[si], "ENVIRONMENT") == 0 || _stricmp(sec_names[si], "EDIT_PARAMETERS") == 0) {
				continue;
			}
		SdWordCtxRule *swcr = NULL;
		int swn = 0;

		(void)sd_derive_word_ctx_rules_section(&T_b_secs[si], &T_secs[si], &swcr, &swn);

		for (int wi = 0; wi < swn; ++wi) {
			if (!swcr[wi].src_word || !swcr[wi].dst_word || !swcr[wi].ctx_seq) continue;

			int lev = sd_levenshtein_bounded_ci(swcr[wi].src_word, swcr[wi].dst_word, 32);
			if (lev < 0) lev = 999;

			void *ctx_model = NULL;
			const char *ctx_model_name = NULL;
			int ctx_dim = 0;

			if (!structurize_delta_select_embed_model(swcr[wi].ctx_seq, &ctx_model, &ctx_model_name, &ctx_dim) ||
				!ctx_model || !ctx_model_name || ctx_dim <= 0) {
				continue;
			}

			float *wvec = (float*)malloc(sizeof(float) * ctx_dim);
			if (!wvec) continue;

			llama_client_get_embeddings(ctx_model, swcr[wi].ctx_seq, wvec);

			(void)structurize_word_rule_upsert(db,
											   swcr[wi].src_word,
											   swcr[wi].dst_word,
											   sec_names[si],
											   lev,
											   ctx_model_name,
											   ctx_dim,
											   swcr[wi].ctx_seq,
											   wvec);

			free(wvec);
		}

		if (swcr) {
			for (int wi = 0; wi < swn; ++wi) sd_wordctxrule_free(&swcr[wi]);
			free(swcr);
		}
	}

	for (int si = 0; si < 7; ++si) sd_toklist_free(&T_b_secs[si]);

	SdInsert *ins = NULL; int in = 0;
	const SdTokList *A_ins_secs[8] = { &T_a_goal, &T_secs[0], &T_secs[1], &T_secs[2], &T_secs[3], &T_secs[4], &T_secs[5], &T_secs[6] };
	const char *A_ins_names[8] = { "GOAL", "REQUIREMENTS", "INPUT", "OUTPUT", "CONSTRAINTS", "ENVIRONMENT", "EDIT_PARAMETERS", "NON_GOALS" };
	(void)sd_derive_inserts(&T_b_goal, A_ins_secs, A_ins_names, 8, &ins, &in);

    if (mn <= 0 && dn <= 0 && cn <= 0 && in <= 0) {
        sd_toklist_free(&T_b_goal);
        sd_toklist_free(&T_a_goal);
        for (int i = 0; i < 7; ++i) sd_toklist_free(&T_secs[i]);
        for (int i = 0; i < dn; ++i) free(deletes[i]);
        free(deletes);
        if (moves) {
            for (int i = 0; i < mn; ++i) free(moves[i].tokseq_lower);
            free(moves);
        }
        if (chg) {
            for (int i = 0; i < cn; ++i) sd_changeword_free(&chg[i]);
            free(chg);
        }
        if (ins) {
			for (int i = 0; i < in; ++i) free(ins[i].tokseq_lower);
			free(ins);
		}
        free(vec);
        free(canon_in);
        return 0;
    }

    /* insert example */
    const char *sql_ex =
        "INSERT INTO struct_delta_examples(pair_id,input_fingerprint,input_text,syntax_hint,ring_hint,embed_model,embed_dim,embedding)"
        " VALUES(?,?,?,?,?,?,?,?);";

    sqlite3_stmt *st_ex = NULL;
    if (sqlite3_prepare_v2(db, sql_ex, -1, &st_ex, NULL) != SQLITE_OK) {
        sd_toklist_free(&T_b_goal);
        sd_toklist_free(&T_a_goal);
        for (int i = 0; i < 7; ++i) sd_toklist_free(&T_secs[i]);
        for (int i = 0; i < dn; ++i) free(deletes[i]);
        free(deletes);
        if (moves) {
            for (int i = 0; i < mn; ++i) free(moves[i].tokseq_lower);
            free(moves);
        }
        if (chg) {
            for (int i = 0; i < cn; ++i) sd_changeword_free(&chg[i]);
            free(chg);
        }
        if (ins) {
			for (int i = 0; i < in; ++i) free(ins[i].tokseq_lower);
			free(ins);
		}
        free(vec);
        free(canon_in);
        return 0;
    }

    sqlite3_bind_int64(st_ex, 1, pair_id);
    sqlite3_bind_text(st_ex,  2, fp, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st_ex,  3, input_text, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st_ex,  4, syntax_hint, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st_ex,   5, ring_hint);
    sqlite3_bind_text(st_ex,  6, model_name ? model_name : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st_ex,   7, dim);
    sqlite3_bind_blob(st_ex,  8, vec, (int)(sizeof(float) * (size_t)dim), SQLITE_TRANSIENT);

    if (sqlite3_step(st_ex) != SQLITE_DONE) {
        sqlite3_finalize(st_ex);
        sd_toklist_free(&T_b_goal);
        sd_toklist_free(&T_a_goal);
        for (int i = 0; i < 7; ++i) sd_toklist_free(&T_secs[i]);
        for (int i = 0; i < dn; ++i) free(deletes[i]);
        free(deletes);
        if (moves) {
            for (int i = 0; i < mn; ++i) free(moves[i].tokseq_lower);
            free(moves);
        }
        if (chg) {
            for (int i = 0; i < cn; ++i) sd_changeword_free(&chg[i]);
            free(chg);
        }
        if (ins) {
			for (int i = 0; i < in; ++i) free(ins[i].tokseq_lower);
			free(ins);
		}
        free(vec);
        free(canon_in);
        return 0;
    }
    sqlite3_finalize(st_ex);

    const sqlite3_int64 example_id = sqlite3_last_insert_rowid(db);

    /* insert program */
    const char *sql_pr =
        "INSERT INTO struct_delta_programs(pair_id,example_id,syntax_hint,ring_hint,embed_model,embed_dim)"
        " VALUES(?,?,?,?,?,?);";

    sqlite3_stmt *st_pr = NULL;
    if (sqlite3_prepare_v2(db, sql_pr, -1, &st_pr, NULL) != SQLITE_OK) {
        sd_toklist_free(&T_b_goal);
        sd_toklist_free(&T_a_goal);
        for (int i = 0; i < 7; ++i) sd_toklist_free(&T_secs[i]);
        for (int i = 0; i < dn; ++i) free(deletes[i]);
        free(deletes);
        if (moves) {
            for (int i = 0; i < mn; ++i) free(moves[i].tokseq_lower);
            free(moves);
        }
        if (chg) {
            for (int i = 0; i < cn; ++i) sd_changeword_free(&chg[i]);
            free(chg);
        }
        if (ins) {
			for (int i = 0; i < in; ++i) free(ins[i].tokseq_lower);
			free(ins);
		}
        free(vec);
        free(canon_in);
        return 0;
    }

    sqlite3_bind_int64(st_pr, 1, pair_id);
    sqlite3_bind_int64(st_pr, 2, example_id);
    sqlite3_bind_text(st_pr,  3, syntax_hint, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st_pr,   4, ring_hint);
    sqlite3_bind_text(st_pr,  5, model_name ? model_name : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st_pr,   6, dim);

    if (sqlite3_step(st_pr) != SQLITE_DONE) {
        sqlite3_finalize(st_pr);
        sd_toklist_free(&T_b_goal);
        sd_toklist_free(&T_a_goal);
        for (int i = 0; i < 7; ++i) sd_toklist_free(&T_secs[i]);
        for (int i = 0; i < dn; ++i) free(deletes[i]);
        free(deletes);
        if (moves) {
            for (int i = 0; i < mn; ++i) free(moves[i].tokseq_lower);
            free(moves);
        }
        if (chg) {
            for (int i = 0; i < cn; ++i) sd_changeword_free(&chg[i]);
            free(chg);
        }
        if (ins) {
			for (int i = 0; i < in; ++i) free(ins[i].tokseq_lower);
			free(ins);
		}
        free(vec);
        free(canon_in);
        return 0;
    }
    sqlite3_finalize(st_pr);

    const sqlite3_int64 program_id = sqlite3_last_insert_rowid(db);

    /* insert ops */
    const char *sql_op =
        "INSERT INTO struct_delta_ops(program_id,op_order,op_type,src_section,dst_section,match_before,match_after,guard,flags)"
        " VALUES(?,?,?,?,?,?,?,?,?);";

    sqlite3_stmt *st_op = NULL;
    if (sqlite3_prepare_v2(db, sql_op, -1, &st_op, NULL) != SQLITE_OK) {
        sd_toklist_free(&T_b_goal);
        sd_toklist_free(&T_a_goal);
        for (int i = 0; i < 7; ++i) sd_toklist_free(&T_secs[i]);
        for (int i = 0; i < dn; ++i) free(deletes[i]);
        free(deletes);
        if (moves) {
            for (int i = 0; i < mn; ++i) free(moves[i].tokseq_lower);
            free(moves);
        }
        if (chg) {
            for (int i = 0; i < cn; ++i) sd_changeword_free(&chg[i]);
            free(chg);
        }
        if (ins) {
			for (int i = 0; i < in; ++i) free(ins[i].tokseq_lower);
			free(ins);
		}
        free(vec);
        free(canon_in);
        return 0;
    }

    int order = 0;
    int ops = 0;

    /* priority: DELETE -> MOVE -> CHANGE */
    for (int i = 0; i < dn; ++i) {
        sqlite3_reset(st_op);
        sqlite3_clear_bindings(st_op);

        sqlite3_bind_int64(st_op, 1, program_id);
        sqlite3_bind_int(st_op,   2, order++);
        sqlite3_bind_int(st_op,   3, SD_OP_DELETE_TOKENS);
        sqlite3_bind_text(st_op,  4, "GOAL", -1, SQLITE_STATIC);
        sqlite3_bind_text(st_op,  5, "", -1, SQLITE_STATIC);
        sqlite3_bind_text(st_op,  6, deletes[i] ? deletes[i] : "", -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st_op,  7, "", -1, SQLITE_STATIC);
        sqlite3_bind_int(st_op,   8, SD_GUARD_SRC_HAS_UNIQUE_LINE);
        sqlite3_bind_int(st_op,   9, 0);

        if (sqlite3_step(st_op) == SQLITE_DONE) ops++;
    }

    for (int i = 0; i < mn; ++i) {
        char meta[128];
        meta[0] = '\0';
        sd_move_meta_make(meta, moves[i].anchor_hash, moves[i].offset, moves[i].payload_hash);

        sqlite3_reset(st_op);
        sqlite3_clear_bindings(st_op);

        sqlite3_bind_int64(st_op, 1, program_id);
        sqlite3_bind_int(st_op,   2, order++);
        sqlite3_bind_int(st_op,   3, SD_OP_MOVE_TOKENS);
        sqlite3_bind_text(st_op,  4, "GOAL", -1, SQLITE_STATIC);
        sqlite3_bind_text(st_op,  5, moves[i].dst_section ? moves[i].dst_section : "", -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st_op,  6, moves[i].tokseq_lower ? moves[i].tokseq_lower : "", -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st_op,  7, meta, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st_op,   8, SD_GUARD_SRC_HAS_UNIQUE_LINE);
        sqlite3_bind_int(st_op,   9, 0);

        if (sqlite3_step(st_op) == SQLITE_DONE) ops++;
    }
	
	for (int i = 0; i < in; ++i) {
		char meta[128];
		meta[0] = '\0';
		sd_move_meta_make(meta, ins[i].anchor_hash, ins[i].offset, ins[i].payload_hash);

		sqlite3_reset(st_op);
		sqlite3_clear_bindings(st_op);

		sqlite3_bind_int64(st_op, 1, program_id);
		sqlite3_bind_int(st_op,   2, order++);
		sqlite3_bind_int(st_op,   3, SD_OP_INSERT_TOKENS);
		sqlite3_bind_text(st_op,  4, "", -1, SQLITE_STATIC); // src_section empty for insert
		sqlite3_bind_text(st_op,  5, ins[i].dst_section ? ins[i].dst_section : "", -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(st_op,  6, ins[i].tokseq_lower ? ins[i].tokseq_lower : "", -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(st_op,  7, meta, -1, SQLITE_TRANSIENT);
		sqlite3_bind_int(st_op,   8, 0);
		sqlite3_bind_int(st_op,   9, 0);

		if (sqlite3_step(st_op) == SQLITE_DONE) ops++;
	}

    for (int i = 0; i < cn; ++i) {
        sqlite3_reset(st_op);
        sqlite3_clear_bindings(st_op);

        sqlite3_bind_int64(st_op, 1, program_id);
        sqlite3_bind_int(st_op,   2, order++);
        sqlite3_bind_int(st_op,   3, SD_OP_CHANGE_WORD);
        sqlite3_bind_text(st_op,  4, "GOAL", -1, SQLITE_STATIC);
        sqlite3_bind_text(st_op,  5, "GOAL", -1, SQLITE_STATIC);
        sqlite3_bind_text(st_op,  6, chg[i].before_tok ? chg[i].before_tok : "", -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st_op,  7, chg[i].after_tok ? chg[i].after_tok : "", -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st_op,   8, 0);
        sqlite3_bind_int(st_op,   9, chg[i].flags);

        if (sqlite3_step(st_op) == SQLITE_DONE) ops++;
    }

    sqlite3_finalize(st_op);

    if (out_ops_count) *out_ops_count = ops;

    sd_toklist_free(&T_b_goal);
    sd_toklist_free(&T_a_goal);
    for (int i = 0; i < 7; ++i) sd_toklist_free(&T_secs[i]);

    for (int i = 0; i < dn; ++i) free(deletes[i]);
    free(deletes);

    if (moves) {
        for (int i = 0; i < mn; ++i) free(moves[i].tokseq_lower);
        free(moves);
    }

    if (chg) {
        for (int i = 0; i < cn; ++i) sd_changeword_free(&chg[i]);
        free(chg);
    }
	
	if (ins) {
		for (int i = 0; i < in; ++i) free(ins[i].tokseq_lower);
		free(ins);
	}

    free(vec);
    free(canon_in);

    return (ops > 0) ? program_id : 0;
}

typedef struct {
    int label_id;
    double score;
    int group;
    char name[64];
} StructOp;

static void structurize_value_rule_mark(sqlite3 *db, sqlite3_int64 rule_id, int success)
{
    if (!db || rule_id <= 0) return;

    const char *sql =
        success
        ? "UPDATE struct_value_rules SET successes = successes + 1 WHERE id = ?;"
        : "UPDATE struct_value_rules SET failures  = failures  + 1 WHERE id = ?;";

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return;
    sqlite3_bind_int64(st, 1, rule_id);
    (void)sqlite3_step(st);
    sqlite3_finalize(st);
}

static int structurize_keytype_note(sqlite3 *db, const char *key, int is_string)
{
    if (!db || !key || !key[0]) return 0;

    /* ensure row exists */
    {
        const char *sql_ins =
            "INSERT OR IGNORE INTO struct_key_types(key,scalar_hits,string_hits,updated_at) "
            "VALUES(?1,0,0,CURRENT_TIMESTAMP);";
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(db, sql_ins, -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, key, -1, SQLITE_TRANSIENT);
            (void)sqlite3_step(st);
        }
        sqlite3_finalize(st);
    }

    /* bump counters */
    {
        const char *sql_up =
            "UPDATE struct_key_types "
            "SET scalar_hits = scalar_hits + ?1, "
            "    string_hits = string_hits + ?2, "
            "    updated_at = CURRENT_TIMESTAMP "
            "WHERE key = ?3;";
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(db, sql_up, -1, &st, NULL) != SQLITE_OK) return 0;
        sqlite3_bind_int(st, 1, is_string ? 0 : 1);
        sqlite3_bind_int(st, 2, is_string ? 1 : 0);
        sqlite3_bind_text(st, 3, key, -1, SQLITE_TRANSIENT);
        int ok = (sqlite3_step(st) == SQLITE_DONE);
        sqlite3_finalize(st);
        return ok ? 1 : 0;
    }
}

static int structurize_keytype_hint_is_string(sqlite3 *db, const char *key)
{
    if (!db || !key || !key[0]) return 0;

    const char *sql = "SELECT scalar_hits, string_hits FROM struct_key_types WHERE key=?1;";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return 0;

    sqlite3_bind_text(st, 1, key, -1, SQLITE_TRANSIENT);

    int is_str = 0;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const int sh  = sqlite3_column_int(st, 0);
        const int sth = sqlite3_column_int(st, 1);

        /* STRING wins as soon as it has any evidence and is not strictly weaker than SCALAR */
        if (sth > 0 && sth >= sh) is_str = 1;
    }
    sqlite3_finalize(st);
    return is_str;
}

static int structurize_value_rules_contains_key(sqlite3 *db, const char *section, const char *key)
{
    if (!db || !section || !section[0] || !key || !key[0]) return 0;

    const char *sql =
        "SELECT 1 FROM struct_value_rules "
        "WHERE section=?1 AND ("
        " key=?2 OR key LIKE ?3 OR key LIKE ?4 OR key LIKE ?5"
        ") LIMIT 1;";

    char p1[256], p2[256], p3[256];
    _snprintf_s(p1, sizeof(p1), _TRUNCATE, "%s|%%", key);       // key|...
    _snprintf_s(p2, sizeof(p2), _TRUNCATE, "%%|%s|%%", key);    // ...|key|...
    _snprintf_s(p3, sizeof(p3), _TRUNCATE, "%%|%s", key);       // ...|key

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return 0;

    sqlite3_bind_text(st, 1, section, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, key, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, p1, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, p2, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 5, p3, -1, SQLITE_TRANSIENT);

    int ok = 0;
    if (sqlite3_step(st) == SQLITE_ROW) ok = 1;
    sqlite3_finalize(st);
    return ok;
}

static int structurize_word_rule_upsert(sqlite3 *db,
                                       const char *src_word,
                                       const char *dst_word,
                                       const char *section,
                                       int lev_dist,
                                       const char *embed_model,
                                       int embed_dim,
                                       const char *ctx_seq,
                                       const float *vec)
{
    if (!db || !src_word || !dst_word || !section || !embed_model || !ctx_seq || !vec || embed_dim <= 0) return 0;

    const char *sql_ins =
        "INSERT OR IGNORE INTO struct_word_change_rules"
        "(src_word,dst_word,section,lev_dist,embed_model,embed_dim,ctx_seq,embedding,successes,failures) "
        "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,1,0);";

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql_ins, -1, &st, NULL) != SQLITE_OK) return 0;

    sqlite3_bind_text(st, 1, src_word, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, dst_word, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, section, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 4, lev_dist);
    sqlite3_bind_text(st, 5, embed_model, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 6, embed_dim);
    sqlite3_bind_text(st, 7, ctx_seq, -1, SQLITE_TRANSIENT);
    sqlite3_bind_blob(st, 8, vec, (int)(sizeof(float) * embed_dim), SQLITE_TRANSIENT);

    int ok = (sqlite3_step(st) == SQLITE_DONE);
    sqlite3_finalize(st);
    if (!ok) return 0;

    if (sqlite3_changes(db) > 0) return 1;

    const char *sql_up =
        "UPDATE struct_word_change_rules SET successes = successes + 1 "
        "WHERE src_word=?1 AND dst_word=?2 AND section=?3 AND embed_model=?4 AND embed_dim=?5 AND ctx_seq=?6;";

    sqlite3_stmt *st2 = NULL;
    if (sqlite3_prepare_v2(db, sql_up, -1, &st2, NULL) != SQLITE_OK) return 1;

    sqlite3_bind_text(st2, 1, src_word, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st2, 2, dst_word, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st2, 3, section, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st2, 4, embed_model, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st2, 5, embed_dim);
    sqlite3_bind_text(st2, 6, ctx_seq, -1, SQLITE_TRANSIENT);

    (void)sqlite3_step(st2);
    sqlite3_finalize(st2);
    return 1;
}

typedef struct SdWordRuleRec {
    sqlite3_int64 id;
    char *src_word;
    char *dst_word;
    float *vec;
    int successes;
    int failures;
} SdWordRuleRec;

typedef struct SdWordRuleCache {
    char section[32];
    char model[64];
    int dim;
    SdWordRuleRec *r;
    int n;
} SdWordRuleCache;

static void sd_wordrulecache_free(SdWordRuleCache *C)
{
    if (!C) return;
    for (int i = 0; i < C->n; ++i) {
        free(C->r[i].src_word);
        free(C->r[i].dst_word);
        free(C->r[i].vec);
    }
    free(C->r);
    C->r = NULL;
    C->n = 0;
    C->dim = 0;
    C->section[0] = 0;
    C->model[0] = 0;
}

static int sd_wordrulecache_load(sqlite3 *db, SdWordRuleCache *C, const char *section, const char *model, int dim)
{
    if (!db || !C || !section || !model || dim <= 0) return 0;

    sd_wordrulecache_free(C);

    strncpy(C->section, section, sizeof(C->section) - 1);
    C->section[sizeof(C->section) - 1] = 0;

    strncpy(C->model, model, sizeof(C->model) - 1);
    C->model[sizeof(C->model) - 1] = 0;

    C->dim = dim;

    const char *sql =
        "SELECT id, src_word, dst_word, embedding, successes, failures "
        "FROM struct_word_change_rules "
        "WHERE section=? AND embed_model=? AND embed_dim=?;";

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return 0;

    sqlite3_bind_text(st, 1, section, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, model, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st,  3, dim);

    int cap = 0;

    while (sqlite3_step(st) == SQLITE_ROW) {
        sqlite3_int64 id = sqlite3_column_int64(st, 0);
        const char *src = (const char*)sqlite3_column_text(st, 1);
        const char *dst = (const char*)sqlite3_column_text(st, 2);

        const void *blob = sqlite3_column_blob(st, 3);
        int bytes = sqlite3_column_bytes(st, 3);

        int succ = sqlite3_column_int(st, 4);
        int fail = sqlite3_column_int(st, 5);

        if (!src || !dst || !blob) continue;
        if (bytes != (int)(sizeof(float) * (size_t)dim)) continue;

        if (C->n >= cap) {
            int ncap = (cap <= 0) ? 64 : (cap * 2);
            SdWordRuleRec *nr = (SdWordRuleRec*)realloc(C->r, (size_t)ncap * sizeof(SdWordRuleRec));
            if (!nr) break;
            C->r = nr;
            cap = ncap;
        }

        SdWordRuleRec *R = &C->r[C->n];
        memset(R, 0, sizeof(*R));

        R->id = id;
        R->successes = succ;
        R->failures  = fail;

        R->src_word = _strdup(src);
        R->dst_word = _strdup(dst);

        R->vec = (float*)malloc(sizeof(float) * (size_t)dim);
        if (!R->src_word || !R->dst_word || !R->vec) {
            free(R->src_word); R->src_word = NULL;
            free(R->dst_word); R->dst_word = NULL;
            free(R->vec);      R->vec = NULL;
            continue;
        }

        memcpy(R->vec, blob, (size_t)bytes);
        C->n++;
    }

    sqlite3_finalize(st);
    return (C->n > 0) ? 1 : 0;
}

static void structurize_word_change_rule_mark(sqlite3 *db, sqlite3_int64 rule_id, int success)
{
    if (!db || rule_id <= 0) return;

    const char *sql =
        success
        ? "UPDATE struct_word_change_rules SET successes=successes+1 WHERE id=?;"
        : "UPDATE struct_word_change_rules SET failures=failures+1 WHERE id=?;";

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return;
    sqlite3_bind_int64(st, 1, rule_id);
    (void)sqlite3_step(st);
    sqlite3_finalize(st);
}

static int structurize_wordctx_apply_to_payload(sqlite3 *db, const char *section, char *payload, size_t payload_cap,
                                               sqlite3_int64 apply_log_id, int *io_ord,
                                               SdSectionLocks *locks)
{
    if (!db || !section || !payload || payload_cap == 0) return 0;

    SdTokList T;
    sd_toklist_init(&T);
    if (!sd_tokenize_payload(payload, &T)) {
        sd_toklist_free(&T);
        return 0;
    }
	
	char section_fp[32];
	structurize_delta_hex64(sd_hash_toklist_ci(&T), section_fp);

    int applied = 0;

    SdWordRuleCache cache;
    memset(&cache, 0, sizeof(cache));

    void *m = NULL;
    const char *mname = NULL;
    int dim = 0;

    float *qvec = NULL;
    int qdim = 0;
	int si = sd_section_index(section);

    for (int i = 0; i < T.n; ++i) {
        const char *tok = T.t[i];
        if (!tok || !tok[0]) continue;
        if (!sd_tok_is_wordlike(tok)) continue;
        if (locks && si >= 0 && sd_occ_overlap(&locks->occ[si], i, i + 1)) continue;

        char *ctx_seq = sd_build_word_ctx_seq_4_4(&T, i);
        if (!ctx_seq || !ctx_seq[0]) {
            free(ctx_seq);
            continue;
        }

        if (!structurize_delta_select_embed_model(ctx_seq, &m, &mname, &dim) || !m || dim <= 0 || !mname) {
            free(ctx_seq);
            continue;
        }

        if (qdim != dim) {
            free(qvec);
            qvec = (float*)malloc(sizeof(float) * (size_t)dim);
            qdim = dim;
            if (!qvec) {
                free(ctx_seq);
                break;
            }
        }

        if (!llama_client_get_embeddings(m, ctx_seq, qvec)) {
            free(ctx_seq);
            continue;
        }

        /* load cache for this (section, model, dim) if needed */
        if (cache.n == 0 || cache.dim != dim || strcmp(cache.section, section) != 0 || strcmp(cache.model, mname) != 0) {
            if (!sd_wordrulecache_load(db, &cache, section, mname, dim)) {
                free(ctx_seq);
                continue;
            }
        }

        /* find best rule by context embedding similarity, but only if src_word matches current token */
        float best_score = -1.0f;
        int best_idx = -1;

        for (int r = 0; r < cache.n; ++r) {
            SdWordRuleRec *R = &cache.r[r];
            if (!R->src_word || !R->dst_word || !R->vec) continue;

            if (_stricmp(R->src_word, tok) != 0) continue;

            const float sim = structurize_delta_cosine_sim(qvec, R->vec, dim);
            if (sim < 0.90f) continue; /* spec threshold */

            const float rel = (float)((double)(R->successes + 1) / (double)(R->successes + R->failures + 2));
            const float score = sim + (0.10f * rel);

            if (score > best_score) {
                best_score = score;
                best_idx = r;
            }
        }

        if (best_idx >= 0) {
            SdWordRuleRec *BR = &cache.r[best_idx];

            /* section fp BEFORE op */
            char section_fp[32];
            structurize_delta_hex64(sd_hash_toklist_ci(&T), section_fp);

            /* op_key: deterministic for penalty */
            unsigned long long ctx_hash = struct_ml_hash_str(ctx_seq);

            char op_key[512];
            _snprintf_s(op_key, sizeof(op_key), _TRUNCATE,
                        "WORDCTX|%s|%s|%s|%016llx",
                        section ? section : "",
                        tok ? tok : "",
                        (BR->dst_word ? BR->dst_word : ""),
                        ctx_hash);

            if (structurize_penalty_is_disabled(db, section_fp, section, op_key)) {
                /* hard-off: don't mark fail/success, just skip */
            } else {
                /* replace token */
                const char *before_word = tok;
                const char *after_word  = BR->dst_word ? BR->dst_word : "";

                free(T.t[i]);
                T.t[i] = _strdup(after_word);
                if (!T.t[i]) T.t[i] = _strdup(before_word ? before_word : "");

                applied = 1;
                if (locks && si >= 0) sd_occ_add(&locks->occ[si], i, i + 1);
                structurize_word_change_rule_mark(db, BR->id, 1);

                /* log for penalty-after-fix */
                if (apply_log_id > 0 && io_ord) {
                    structurize_apply_log_add_op(
                        db,
                        apply_log_id,
                        (*io_ord)++,
                        section ? section : "",
                        section_fp,
                        op_key,
                        SD_OP_CHANGE_WORD,
                        before_word ? before_word : "",
                        after_word  ? after_word  : "",
                        "",
                        ""
                    );
                }
            }

            /* single-pass */
        }

        free(ctx_seq);
    }

    if (applied) {
        sd_toklist_to_payload(payload, payload_cap, &T);
    }

    sd_wordrulecache_free(&cache);
    free(qvec);
    sd_toklist_free(&T);
    return applied;
}

static int structurize_wordctx_try_apply_rules(sqlite3 *db, char *io_struct, size_t io_sz,
                                               sqlite3_int64 apply_log_id, int *io_ord,
                                               SdSectionLocks *locks)
{
    if (!db || !io_struct || io_sz == 0) return 0;

    char goal[65536] = {0};
    char reqs[65536] = {0};
    char inp[65536]  = {0};
    char out[65536]  = {0};
    char cons[65536] = {0};
    char env[65536]  = {0};
    char edit[65536] = {0};
    char ng[65536]   = {0};

    tr_copy_section_payload(io_struct, "GOAL", goal, sizeof(goal));
    tr_copy_section_payload(io_struct, "REQUIREMENTS", reqs, sizeof(reqs));
    tr_copy_section_payload(io_struct, "INPUT", inp, sizeof(inp));
    tr_copy_section_payload(io_struct, "OUTPUT", out, sizeof(out));
    tr_copy_section_payload(io_struct, "CONSTRAINTS", cons, sizeof(cons));
    tr_copy_section_payload(io_struct, "ENVIRONMENT", env, sizeof(env));
    tr_copy_section_payload(io_struct, "EDIT_PARAMETERS", edit, sizeof(edit));
    tr_copy_section_payload(io_struct, "NON_GOALS", ng, sizeof(ng));

    int applied_total = 0;
    applied_total += structurize_wordctx_apply_to_payload(db, "GOAL", goal, sizeof(goal), apply_log_id, io_ord, locks);
    applied_total += structurize_wordctx_apply_to_payload(db, "REQUIREMENTS", reqs, sizeof(reqs), apply_log_id, io_ord, locks);
    applied_total += structurize_wordctx_apply_to_payload(db, "INPUT", inp, sizeof(inp), apply_log_id, io_ord, locks);
    applied_total += structurize_wordctx_apply_to_payload(db, "OUTPUT", out, sizeof(out), apply_log_id, io_ord, locks);
    applied_total += structurize_wordctx_apply_to_payload(db, "CONSTRAINTS", cons, sizeof(cons), apply_log_id, io_ord, locks);
    /* do not apply wordctx to ENVIRONMENT / EDIT_PARAMETERS */
    applied_total += structurize_wordctx_apply_to_payload(db, "NON_GOALS", ng, sizeof(ng), apply_log_id, io_ord, locks);

    if (applied_total <= 0) return 0;

    char rebuilt[65536 * 2] = {0};
    tt_rebuild_struct(rebuilt, sizeof(rebuilt),
                      goal, reqs, inp, out, cons, env, edit, ng);

    if (hybrid_validate_clarified_output(rebuilt)) {
        sd_trunc_str(rebuilt, io_struct, io_sz);
        return applied_total;
    }
    return 0;
}

typedef struct SdValueKeyAgg {
    char *key;
    double sum_p;
    int cnt;
    sqlite3_int64 best_id;
    float best_sim;
} SdValueKeyAgg;

static int structurize_value_pick_key(
    sqlite3 *db,
    const char *section,
    const char *embed_model,
    int embed_dim,
    const float *qvec,
    int want_count,
    char *out_key,
    size_t out_key_cap,
    sqlite3_int64 *out_rule_id,
    float *out_prob)
{
    if (out_key && out_key_cap) out_key[0] = 0;
    if (out_rule_id) *out_rule_id = 0;
    if (out_prob) *out_prob = 0.f;

    if (!db || !section || !embed_model || embed_dim <= 0 || !qvec) return 0;

    const char *sql =
        "SELECT id, key, embedding "
        "FROM struct_value_rules "
        "WHERE section = ? AND embed_model = ? AND embed_dim = ? "
        "ORDER BY id DESC "
        "LIMIT 400;";

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return 0;

    sqlite3_bind_text(st, 1, section, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, embed_model, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 3, embed_dim);

    SdValueKeyAgg *A = NULL;
    int an = 0, acap = 0;

    while (sqlite3_step(st) == SQLITE_ROW) {
        sqlite3_int64 rid = sqlite3_column_int64(st, 0);
        const char *k = (const char *)sqlite3_column_text(st, 1);
        const void *blob = sqlite3_column_blob(st, 2);
        int bytes = sqlite3_column_bytes(st, 2);
        if (!k || !k[0] || !blob || bytes != (int)(sizeof(float) * (size_t)embed_dim)) continue;

        if (sd_key_count_pipe(k) != want_count) continue;

        const float *avec = (const float *)blob;
        float sim = structurize_delta_cosine_sim(qvec, avec, embed_dim);
        if (sim < 0.75f) continue;

        float p = sd_value_prob_from_similarity(sim);
        if (p <= 0.f) continue;

        int idx = -1;
        for (int i = 0; i < an; ++i) {
            if (_stricmp(A[i].key, k) == 0) { idx = i; break; }
        }
        if (idx < 0) {
            if (an + 1 > acap) {
                int ncap = acap ? acap * 2 : 8;
                SdValueKeyAgg *tmp = (SdValueKeyAgg *)realloc(A, (size_t)ncap * sizeof(SdValueKeyAgg));
                if (!tmp) break;
                A = tmp;
                acap = ncap;
            }
            memset(&A[an], 0, sizeof(A[an]));
            A[an].key = _strdup(k);
            if (!A[an].key) break;
            A[an].sum_p = 0.0;
            A[an].cnt = 0;
            A[an].best_id = rid;
            A[an].best_sim = sim;
            idx = an;
            an++;
        }

        A[idx].sum_p += (double)p;
        A[idx].cnt += 1;
        if (sim > A[idx].best_sim) {
            A[idx].best_sim = sim;
            A[idx].best_id = rid;
        }
    }

    sqlite3_finalize(st);

    if (!A || an == 0) {
        if (A) {
            for (int i = 0; i < an; ++i) free(A[i].key);
            free(A);
        }
        return 0;
    }

    int best = -1;
    double best_avg = 0.0;
    sqlite3_int64 best_id = 0;

    for (int i = 0; i < an; ++i) {
        if (A[i].cnt <= 0) continue;
        double avg = A[i].sum_p / (double)A[i].cnt; // как у тебя: 90% + 40% => 65%
        if (best < 0 || avg > best_avg) {
            best = i;
            best_avg = avg;
            best_id = A[i].best_id;
        }
    }

    if (best >= 0 && out_key && out_key_cap) {
        _snprintf_s(out_key, out_key_cap, _TRUNCATE, "%s", A[best].key ? A[best].key : "");
    }
    if (out_rule_id) *out_rule_id = best_id;
    if (out_prob) *out_prob = (float)best_avg;

    for (int i = 0; i < an; ++i) free(A[i].key);
    free(A);

    return (best >= 0 && out_key && out_key[0]) ? 1 : 0;
}

static char *sd_build_value_ctx_seq_intxint_window_4_4(const SdTokList *T,
                                                       int sent_start,
                                                       int sent_len,
                                                       int val_tok_abs)
{
    const int sent_end = sent_start + sent_len;

    int left = val_tok_abs;
    int wc = 0;
    for (int i = val_tok_abs - 1; i >= sent_start; --i) {
        left = i;
        if (sd_tok_is_wordlike(T->t[i])) {
            if (++wc >= 4) break;
        }
    }

    int right = val_tok_abs;
    wc = 0;
    for (int i = val_tok_abs + 1; i < sent_end; ++i) {
        right = i;
        if (sd_tok_is_wordlike(T->t[i])) {
            if (++wc >= 4) break;
        }
    }

    size_t cap = 64;
    for (int i = left; i <= right; ++i) cap += strlen(T->t[i]) + 1;

    char *out = (char *)malloc(cap);
    if (!out) return NULL;

    size_t w = 0;
    for (int i = left; i <= right; ++i) {
        if (w > 0) out[w++] = ' ';

        if (i == val_tok_abs) {
            const char *rep = "{{VALUE}} x {{VALUE}}";
            const size_t n = strlen(rep);
            memcpy(out + w, rep, n);
            w += n;
            continue;
        }

        const unsigned char *p = (const unsigned char *)T->t[i];
        while (*p) out[w++] = (char)tolower(*p++);
    }

    out[w] = '\0';
    return out;
}

static int structurize_value_apply_to_section(
    sqlite3 *db,
    const char *section_name,
    char *payload,
    size_t payload_cap,
    char *edit_payload,
    size_t edit_cap,
    const char *value_fp,
    sqlite3_int64 apply_log_id,
    int *io_ord,
    SdSectionLocks *locks)
{
    if (!db || !section_name || !payload || payload_cap == 0 || !edit_payload || edit_cap == 0 || !value_fp) return 0;

    char chk[64];
    _snprintf_s(chk, sizeof(chk), _TRUNCATE, "%s", payload);
    clar_trim(chk);
    if (!chk[0] || tt_is_none_text(chk)) return 0;

    SdTokList T;
    sd_tokenize_payload(payload, &T);
    char section_fp[32];
    structurize_delta_hex64(sd_hash_toklist_ci(&T), section_fp);

    SdSpan *spans = NULL;
    int spn = 0;
    sd_collect_sentence_spans(&T, &spans, &spn);

    int applied = 0;
    const int sec_idx = sd_section_index(section_name);

    for (int si = 0; si < spn; ++si) {
        const int sent_start = spans[si].start;
        const int sent_len   = spans[si].len;

        SdValueItem *items = NULL;
        int in = 0;
        if (!sd_collect_value_items_apply(&T, sent_start, sent_len, &items, &in)) {
            continue;
        }

        SdValueGroup *G = NULL;
        int gn = 0;
        if (!sd_build_value_groups(&T, items, in, &G, &gn)) {
            free(items);
            continue;
        }

        for (int gi = 0; gi < gn; ++gi) {
            SdValueGroup g = G[gi];
            if (g.count <= 0) continue;

            char *ctx_seq = sd_build_value_ctx_seq_generic(&T, sent_start, sent_len, items, g.first_item, g.count);
            if (!ctx_seq || !ctx_seq[0]) {
                if (ctx_seq) free(ctx_seq);
                continue;
            }
            unsigned long long ctx_hash = struct_ml_hash_str(ctx_seq);

            void *m = NULL;
            const char *mname = "";
            int dim = 0;
            if (!structurize_delta_select_embed_model(ctx_seq, &m, &mname, &dim) || !m || dim <= 0) {
                free(ctx_seq);
                continue;
            }

            float *qvec = (float *)malloc(sizeof(float) * (size_t)dim);
            if (!qvec) { free(ctx_seq); continue; }
            if (!llama_client_get_embeddings(m, ctx_seq, qvec)) {
                free(qvec);
                free(ctx_seq);
                continue;
            }

            char picked_key[256];
            sqlite3_int64 rule_id = 0;
            float prob = 0.f;
            int ok = structurize_value_pick_key(db, section_name, mname, dim, qvec, g.count,
                                                picked_key, sizeof(picked_key),
                                                &rule_id, &prob);

            if (!ok && section_name && _stricmp(section_name, "GOAL") == 0) {
                ok = structurize_value_pick_key(db, "REQUIREMENTS", mname, dim, qvec, g.count,
                                                picked_key, sizeof(picked_key),
                                                &rule_id, &prob);
            }
			
			/* FALLBACK: если это intxint (100x400) и ключ не подобрался — попробуем локальный контекст 4+4 */
			if (!ok && g.count == 2) {
				const SdValueItem *i0 = &items[g.first_item + 0];
				const SdValueItem *i1 = &items[g.first_item + 1];

				if (i0->start_tok == i1->start_tok) {
					char *ctx2 = sd_build_value_ctx_seq_intxint_window_4_4(&T, sent_start, sent_len, i0->start_tok);
					if (ctx2 && ctx2[0]) {
						float *q2 = (float *)malloc(sizeof(float) * (size_t)dim);
						if (q2 && llama_client_get_embeddings(m, ctx2, q2)) {

							sqlite3_int64 rule2 = 0;
							float prob2 = 0.f;

							int ok2 = structurize_value_pick_key(db, section_name, mname, dim, q2, g.count,
																 picked_key, sizeof(picked_key),
																 &rule2, &prob2);

							if (!ok2 && section_name && _stricmp(section_name, "GOAL") == 0) {
								ok2 = structurize_value_pick_key(db, "REQUIREMENTS", mname, dim, q2, g.count,
																 picked_key, sizeof(picked_key),
																 &rule2, &prob2);
							}

							if (ok2) {
								ok = 1;
								rule_id = rule2;
								prob = prob2;
							}
						}
						if (q2) free(q2);
						free(ctx2);
					} else {
						if (ctx2) free(ctx2);
					}
				}
			}
            free(qvec);
            free(ctx_seq);

            if (!ok || !picked_key[0] || prob <= 0.f) continue;

            char op_key_apply[1024];
            _snprintf_s(op_key_apply, sizeof(op_key_apply), _TRUNCATE,
                        "VALUEP|%s|%016llx|%d",
                        picked_key,
                        ctx_hash,
                        g.count);

            if (!sd_should_apply(value_fp, op_key_apply, prob)) {
                structurize_value_rule_mark(db, rule_id, 0);
                continue;
            }

            char keybuf[256];
            _snprintf_s(keybuf, sizeof(keybuf), _TRUNCATE, "%s", picked_key);

            char *keys[16];
            int kc = 0;
            char *ctx = NULL;
            for (char *p = strtok_s(keybuf, "|", &ctx); p && kc < 16; p = strtok_s(NULL, "|", &ctx)) {
                clar_trim(p);
                if (p[0]) keys[kc++] = p;
            }
            if (kc != g.count) {
                structurize_value_rule_mark(db, rule_id, 0);
                continue;
            }

            int dup_same_tok = 0;
            for (int a = 0; a < g.count; ++a) {
                for (int b = a + 1; b < g.count; ++b) {
                    const SdValueItem *ia = &items[g.first_item + a];
                    const SdValueItem *ib = &items[g.first_item + b];
                    if (ia->start_tok == ib->start_tok) { dup_same_tok = 1; break; }
                }
                if (dup_same_tok) break;
            }

            int is_intxint = 0;
            int tok_pos = -1;
            char sep_ch = 'x';
            if (dup_same_tok && g.count == 2) {
                SdValueItem *i0 = &items[g.first_item + 0];
                SdValueItem *i1 = &items[g.first_item + 1];
                if (i0->start_tok == i1->start_tok && i0->end_tok == i1->end_tok && T.t[i0->start_tok]) {
                    long long a = 0, b = 0;
                    if (sd_tok_parse_int_x_int_token(T.t[i0->start_tok], &a, &b)) {
                        is_intxint = 1;
                        tok_pos = i0->start_tok;
                        if (strchr(T.t[tok_pos], 'X')) sep_ch = 'X';
                    }
                }
            }

            if (is_intxint) {
                char op_key0[512];
                char op_key1[512];
                _snprintf_s(op_key0, sizeof(op_key0), _TRUNCATE,
                            "VALUE|%s|%s|%016llx",
                            section_name ? section_name : "",
                            keys[0] ? keys[0] : "",
                            ctx_hash);
                _snprintf_s(op_key1, sizeof(op_key1), _TRUNCATE,
                            "VALUE|%s|%s|%016llx",
                            section_name ? section_name : "",
                            keys[1] ? keys[1] : "",
                            ctx_hash);

                if (structurize_penalty_is_disabled(db, section_fp, section_name, op_key0) ||
                    structurize_penalty_is_disabled(db, section_fp, section_name, op_key1)) {
                    continue;
                }

                char mbuf[64];
                mbuf[0] = '\0';
                if (T.t[tok_pos]) {
                    _snprintf_s(mbuf, sizeof(mbuf), _TRUNCATE, "%s", T.t[tok_pos]);
                }

                if (locks && sec_idx >= 0 && sd_occ_overlap(&locks->occ[sec_idx], tok_pos, tok_pos + 1)) {
                    continue;
                }

                char repl2[256];
                _snprintf_s(repl2, sizeof(repl2), _TRUNCATE, "{{%s}}%c{{%s}}", keys[0], sep_ch, keys[1]);

                free(T.t[tok_pos]);
                T.t[tok_pos] = _strdup(repl2);

                long long ex0 = 0, ex1 = 0;
                const int has0 = sd_editparams_get_scalar_int(edit_payload, keys[0], &ex0);
                const int has1 = sd_editparams_get_scalar_int(edit_payload, keys[1], &ex1);

                if (!has0) (void)sd_editparams_set_scalar_int(edit_payload, edit_cap, keys[0], items[g.first_item + 0].value);
                if (!has1) (void)sd_editparams_set_scalar_int(edit_payload, edit_cap, keys[1], items[g.first_item + 1].value);

                if (locks && sec_idx >= 0) sd_occ_add(&locks->occ[sec_idx], tok_pos, tok_pos + 1);

                if (apply_log_id > 0 && io_ord) {
                    structurize_apply_log_add_op(
                        db, apply_log_id, (*io_ord)++,
                        section_name ? section_name : "",
                        section_fp, op_key0, SD_OP_CHANGE_WORD,
                        mbuf, repl2, "", ""
                    );
                    structurize_apply_log_add_op(
                        db, apply_log_id, (*io_ord)++,
                        section_name ? section_name : "",
                        section_fp, op_key1, SD_OP_CHANGE_WORD,
                        mbuf, repl2, "", ""
                    );
                }
            } else {
                for (int k = 0; k < g.count; ++k) {
                    SdValueItem *it = &items[g.first_item + k];

                    char repl[256];
                    _snprintf_s(repl, sizeof(repl), _TRUNCATE, "{{%s}}", keys[k]);

                    char op_key[512];
                    _snprintf_s(op_key, sizeof(op_key), _TRUNCATE,
                                "VALUE|%s|%s|%016llx",
                                section_name ? section_name : "",
                                keys[k] ? keys[k] : "",
                                ctx_hash);

                    if (structurize_penalty_is_disabled(db, section_fp, section_name, op_key)) {
                        continue;
                    }

                    char mbuf[64];
                    mbuf[0] = '\0';
                    if (T.t[it->start_tok]) {
                        _snprintf_s(mbuf, sizeof(mbuf), _TRUNCATE, "%s", T.t[it->start_tok]);
                    }

                    if (locks && sec_idx >= 0 && sd_occ_overlap(&locks->occ[sec_idx], it->start_tok, it->start_tok + 1)) {
                        continue;
                    }

                    free(T.t[it->start_tok]);
                    T.t[it->start_tok] = _strdup(repl);

                                        long long exv = 0;
                    const int hasv = sd_editparams_get_scalar_int(edit_payload, keys[k], &exv);
                    if (!hasv) {
                        (void)sd_editparams_set_scalar_int(edit_payload, edit_cap, keys[k], it->value);
                    }

                    if (locks && sec_idx >= 0) sd_occ_add(&locks->occ[sec_idx], it->start_tok, it->start_tok + 1);

                    if (apply_log_id > 0 && io_ord) {
                        structurize_apply_log_add_op(
                            db, apply_log_id, (*io_ord)++,
                            section_name ? section_name : "",
                            section_fp, op_key, SD_OP_CHANGE_WORD,
                            mbuf, repl, "", ""
                        );
                    }
                }
            }

            structurize_value_rule_mark(db, rule_id, 1);
            applied++;
        }

        free(G);
        free(items);
    }

    free(spans);

    if (applied > 0) {
        sd_toklist_to_payload(payload, payload_cap, &T);
    }
    sd_toklist_free(&T);

    return applied;
}

/* если эти функции объявлены ниже по файлу — оставь прототипы, чтобы не было implicit declaration */
static int sd_collect_sentence_hashset_ci(const SdTokList *T, int start, int len,
                                         unsigned long long **out_arr, int *out_n);

static float sd_jaccard_sets(const unsigned long long *A, int an, const unsigned long long *B, int bn);

static int sd_intersection_count_sets(const unsigned long long *a, int an,
                                      const unsigned long long *b, int bn)
{
    if (!a || !b || an <= 0 || bn <= 0) return 0;

    int i = 0, j = 0, inter = 0;
    while (i < an && j < bn) {
        if (a[i] == b[j]) { inter++; i++; j++; }
        else if (a[i] < b[j]) i++;
        else j++;
    }
    return inter;
}

static float sd_coverage_sets(const unsigned long long *a, int an,
                             const unsigned long long *b, int bn)
{
    const int mn = (an < bn) ? an : bn;
    if (mn <= 0) return 0.0f;
    const int inter = sd_intersection_count_sets(a, an, b, bn);
    return (float)inter / (float)mn;
}

static int sd_utf8_copy_prefix_chars(const char *s, int max_chars, char *out, size_t cap)
{
    if (out && cap) out[0] = '\0';
    if (!s || !s[0] || !out || cap == 0 || max_chars <= 0) return 0;

    const unsigned char *p = (const unsigned char*)s;
    size_t oi = 0;
    int chars = 0;

    while (*p && chars < max_chars) {
        unsigned char c = *p;
        int len = 1;
        if (c < 0x80) len = 1;
        else if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        else break;

        if (oi + (size_t)len + 1 > cap) break;
        for (int i = 0; i < len; ++i) out[oi++] = (char)p[i];
        p += len;
        chars++;
    }

    out[oi] = '\0';
    return chars;
}

static int sd_utf8_copy_prefix_chars_casefold(const char *s, int max_chars, char *out, size_t cap)
{
    if (out && cap) out[0] = '\0';
    if (!s || !s[0] || !out || cap == 0 || max_chars <= 0) return 0;

    const unsigned char *p = (const unsigned char*)s;
    size_t oi = 0;
    int chars = 0;

    while (*p && chars < max_chars) {
        unsigned char c = *p;

        /* ASCII */
        if (c < 0x80) {
            if (oi + 2 > cap) break;
            out[oi++] = (char)tolower((int)c);
            p++;
            chars++;
            continue;
        }

        /* Cyrillic 2-byte fold (А-Я, Ё) */
        if (p[0] == 0xD0 && p[1]) {
            unsigned char b = p[1];

            /* Ё (D0 81) -> ё (D1 91) */
            if (b == 0x81) {
                if (oi + 3 > cap) break;
                out[oi++] = (char)0xD1;
                out[oi++] = (char)0x91;
                p += 2;
                chars++;
                continue;
            }

            /* А..П (D0 90..9F) -> а..п (D0 B0..BF) */
            if (b >= 0x90 && b <= 0x9F) {
                if (oi + 3 > cap) break;
                out[oi++] = (char)0xD0;
                out[oi++] = (char)(b + 0x20);
                p += 2;
                chars++;
                continue;
            }

            /* Р..Я (D0 A0..AF) -> р..я (D1 80..8F) */
            if (b >= 0xA0 && b <= 0xAF) {
                if (oi + 3 > cap) break;
                out[oi++] = (char)0xD1;
                out[oi++] = (char)(b - 0x20);
                p += 2;
                chars++;
                continue;
            }
        }

        /* default: copy UTF-8 seq as-is */
        int len = 1;
        if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;

        if (oi + (size_t)len + 1 > cap) break;
        for (int k = 0; k < len && p[k]; ++k) out[oi++] = (char)p[k];
        p += len;
        chars++;
    }

    out[oi] = '\0';
    return chars;
}

static int sd_tok_is_ascii_short_unitlike(const char *tok)
{
    if (!tok || !tok[0]) return 0;
    int n = 0;
    for (const unsigned char *p = (const unsigned char*)tok; *p; ++p) {
        if (*p >= 0x80) return 0;
        if (!isalpha((int)*p)) return 0;
        n++;
        if (n > 3) return 0;
    }
    return (n >= 1) ? 1 : 0;
}

static int sd_tok_is_single_punct(const char *tok)
{
    if (!tok || !tok[0] || tok[1] != '\0') return 0;
    const unsigned char c = (unsigned char)tok[0];
    return (c == '.' || c == ',' || c == ':' || c == ';' || c == '!' || c == '?' ||
            c == '(' || c == ')' || c == '[' || c == ']' || c == '"' || c == '\'' );
}

static unsigned long long sd_hash_token_fuzzy_ci(const char *tok)
{
    if (!tok || !tok[0]) return 0ULL;

    /* ASCII alpha tokens: lower-case full token */
    int all_ascii = 1;
    for (const unsigned char *p = (const unsigned char*)tok; *p; ++p) {
        if (*p >= 0x80) { all_ascii = 0; break; }
    }

    if (all_ascii) {
        char tmp[128];
        size_t n = strlen(tok);
        if (n >= sizeof(tmp)) n = sizeof(tmp) - 1;
        for (size_t i = 0; i < n; ++i) {
            char c = tok[i];
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            tmp[i] = c;
        }
        tmp[n] = '\0';
        return struct_ml_hash_str(tmp);
    }

    /* UTF-8: take prefix of 3 chars (stem-like), do not case-fold (safe, no unicode tables) */
    char pref[64];
    sd_utf8_copy_prefix_chars(tok, 3, pref, sizeof(pref));
    if (!pref[0]) return 0ULL;
    return struct_ml_hash_str(pref);
}

static int ull_cmp(const void *a, const void *b)
{
    const unsigned long long A = *(const unsigned long long*)a;
    const unsigned long long B = *(const unsigned long long*)b;
    return (A < B) ? -1 : (A > B) ? 1 : 0;
}

/* Like sd_collect_sentence_hashset_ci, but:
   - skips tokens inside "{{...}}" (tokenized as "{" "{" ... "}" "}")
   - skips punctuation tokens
   - hashes tokens with fuzzy-stem (utf8 prefix) */
static int sd_collect_sentence_hashset_ci_fuzzy_no_placeholders(const SdTokList *T, int start, int len,
                                                                unsigned long long **out_arr, int *out_n)
{
    if (out_arr) *out_arr = NULL;
    if (out_n) *out_n = 0;
    if (!T || start < 0 || len <= 0 || start + len > T->n || !out_arr || !out_n) return 0;

    unsigned long long *arr = (unsigned long long*)malloc((size_t)len * sizeof(unsigned long long));
    if (!arr) return 0;

    int an = 0;
    int in_ph = 0;

    for (int i = start; i < start + len; ++i) {
        const char *tok = T->t[i];
        if (!tok || !tok[0]) continue;
        if (strcmp(tok, "\\n") == 0) continue;

        /* placeholder enter/exit: "{" "{" ... "}" "}" */
        if (!in_ph && i + 1 < start + len && strcmp(T->t[i], "{") == 0 && strcmp(T->t[i + 1], "{") == 0) {
            in_ph = 1;
            i++; /* skip second "{" */
            continue;
        }
        if (in_ph) {
            if (i + 1 < start + len && strcmp(T->t[i], "}") == 0 && strcmp(T->t[i + 1], "}") == 0) {
                in_ph = 0;
                i++; /* skip second "}" */
            }
            continue;
        }

        if (sd_tok_is_single_punct(tok)) continue;
		/* Gate must not match on short stop-tokens ("к", "и", "на", ...).
           Use only tokens with >= 3 UTF-8 characters for "fits current text". */
        {
            char _pref[64];
            int _nch = sd_utf8_copy_prefix_chars(tok, 3, _pref, sizeof(_pref));
            if (_nch < 3) continue;
        }

        unsigned long long h = sd_hash_token_fuzzy_ci(tok);
        if (!h) continue;
        arr[an++] = h;
    }

    if (an <= 0) {
        free(arr);
        return 0;
    }

    qsort(arr, (size_t)an, sizeof(unsigned long long), ull_cmp);

    /* uniq */
    int un = 0;
    for (int i = 0; i < an; ++i) {
        if (i == 0 || arr[i] != arr[i - 1]) arr[un++] = arr[i];
    }

    unsigned long long *final = (unsigned long long*)malloc((size_t)un * sizeof(unsigned long long));
    if (!final) { free(arr); return 0; }
    memcpy(final, arr, (size_t)un * sizeof(unsigned long long));
    free(arr);

    *out_arr = final;
    *out_n = un;
    return 1;
}

static int sd_insert_supported_by_input_sentences(const SdTokList *needle,
                                                  unsigned long long **in_set,
                                                  const int *in_n,
                                                  int in_sn)
{
    if (!needle || needle->n <= 0 || !in_set || !in_n || in_sn <= 0) return 0;

    unsigned long long *ref_set = NULL;
    int ref_n = 0;

    if (!sd_collect_sentence_hashset_ci_fuzzy_no_placeholders(needle, 0, needle->n, &ref_set, &ref_n)) {
        if (ref_set) free(ref_set);
        return 0;
    }
    if (!ref_set || ref_n <= 0) {
        if (ref_set) free(ref_set);
        return 0;
    }

    /* “fits current text” thresholds:
       - require at least 2 common content-tokens
       - require coverage >= 0.50 (relative to smaller set)
       - OR jaccard >= 0.60
       These are conservative enough to block stale inserts like “... папки music ...”
       when the folder sentence was removed, but still allow normal inserts like “DONE”. */
	const int   TH_INTER = 1;
	const float TH_COV   = 0.20f;
	const float TH_J     = 0.20f;

    int ok = 0;
    for (int i = 0; i < in_sn; ++i) {
        if (!in_set[i] || in_n[i] <= 0) continue;

        const int   inter = sd_intersection_count_sets(ref_set, ref_n, in_set[i], in_n[i]);
        const float cov   = sd_coverage_sets(ref_set, ref_n, in_set[i], in_n[i]);
        const float jac   = sd_jaccard_sets(ref_set, ref_n, in_set[i], in_n[i]);

        if ((inter >= TH_INTER && cov >= TH_COV) || (jac >= TH_J)) { ok = 1; break; }
    }

    free(ref_set);
    return ok;
}

static int sd_is_key_char(unsigned char c)
{
    return (isalnum(c) || c == '_' || c == '.' || c == '-' );
}

static int sd_line_is_header_like(const char *ls, const char *le)
{
    while (ls < le && (*ls==' '||*ls=='\t'||*ls=='\r')) ls++;
    const char *p = ls;
    int n = 0;
    while (p < le && n < 64) {
        char c = *p;
        if (c == ':') break;
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_') { p++; n++; continue; }
        return 0;
    }
    if (p < le && *p == ':' && n >= 2) return 1;
    return 0;
}

static int sd_is_inside_placeholder_backscan(const char *text, const char *p)
{
    /* returns 1 if nearest brace-pair boundary when scanning left is "{{" (i.e., we are inside {{...}}) */
    const char *q = p;
    while (q > text) {
        if (q - 2 >= text) {
            if (q[-2] == '}' && q[-1] == '}') return 0;
            if (q[-2] == '{' && q[-1] == '{') return 1;
        }
        q--;
    }
    return 0;
}

static void sd_find_sentence_bounds(const char *text, const char *pos, const char **out_s, const char **out_e)
{
    if (out_s) *out_s = pos;
    if (out_e) *out_e = pos;
    if (!text || !pos) return;

    const char *s = pos;
    while (s > text) {
        const char c = s[-1];
        if ((c == '.' || c == '?' || c == '!') && !sd_is_inside_placeholder_backscan(text, s - 1)) break;

        if (c == '\n') {
            const char *nl = s - 1;

            /* blank line => hard boundary */
            if (nl > text && nl[-1] == '\n') break;

            /* do not cross section headers like "OUTPUT:" */
            const char *ls = nl;
            while (ls > text && ls[-1] != '\n') ls--;
            if (sd_line_is_header_like(ls, nl)) break;

            /* newline after explicit terminator => boundary */
            if (nl > text) {
                const char pc = nl[-1];
                if (pc == '.' || pc == '?' || pc == '!') break;
            }

            /* soft-wrap: treat newline as whitespace */
            s--;
            continue;
        }

        s--;
    }

    const char *e = pos;
    while (*e) {
        const char c = *e;

		if ((c == '.' || c == '?' || c == '!') && !sd_is_inside_placeholder_backscan(text, e)) { e++; break; }

        if (c == '\n') {
            const char *nl = e;

            const char pc = (nl > text) ? nl[-1] : '\0';
            if (pc == '.' || pc == '?' || pc == '!') { e++; break; }
            if (pc == '\n') { e++; break; }

            /* if next line is header-like, stop */
            const char *ls = nl + 1;
            while (*ls == ' ' || *ls == '\t' || *ls == '\r') ls++;
            const char *le = ls;
            while (*le && *le != '\n') le++;
            if (sd_line_is_header_like(ls, le)) { e++; break; }

            /* soft-wrap */
            e++;
            continue;
        }

        e++;
    }

    if (out_s) *out_s = s;
    if (out_e) *out_e = e;
}

static float sd_sentence_overlap_sim_ci(const SdTokList *inT, int in_start, int in_end,
                                        const char *ref_sentence)
{
    if (!inT || !ref_sentence || !ref_sentence[0]) return 0.f;

    SdTokList R;
    sd_tokenize_payload(ref_sentence, &R);

    int a_words = 0;
    int b_words = 0;
    int common = 0;

    for (int i = 0; i < R.n; ++i) {
        if (sd_tok_is_wordlike(R.t[i])) a_words++;
    }
    for (int j = in_start; j < in_end; ++j) {
        if (sd_tok_is_wordlike(inT->t[j])) b_words++;
    }

    for (int i = 0; i < R.n; ++i) {
        const char *rt = R.t[i];
        if (!sd_tok_is_wordlike(rt)) continue;

        for (int j = in_start; j < in_end; ++j) {
            const char *it = inT->t[j];
            if (!sd_tok_is_wordlike(it)) continue;
            if (_stricmp(rt, it) == 0) { common++; break; }

			/* fuzzy UTF-8 prefix (3 chars) to tolerate inflections: "радиус" vs "радиуса" */
			{
				char ra[64], ia[64];
				ra[0] = ia[0] = '\0';
				int rch = sd_utf8_copy_prefix_chars_casefold(rt, 3, ra, sizeof(ra));
				int ich = sd_utf8_copy_prefix_chars_casefold(it, 3, ia, sizeof(ia));
				if (rch >= 3 && ich >= 3 && strcmp(ra, ia) == 0) { common++; break; }
			}
        }
    }

    sd_toklist_free(&R);

    int denom = (a_words > b_words) ? a_words : b_words;
    if (denom <= 0) return 0.f;
    return (float)common / (float)denom;
}

static int sd_tok_is_ascii_alpha_len_2_4(const char *tok)
{
    if (!tok || !tok[0]) return 0;
    int n = 0;
    for (const unsigned char *p = (const unsigned char*)tok; *p; ++p) {
        if (*p >= 0x80) return 0;
        if (!isalpha((int)*p)) return 0;
        n++;
        if (n > 4) return 0;
    }
    return (n >= 2 && n <= 4) ? 1 : 0;
}

static void sd_ascii_lower_copy(const char *in, char *out, size_t cap)
{
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!in) return;

    size_t w = 0;
    for (const unsigned char *p = (const unsigned char*)in; *p && (w + 1) < cap; ++p) {
        unsigned char c = *p;
        if (c >= 0x80) break;
        out[w++] = (char)tolower((int)c);
    }
    out[w] = '\0';
}

static void sd_ref_unit_hint_from_toklist(const SdTokList *R, char unit_out[8])
{
    if (unit_out) unit_out[0] = '\0';
    if (!R || R->n <= 0 || !unit_out) return;

    char last[8];
    last[0] = '\0';

    for (int i = 0; i < R->n; ++i) {
        const char *t = R->t[i];
        if (!t || !t[0]) continue;
        if (!sd_tok_is_ascii_alpha_len_2_4(t)) continue;
        if (_stricmp(t, "VAL") == 0) continue;
        sd_ascii_lower_copy(t, last, sizeof(last));
    }

    if (last[0]) _snprintf_s(unit_out, 8, _TRUNCATE, "%s", last);
}

static void sd_candidate_unit_from_input(const SdTokList *inT, int tok_index, int in_end, char unit_out[8])
{
    if (unit_out) unit_out[0] = '\0';
    if (!inT || tok_index < 0 || tok_index >= inT->n || tok_index >= in_end || !unit_out) return;

    const char *tok = inT->t[tok_index];
    if (!tok || !tok[0]) return;

    /* digits+letters in one token: "200ms" */
    int i = 0;
    while (tok[i] && isdigit((int)(unsigned char)tok[i])) i++;
    if (i > 0 && tok[i]) {
        const char *suf = tok + i;
        if (sd_tok_is_ascii_alpha_len_2_4(suf)) {
            sd_ascii_lower_copy(suf, unit_out, 8);
            return;
        }
    }

    /* separate unit token: "200" "ms" */
    if ((tok_index + 1) < in_end) {
        const char *u = inT->t[tok_index + 1];
        if (u && u[0] && sd_tok_is_ascii_alpha_len_2_4(u)) {
            sd_ascii_lower_copy(u, unit_out, 8);
            return;
        }
    }
}

static int sd_pick_best_single_int_by_ref_overlap(const SdTokList *inT, int in_start, int in_end,
                                                  const char *ref_sentence,
                                                  const long long *vals, const int *ipos, int vn,
                                                  long long *out_v)
{
    if (out_v) *out_v = 0;
    if (!inT || !ref_sentence || !ref_sentence[0] || !vals || !ipos || vn <= 0) return 0;

    SdTokList R;
    sd_toklist_init(&R);
    if (!sd_tokenize_payload(ref_sentence, &R)) {
        sd_toklist_free(&R);
        return 0;
    }

    int mpos[128];
    int mn = 0;

    for (int i = 0; i < R.n; ++i) {
		const char *rt = R.t[i];
		if (!sd_tok_is_wordlike(rt)) continue;

		/* anti-guess: ignore short ASCII unitlike tokens ("ms","hz","px") as anchors */
		if (sd_tok_is_ascii_short_unitlike(rt)) continue;

        for (int j = in_start; j < in_end; ++j) {
            const char *it = inT->t[j];
            if (!sd_tok_is_wordlike(it)) continue;

            if (_stricmp(rt, it) == 0) { if (mn < 128) mpos[mn++] = j; break; }

            /* fuzzy UTF-8 prefix (3 chars), same as overlap metric */
            {
                char ra[64], ia[64];
                ra[0] = ia[0] = '\0';
				int rch = sd_utf8_copy_prefix_chars_casefold(rt, 3, ra, sizeof(ra));
				int ich = sd_utf8_copy_prefix_chars_casefold(it, 3, ia, sizeof(ia));
				if (rch >= 3 && ich >= 3 && strcmp(ra, ia) == 0) { if (mn < 128) mpos[mn++] = j; break; }
            }
        }
    }

	 char unit_hint[8];
	unit_hint[0] = '\0';
	sd_ref_unit_hint_from_toklist(&R, unit_hint);

	sd_toklist_free(&R);

	if (mn <= 0) return 0;

	int best_i = -1;
	int best_d = 0x7fffffff;

	for (int i = 0; i < vn; ++i) {
		char cand_unit[8];
		cand_unit[0] = '\0';
		sd_candidate_unit_from_input(inT, ipos[i], in_end, cand_unit);

		if (unit_hint[0]) {
			if (!cand_unit[0]) continue;
			if (_stricmp(cand_unit, unit_hint) != 0) continue;
		} else {
			if (cand_unit[0]) continue; /* unitless ref -> do not take unit-attached numbers */
		}

		int dmin = 0x7fffffff;
		for (int k = 0; k < mn; ++k) {
			int d = ipos[i] - mpos[k];
			if (d < 0) d = -d;
			if (d < dmin) dmin = d;
		}
		if (dmin < best_d) { best_d = dmin; best_i = i; }
	}

	if (best_i < 0) return 0;

	if (out_v) *out_v = vals[best_i];
	return 1;
}

static int sd_tok_is_x_token(const char *tok);

static int sd_collect_ints_from_input_span(const SdTokList *inT, int in_start, int in_end,
                                          long long *vals, int vals_cap)
{
    int vn = 0;
    for (int j = in_start; j < in_end && vn < vals_cap; ++j) {
        const char *tok = inT->t[j];
        if (!tok || !tok[0]) continue;

        long long a = 0, b = 0;
        if (sd_tok_parse_int_x_int_token(tok, &a, &b)) {
            if (vn < vals_cap) vals[vn++] = a;
            if (vn < vals_cap) vals[vn++] = b;
            continue;
        }

        long long v = 0;
		if (sd_tok_is_int_token(tok, &v) || sd_tok_parse_int_prefix_unit_token(tok, &v)) {
            /* skip list labels like "Окно 2:" / "Step 2:" -> WORD INT ':' */
            if ((j + 1) < in_end) {
                const char *nxt = inT->t[j + 1];
                const char *prv = (j > in_start) ? inT->t[j - 1] : NULL;
                if (nxt && nxt[0] == ':' && nxt[1] == '\0' &&
                    prv && sd_tok_is_wordlike(prv) &&
                    v >= 0 && v <= 64)
                {
                    continue;
                }
            }
			/* do not treat hex color digits as integer values: "#101010" -> skip "101010" */
            if (j > in_start) {
                const char *prv2 = inT->t[j - 1];
                if (prv2 && prv2[0] == '#' && prv2[1] == '\0') {
                    if (strlen(tok) == 6) {
                        continue;
                    }
                }
            }
			/* handle separated form: "120 x 80" */
            if ((j + 2) < in_end) {
                const char *mid = inT->t[j + 1];
                const char *rhs = inT->t[j + 2];
                long long v2 = 0;
                if (sd_tok_is_x_token(mid) && sd_tok_is_int_token(rhs, &v2)) {
                    vals[vn++] = v;
                    if (vn < vals_cap) vals[vn++] = v2;
                    j += 2;
                    if (vn >= vals_cap) break;
                    continue;
                }
            }

            vals[vn++] = v;
            continue;
        }
    }
    return vn;
}

static int sd_collect_ints_from_input_span_pos(const SdTokList *inT, int in_start, int in_end,
                                               long long *vals, int *ipos, int cap)
{
    int vn = 0;
    for (int j = in_start; j < in_end && vn < cap; ++j) {
        const char *tok = inT->t[j];
        if (!tok || !tok[0]) continue;

        long long x = 0, y = 0;

        /* packed form: "120x80" */
        if (sd_tok_parse_int_x_int_token(tok, &x, &y)) {
            vals[vn] = x; ipos[vn] = j; vn++;
            if (vn < cap) { vals[vn] = y; ipos[vn] = j; vn++; }
            continue;
        }

        if (sd_tok_is_int_token(tok, &x) || sd_tok_parse_int_prefix_unit_token(tok, &x)) {

            /* skip list labels like "Окно 2:" */
            if ((j + 1) < in_end) {
                const char *nxt = inT->t[j + 1];
                const char *prv = (j > in_start) ? inT->t[j - 1] : NULL;
                if (nxt && nxt[0] == ':' && nxt[1] == '\0' &&
                    prv && sd_tok_is_wordlike(prv) &&
                    x >= 0 && x <= 64)
                {
                    continue;
                }
            }
			
			/* do not treat hex color digits as integer values: "#101010" -> skip "101010" */
            if (j > in_start) {
                const char *prv2 = inT->t[j - 1];
                if (prv2 && prv2[0] == '#' && prv2[1] == '\0') {
                    if (strlen(tok) == 6) {
                        continue;
                    }
                }
            }
            
            /* separated form: "120 x 80" */
            if ((j + 2) < in_end) {
                const char *mid = inT->t[j + 1];
                const char *rhs = inT->t[j + 2];
                long long y2 = 0;
                if (sd_tok_is_x_token(mid) && sd_tok_is_int_token(rhs, &y2)) {
                    vals[vn] = x;  ipos[vn] = j;   vn++;
                    if (vn < cap) { vals[vn] = y2; ipos[vn] = j+2; vn++; }
                    j += 2;
                    continue;
                }
            }

            vals[vn] = x;
            ipos[vn] = j;
            vn++;
        }
    }
    return vn;
}

static int sd_tok_is_x_token(const char *tok)
{
    return tok && ((tok[0] == 'x' || tok[0] == 'X') && tok[1] == '\0');
}

static int sd_toklist_span_to_text_for_embed(const SdTokList *T, int start, int len, char *out, size_t cap)
{
    if (!out || cap == 0) return 0;
    out[0] = '\0';

    if (!T || !T->t || T->n <= 0) return 0;
    if (start < 0 || len <= 0 || start + len > T->n) return 0;

    const int end = start + len;
    size_t w = 0;
    int skip_placeholder = 0;

    for (int i = start; i < end; ++i) {
        const char *tok = T->t[i];
        if (!tok || !tok[0]) continue;

        if (!skip_placeholder) {
            if (tok[0] == '{' && tok[1] == '\0' && (i + 1) < end) {
                const char *nxt = T->t[i + 1];
                if (nxt && nxt[0] == '{' && nxt[1] == '\0') {
                    skip_placeholder = 1;
                    i++; // consume 2nd '{'
                    const char *val = "VAL";
                    if (w > 0 && w + 1 < cap) out[w++] = ' ';
                    for (size_t k = 0; val[k] && w + 1 < cap; ++k) out[w++] = val[k];
                    continue;
                }
            }
        } else {
            if (tok[0] == '}' && tok[1] == '\0' && (i + 1) < end) {
                const char *nxt = T->t[i + 1];
                if (nxt && nxt[0] == '}' && nxt[1] == '\0') {
                    skip_placeholder = 0;
                    i++; // consume 2nd '}'
                }
            }
            continue;
        }

        if (strcmp(tok, "\n") == 0) continue;
        if (tok[1] == '\0' && sd_is_punct_char((unsigned char)tok[0])) continue;

        if (w > 0 && w + 1 < cap) out[w++] = ' ';
        for (size_t k = 0; tok[k] && w + 1 < cap; ++k) out[w++] = tok[k];
    }

    if (w >= cap) w = cap - 1;
    out[w] = '\0';
    clar_trim(out);
    return out[0] != '\0';
}

static void sd_strip_placeholders_text(const char *in, char *out, size_t cap)
{
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!in) return;

    size_t oi = 0;
    for (size_t i = 0; in[i] && (oi + 1) < cap; ) {
        if (in[i] == '{' && in[i + 1] == '{') {
            i += 2;
            while (in[i] && !(in[i] == '}' && in[i + 1] == '}')) i++;
            if (in[i]) i += 2;
            continue;
        }
        out[oi++] = in[i++];
    }
    out[oi] = '\0';
    clar_trim(out);
}

static int sd_editparams_set_string(char *edit_payload, size_t cap, const char *key, const char *value)
{
    if (!edit_payload || cap == 0 || !key || !key[0] || !value) return 0;

    char trimmed[64];
    _snprintf_s(trimmed, sizeof(trimmed), _TRUNCATE, "%s", edit_payload);
    clar_trim(trimmed);

    if (tt_is_none_text(trimmed) || !edit_payload[0]) {
        sd_editparams_make_skeleton(edit_payload, cap);
    }

    char *p_str = (char*)clar_strcasestr_local(edit_payload, "STRING:");
    if (!p_str) return 0;

    char *p_after_hdr = strchr(p_str, '\n');
    if (!p_after_hdr) return 0;
    p_after_hdr++;

    const char *end_payload = edit_payload + strlen(edit_payload);

    /* find end of STRING section (next section header or end of payload) */
    char *p_end = (char*)end_payload;
    {
        const char *hdrs[] = { "\nHARDWARE:", "\nNETWORK:", "\nDOMAIN:", "\nNON_GOALS:", "\nCONSTRAINTS:", "\nENVIRONMENT:", "\nEDIT_PARAMETERS:" };
        for (int i = 0; i < (int)(sizeof(hdrs)/sizeof(hdrs[0])); ++i) {
            char *h = (char*)clar_strcasestr_local(p_str, hdrs[i]);
            if (h && h > p_str && h < p_end) p_end = h;
        }
    }

    /* build value with quotes if needed */
    char vbuf[256];
    if (value[0] == '"' || value[0] == '\'') {
        _snprintf_s(vbuf, sizeof(vbuf), _TRUNCATE, "%s", value);
    } else {
        _snprintf_s(vbuf, sizeof(vbuf), _TRUNCATE, "\"%s\"", value);
    }

    char newline[512];
    _snprintf_s(newline, sizeof(newline), _TRUNCATE, "STRING: %s: %s\n", key, vbuf);
    const size_t new_len = strlen(newline);
    if (new_len == 0) return 0;

    /* try replace existing */
    for (char *it = p_after_hdr; it && it < p_end && it[0]; ) {
        if (it[0] == '\n') break;

        char *le = strchr(it, '\n');
        if (!le) le = (char*)end_payload;
        else le++;

        if (_strnicmp(it, "STRING:", 7) == 0) {
            char *ks = it + 7;
            while (ks < le && (*ks == ' ' || *ks == '\t')) ks++;
            char *kc = (char*)memchr(ks, ':', (size_t)(le - ks));
            if (kc) {
                while (kc > ks && (kc[-1] == ' ' || kc[-1] == '\t')) kc--;
                char kbuf[128];
                size_t klen = (size_t)(kc - ks);
                if (klen >= sizeof(kbuf)) klen = sizeof(kbuf) - 1;
                memcpy(kbuf, ks, klen);
                kbuf[klen] = '\0';
                clar_trim(kbuf);

                if (_stricmp(kbuf, key) == 0) {
                    const size_t cur_len = strlen(edit_payload);
                    const size_t old_len = (size_t)(le - it);
                    if (cur_len - old_len + new_len + 1 > cap) return 0;
                    memmove(it + new_len, le, cur_len - (size_t)(le - edit_payload) + 1);
                    memcpy(it, newline, new_len);
                    return 1;
                }
            }
        }

        it = le;
    }

    /* insert new line (replace NONE if present right after header) */
    {
        char *ins = p_after_hdr;
        if (ins < p_end && _strnicmp(ins, "NONE", 4) == 0) {
            char *le = strchr(ins, '\n');
            if (!le) le = ins + strlen(ins);
            else le++;
            const size_t cur_len = strlen(edit_payload);
            const size_t old_len = (size_t)(le - ins);
            if (cur_len - old_len + new_len + 1 > cap) return 0;
            memmove(ins + new_len, le, cur_len - (size_t)(le - edit_payload) + 1);
            memcpy(ins, newline, new_len);
            return 1;
        }

        const size_t cur_len = strlen(edit_payload);
        if (cur_len + new_len + 1 > cap) return 0;
        memmove(ins + new_len, ins, cur_len - (size_t)(ins - edit_payload) + 1);
        memcpy(ins, newline, new_len);
        return 1;
    }
}

typedef struct SdScalarKV {
    char key[128];
    long long v;
} SdScalarKV;

static int sd_scalar_kv_find(const SdScalarKV *M, int mn, const char *key, long long *out_v)
{
    if (out_v) *out_v = 0;
    if (!M || mn <= 0 || !key || !key[0]) return 0;
    for (int i = 0; i < mn; ++i) {
        if (_stricmp(M[i].key, key) == 0) {
            if (out_v) *out_v = M[i].v;
            return 1;
        }
    }
    return 0;
}

static void sd_scalar_kv_upsert(SdScalarKV *M, int *io_mn, int cap, const char *key, long long v)
{
    if (!M || !io_mn || cap <= 0 || !key || !key[0]) return;
    for (int i = 0; i < *io_mn; ++i) {
        if (_stricmp(M[i].key, key) == 0) { M[i].v = v; return; }
    }
    if (*io_mn < cap) {
        _snprintf_s(M[*io_mn].key, sizeof(M[*io_mn].key), _TRUNCATE, "%s", key);
        M[*io_mn].v = v;
        (*io_mn)++;
    }
}

/* Build scalar key->value map from ONE input sentence using value-rules (struct_value_rules).
   This follows v2.2: context=sentence, multiple ints -> all, groups by one-word separation,
   and KEY picked by most similar stored value-example (embedding). */
static int sd_build_scalar_map_from_input_sentence(
    sqlite3 *db,
    const char *section_name,
    const SdTokList *inT,
    int sent_start,
    int sent_len,
    SdScalarKV *outM,
    int cap)
{
    if (!db || !section_name || !section_name[0] || !inT || sent_len <= 0 || !outM || cap <= 0) return 0;

    SdValueItem *items = NULL;
    int in = 0;
    if (!sd_collect_value_items_apply(inT, sent_start, sent_len, &items, &in)) return 0;

    SdValueGroup *G = NULL;
    int gn = 0;
    if (!sd_build_value_groups(inT, items, in, &G, &gn)) {
        free(items);
        return 0;
    }

    int mn = 0;

    for (int gi = 0; gi < gn; ++gi) {
        SdValueGroup g = G[gi];
        if (g.count <= 0) continue;

        char *ctx_seq = sd_build_value_ctx_seq_generic(inT, sent_start, sent_len, items, g.first_item, g.count);
        if (!ctx_seq || !ctx_seq[0]) { if (ctx_seq) free(ctx_seq); continue; }

        void *m = NULL;
        const char *mname = "";
        int dim = 0;
        if (!structurize_delta_select_embed_model(ctx_seq, &m, &mname, &dim) || !m || dim <= 0) {
            free(ctx_seq);
            continue;
        }

        float *qvec = (float*)malloc(sizeof(float) * (size_t)dim);
        if (!qvec) { free(ctx_seq); continue; }

        if (!llama_client_get_embeddings(m, ctx_seq, qvec)) {
            free(qvec);
            free(ctx_seq);
            continue;
        }

        char picked_key[256];
        sqlite3_int64 rule_id = 0;
        float prob = 0.f;

        int ok = structurize_value_pick_key(db, section_name, mname, dim, qvec, g.count,
                                            picked_key, sizeof(picked_key),
                                            &rule_id, &prob);

        /* mirror the same fallback as value-apply: GOAL can use REQUIREMENTS rules */
        if (!ok && section_name && _stricmp(section_name, "GOAL") == 0) {
            ok = structurize_value_pick_key(db, "REQUIREMENTS", mname, dim, qvec, g.count,
                                            picked_key, sizeof(picked_key),
                                            &rule_id, &prob);
        }

        free(qvec);
        free(ctx_seq);

        if (!ok || !picked_key[0]) continue;

        char keybuf[256];
        _snprintf_s(keybuf, sizeof(keybuf), _TRUNCATE, "%s", picked_key);

        char *keys[16];
        int kc = 0;
        char *ctx = NULL;
        for (char *p = strtok_s(keybuf, "|", &ctx); p && kc < 16; p = strtok_s(NULL, "|", &ctx)) {
            clar_trim(p);
            if (p[0]) keys[kc++] = p;
        }
        if (kc != g.count) continue;

        for (int k = 0; k < g.count; ++k) {
            const SdValueItem *it = &items[g.first_item + k];
            sd_scalar_kv_upsert(outM, &mn, cap, keys[k], it->value);
        }
    }

    free(G);
    free(items);
    return mn;
}

static int sd_sentence_has_string_candidate(const SdTokList *T, int start, int end)
{
    if (!T || start < 0 || end > T->n || start >= end) return 0;

    char path[512];
    path[0] = '\0';
    if (sd_span_extract_first_win_path(T, start, end, path, sizeof(path))) return 1;

    char hex[16];
    hex[0] = '\0';
    if (sd_span_extract_first_hex_color_or_bare(T, start, end, 1, hex)) return 1;

    char up[64];
    up[0] = '\0';
    if (sd_span_extract_first_upper_token(T, start, end, up, sizeof(up))) return 1;

    return 0;
}

static int sd_editparams_prefill_from_input_text(const char *input_text, const char *io_struct,
                                                 char *edit_payload, size_t edit_cap)
{
    if (!input_text || !input_text[0] || !io_struct || !io_struct[0] || !edit_payload || edit_cap == 0) return 0;

    // 1) tokenize input and split into sentences
    SdTokList inT;
    sd_toklist_init(&inT);
    if (!sd_tokenize_payload(input_text, &inT)) {
        sd_toklist_free(&inT);
        return 0;
    }

    SdSpan *in_sp = NULL;
    int in_sn = 0;
    sd_collect_sentence_spans(&inT, &in_sp, &in_sn);
    if (!in_sp || in_sn <= 0) {
        if (in_sp) free(in_sp);
        sd_toklist_free(&inT);
        return 0;
    }

    // precompute hashsets for input sentences (for fallback matching)
    unsigned long long **in_set = (unsigned long long**)calloc((size_t)in_sn, sizeof(*in_set));
    int *in_n = (int*)calloc((size_t)in_sn, sizeof(*in_n));
    if (!in_set || !in_n) {
        free(in_set); free(in_n);
        free(in_sp);
        sd_toklist_free(&inT);
        return 0;
    }
    for (int i = 0; i < in_sn; ++i) {
        (void)sd_collect_sentence_hashset_ci(&inT, in_sp[i].start, in_sp[i].len, &in_set[i], &in_n[i]);
    }

    typedef struct SdPHGroup {
        const char *ss;
        const char *se;
        char keys[16][128];
        int kn;
    } SdPHGroup;

    SdPHGroup G[64];
    int gn = 0;
    memset(G, 0, sizeof(G));

    // 2) scan placeholders {{KEY}} in io_struct and group by sentence
    const char *p = io_struct;
    while ((p = strstr(p, "{{")) != NULL) {
        const char *k0 = p + 2;
        const char *k1 = strstr(k0, "}}");
        if (!k1) break;

        char key[128];
        int kl = 0;
        const char *q = k0;
        while (q < k1 && kl < (int)sizeof(key) - 1) {
            unsigned char c = (unsigned char)*q;
            if (!sd_is_key_char(c)) { kl = 0; break; }
            key[kl++] = (char)c;
            q++;
        }
        key[kl] = '\0';

        if (kl > 0) {
            const char *ss = NULL, *se = NULL;
            sd_find_sentence_bounds(io_struct, p, &ss, &se);

            int gi = -1;
            for (int i = 0; i < gn; ++i) {
                if (G[i].ss == ss && G[i].se == se) { gi = i; break; }
            }
            if (gi < 0 && gn < 64) {
                gi = gn++;
                G[gi].ss = ss;
                G[gi].se = se;
                G[gi].kn = 0;
            }

            if (gi >= 0 && G[gi].kn < 16) {
                /* keep placeholder arity/order (do not dedupe); important for mapping multiple values in one sentence */
                _snprintf_s(G[gi].keys[G[gi].kn], sizeof(G[gi].keys[G[gi].kn]), _TRUNCATE, "%s", key);
                G[gi].kn++;
            }
        }

        p = k1 + 2;
    }

    int applied = 0;

    // 3) for each sentence-group: match to input sentence, extract ints, assign by placeholder order
    for (int gi = 0; gi < gn; ++gi) {
        const char *ss = G[gi].ss;
        const char *se = G[gi].se;
        if (!ss || !se || se <= ss) continue;
        if (G[gi].kn <= 0) continue;
        
        /* classify keys using learned key types (no name-based heuristics) */
        int key_is_string[16];
        int need_scalar = 0;
        int need_string = 0;
        for (int kk = 0; kk < G[gi].kn && kk < 16; ++kk) {
            const char *kname = G[gi].keys[kk];

            int is_str = 1;

            /* If we already learned this key as STRING -> never allow scalar-prefill to overwrite it */
            if (!structurize_keytype_hint_is_string(g_db_structurize, kname)) {
                const int has_value_rule =
                    structurize_value_rules_contains_key(g_db_structurize, "REQUIREMENTS", kname);
                is_str = has_value_rule ? 0 : 1;
            }

            /* If current payload already has a STRING key -> keep it STRING */
            if (sd_editparams_has_string_key(edit_payload, kname)) is_str = 1;

            key_is_string[kk] = is_str;
            if (is_str) need_string++;
            else need_scalar++;
        }

        char ref[1024];
        size_t rn = (size_t)(se - ss);
        if (rn >= sizeof(ref)) rn = sizeof(ref) - 1;
        memcpy(ref, ss, rn);
        ref[rn] = '\0';
        clar_trim(ref);
        if (!ref[0]) continue;

        /* strip {{...}} placeholders from the reference sentence for matching */
        char ref_clean[1024];
        ref_clean[0] = '\0';
        sd_strip_placeholders_text(ref, ref_clean, sizeof(ref_clean));
        const char *ref_use = (ref_clean[0] ? ref_clean : ref);
		
		/* embedding-friendly ref: tokenize + replace {{...}} -> VAL */
        char ref_emb[1024];
        ref_emb[0] = '\0';
        {
            SdTokList RT;
            sd_tokenize_payload(ref, &RT);
            (void)sd_toklist_span_to_text_for_embed(&RT, 0, RT.n, ref_emb, sizeof(ref_emb));
            sd_toklist_free(&RT);
        }
        const char *ref_for_emb = (ref_emb[0] ? ref_emb : ref_use);

        const int dbg = structurize_debug_enabled();
        if (dbg) {
            fprintf(stderr, "[SKYNET][STRUCTURIZE][PREFILL] ref='%s'\n", ref_for_emb);
            fprintf(stderr, "[SKYNET][STRUCTURIZE][PREFILL] keys:");
            for (int kk = 0; kk < G[gi].kn; ++kk) fprintf(stderr, " %s", G[gi].keys[kk]);
            fprintf(stderr, "\n");
        }

        int best = -1;
        float best_sim = 0.f;
        float best_esim = -1.0f;

        // primary: lexical overlap (cheap + deterministic)
		for (int si = 0; si < in_sn; ++si) {
			const int a = in_sp[si].start;
			const int b = a + in_sp[si].len;

			/* anti-guess: sentence must contain enough values */
			long long tmpv[8];
			int tmpvn = sd_collect_ints_from_input_span(&inT, a, b, tmpv, 8);

            if (need_scalar > 0 && tmpvn < need_scalar) continue;
			
			if (need_string > 0 && need_scalar == 0) {
				if (!sd_sentence_has_string_candidate(&inT, a, b)) continue;
			}

			float sim = sd_sentence_overlap_sim_ci(&inT, a, b, ref_use);
			if (sim > best_sim) {
				best_sim = sim;
				best = si;
			}
		}
		/* anti-guess: if there is zero lexical overlap, do not accept primary match */
        if (best >= 0 && best_sim <= 0.0001f) best = -1;
        
        // fallback: hashset similarity (handles small paraphrases better)
        if (best < 0 || best_sim < 0.75f) {
            SdTokList R;
            sd_toklist_init(&R);
            if (sd_tokenize_payload(ref_use, &R)) {
                unsigned long long *ref_set = NULL;
                int ref_n = 0;
                (void)sd_collect_sentence_hashset_ci(&R, 0, R.n, &ref_set, &ref_n);

                float best_score = 0.0f;
                int bi = -1;
                float bj = 0.0f;
                float bcov = 0.0f;
                int binter = 0;

                if (ref_n >= 4) {
                    for (int si = 0; si < in_sn; ++si) {
                        if (in_n[si] < 4) continue;
						
						if (need_string > 0 && need_scalar == 0) {
							const int a = in_sp[si].start;
							const int b = a + in_sp[si].len;
							if (!sd_sentence_has_string_candidate(&inT, a, b)) continue;
						}

                        const float jacc = sd_jaccard_sets(ref_set, ref_n, in_set[si], in_n[si]);
                        const float cov  = sd_coverage_sets(ref_set, ref_n, in_set[si], in_n[si]);
                        const int   inter = sd_intersection_count_sets(ref_set, ref_n, in_set[si], in_n[si]);

                        const float score = jacc + cov * 0.15f;
                        if (score > best_score) {
                            best_score = score;
                            bi = si;
                            bj = jacc;
                            bcov = cov;
                            binter = inter;
                        }
                    }
                }

                if (bi >= 0 && (bj >= 0.70f || (binter >= 4 && bcov >= 0.45f))) {
                    best = bi;
                    best_sim = 0.75f;
                } else {
                    /* relaxed: keep primary best if there is some overlap */
                    if (best_sim < 0.25f) best = -1;
                }

                free(ref_set);
            }
            sd_toklist_free(&R);
        }

		/* fallback2: embedding similarity (context=sentence) + anti-guess by value-arity */
		if (best < 0) {
			void *m = NULL;
			const char *mname = NULL;
			int dim = 0;

			if (structurize_delta_select_embed_model(ref_for_emb, &m, &mname, &dim) && m && dim > 0) {
				float *qvec = (float*)malloc(sizeof(float) * (size_t)dim);
				float *svec = (float*)malloc(sizeof(float) * (size_t)dim);

				if (qvec && svec && llama_client_get_embeddings(m, ref_for_emb, qvec)) {
					int bi = -1;
					float second = -1.0f;
					int qual = 0;

					for (int si = 0; si < in_sn; ++si) {
						const int a = in_sp[si].start;
						const int b = a + in_sp[si].len;
						
						if (need_string > 0 && need_scalar == 0) {
							if (!sd_sentence_has_string_candidate(&inT, a, b)) continue;
						}

						long long tmpv[8];
						int tmpvn = sd_collect_ints_from_input_span(&inT, a, b, tmpv, 8);

						if (need_scalar > 0 && tmpvn < need_scalar) continue;

						char in_txt[1024];
						if (!sd_toklist_span_to_text_for_embed(&inT, a, in_sp[si].len, in_txt, sizeof(in_txt)))
							continue;
						if (!llama_client_get_embeddings(m, in_txt, svec))
							continue;

						qual++;

						const float sim = structurize_delta_cosine_sim(qvec, svec, dim);
						if (sim > best_esim) {
							second = best_esim;
							best_esim = sim;
							bi = si;
						} else if (sim > second) {
							second = sim;
						}
					}

					/* accept only if unambiguous */
					if (bi >= 0) {
						if (qual <= 1) {
							best = bi;
						} else if (best_esim >= 0.20f && (best_esim - second) >= 0.05f) {
							best = bi;
						}
					}
				}

				free(qvec);
				free(svec);
			}
		}
		
		if (dbg) {
            fprintf(stderr, "[SKYNET][STRUCTURIZE][PREFILL] match best=%d lex=%.3f emb=%.3f\n", best, best_sim, best_esim);
        }

        // still weak -> do not guess
        if (best < 0) {
            if (dbg) fprintf(stderr, "[SKYNET][STRUCTURIZE][PREFILL] skip: no sentence match\n");
            continue;
        }

        int in_a = in_sp[best].start;
        int in_b = in_a + in_sp[best].len;
		
		SdScalarKV smap[64];
        memset(smap, 0, sizeof(smap));
        int smn = sd_build_scalar_map_from_input_sentence(g_db_structurize, "REQUIREMENTS", &inT, in_a, in_sp[best].len, smap, 64);
        
        /* current prefill supports at most one string value (hex) */
        if (need_string > 1) {
            continue;
        }

        /* collect ints with token positions */
        long long vals[32];
        int ipos[32];
        int vn = sd_collect_ints_from_input_span_pos(&inT, in_a, in_b, vals, ipos, 32);

		/* extract hex/path/upper-token (string candidates) */
		char hex[16];
		int hashex = sd_span_extract_first_hex_color_or_bare(&inT, in_a, in_b, 1, hex);

		char path[512];
		path[0] = '\0';
		int haspath = sd_span_extract_first_win_path(&inT, in_a, in_b, path, sizeof(path));

        char up[64];
        up[0] = '\0';
        int hasup = 0;
        if (vn == 0 && !haspath && !hashex) {
            hasup = sd_span_extract_first_upper_token(&inT, in_a, in_b, up, sizeof(up));
        }

        const int hasstr = (haspath || hashex || hasup);

		/* single-key promotion: if we extracted only STRING but key was treated as scalar, promote deterministically */
		if (G[gi].kn == 1 && need_scalar == 1 && need_string == 0 && vn == 0 && hasstr) {
			key_is_string[0] = 1;
			need_scalar = 0;
			need_string = 1;
		}

		const int avail = vn + (hasstr ? 1 : 0);
		const int need  = need_scalar + need_string;
		
		if (dbg) {
            fprintf(stderr, "[SKYNET][STRUCTURIZE][PREFILL] extracted: vn=%d hex=%s avail=%d need=%d\n",
                    vn, (hashex ? hex : "-"), avail, need);
        }

        if (avail < need) continue;
        if (need_scalar > 0 && vn < need_scalar) continue;

		/* choose scalar values (anti-guess for single-key when multiple numbers exist) */
		long long picked[32];
		int scalar_ambiguous = 0;

		if (need_scalar > 0) {
			if (need_scalar == 1 && vn > 1) {
				long long best_v = 0;
				if (sd_pick_best_single_int_by_ref_overlap(&inT, in_a, in_b, ref_use, vals, ipos, vn, &best_v)) {
					picked[0] = best_v;
				} else {
					scalar_ambiguous = 1;
				}
			} else {
				int best_i = 0;
				int best_span = 0x7fffffff;
				for (int i = 0; i + need_scalar - 1 < vn; ++i) {
					const int span = ipos[i + need_scalar - 1] - ipos[i];
					if (span < best_span) { best_span = span; best_i = i; }
				}
				for (int i = 0; i < need_scalar; ++i) picked[i] = vals[best_i + i];
			}
		}

        /* apply scalars in placeholder order, skipping STRING-typed keys */
        char used_keys[16][128];
        int used_n = 0;

        int si = 0;
        for (int k = 0; k < G[gi].kn && k < 16; ++k) {
            long long mapped_v = 0;
            const int has_map = sd_scalar_kv_find(smap, smn, G[gi].keys[k], &mapped_v);

            if (key_is_string[k]) continue;

			long long use_v = 0;
			if (has_map) {
				use_v = mapped_v;
			} else {
				if (scalar_ambiguous) {
					continue; /* anti-guess: do not assign without overlap */
				}
				if (si >= need_scalar) break;
				use_v = picked[si];
				si++;
			}

            int seen = 0;
            for (int u = 0; u < used_n; ++u) {
                if (_stricmp(used_keys[u], G[gi].keys[k]) == 0) { seen = 1; break; }
            }
            if (!seen) {
                if (used_n < 16) {
                    _snprintf_s(used_keys[used_n], sizeof(used_keys[used_n]), _TRUNCATE, "%s", G[gi].keys[k]);
                    used_n++;
                }
                applied += sd_editparams_set_scalar_int(edit_payload, edit_cap, G[gi].keys[k], use_v) ? 1 : 0;
            }
        }

		/* apply STRING (prefer path > hex > upper-token) */
		if (need_string == 1 && hasstr) {
			const char *sval = NULL;
			if (haspath && path[0]) sval = path;
			else if (hashex && hex[0]) sval = hex;
			else if (hasup && up[0]) sval = up;

			if (sval && sval[0]) {
				for (int k = 0; k < G[gi].kn && k < 16; ++k) {
					if (!key_is_string[k]) continue;

					int seen = 0;
					for (int u = 0; u < used_n; ++u) {
						if (_stricmp(used_keys[u], G[gi].keys[k]) == 0) { seen = 1; break; }
					}
					if (!seen) {
						(void)sd_editparams_delete_scalar_key(edit_payload, edit_cap, G[gi].keys[k]);
						applied += sd_editparams_set_string(edit_payload, edit_cap, G[gi].keys[k], sval) ? 1 : 0;
					}
					break;
				}
			}
		}
	}

    for (int i = 0; i < in_sn; ++i) free(in_set[i]);
    free(in_set);
    free(in_n);

    free(in_sp);
    sd_toklist_free(&inT);

    return applied;
}

static int sd_tok_is_digits_only(const char *tok)
{
    if (!tok || !tok[0]) return 0;
    for (const unsigned char *p = (const unsigned char*)tok; *p; ++p) {
        if (*p < '0' || *p > '9') return 0;
    }
    return 1;
}

static unsigned long long sd_hash_tok_ci_simple(const char *tok)
{
    char buf[128];
    size_t n = tok ? strlen(tok) : 0;
    if (n >= sizeof(buf)) n = sizeof(buf) - 1;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)tok[i];
        buf[i] = (char)tolower((int)c);
    }
    buf[n] = 0;
    return struct_ml_hash_str(buf);
}

static int sd_cmp_u64_qsort(const void *pa, const void *pb)
{
    const unsigned long long a = *(const unsigned long long *)pa;
    const unsigned long long b = *(const unsigned long long *)pb;
    if (a < b) return -1;
    if (a > b) return  1;
    return 0;
}

static int sd_collect_sentence_hashset_ci(const SdTokList *T, int start, int len,
                                         unsigned long long **out_arr, int *out_n)
{
    if (!out_arr || !out_n) return 0;
    *out_arr = NULL;
    *out_n = 0;
    if (!T || !T->t || T->n <= 0 || len <= 0) return 1;

    const int end = start + len;
    if (start < 0 || end > T->n) return 0;

    unsigned long long *arr = (unsigned long long*)malloc(sizeof(unsigned long long) * (size_t)len);
    if (!arr) return 0;

    int an = 0;
    int skip_placeholder = 0;

    for (int i = start; i < end; ++i) {
        const char *tok = T->t[i];
        if (!tok || !tok[0]) continue;

        if (!skip_placeholder) {
            if (tok[0] == '{' && tok[1] == '\0' && (i + 1) < end) {
                const char *nxt = T->t[i + 1];
                if (nxt && nxt[0] == '{' && nxt[1] == '\0') {
                    skip_placeholder = 1;
                    i++; // consume second '{'
                    continue;
                }
            }
        } else {
            if (tok[0] == '}' && tok[1] == '\0' && (i + 1) < end) {
                const char *nxt = T->t[i + 1];
                if (nxt && nxt[0] == '}' && nxt[1] == '\0') {
                    skip_placeholder = 0;
                    i++; // consume second '}'
                }
            }
            continue;
        }

        if (strcmp(tok, "\\n") == 0) continue;

        if (tok[1] == '\0' && sd_is_punct_char((unsigned char)tok[0])) continue;

        if (sd_tok_is_digits_only(tok)) continue;

        arr[an++] = sd_hash_tok_ci_simple(tok);
    }

    if (an <= 0) {
        free(arr);
        return 1;
    }

	qsort(arr, (size_t)an, sizeof(unsigned long long), sd_cmp_u64_qsort);

    int wn = 0;
    for (int i = 0; i < an; ++i) {
        if (i == 0 || arr[i] != arr[i - 1]) arr[wn++] = arr[i];
    }

    *out_arr = arr;
    *out_n = wn;
    return 1;
}

static float sd_jaccard_sets(const unsigned long long *a, int an,
                             const unsigned long long *b, int bn)
{
    if (!a || !b || an <= 0 || bn <= 0) return 0.0f;

    int i = 0, j = 0, inter = 0;
    while (i < an && j < bn) {
        if (a[i] == b[j]) { inter++; i++; j++; }
        else if (a[i] < b[j]) i++;
        else j++;
    }

    const int uni = an + bn - inter;
    if (uni <= 0) return 0.0f;
    return (float)inter / (float)uni;
}

static int sd_toklist_replace_range_with_range(SdTokList *dst, int dst_start, int dst_len,
                                               const SdTokList *src, int src_start, int src_len)
{
    if (!dst || !src || dst_start < 0 || dst_len < 0 || src_start < 0 || src_len < 0) return 0;
    if (dst_start + dst_len > dst->n) return 0;
    if (src_start + src_len > src->n) return 0;

    SdTokList out;
    sd_toklist_init(&out);
    sd_toklist_reserve(&out, dst->n - dst_len + src_len + 8);

    for (int i = 0; i < dst_start; ++i) {
        if (!sd_toklist_push_copy(&out, dst->t[i])) { sd_toklist_free(&out); return 0; }
    }
    for (int i = 0; i < src_len; ++i) {
        if (!sd_toklist_push_copy(&out, src->t[src_start + i])) { sd_toklist_free(&out); return 0; }
    }
    for (int i = dst_start + dst_len; i < dst->n; ++i) {
        if (!sd_toklist_push_copy(&out, dst->t[i])) { sd_toklist_free(&out); return 0; }
    }

    sd_toklist_free(dst);
    *dst = out;
    return 1;
}

static int sd_sync_section_sentences_from_input_text(const char *input_text, char *section_payload, size_t cap)
{
	    /* Disabled: sentence-sync overwrote canonical placeholders and created duplicates. */
    return 0;

    if (!input_text || !input_text[0]) return 0;
    if (!section_payload || cap == 0 || !section_payload[0]) return 0;

    SdTokList inT;
    sd_toklist_init(&inT);
    if (!sd_tokenize_payload(input_text, &inT)) {
        sd_toklist_free(&inT);
        return 0;
    }

    SdSpan *in_sp = NULL;
    int in_sn = 0;
    sd_collect_sentence_spans(&inT, &in_sp, &in_sn);

    SdTokList secT;
    sd_toklist_init(&secT);
    if (!sd_tokenize_payload(section_payload, &secT)) {
        sd_toklist_free(&inT);
        if (in_sp) free(in_sp);
        sd_toklist_free(&secT);
        return 0;
    }

    SdSpan *sec_sp = NULL;
    int sec_sn = 0;
    sd_collect_sentence_spans(&secT, &sec_sp, &sec_sn);

    if (!in_sp || in_sn <= 0 || !sec_sp || sec_sn <= 0) {
        sd_toklist_free(&inT);
        sd_toklist_free(&secT);
        if (in_sp) free(in_sp);
        if (sec_sp) free(sec_sp);
        return 0;
    }

    const int IN_MAX = (in_sn > 64) ? 64 : in_sn;

    unsigned long long **in_set = (unsigned long long**)calloc((size_t)IN_MAX, sizeof(*in_set));
    int *in_n = (int*)calloc((size_t)IN_MAX, sizeof(*in_n));
    if (!in_set || !in_n) {
        if (in_set) free(in_set);
        if (in_n) free(in_n);
        sd_toklist_free(&inT);
        sd_toklist_free(&secT);
        free(in_sp);
        free(sec_sp);
        return 0;
    }

    for (int i = 0; i < IN_MAX; ++i) {
        (void)sd_collect_sentence_hashset_ci(&inT, in_sp[i].start, in_sp[i].len, &in_set[i], &in_n[i]);
    }

    int best_i[64];
    for (int j = 0; j < 64; ++j) best_i[j] = -1;

    const float TH_J     = 0.88f;
    const float TH_COV   = 0.75f;
    const int   TH_INTER = 3;
    const float TH_EMB   = 0.88f;

    /* embedding caches (lazy) */
    char **in_txt = NULL;
    float **in_vec = NULL;
    void *memb = NULL;
    const char *mname = NULL;
    int dim = 0;

    const int SEC_MAX = (sec_sn > 64) ? 64 : sec_sn;

    for (int j = 0; j < SEC_MAX; ++j) {
        unsigned long long *sec_set = NULL;
        int sec_n = 0;

        (void)sd_collect_sentence_hashset_ci(&secT, sec_sp[j].start, sec_sp[j].len, &sec_set, &sec_n);

        int bi = -1;
        float bj = 0.0f;
        float bcov = 0.0f;
        int binter = 0;

        for (int i = 0; i < IN_MAX; ++i) {
            const float jacc  = sd_jaccard_sets(in_set[i], in_n[i], sec_set, sec_n);
            const float cov   = sd_coverage_sets(in_set[i], in_n[i], sec_set, sec_n);
            const int   inter = sd_intersection_count_sets(in_set[i], in_n[i], sec_set, sec_n);

            /* pick best by coverage first (robust to placeholders), then by jaccard */
            if (cov > bcov || (cov == bcov && jacc > bj)) {
                bcov = cov;
                bj = jacc;
                binter = inter;
                bi = i;
            }
        }

        if (sec_set) free(sec_set);

        if (bi >= 0 && (bj >= TH_J || (binter >= TH_INTER && bcov >= TH_COV))) {
            best_i[j] = bi;
            continue;
        }

        /* embedding fallback: только если в предложении есть плейсхолдер или цифры */
        int need_embed = 0;
        const int sa = sec_sp[j].start;
        const int sb = sa + sec_sp[j].len;

        for (int k = sa; k < sb; ++k) {
            const char *tok = secT.t[k];
            if (!tok || !tok[0]) continue;

            if (tok[0] == '{' && tok[1] == '\0' && (k + 1) < sb) {
                const char *nxt = secT.t[k + 1];
                if (nxt && nxt[0] == '{' && nxt[1] == '\0') { need_embed = 1; break; }
            }
            if (tok[0] >= '0' && tok[0] <= '9') { need_embed = 1; break; }
        }
        if (!need_embed) continue;

        char sec_emb[2048];
        sec_emb[0] = '\0';
        sd_toklist_span_to_text_for_embed(&secT, sec_sp[j].start, sec_sp[j].len, sec_emb, sizeof(sec_emb));
        if (!sec_emb[0]) continue;

        if (!memb || dim <= 0) {
            if (!structurize_delta_select_embed_model(sec_emb, &memb, &mname, &dim) || !memb || dim <= 0) {
                memb = NULL;
                dim = 0;
                continue;
            }
        }

        if (!in_txt) {
            in_txt = (char**)calloc((size_t)IN_MAX, sizeof(char*));
            in_vec = (float**)calloc((size_t)IN_MAX, sizeof(float*));
        }
        if (!in_txt || !in_vec) continue;

        float *svec = (float*)malloc(sizeof(float) * (size_t)dim);
        if (!svec) continue;
        if (!llama_client_get_embeddings(memb, sec_emb, svec)) {
            free(svec);
            continue;
        }

        int ebest = -1;
        float ebest_sim = 0.0f;

        for (int i = 0; i < IN_MAX; ++i) {
            if (!in_txt[i]) {
                in_txt[i] = (char*)calloc(1, 2048);
                if (in_txt[i]) {
                    sd_toklist_span_to_text_for_embed(&inT, in_sp[i].start, in_sp[i].len, in_txt[i], 2048);
                }
            }
            if (!in_txt[i] || !in_txt[i][0]) continue;

            if (!in_vec[i]) {
                in_vec[i] = (float*)malloc(sizeof(float) * (size_t)dim);
                if (!in_vec[i] || !llama_client_get_embeddings(memb, in_txt[i], in_vec[i])) {
                    if (in_vec[i]) free(in_vec[i]);
                    in_vec[i] = NULL;
                    continue;
                }
            }

            const float sim = structurize_delta_cosine_sim(svec, in_vec[i], dim);
            if (sim > ebest_sim) { ebest_sim = sim; ebest = i; }
        }

        free(svec);

        if (ebest >= 0 && ebest_sim >= TH_EMB) {
            best_i[j] = ebest;
        }
    }

    /* apply replacements from end → start */
    int changed = 0;
    for (int j = SEC_MAX - 1; j >= 0; --j) {
        if (best_i[j] < 0) continue;

        if (sd_toklist_replace_range_with_range(&secT,
                                                sec_sp[j].start, sec_sp[j].len,
                                                &inT,
                                                in_sp[best_i[j]].start, in_sp[best_i[j]].len)) {
            changed = 1;
        }
    }

    if (changed) {
        sd_toklist_to_payload(section_payload, cap, &secT);
    }

    if (in_txt) {
        for (int i = 0; i < IN_MAX; ++i) {
            if (in_txt[i]) free(in_txt[i]);
            if (in_vec && in_vec[i]) free(in_vec[i]);
        }
        free(in_txt);
        free(in_vec);
    }

    for (int i = 0; i < IN_MAX; ++i) {
        if (in_set[i]) free(in_set[i]);
    }
    free(in_set);
    free(in_n);

    sd_toklist_free(&inT);
    sd_toklist_free(&secT);
    free(in_sp);
    free(sec_sp);

    return changed;
}

static void sd_environment_force_canon_inplace(char *io_struct, size_t io_sz,
                                               const char *syntax_hint, int ring_hint)
{
    if (!io_struct || io_sz == 0) return;

    enum { CAP = 65536 };
    char *blk = (char *)calloc(8, CAP);
    if (!blk) return;

    char *goal = blk + 0 * CAP;
    char *req  = blk + 1 * CAP;
    char *inp  = blk + 2 * CAP;
    char *outp = blk + 3 * CAP;
    char *cons = blk + 4 * CAP;
    char *env  = blk + 5 * CAP;
    char *edit = blk + 6 * CAP;
    char *ng   = blk + 7 * CAP;

    tr_copy_section_payload(io_struct, "GOAL:", goal, CAP);
    tr_copy_section_payload(io_struct, "REQUIREMENTS:", req, CAP);
    tr_copy_section_payload(io_struct, "INPUT:", inp, CAP);
    tr_copy_section_payload(io_struct, "OUTPUT:", outp, CAP);
    tr_copy_section_payload(io_struct, "CONSTRAINTS:", cons, CAP);
    tr_copy_section_payload(io_struct, "ENVIRONMENT:", env, CAP);
    tr_copy_section_payload(io_struct, "EDIT_PARAMETERS:", edit, CAP);
    tr_copy_section_payload(io_struct, "NON_GOALS:", ng, CAP);

    char os_val[64];
    os_val[0] = '\0';
	
	char arch_val[64];
	arch_val[0] = '\0';
	
    /* try to keep OS if it was present in any form */
    const char *p = env;
    while (p && *p) {
        const char *ls = p;
        const char *le = strchr(p, '\n');
        if (!le) le = p + strlen(p);

        int n = (int)(le - ls);
        if (n > 0) {
            char line[256];
            if (n > (int)(sizeof(line) - 1)) n = (int)(sizeof(line) - 1);
            memcpy(line, ls, (size_t)n);
            line[n] = '\0';
            clar_trim(line);

            if (!_strnicmp(line, "OS:", 3)) {
                char *v = line + 3;
                while (*v == ' ' || *v == '\t') v++;
                if (v[0]) {
                    _snprintf_s(os_val, sizeof(os_val), _TRUNCATE, "%s", v);
                    //Скан arch
                }
            }
            if (!_strnicmp(line, "ARCH:", 5)) {
				char *v = line + 5;
				while (*v == ' ' || *v == '\t') v++;
				if (v[0]) {
					_snprintf_s(arch_val, sizeof(arch_val), _TRUNCATE, "%s", v);
					// не break: OS и ARCH могут быть в разных строках
				}
			}
        }

        p = (*le ? le + 1 : le);
    }

	if (!os_val[0]) _snprintf_s(os_val, sizeof(os_val), _TRUNCATE, "ANY");
	if (_stricmp(os_val, "any") == 0)  _snprintf_s(os_val, sizeof(os_val), _TRUNCATE, "ANY");
	if (_stricmp(os_val, "none") == 0) _snprintf_s(os_val, sizeof(os_val), _TRUNCATE, "NONE");

	if (!arch_val[0]) _snprintf_s(arch_val, sizeof(arch_val), _TRUNCATE, "ANY");
	if (_stricmp(arch_val, "any") == 0)  _snprintf_s(arch_val, sizeof(arch_val), _TRUNCATE, "ANY");
	if (_stricmp(arch_val, "none") == 0) _snprintf_s(arch_val, sizeof(arch_val), _TRUNCATE, "NONE");

    char env_new[CAP];
	_snprintf_s(env_new, sizeof(env_new), _TRUNCATE,
				"SYNTAX: %s\n"
				"RING: %d\n"
				"OS: %s\n"
				"ARCH: %s\n",
				(syntax_hint ? syntax_hint : ""),
				ring_hint,
				os_val,
				arch_val);

    tt_rebuild_struct(io_struct, io_sz, goal, req, inp, outp, cons, env_new, edit, ng);

    free(blk);
}

static int structurize_value_try_apply_rules(sqlite3 *db, const char *input_text, char *io_struct, size_t io_sz,
                                             sqlite3_int64 apply_log_id, int *io_ord,
                                             SdSectionLocks *locks)
{
    if (!db || !io_struct || io_sz == 0) return 0;
    char value_fp[32];
    value_fp[0] = '\0';
    {
        char *canon = sd_canonize_alloc(io_struct);
        if (!canon) canon = _strdup(io_struct ? io_struct : "");
        if (canon) {
            structurize_delta_hex64(struct_ml_hash_str(canon), value_fp);
            free(canon);
        }
        if (!value_fp[0]) _snprintf_s(value_fp, sizeof(value_fp), _TRUNCATE, "0");
    }
		
    char goal[65536], req[65536], inp[65536], outp[65536], cons[65536], env[65536], edit[65536], ng[65536];
    goal[0]=req[0]=inp[0]=outp[0]=cons[0]=env[0]=edit[0]=ng[0]=0;

    tr_copy_section_payload(io_struct, "GOAL:", goal, sizeof(goal));
    tr_copy_section_payload(io_struct, "REQUIREMENTS:", req, sizeof(req));
    tr_copy_section_payload(io_struct, "INPUT:", inp, sizeof(inp));
    tr_copy_section_payload(io_struct, "OUTPUT:", outp, sizeof(outp));
    tr_copy_section_payload(io_struct, "CONSTRAINTS:", cons, sizeof(cons));
    tr_copy_section_payload(io_struct, "ENVIRONMENT:", env, sizeof(env));
    tr_copy_section_payload(io_struct, "EDIT_PARAMETERS:", edit, sizeof(edit));
    tr_copy_section_payload(io_struct, "NON_GOALS:", ng, sizeof(ng));
	
    int pre = 0;
    int sync_changed = 0;

    // Prefill: если APPLY/шаблоны заменили фразы на плейсхолдеры, но “сырые” значения пропали из секций,
    // берём их из исходного input_text и проставляем в EDIT_PARAMETERS детерминированно.
    if (input_text && input_text[0]) {
        pre = sd_editparams_prefill_from_input_text(input_text, io_struct, edit, sizeof(edit));
        if (pre > 0 && structurize_debug_enabled()) {
            fprintf(stderr, "[SKYNET][STRUCTURIZE][AUTO] value-prefill-from-input applied: n=%d\n", pre);
        }
    }

    // Sentence-sync: подтягиваем “живые” предложения из input_text (без привязки к value-правилам).
    if (input_text && input_text[0]) {
        sync_changed += sd_sync_section_sentences_from_input_text(input_text, goal, sizeof(goal));
        sync_changed += sd_sync_section_sentences_from_input_text(input_text, req,  sizeof(req));
        sync_changed += sd_sync_section_sentences_from_input_text(input_text, outp, sizeof(outp));
    }

    int total = 0;

	total += structurize_value_apply_to_section(db, "GOAL",         goal, sizeof(goal), edit, sizeof(edit), value_fp, apply_log_id, io_ord, locks);
	total += structurize_value_apply_to_section(db, "REQUIREMENTS", req,  sizeof(req),  edit, sizeof(edit), value_fp, apply_log_id, io_ord, locks);
	total += structurize_value_apply_to_section(db, "INPUT",        inp,  sizeof(inp),  edit, sizeof(edit), value_fp, apply_log_id, io_ord, locks);
	total += structurize_value_apply_to_section(db, "OUTPUT",       outp, sizeof(outp), edit, sizeof(edit), value_fp, apply_log_id, io_ord, locks);
	total += structurize_value_apply_to_section(db, "CONSTRAINTS",  cons, sizeof(cons), edit, sizeof(edit), value_fp, apply_log_id, io_ord, locks);
	total += structurize_value_apply_to_section(db, "NON_GOALS",    ng,   sizeof(ng),   edit, sizeof(edit), value_fp, apply_log_id, io_ord, locks);

    if (total > 0 || pre > 0 || sync_changed > 0) {
        tt_rebuild_struct(io_struct, io_sz, goal, req, inp, outp, cons, env, edit, ng);

        if (total > 0) {
            fprintf(stderr, "[SKYNET][STRUCTURIZE][AUTO] value-transformer applied: n=%d\n", total);
        } else if (structurize_debug_enabled()) {
            fprintf(stderr, "[SKYNET][STRUCTURIZE][AUTO] value-prefill/sentence-sync applied: pre=%d sync=%d\n", pre, sync_changed);
        }
    }
    return total;
}

static int structurize_key_types_learn_from_after_struct(sqlite3 *db, const char *after_struct);

static int structurize_value_rules_learn_from_after_struct(sqlite3 *db, const char *after_struct)
{
    if (!db || !after_struct || !after_struct[0]) return 0;

    const struct { const char *hdr; const char *sec; } S[] = {
        {"GOAL:", "GOAL"},
        {"REQUIREMENTS:", "REQUIREMENTS"},
        {"INPUT:", "INPUT"},
        {"OUTPUT:", "OUTPUT"},
        {"CONSTRAINTS:", "CONSTRAINTS"},
        {"NON_GOALS:", "NON_GOALS"},
    };

    const char *sql_ins =
        "INSERT OR IGNORE INTO struct_value_rules(key, section, embed_model, embed_dim, ctx_seq, embedding) "
        "VALUES(?,?,?,?,?,?);";

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql_ins, -1, &st, NULL) != SQLITE_OK) {
        return 0;
    }

    int learned = 0;
	
	    /* v2.2: value rules are integer-only -> learn only for keys that are SCALAR in EDIT_PARAMETERS */
    char edit_payload[65536];
    edit_payload[0] = '\0';
    tr_copy_section_payload(after_struct, "EDIT_PARAMETERS:", edit_payload, sizeof(edit_payload));

    char scalar_keys[512][128];
    int skn = 0;
    memset(scalar_keys, 0, sizeof(scalar_keys));

    int mode = 0; /* 1=SCALAR, 2=STRING */
    const char *ep = edit_payload;
    while (*ep) {
        const char *ls = ep;
        while (*ep && *ep != '\n') ep++;
        const char *le = ep;
        if (*ep == '\n') ep++;

        char line[512];
        size_t ln = (size_t)(le - ls);
        if (ln >= sizeof(line)) ln = sizeof(line) - 1;
        memcpy(line, ls, ln);
        line[ln] = '\0';
        clar_trim(line);
        if (!line[0]) continue;

        if (_stricmp(line, "SCALAR:") == 0) { mode = 1; continue; }
        if (_stricmp(line, "STRING:") == 0) { mode = 2; continue; }

        int line_mode = mode;
        const char *s = line;

        if (!_strnicmp(s, "SCALAR:", 7)) { line_mode = 1; s = strchr(s, ':'); if (s) s++; }
        else if (!_strnicmp(s, "INT:", 4)) { line_mode = 1; s = strchr(s, ':'); if (s) s++; }
        else if (!_strnicmp(s, "STRING:", 7)) { line_mode = 2; s = strchr(s, ':'); if (s) s++; }

        if (line_mode != 1 || !s) continue;

        while (*s == ' ' || *s == '\t') s++;
        const char *c = strchr(s, ':');
        if (!c) continue;

        const char *after = c + 1;
        while (*after == ' ' || *after == '\t') after++;
        if (!*after) { mode = 0; continue; }

        char key[128];
        size_t kn = (size_t)(c - s);
        if (kn >= sizeof(key)) kn = sizeof(key) - 1;
        memcpy(key, s, kn);
        key[kn] = '\0';
        clar_trim(key);
        if (!key[0]) continue;

        int dup = 0;
        for (int i = 0; i < skn; ++i) {
            if (_stricmp(scalar_keys[i], key) == 0) { dup = 1; break; }
        }
        if (!dup && skn < 512) {
            _snprintf_s(scalar_keys[skn], sizeof(scalar_keys[skn]), _TRUNCATE, "%s", key);
            skn++;
        }
    }
    
    for (int si = 0; si < (int)(sizeof(S)/sizeof(S[0])); ++si) {
        char payload[65536];
        payload[0] = 0;
        tr_copy_section_payload(after_struct, S[si].hdr, payload, sizeof(payload));
        clar_trim(payload);
        if (!payload[0] || tt_is_none_text(payload)) continue;

        SdTokList T;
        sd_tokenize_payload(payload, &T);

        SdSpan *spans = NULL;
        int spn = 0;
        sd_collect_sentence_spans(&T, &spans, &spn);

        for (int sxi = 0; sxi < spn; ++sxi) {
            int sent_start = spans[sxi].start;
            int sent_len   = spans[sxi].len;

            SdValueItem *items = NULL;
            int in = 0;
            if (!sd_collect_value_items_learn(&T, sent_start, sent_len, &items, &in)) continue;

            SdValueGroup *G = NULL;
            int gn = 0;
            if (!sd_build_value_groups(&T, items, in, &G, &gn)) {
                free(items);
                continue;
            }

            for (int gi = 0; gi < gn; ++gi) {
                SdValueGroup g = G[gi];
                if (g.count <= 0) continue;
				
				/* learn value-rules only if all keys in this group are SCALAR keys */
                int all_scalar = 1;
                for (int k = 0; k < g.count; ++k) {
                    const char *kstr = items[g.first_item + k].key;
                    if (!kstr || !kstr[0]) { all_scalar = 0; break; }

                    int ok = 0;
                    for (int z = 0; z < skn; ++z) {
                        if (_stricmp(scalar_keys[z], kstr) == 0) { ok = 1; break; }
                    }
                    if (!ok) { all_scalar = 0; break; }
                }
                if (!all_scalar) continue;
                
                // key = "k1|k2|..."
                char key_join[256];
                key_join[0] = 0;
                for (int k = 0; k < g.count; ++k) {
                    const char *kstr = items[g.first_item + k].key;
                    if (!kstr || !kstr[0]) { key_join[0] = 0; break; }
                    if (k > 0) strncat_s(key_join, sizeof(key_join), "|", _TRUNCATE);
                    strncat_s(key_join, sizeof(key_join), kstr, _TRUNCATE);
                }
                if (!key_join[0]) continue;

                char *ctx_seq = sd_build_value_ctx_seq_generic(&T, sent_start, sent_len, items, g.first_item, g.count);
                if (!ctx_seq || !ctx_seq[0]) {
                    if (ctx_seq) free(ctx_seq);
                    continue;
                }

                void *m = NULL;
                const char *mname = "";
                int dim = 0;
                if (!structurize_delta_select_embed_model(ctx_seq, &m, &mname, &dim) || !m || dim <= 0) {
                    free(ctx_seq);
                    continue;
                }

                float *vec = (float *)malloc(sizeof(float) * (size_t)dim);
                if (!vec) { free(ctx_seq); continue; }
                if (!llama_client_get_embeddings(m, ctx_seq, vec)) {
                    free(vec);
                    free(ctx_seq);
                    continue;
                }

                sqlite3_reset(st);
                sqlite3_clear_bindings(st);

                sqlite3_bind_text(st, 1, key_join, -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(st, 2, S[si].sec, -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(st, 3, mname, -1, SQLITE_TRANSIENT);
                sqlite3_bind_int(st, 4, dim);
                sqlite3_bind_text(st, 5, ctx_seq, -1, SQLITE_TRANSIENT);
                sqlite3_bind_blob(st, 6, vec, (int)(sizeof(float) * (size_t)dim), SQLITE_TRANSIENT);

                if (sqlite3_step(st) == SQLITE_DONE) {
                    learned += 1;
                }

                free(vec);
                free(ctx_seq);
            }

            free(G);
            free(items);
        }

        free(spans);
        sd_toklist_free(&T);
    }

    sqlite3_finalize(st);

    if (learned > 0) {
        fprintf(stderr, "[SKYNET][STRUCTURIZE][LEARN] value-rules learned: n=%d\n", learned);
    }
	const int kt_n = structurize_key_types_learn_from_after_struct(db, after_struct);
	if (kt_n > 0) {
		fprintf(stderr, "[SKYNET][STRUCTURIZE][LEARN] key-types learned: n=%d\n", kt_n);
	}

    return learned;
}

static int structurize_key_types_learn_from_after_struct(sqlite3 *db, const char *after_struct)
{
    if (!db || !after_struct) return 0;

    char edit_payload[65536];
    edit_payload[0] = '\0';
    tr_copy_section_payload(after_struct, "EDIT_PARAMETERS:", edit_payload, sizeof(edit_payload));

    int mode = 0; /* 1=SCALAR, 2=STRING */
    int learned = 0;

    const char *p = edit_payload;
    while (*p) {
        const char *ls = p;
        while (*p && *p != '\n') p++;
        const char *le = p;
        if (*p == '\n') p++;

        char line[512];
        size_t ln = (size_t)(le - ls);
        if (ln >= sizeof(line)) ln = sizeof(line) - 1;
        memcpy(line, ls, ln);
        line[ln] = '\0';
        clar_trim(line);

        if (!line[0]) continue;

        /* block headers */
        if (_stricmp(line, "SCALAR:") == 0) { mode = 1; continue; }
        if (_stricmp(line, "STRING:") == 0) { mode = 2; continue; }

        int line_mode = mode;
        const char *s = line;

        /* inline typed lines like "SCALAR: key: val" / "STRING: key: val" / "INT: key: val" */
        if (!_strnicmp(s, "SCALAR:", 7)) { line_mode = 1; s = strchr(s, ':'); if (s) s++; }
        else if (!_strnicmp(s, "INT:", 4)) { line_mode = 1; s = strchr(s, ':'); if (s) s++; }
        else if (!_strnicmp(s, "STRING:", 7)) { line_mode = 2; s = strchr(s, ':'); if (s) s++; }

        if (line_mode == 0 || !s) continue;

        while (*s == ' ' || *s == '\t') s++;

        const char *c = strchr(s, ':');
        if (!c) continue;

        /* header-like "HARDWARE:" => value missing => stop consuming as key/value */
        {
            const char *after = c + 1;
            while (*after == ' ' || *after == '\t') after++;
            if (!*after) { mode = 0; continue; }
        }

        char key[128];
        size_t kn = (size_t)(c - s);
        if (kn >= sizeof(key)) kn = sizeof(key) - 1;
        memcpy(key, s, kn);
        key[kn] = '\0';
        clar_trim(key);

        if (!key[0]) continue;

        learned += structurize_keytype_note(db, key, (line_mode == 2));
    }

    return learned;
}

static void structurize_auto_apply_transforms_impl(const char *input_text,
                                                   const char *syntax_hint,
                                                   int ring_hint,
                                                   char *io_struct,
                                                   size_t io_sz,
                                                   int do_log)
{
    if (!io_struct || io_sz == 0) return;

    if (!g_db_structurize) {
        fprintf(stderr, "[SKYNET][STRUCTURIZE][AUTO] WARNING: Structurize DB not open; delta/apply disabled.\n");
        return;
    }

    if (!structurize_ops_ensure_schema(g_db_structurize)) {
        fprintf(stderr, "[SKYNET][STRUCTURIZE][AUTO] WARNING: ensure_schema failed; skip auto.\n");
        return;
    }
	
	SdSectionLocks locks;
	sd_locks_init(&locks);

    sqlite3_int64 program_id =
        structurize_delta_try_apply_programs(g_db_structurize,
                                             input_text,
                                             syntax_hint,
                                             ring_hint,
                                             io_struct,
                                             io_sz,
                                             do_log,
                                             &locks);

	if (program_id <= 0) {
		fprintf(stderr, "[SKYNET][STRUCTURIZE][AUTO] fallback: apply templates + wordctx/value (no delta-program)\n");

		// 1) templates
		{
			const size_t tmp_sz = 65536 * 2;
			char *tmp = (char*)calloc(1, tmp_sz);
			if (tmp) {
				sd_trunc_str(io_struct, tmp, tmp_sz);
				(void)structurize_try_apply_transfer_templates(g_db_structurize, tmp, tmp_sz);
				if (hybrid_validate_clarified_output(tmp)) {
					sd_trunc_str(tmp, io_struct, io_sz);
				}
				free(tmp);
			}
		}

		// 2) even without delta-programs, still allow these deterministic transformers
		//    IMPORTANT: keep apply-log so penalty-after-fix can disable bad wordctx/value ops
		sqlite3_int64 fb_log_id = 0;
		int fb_next_ord = 0;

		if (do_log && input_text && input_text[0]) {
			char *canon_in = sd_canonize_alloc(input_text);
			if (!canon_in) canon_in = _strdup(input_text);

			if (canon_in) {
				char fp[32];
				structurize_delta_hex64(struct_ml_hash_str(canon_in), fp);
				free(canon_in);

				fb_log_id = structurize_apply_log_begin(
					g_db_structurize,
					fp,
					"fallback",
					0,
					(syntax_hint ? syntax_hint : ""),
					ring_hint
				);
			}
		}

		(void)structurize_wordctx_try_apply_rules(
			g_db_structurize, io_struct, io_sz,
			fb_log_id, (fb_log_id > 0 ? &fb_next_ord : NULL),
			&locks
		);

        SdSectionLocks vlocks;
        sd_locks_init(&vlocks);

        (void)structurize_value_try_apply_rules(
            g_db_structurize, input_text, io_struct, io_sz,
            fb_log_id, (fb_log_id > 0 ? &fb_next_ord : NULL),
            &vlocks
        );
        
        sd_environment_force_canon_inplace(io_struct, io_sz, syntax_hint, ring_hint);
        
		// 3) final validation gate
		if (!hybrid_validate_clarified_output(io_struct)) {
			/* keep as-is */
		}
		return;
	}

    /* Step 2.3: word-change transformer by ctx(4+4) embedding */
    sqlite3_int64 wl_log_id = 0;
	int wl_next_ord = 0;
	if (do_log) {
		wl_log_id = structurize_apply_log_get_latest_and_next_ord(g_db_structurize, input_text, syntax_hint, ring_hint, &wl_next_ord);
	}
	(void)structurize_wordctx_try_apply_rules(g_db_structurize, io_struct, io_sz, wl_log_id, do_log ? &wl_next_ord : NULL, &locks);

    /* Step 2.2: value transformer (replaces values inside EDIT_PARAMETERS) */
    sqlite3_int64 vl_log_id = 0;
	int vl_next_ord = 0;
	if (do_log) {
		vl_log_id = structurize_apply_log_get_latest_and_next_ord(g_db_structurize, input_text, syntax_hint, ring_hint, &vl_next_ord);
	}
	SdSectionLocks vlocks;
    sd_locks_init(&vlocks);

    (void)structurize_value_try_apply_rules(g_db_structurize, input_text, io_struct, io_sz, vl_log_id, do_log ? &vl_next_ord : NULL, &vlocks);
	
	sd_environment_force_canon_inplace(io_struct, io_sz, syntax_hint, ring_hint);
	    
    /* Final validation gate */
    if (!hybrid_validate_clarified_output(io_struct)) {
        /* If the transform pipeline broke structure (shouldn't), do nothing further. */
    }
}

static void structurize_auto_apply_transforms(const char *input_text,
                                             const char *syntax_hint,
                                             int ring_hint,
                                             char *io_struct,
                                             size_t io_sz)
{
    structurize_auto_apply_transforms_impl(input_text, syntax_hint, ring_hint, io_struct, io_sz, 1);
}

static void structurize_auto_reapply_transforms_after_fix(const char *input_text,
                                                         const char *syntax_hint,
                                                         int ring_hint,
                                                         char *io_struct,
                                                         size_t io_sz)
{
    structurize_auto_apply_transforms_impl(input_text, syntax_hint, ring_hint, io_struct, io_sz, 0);
}

int rag_structurize_reapply_transforms_after_fix(const char *input_text,
                                                const char *syntax_hint,
                                                int ring_hint,
                                                char *io_struct,
                                                size_t io_sz)
{
    if (!io_struct || io_sz == 0) return 0;
    if (!input_text) input_text = "";
    if (!syntax_hint) syntax_hint = "";
    if (ring_hint < 0) ring_hint = 0;

    structurize_auto_reapply_transforms_after_fix(input_text, syntax_hint, ring_hint, io_struct, io_sz);
    return 1;
}
// Baseline builder: deterministic canonical skeleton (NO ML transforms).
// Used for: (1) initial baseline Structurize, (2) baseline BEFORE in delta-learning.
static void structurize_build_baseline_only(
    const char *initial_desc,
    const char *syntax_hint,
    int         ring_hint,
    char       *out,
    size_t      out_sz)
{
    if (!out || out_sz == 0) return;
    out[0] = '\0';

    char ring_buf[32];
    _snprintf_s(ring_buf, sizeof(ring_buf), _TRUNCATE, "%d", ring_hint);
	
	char edit_skel[2048];
    edit_skel[0] = '\0';
    sd_editparams_make_skeleton(edit_skel, sizeof(edit_skel));

    _snprintf_s(out, out_sz, _TRUNCATE,
        "CLARIFIED_SPEC:\n"
        "GOAL:\n"
        "%s\n"
        "\n"
        "REQUIREMENTS:\n"
        "NONE\n"
        "\n"
        "INPUT:\n"
        "NONE\n"
        "\n"
        "OUTPUT:\n"
        "NONE\n"
        "\n"
        "CONSTRAINTS:\n"
        "NONE\n"
        "\n"
        "ENVIRONMENT:\n"
        "SYNTAX: %s\n"
        "RING: %s\n"
        "OS: ANY\n"
		"ARCH: ANY\n"
        "\n"
        "EDIT_PARAMETERS:\n"
        "%s\n"
        "\n"
        "NON_GOALS:\n"
        "NONE\n",
        (initial_desc ? initial_desc : ""),
        (syntax_hint ? syntax_hint : ""),
        ring_buf,
        edit_skel
    );
}

#define STRUCTURIZE_MODE_AUTO             (-1)  // baseline + ML transforms, NO LLM
#define STRUCTURIZE_MODE_BASELINE_ONLY    (-2)  // pure baseline, NO ML transforms (for BEFORE in delta-learning)

int rag_structurize_task_description(const char *initial_desc,
                                    const char *syntax_hint,
                                    int ring_hint,
                                    int use_external_llm,
                                    char *out_struct,
                                    size_t out_struct_size)
{
    if (!out_struct || out_struct_size == 0) return -1;
    out_struct[0] = '\0';

		// baseline: детерминированная минимальная структура (канон v2.0)
	char baseline[65536];
	structurize_build_baseline_only(initial_desc, syntax_hint, ring_hint, baseline, sizeof(baseline));

	// Modes:
	//  - STRUCTURIZE_MODE_BASELINE_ONLY (-2): pure baseline (no ML transforms). Used only for BEFORE in delta-learning.
	//  - STRUCTURIZE_MODE_AUTO (-1): baseline + ML-predicted deterministic transforms (no LLM).
	if (use_external_llm == STRUCTURIZE_MODE_BASELINE_ONLY) {
		strncpy(out_struct, baseline, out_struct_size - 1);
		out_struct[out_struct_size - 1] = '\0';
		return 1;
	}
	if (use_external_llm == STRUCTURIZE_MODE_AUTO) {
		strncpy(out_struct, baseline, out_struct_size - 1);
		out_struct[out_struct_size - 1] = '\0';
		structurize_auto_apply_transforms(
			initial_desc ? initial_desc : "",
			syntax_hint ? syntax_hint : "",
			ring_hint,
			out_struct,
			out_struct_size
		);
		return 1;
	}

    // LLM repair (разрешён): baseline+AUTO (детерминированные transforms) передаём как “лог” для минимальных правок
    char seed[65536];
    seed[0] = '\0';
    strncpy(seed, baseline, sizeof(seed) - 1);
    seed[sizeof(seed) - 1] = '\0';

    // Детерминированная часть всегда идёт ДО LLM (по принятой схеме “ML выбирает transforms, не правит текст”)
    structurize_auto_apply_transforms(
        initial_desc ? initial_desc : "",
        syntax_hint ? syntax_hint : "",
        ring_hint,
        seed,
        sizeof(seed)
    );

    char repaired[65536];
    repaired[0] = '\0';

    int rc = clar_structurize_task_description(
        initial_desc ? initial_desc : "",
        seed,
        syntax_hint ? syntax_hint : "",
        ring_hint,
        use_external_llm,
        repaired,
        sizeof(repaired)
    );

    if (rc > 0 && repaired[0]) {
        strncpy(out_struct, repaired, out_struct_size - 1);
        out_struct[out_struct_size - 1] = '\0';
        structurize_auto_apply_transforms(initial_desc ? initial_desc : "", syntax_hint ? syntax_hint : "", ring_hint, out_struct, out_struct_size);
        return 1;
    }

	// Deterministic fallback: baseline. If not explicit BASELINE_ONLY, still apply ML deterministic transforms.
	strncpy(out_struct, baseline, out_struct_size - 1);
	out_struct[out_struct_size - 1] = '\0';
	if (use_external_llm != STRUCTURIZE_MODE_BASELINE_ONLY) {
		structurize_auto_apply_transforms(
			initial_desc ? initial_desc : "",
			syntax_hint ? syntax_hint : "",
			ring_hint,
			out_struct,
			out_struct_size
		);
	}
	return 1;
}

static int sd_editparams_collect_scalar_pairs(const char *edit_payload,
                                              char keys[][128], long long vals[],
                                              int maxn)
{
    if (!edit_payload || !edit_payload[0] || !keys || !vals || maxn <= 0) return 0;

    const char *p_scalar = clar_strcasestr_local(edit_payload, "SCALAR:");
    if (!p_scalar) return 0;

    const char *p_string = NULL;
    {
        const char *scan = p_scalar;
        while ((scan = clar_strcasestr_local(scan, "STRING:")) != NULL) {
            if (scan == edit_payload || scan[-1] == '\n') { p_string = scan; break; }
            scan += 6;
        }
    }

    const char *p = strchr(p_scalar, '\n');
    if (!p) return 0;
    p++; // after "SCALAR:\n"

    const char *end = p_string ? p_string : (edit_payload + strlen(edit_payload));

    int n = 0;

    while (p < end && n < maxn) {
        const char *ls = p;
        const char *le = strchr(p, '\n');
        if (!le || le > end) le = end;

        int len = (int)(le - ls);
        if (len > 0) {
            char line[512];
            if (len >= (int)sizeof(line)) len = (int)sizeof(line) - 1;
            memcpy(line, ls, (size_t)len);
            line[len] = '\0';
            clar_trim(line);

            if (line[0] && !tt_is_none_text(line)) {
                char *c1 = strchr(line, ':');
                if (c1) {
                    char *c2 = strchr(c1 + 1, ':');
                    if (c2) {
                        char key[128];
                        int klen = (int)(c2 - (c1 + 1));
                        if (klen > 0) {
                            if (klen >= (int)sizeof(key)) klen = (int)sizeof(key) - 1;
                            memcpy(key, c1 + 1, (size_t)klen);
                            key[klen] = '\0';
                            clar_trim(key);

                            char *vstr = c2 + 1;
                            clar_trim(vstr);

                            if (key[0] && vstr[0]) {
                                char *endp = NULL;
                                long long v = strtoll(vstr, &endp, 10);
                                if (endp && endp != vstr) {
                                    _snprintf_s(keys[n], 128, _TRUNCATE, "%s", key);
                                    vals[n] = v;
                                    n++;
                                }
                            }
                        }
                    }
                }
            }
        }

        if (le >= end) break;
        p = le + 1;
    }

    return n;
}

static int sd_val_count(long long v, const long long *vals, int n)
{
    int c = 0;
    for (int i = 0; i < n; ++i) if (vals[i] == v) c++;
    return c;
}

static int sd_first_idx_for_val(long long v, const long long *vals, int n, int start)
{
    for (int i = start; i < n; ++i) if (vals[i] == v) return i;
    return -1;
}

static int sd_canonize_payload_numbers_to_placeholders(char *payload, size_t cap,
                                                       char keys[][128], const long long *vals, int n)
{
    if (!payload || cap == 0) return 0;

    char tmpchk[32];
    _snprintf_s(tmpchk, sizeof(tmpchk), _TRUNCATE, "%s", payload);
    clar_trim(tmpchk);
    if (!payload[0] || tt_is_none_text(tmpchk) || n <= 0) return 0;

    SdTokList T;
    sd_toklist_init(&T);
    if (!sd_tokenize_payload(payload, &T)) {
        sd_toklist_free(&T);
        return 0;
    }

    SdTokList out;
    sd_toklist_init(&out);
    sd_toklist_reserve(&out, T.n + 8);

    int changed = 0;

    for (int i = 0; i < T.n; ++i) {
        const char *tok = T.t[i];
        if (!tok) continue;

        long long a = 0, b = 0;
        if (sd_tok_parse_int_x_int_token(tok, &a, &b)) {
            int ia = sd_first_idx_for_val(a, vals, n, 0);
            int ib = sd_first_idx_for_val(b, vals, n, 0);

            if (ia >= 0 && ib >= 0) {
                if (a == b) {
                    int ib2 = sd_first_idx_for_val(b, vals, n, ia + 1);
                    if (ib2 >= 0) ib = ib2;
                }

                char repl[320];
                _snprintf_s(repl, sizeof(repl), _TRUNCATE, "{{%s}}x{{%s}}", keys[ia], keys[ib]);
                sd_toklist_push_copy(&out, repl);
                changed++;
                continue;
            }
        }

        if (sd_tok_is_int_token(tok, &a)) {
            if (sd_val_count(a, vals, n) == 1) {
                int ia = sd_first_idx_for_val(a, vals, n, 0);
                if (ia >= 0) {
                    char repl[256];
                    _snprintf_s(repl, sizeof(repl), _TRUNCATE, "{{%s}}", keys[ia]);
                    sd_toklist_push_copy(&out, repl);
                    changed++;
                    continue;
                }
            }
        }

        sd_toklist_push_copy(&out, tok);
    }

    if (changed > 0) {
        sd_toklist_to_payload(payload, cap, &out);
    }

    sd_toklist_free(&out);
    sd_toklist_free(&T);
    return changed;
}

static int sd_after_struct_canonize_values_by_editparams(const char *after_struct,
                                                         char *out, size_t out_sz)
{
    if (!out || out_sz == 0) return 0;
    out[0] = '\0';
    if (!after_struct || !after_struct[0]) return 0;

    enum { SD_CANON_SEC_CAP = 65536 };
    char *sec_blk = (char*)calloc(8, (size_t)SD_CANON_SEC_CAP);
    if (!sec_blk) return 0;

    char *goal = sec_blk + 0 * SD_CANON_SEC_CAP;
    char *req  = sec_blk + 1 * SD_CANON_SEC_CAP;
    char *inp  = sec_blk + 2 * SD_CANON_SEC_CAP;
    char *outp = sec_blk + 3 * SD_CANON_SEC_CAP;
    char *cons = sec_blk + 4 * SD_CANON_SEC_CAP;
    char *env  = sec_blk + 5 * SD_CANON_SEC_CAP;
    char *edit = sec_blk + 6 * SD_CANON_SEC_CAP;
    char *ng   = sec_blk + 7 * SD_CANON_SEC_CAP;

    tr_copy_section_payload(after_struct, "GOAL:", goal, SD_CANON_SEC_CAP);
    tr_copy_section_payload(after_struct, "REQUIREMENTS:", req, SD_CANON_SEC_CAP);
    tr_copy_section_payload(after_struct, "INPUT:", inp, SD_CANON_SEC_CAP);
    tr_copy_section_payload(after_struct, "OUTPUT:", outp, SD_CANON_SEC_CAP);
    tr_copy_section_payload(after_struct, "CONSTRAINTS:", cons, SD_CANON_SEC_CAP);
    tr_copy_section_payload(after_struct, "ENVIRONMENT:", env, SD_CANON_SEC_CAP);
    tr_copy_section_payload(after_struct, "EDIT_PARAMETERS:", edit, SD_CANON_SEC_CAP);
    tr_copy_section_payload(after_struct, "NON_GOALS:", ng, SD_CANON_SEC_CAP);

    char keys[64][128];
    long long vals[64];
    int n = sd_editparams_collect_scalar_pairs(edit, keys, vals, 64);

    int changed = 0;
    if (n > 0) {
        changed += sd_canonize_payload_numbers_to_placeholders(goal, SD_CANON_SEC_CAP, keys, vals, n);
        changed += sd_canonize_payload_numbers_to_placeholders(req,	 SD_CANON_SEC_CAP, keys, vals, n);
        changed += sd_canonize_payload_numbers_to_placeholders(inp,	 SD_CANON_SEC_CAP, keys, vals, n);
        changed += sd_canonize_payload_numbers_to_placeholders(outp, SD_CANON_SEC_CAP, keys, vals, n);
        changed += sd_canonize_payload_numbers_to_placeholders(cons, SD_CANON_SEC_CAP, keys, vals, n);
        changed += sd_canonize_payload_numbers_to_placeholders(ng,	 SD_CANON_SEC_CAP, keys, vals, n);
    }

    tt_rebuild_struct(out, out_sz, goal, req, inp, outp, cons, env, edit, ng);
    free(sec_blk);
    return changed;
}

// Public: коммит обучающего примера (веса/примеры) — вызывается только после Clarify или ручной правки
int rag_structurize_learn_commit_pair(const char *input_text,
                                      const char *syntax_hint,
                                      int ring_hint,
                                      const char *before_struct,
                                      const char *after_struct)
{
    if (!g_db_structurize) return 0;

    // Print DB path once
    {
        static int printed = 0;
        if (!printed) {
            char cwd[MAX_PATH];
            if (_getcwd(cwd, MAX_PATH)) {
                fprintf(stderr, "[SKYNET][STRUCTURIZE][LEARN] DB file: %s\\Database\\skynet_structurize.db\n", cwd);
            }
            printed = 1;
        }
    }

    if (!input_text || !input_text[0]) return 0;
    if (!after_struct || !after_struct[0]) return 0;

    if (!structurize_ops_ensure_schema(g_db_structurize)) return 0;
	
	char after_canon[65536];
    after_canon[0] = '\0';
    (void)sd_after_struct_canonize_values_by_editparams(after_struct ? after_struct : "", after_canon, sizeof(after_canon));
    const char *after_learn = after_canon[0] ? after_canon : (after_struct ? after_struct : "");

    // Project S v2.1: BEFORE всегда = AUTO baseline (весь input в GOAL, без догадок)
    char before_buf[65536];
    structurize_build_baseline_only(input_text, syntax_hint, ring_hint, before_buf, sizeof(before_buf));
    const char *before = before_buf;
    (void)before_struct; // игнорируем внешний before_struct: LEARN учится строго на AUTO->FIX

    // 1) Store audit pair (before/after)
    sqlite3_stmt *st = NULL;
    const char *sql_ins =
        "INSERT INTO struct_fix_pairs(input_text,before_struct,after_struct,syntax_hint,ring_hint,created_at)"
        "VALUES(?,?,?,?,?,CURRENT_TIMESTAMP);";

    if (sqlite3_prepare_v2(g_db_structurize, sql_ins, -1, &st, NULL) != SQLITE_OK) {
        LEARN_DB_PREP_FAIL("insert_fix_pair");
        return 0;
    }

    sqlite3_bind_text(st, 1, input_text ? input_text : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, before ? before : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, after_struct ? after_struct : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, syntax_hint ? syntax_hint : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 5, ring_hint);

    if (sqlite3_step(st) != SQLITE_DONE) {
        LEARN_DB_STEP_FAIL("insert_fix_pair");
        sqlite3_finalize(st);
        return 0;
    }
    sqlite3_finalize(st);

    sqlite3_int64 pair_id = sqlite3_last_insert_rowid(g_db_structurize);
        if (pair_id <= 0) {
        return 0;
    }

    // Step1.4: persist embeddings artifacts for this AUTO->FIX pair
    structurize_fix_pair_save_basic_embeddings(g_db_structurize, pair_id, input_text ? input_text : "", after_learn);
    structurize_fix_pair_save_span_embeddings_from_pair(g_db_structurize, pair_id, before, after_learn);

	// Step3: penalty update (0%) for ops that were applied in AUTO but fully corrected in FIX
    structurize_penalty_update_from_last_log(g_db_structurize,
                                             input_text ? input_text : "",
                                             syntax_hint ? syntax_hint : "",
                                             ring_hint,
                                             after_struct ? after_struct : "");

     // 2) Learn Delta Program (AUTO->FIX): операции + guards
    int ops = 0;
    sqlite3_int64 prog_id = structurize_delta_learn_program_from_pair(
        g_db_structurize,
        pair_id,
        input_text ? input_text : "",
        syntax_hint ? syntax_hint : "",
        ring_hint,
        before,
        after_learn,
        &ops
    );

    int ret = 0;

    if (prog_id > 0 && ops > 0) {
        fprintf(stderr, "[SKYNET][STRUCTURIZE][LEARN] delta_program learned: pair_id=%lld program_id=%lld ops=%d\n",
                (long long)pair_id, (long long)prog_id, ops);
        ret = ops; // кол-во операций
    } else {
        // Backward-compat: если delta не смогла построиться — пробуем старые transfer templates
        sqlite3_int64 tpl_id = structurize_tt_learn_from_pair(g_db_structurize, pair_id, before, after_learn);
        if (tpl_id > 0) {
            fprintf(stderr, "[SKYNET][STRUCTURIZE][LEARN] transfer_template learned (fallback): pair_id=%lld template_id=%lld\n",
                    (long long)pair_id, (long long)tpl_id);
            ret = 1;
        } else {
            fprintf(stderr, "[SKYNET][STRUCTURIZE][LEARN] no delta/tt learned: pair_id=%lld\n", (long long)pair_id);
            ret = 0;
        }
    }

    // Step2 LEARN: value-transformer rules из FIX (после FIX в секциях видны {{key}})
    (void)structurize_value_rules_learn_from_after_struct(g_db_structurize, after_learn);

    return ret;
}

int rag_structurize_learn_commit(const char *input_text,
                                 const char *syntax_hint,
                                 int ring_hint,
                                 const char *final_struct)
{
	if (!input_text || !input_text[0] || !final_struct || !final_struct[0]) return 0;

    char baseline[65536];
    structurize_build_baseline_only(input_text, syntax_hint, ring_hint, baseline, (int)sizeof(baseline));

    return rag_structurize_learn_commit_pair(input_text, syntax_hint, ring_hint, baseline, final_struct);
}

typedef struct ClarMemory {
    char  *memory;
    size_t size;
} ClarMemory;

static int clar_is_transient_gemini_error(int code) {
    return code == 503   // overloaded
        || code == 502   // bad gateway
        || code == 500   // internal
        || code == 429;  // rate limit / quota
}

// Структурирование описания задачи через внешнюю LLM (Gemini).
// Возвращает 1 при успехе (out_struct заполнен), 0 при ошибке.
// Никаких локальных моделей здесь НЕ используем.
static int clar_structurize_with_gemini(
    const char *prompt,
    char       *out_struct,
    size_t      out_struct_size
) {
    if (!out_struct || out_struct_size == 0) {
        return 0;
    }
    out_struct[0] = '\0';

    if (!GEMINI_API_KEY || !GEMINI_API_KEY[0]) {
        fprintf(stderr,
                "[CLARIFY][STRUCT][GEMINI] GEMINI_API_KEY is not set.\n");
        return 0;
    }

    for (int attempt = 1; attempt <= GEMINI_MAX_RETRIES; ++attempt) {
        ClarMemory chunk;
        chunk.memory = NULL;
        chunk.size   = 0;

        CURL *curl = curl_easy_init();
        if (!curl) {
            fprintf(stderr, "[CLARIFY][STRUCT][GEMINI] curl_easy_init failed.\n");
            return 0;
        }

        clar_configure_curl_tls(curl);

        char url[256];
        snprintf(url, sizeof(url),
                 "https://generativelanguage.googleapis.com/v1beta/models/gemini-2.5-flash:generateContent?key=%s",
                 GEMINI_API_KEY);

        curl_easy_setopt(curl, CURLOPT_URL, url);

        // Тело запроса: { "contents":[{"parts":[{"text": "<prompt>"}]}] }
        cJSON *root = cJSON_CreateObject();
        cJSON *contents = cJSON_AddArrayToObject(root, "contents");
        cJSON *content0 = cJSON_CreateObject();
        cJSON_AddItemToArray(contents, content0);
        cJSON *parts = cJSON_AddArrayToObject(content0, "parts");
        cJSON *part0 = cJSON_CreateObject();
        cJSON_AddItemToArray(parts, part0);
        cJSON_AddStringToObject(part0, "text", prompt ? prompt : "");

        char *body = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);

        if (!body) {
            fprintf(stderr,
                    "[CLARIFY][STRUCT][GEMINI] cJSON_PrintUnformatted failed.\n");
            curl_easy_cleanup(curl);
            return 0;
        }

        struct curl_slist *headers = NULL;
        headers = curl_slist_append(headers, "Content-Type: application/json");

        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);

        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, clar_write_memory_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);

        CURLcode cres = curl_easy_perform(curl);

        curl_easy_cleanup(curl);
        curl_slist_free_all(headers);
        free(body);

        if (cres != CURLE_OK) {
            fprintf(stderr,
                    "[CLARIFY][STRUCT][GEMINI] curl_easy_perform failed (code=%d) on attempt %d/%d.\n",
                    (int)cres, attempt, GEMINI_MAX_RETRIES);
            if (chunk.memory) free(chunk.memory);

            if (attempt < GEMINI_MAX_RETRIES) {
                Sleep(GEMINI_RETRY_BACKOFF_MS * attempt);
                continue;
            }
            return 0;
        }

        if (!chunk.memory || chunk.size == 0) {
            fprintf(stderr,
                    "[CLARIFY][STRUCT][GEMINI] Empty response from Gemini on attempt %d/%d.\n",
                    attempt, GEMINI_MAX_RETRIES);
            if (chunk.memory) free(chunk.memory);

            if (attempt < GEMINI_MAX_RETRIES) {
                Sleep(GEMINI_RETRY_BACKOFF_MS * attempt);
                continue;
            }
            return 0;
        }

        cJSON *json = cJSON_Parse(chunk.memory);
        if (!json) {
            fprintf(stderr,
                    "[CLARIFY][STRUCT][GEMINI] cJSON_Parse failed on attempt %d/%d.\n",
                    attempt, GEMINI_MAX_RETRIES);

            size_t raw_len  = strlen(chunk.memory);
            size_t dump_len = raw_len < 800 ? raw_len : 800;
            fprintf(stderr,
                    "[CLARIFY][STRUCT][DEBUG] Gemini raw response dump (%lu bytes, showing %lu):\n",
                    (unsigned long) raw_len,
                    (unsigned long) dump_len);
            fprintf(stderr, "%.*s\n", (int)dump_len, chunk.memory);

            free(chunk.memory);

            if (attempt < GEMINI_MAX_RETRIES) {
                Sleep(GEMINI_RETRY_BACKOFF_MS * attempt);
                continue;
            }
            return 0;
        }

        // Разбираем "error", если есть
        cJSON *err = cJSON_GetObjectItemCaseSensitive(json, "error");
        if (cJSON_IsObject(err)) {
            cJSON *code    = cJSON_GetObjectItemCaseSensitive(err, "code");
            cJSON *message = cJSON_GetObjectItemCaseSensitive(err, "message");

            int         code_int = cJSON_IsNumber(code) ? code->valueint : 0;
            const char *msg      = cJSON_IsString(message) ? message->valuestring : "(no message)";

            fprintf(stderr,
                    "[CLARIFY][STRUCT][GEMINI] API error: code=%d, message=%s (attempt %d/%d)\n",
                    code_int,
                    msg ? msg : "(null)",
                    attempt,
                    GEMINI_MAX_RETRIES);

            cJSON_Delete(json);
            free(chunk.memory);

            if (clar_is_transient_gemini_error(code_int) &&
                attempt < GEMINI_MAX_RETRIES)
            {
                Sleep(GEMINI_RETRY_BACKOFF_MS * attempt);
                continue;
            }

            return 0;
        }

        // Нормальный ответ: достаём текст из candidates[0].content.parts[0].text
        int rc_ok = 0;

        cJSON *candidates = cJSON_GetObjectItemCaseSensitive(json, "candidates");
        if (candidates && cJSON_IsArray(candidates)) {
            cJSON *candidate = cJSON_GetArrayItem(candidates, 0);
            if (candidate) {
                cJSON *content_json = cJSON_GetObjectItemCaseSensitive(candidate, "content");
                cJSON *parts_json   = cJSON_GetObjectItemCaseSensitive(content_json, "parts");
				if (parts_json && cJSON_IsArray(parts_json)) {
					size_t used = 0;
					int parts_n = cJSON_GetArraySize(parts_json);

					out_struct[0] = '\0';

					for (int pi = 0; pi < parts_n; ++pi) {
						cJSON *part_json = cJSON_GetArrayItem(parts_json, pi);
						if (!part_json) continue;

						cJSON *text_json = cJSON_GetObjectItemCaseSensitive(part_json, "text");
						if (text_json && cJSON_IsString(text_json)) {
							const char *s = cJSON_GetStringValue(text_json);
							if (s && s[0]) {
								size_t sl = strlen(s);

								if (used >= out_struct_size - 1) break;
								if (used + sl > out_struct_size - 1) sl = (out_struct_size - 1) - used;

								if (sl > 0) {
									memcpy(out_struct + used, s, sl);
									used += sl;
									out_struct[used] = '\0';
								}
							}
						}
					}

					clar_trim(out_struct);
					if (out_struct[0] != '\0') {
						rc_ok = 1;
					}
				}
            }
        }

        if (!rc_ok) {
            size_t raw_len  = strlen(chunk.memory);
            size_t dump_len = raw_len < 800 ? raw_len : 800;
            fprintf(stderr,
                    "[CLARIFY][STRUCT][DEBUG] Gemini raw response dump (%lu bytes, showing %lu):\n",
                    (unsigned long) raw_len,
                    (unsigned long) dump_len);
            fprintf(stderr, "%.*s\n", (int)dump_len, chunk.memory);
            fprintf(stderr,
                    "[CLARIFY][STRUCT][GEMINI] Response did not contain a valid structured spec.\n");
        }

        cJSON_Delete(json);
        free(chunk.memory);

        if (rc_ok) {
            return 1;
        }

        if (attempt < GEMINI_MAX_RETRIES) {
            Sleep(GEMINI_RETRY_BACKOFF_MS * attempt);
            continue;
        }

        return 0;
    }

    return 0;
}

static int clar_struct_has_header_loose(const char *spec, const char *want_key)
{
    if (!spec || !want_key || !want_key[0]) return 0;

    const char *p = spec;
    while (*p) {
        const char *ls = p;
        const char *le = strchr(p, '\n');
        if (!le) le = p + strlen(p);

        // skip leading spaces and simple bullets
        const char *q = ls;
        while (q < le && (*q == ' ' || *q == '\t' || *q == '*' || *q == '-' )) q++;

        // build normalized key from line start until ':'
        char key[64];
        int k = 0;
        int prev_us = 0;
        while (q < le && *q != ':') {
            unsigned char c = (unsigned char)*q++;
            if (isalnum(c)) {
                if (k < (int)sizeof(key) - 1) key[k++] = (char)toupper(c);
                prev_us = 0;
            } else if (c == ' ' || c == '_' || c == '-' || c == '/') {
                if (!prev_us && k > 0 && k < (int)sizeof(key) - 1) key[k++] = '_';
                prev_us = 1;
            }
        }
        while (k > 0 && key[k - 1] == '_') k--;
        key[k] = '\0';

        // canonicalize a few common variants
        if (!strcmp(key, "NONGOALS")) _snprintf_s(key, sizeof(key), _TRUNCATE, "NON_GOALS");
        if (!strcmp(key, "EDITPARAMETERS")) _snprintf_s(key, sizeof(key), _TRUNCATE, "EDIT_PARAMETERS");
        if (!strcmp(key, "CLARIFIEDSPEC")) _snprintf_s(key, sizeof(key), _TRUNCATE, "CLARIFIED_SPEC");
        if (!strcmp(key, "I_O")) _snprintf_s(key, sizeof(key), _TRUNCATE, "IO");

        if (!strcmp(key, want_key)) return 1;

        p = (*le == '\n') ? (le + 1) : le;
    }
    return 0;
}

static void clar_escape_ep_text(const char *in, char *out, size_t out_sz)
{
    if (!out || out_sz == 0) return;
    out[0] = '\0';
    if (!in) return;

    size_t w = 0;
    for (size_t i = 0; in[i] && w + 2 < out_sz; ++i) {
        unsigned char c = (unsigned char)in[i];
        if (c == '\r') continue;

        if (c == '\n') {
            if (w + 2 < out_sz) { out[w++] = '\\'; out[w++] = 'n'; }
            continue;
        }

        if (c == '\\' || c == '"') {
            if (w + 2 < out_sz) { out[w++] = '\\'; out[w++] = (char)c; }
            continue;
        }

        if (c < 32) continue; // убрать прочие control chars
        out[w++] = (char)c;
    }
    out[w] = '\0';
}

static void clar_autofill_edit_parameters_inplace(char *spec, size_t spec_sz);
// Структурирует описание задачи + лог уточнений в блоки
// Возвращает 1 при успехе (out_struct заполнен), 0 при ошибке.
static int clar_structurize_task_description(
    const char *initial_desc,
    const char *qa_log,          // может быть NULL
    const char *syntax_hint,     // "C", "NASM", "BIN" или NULL
    int         ring_hint,       // 0/3/… — только как доп. инфа
    int         use_external_llm,
    char       *out_struct,
    size_t      out_struct_size
) {
    if (!out_struct || out_struct_size == 0) {
        return 0;
    }
    out_struct[0] = '\0';

    // Язык для содержимого буллетов
    int is_ru = clar_is_russian(initial_desc ? initial_desc : "");
	const char *lang_hint = is_ru
        ? "Use Russian for all section content."
        : "Use English for all section content.";

	// Собираем единый prompt для STRUCT (и для Gemini, и для локальной LLM)
	char prompt[65536];

	// safe fallback for %s
	const char *syntax_for_prompt = (syntax_hint && syntax_hint[0]) ? syntax_hint : "UNKNOWN";
	const char *orig_for_prompt   = initial_desc ? initial_desc : "";
	const char *qa_for_prompt     = qa_log ? qa_log : "";

	snprintf(prompt, sizeof(prompt),
		"You are a Technical Specification Compiler.\n"
		"\n"
		"What you have:\n"
		"ORIGINAL is the user's raw task text.\n"
		"CLARIFICATION_LOG is follow-up Q/A that adds missing details.\n"
		"CONTEXT provides SYNTAX and RING that describe the fixed execution context.\n"
		"\n"
		"Why this matters:\n"
		"The Planner builds a deterministic plan by extracting facts from your structured output.\n"
		"Your job is lossless structure: keep ALL explicit details, avoid invention, and write facts as flat, atomic lines.\n"
		"\n"
		"Sections and intent (new contract):\n"
		"GOAL = short brief describing what is being built/achieved. Keep it stable and free of constants.\n"
		"REQUIREMENTS = all detailed requirements and facts that must hold (do not summarize away details).\n"
		"INPUT = runtime inputs and required dependencies (files, args, APIs, DLLs, devices, subsystems).\n"
		"OUTPUT = user-observable results/effects (artifacts, visible behavior, created resources, exit behavior).\n"
		"Do not invent a numeric success exit code. Mention an explicit number only if it appears in ORIGINAL/CLARIFICATION_LOG.\n"
		"If a duration is specified for staying visible, include it as an observable OUTPUT line.\n"
		"CONSTRAINTS = explicit prohibitions/limits only (must not / forbidden / prohibited / do not).\n"
		"ENVIRONMENT = fixed execution context and hard assumptions (OS/ARCH/privileges + SYNTAX/RING from CONTEXT).\n"
		"EDIT_PARAMETERS = tunable constants (data, not logic) that can be changed without changing the execution context.\n"
		"NON_GOALS = what is explicitly out of scope.\n"
		"\n"
		"Placement guidance (intent-first):\n"
		"1) Preserve every explicit detail from ORIGINAL and CLARIFICATION_LOG.\n"
		"2) Keep GOAL concise; put details into REQUIREMENTS.\n"
		"3) Put prohibitions into CONSTRAINTS and out-of-scope items into NON_GOALS.\n"
		"4) Put immutable context into ENVIRONMENT, tunable values into EDIT_PARAMETERS.\n"
		"5) Keep INPUT/OUTPUT factual and observable.\n"
		"\n"
		"Placeholders and parameters:\n"
		"When you see a concrete tunable value (sizes, colors, durations, frequencies, coordinates, port addresses, etc.),\n"
		"replace only the value with a {{scope.key}} placeholder and define that key in EDIT_PARAMETERS.\n"
		"If a {{...}} placeholder already exists in the input, keep its exact name.\n"
		"Do not turn execution context (OS/SYNTAX/RING/ARCH/privileges) into placeholders; keep those literal in ENVIRONMENT.\n"
		"API/function names are usually fixed dependencies; put them in EDIT_PARAMETERS only if explicitly configurable.\n"
		"\n"
		"Style for deterministic parsing:\n"
		"Write plain lines (no '-', '*', bullets, or leading indentation).\n"
		"Do not use backticks or markdown formatting.\n"
		"Prefer one fact per line in REQUIREMENTS/INPUT/OUTPUT/CONSTRAINTS/NON_GOALS.\n"
		"If there are user-observable effects (window shown, sound played, files created, exit behavior), they must be listed in OUTPUT (not NONE).\n"
		"If dependencies are named (DLLs/APIs/devices), they must be listed in INPUT (not NONE).\n"
		"\n"
		"Return ONLY the structured spec between markers (no extra text):\n"
		"BEGIN_CLARIFIED_SPEC\n"
		"CLARIFIED_SPEC:\n"
		"GOAL:\n"
		"REQUIREMENTS:\n"
		"INPUT:\n"
		"OUTPUT:\n"
		"CONSTRAINTS:\n"
		"ENVIRONMENT:\n"
		"EDIT_PARAMETERS:\n"
		"NON_GOALS:\n"
		"END_CLARIFIED_SPEC\n"
		"\n"
		"EDIT_PARAMETERS format (flat, grouped):\n"
		"Write ALL group headers exactly once in this order, even if a group is NONE:\n"
		"SCALAR:\nSTRING:\nHARDWARE:\nNETWORK:\nDOMAIN:\n"
		"Entries must NOT have leading indentation and must start at column 0.\n"
		"CRITICAL: Every entry line MUST begin with \"SCALAR:\" or \"STRING:\" at column 0 (not only the group header).\n"
		"Do NOT output bare \"key: value\" lines.\n"
		"Canonical entries ONLY:\n"
		"Inside SCALAR group, each entry MUST be:\n"
		"SCALAR: scope.key: <number>\n"
		"Inside STRING group, each entry MUST be:\n"
		"STRING: scope.key: \"<string>\"\n"
		"Do not use INT:/TEXT:/INTEGER:/HEX_COLOR:/FILE_PATH:/FOLDER_PATH: prefixes. No '-' list markers.\n"
		"\n"
		"Normalization rule (LLM FIX, required): convert abstract values into concrete editable constants:\n"
		"- Named colors (red/blue/yellow/etc. and Russian equivalents) MUST be converted to \"#RRGGBB\" in STRING.\n"
		"- Qualitative sizes (small/medium/large, маленький/средний/большой, etc.): if they affect a numeric parameter and no number is given, choose a reasonable concrete number consistent with the rest of the spec; if ambiguous, keep it as STRING and do NOT invent additional requirements.\n"
		"- Units: store only numbers in SCALAR (px, ms, Hz). Strip unit suffixes.\n"
		"- Paths/folders: keep a clean literal path or a clean folder id in STRING (avoid phrases like \"моей папки\").\n"
		"- Be consistent: the same abstract value must map to the same concrete value within the same spec.\n"
		"\n"
		"Strings use double quotes. No arrays.\n"
		"\n"
		"Language rule:\n"
		"%s\n"
		"\n"
		"ORIGINAL:\n%.24000s\n"
		"\n"
		"CLARIFICATION_LOG:\n%.24000s\n"
		"\n"
		"CONTEXT (copy these as the first literal lines inside ENVIRONMENT, then add other fixed assumptions if present):\n"
		"SYNTAX: %s\n"
		"RING: %d\n",
		lang_hint,
		orig_for_prompt,
		qa_for_prompt,
		syntax_for_prompt,
		ring_hint
	);
	
    fprintf(stderr,
            "[SKYNET][CLARIFY][STRUCT] Начинаю фазу 2 - Cтруктурирование...\n");
    // Policy (v2.0): если ONLINE включён и есть ключ Gemini — структурирование по умолчанию внешней LLM.
    // Это устраняет “локальная LLM при ONLINE=ON” без ручных меню.
    if (g_skynet_online_enabled && GEMINI_API_KEY && GEMINI_API_KEY[0] != '\0') {
        use_external_llm = 1;
    }

    // --- Вариант 1: внешняя LLM (Gemini) ---
    if (use_external_llm) {
        fprintf(stderr,
                "[SKYNET][CLARIFY][STRUCT] Режим внешней LLM: использую Gemini для структурирования.\n");

        if (clar_structurize_with_gemini(prompt, out_struct, out_struct_size)) {
        fprintf(stderr,
                "[SKYNET][CLARIFY][STRUCT] Структурирование через Gemini успешно.\n");

        clar_trim(out_struct);

		// Sanity-check: required headers must exist (loose match; tolerant to formatting)
		{
			int ok_cl   = clar_struct_has_header_loose(out_struct, "CLARIFIED_SPEC");

			// New contract (preferred)
			int ok_goal = clar_struct_has_header_loose(out_struct, "GOAL");
			int ok_req  = clar_struct_has_header_loose(out_struct, "REQUIREMENTS");

			// Legacy contract (accepted)
			int ok_in   = clar_struct_has_header_loose(out_struct, "INPUT");
			int ok_out  = clar_struct_has_header_loose(out_struct, "OUTPUT");
			int ok_cons = clar_struct_has_header_loose(out_struct, "CONSTRAINTS");
			int ok_env  = clar_struct_has_header_loose(out_struct, "ENVIRONMENT");
			int ok_edit = clar_struct_has_header_loose(out_struct, "EDIT_PARAMETERS");
			int ok_ng   = clar_struct_has_header_loose(out_struct, "NON_GOALS");

			int ok_main = (ok_goal && ok_req);
			int ok_inout = (ok_in && ok_out);

			// If ONLY NON_GOALS is missing, imply NONE (avoid rejecting otherwise good outputs)
			if (!ok_ng && ok_cl && ok_main && ok_inout && ok_cons && ok_env && ok_edit) {
				strncat(out_struct, "\n\nNON_GOALS:\nNONE\n", out_struct_size - strlen(out_struct) - 1);
				ok_ng = 1;
			}

			if (!(ok_cl && ok_main && ok_inout && ok_cons && ok_env && ok_edit && ok_ng)) {
			fprintf(stderr,
					"[SKYNET][CLARIFY][STRUCT] WARNING: Gemini output missing required headers. Rejecting. "
					"CL=%d GOAL=%d REQ=%d IN=%d OUT=%d CONSTRAINTS=%d ENVIRONMENT=%d EDIT_PARAMETERS=%d NON_GOALS=%d\n",
					ok_cl, ok_goal, ok_req, ok_in, ok_out, ok_cons, ok_env, ok_edit, ok_ng);
				hybrid_debug_dump_reject_reasons(out_struct);
				out_struct[0] = '\0';
				return 0;
			}
		}
        
        // Автозаполнение параметров перед выходом
		clar_autofill_edit_parameters_inplace(out_struct, out_struct_size);
		clar_trim(out_struct);
		return 1;
	}

        fprintf(stderr,
                "[SKYNET][CLARIFY][STRUCT] Gemini не смог структурировать описание "
                "(режим внешней LLM — локальную модель не использую).\n");
        // Внешний режим: не переключаемся на локальную LLM, вернём 0,
        // а вызывающий код использует fallback combined-текст.
        return 0;
    }

    // --- Вариант 2: локальная LLM (Qwen) ---
    void *model = g_llama_model_gen;
    const char *model_name = "qwen2.5_coder_32b_q5";

    if (!model) {
        fprintf(stderr,
                "[SKYNET][CLARIFY][STRUCT] Нет локальной LLM для структурирования описания.\n");
        return 0;
    }

    fprintf(stderr,
            "[SKYNET][CLARIFY][STRUCT] Режим локальной LLM: структурирую описание с помощью %s...\n",
            model_name);

    if (!llama_client_generate(
            model,
            prompt,
            out_struct,
            out_struct_size - 1,
            0.2f,
            0))
    {
        fprintf(stderr,
                "[SKYNET][CLARIFY][STRUCT] llama_client_generate failed.\n");
        out_struct[0] = '\0';
        return 0;
    }

    clar_trim(out_struct);

    if (out_struct[0] == '\0') {
        fprintf(stderr,
                "[SKYNET][CLARIFY][STRUCT] Пустой ответ от LLM при структурировании.\n");
        return 0;
    }
	// NEW: sanity-check, что есть все секции (Project S v2: GOAL + REQUIREMENTS, без TASK)
	if (!strstr(out_struct, "CLARIFIED_SPEC:") ||
		!(strstr(out_struct,"GOAL:") && strstr(out_struct, "REQUIREMENTS:")) ||
		!strstr(out_struct, "INPUT:") ||
		!strstr(out_struct, "OUTPUT:") ||
		!strstr(out_struct, "CONSTRAINTS:") ||
		!strstr(out_struct, "ENVIRONMENT:") ||
		!strstr(out_struct, "EDIT_PARAMETERS:") ||
		!strstr(out_struct, "NON_GOALS:"))
	{
		fprintf(stderr,
				"[SKYNET][CLARIFY][STRUCT] WARNING: structured spec is missing required headers. "
				"Treating as failure and falling back to flat description.\n");
		out_struct[0] = '\0';
		return 0;
	}
	
// v2.0: автозаполнение параметров нужно и для локальной LLM, иначе EDIT_PARAMETERS часто остаётся пустым
clar_autofill_edit_parameters_inplace(out_struct, out_struct_size);
return 1;
}

static size_t clar_write_memory_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    ClarMemory *mem = (ClarMemory *) userp;

    char *ptr = (char *) realloc(mem->memory, mem->size + realsize + 1);
    if (!ptr) {
        return 0;
    }

    mem->memory = ptr;
    memcpy(mem->memory + mem->size, contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = '\0';

    return realsize;
}

// -------------------------
// Общие утилиты Clarifier
// -------------------------

static int clar_extract_section_text_local(const char *text, const char *heading,
                                           char *out, size_t out_sz)
{
    if (!out || out_sz == 0) return 0;
    out[0] = '\0';
    if (!text || !heading || !heading[0]) return 0;

    const char *p = strstr(text, heading);
    if (!p) return 0;

    p += strlen(heading);
    while (*p == '\r' || *p == '\n' || *p == ' ' || *p == '\t') p++;

    // stop at next known heading in our structured format
    const char *stops[] = {
        "\nCLARIFIED_SPEC:",
        "\nGOAL:",
        "\nREQUIREMENTS:",
        "\nINPUT:",
        "\nOUTPUT:",
        "\nCONSTRAINTS:",
        "\nENVIRONMENT:",
        "\nEDIT_PARAMETERS:",
        "\nNON_GOALS:",
    };

    const char *end = NULL;
    for (int i = 0; i < (int)(sizeof(stops)/sizeof(stops[0])); ++i) {
        const char *q = strstr(p, stops[i]);
        if (q && (!end || q < end)) end = q;
    }

    size_t n = end ? (size_t)(end - p) : strlen(p);
    if (n >= out_sz) n = out_sz - 1;
    memcpy(out, p, n);
    out[n] = '\0';
    return (out[0] != '\0');
}

static int clar_extract_first_quoted_span_local(const char *s, char *out, size_t out_sz)
{
    if (!out || out_sz == 0) return 0;
    out[0] = '\0';
    if (!s || !s[0]) return 0;

    const unsigned char *u = (const unsigned char*)s;

    // quote pairs: "  '  «»  “”
    static const unsigned char q_la[] = {0xC2,0xAB};      // «
    static const unsigned char q_ra[] = {0xC2,0xBB};      // »
    static const unsigned char q_ld[] = {0xE2,0x80,0x9C}; // “
    static const unsigned char q_rd[] = {0xE2,0x80,0x9D}; // ”

    struct Q { const unsigned char *o; int ol; const unsigned char *c; int cl; };
    static const struct Q qs[] = {
        {(const unsigned char*)"\"", 1, (const unsigned char*)"\"", 1},
        {(const unsigned char*)"'",  1, (const unsigned char*)"'",  1},
        {q_la, 2, q_ra, 2},
        {q_ld, 3, q_rd, 3},
    };

    for (size_t i = 0; u[i]; ++i) {
        for (size_t qi = 0; qi < sizeof(qs)/sizeof(qs[0]); ++qi) {
            const struct Q *q = &qs[qi];
            if (memcmp(u + i, q->o, (size_t)q->ol) == 0) {
                size_t j = i + (size_t)q->ol;
                while (u[j]) {
                    if (memcmp(u + j, q->c, (size_t)q->cl) == 0) {
                        size_t span = j - (i + (size_t)q->ol);
                        if (span >= out_sz) span = out_sz - 1;
                        memcpy(out, u + i + (size_t)q->ol, span);
                        out[span] = '\0';
                        return (out[0] != '\0');
                    }
                    j++;
                }
            }
        }
    }
    return 0;
}

static void clar_autofill_edit_parameters_inplace(char *spec, size_t spec_sz)
{
    if (!spec || !spec[0] || spec_sz == 0) return;

    // find EDIT_PARAMETERS line
    char *pos = strstr(spec, "EDIT_PARAMETERS:");
    if (!pos) return;

    // check if it is NONE (allow spaces/tabs and CRLF)
    char *val = pos + (int)strlen("EDIT_PARAMETERS:");
    while (*val == ' ' || *val == '\t' || *val == '\r' || *val == '\n') val++;
    if (_strnicmp(val, "NONE", 4) != 0) {
        // already filled -> nothing to do
        return;
    }

    // locate end of the EDIT_PARAMETERS line
    char *line_end = strchr(pos, '\n');
    if (!line_end) line_end = pos + strlen(pos);

    // tail after the EDIT_PARAMETERS line
    char *tail = line_end;
    if (*tail == '\n') tail++;

    // extract INPUT/OUTPUT sections and pull quoted literals (legacy: IO)
    char input_sec[1024]  = {0};
    char output_sec[1024] = {0};

    char in_q[512]  = {0};
    char out_q[512] = {0};

    int has_in  = clar_extract_section_text_local(spec, "INPUT:",  input_sec,  sizeof(input_sec));
    int has_out = clar_extract_section_text_local(spec, "OUTPUT:", output_sec, sizeof(output_sec));

    if (has_in)  (void)clar_extract_first_quoted_span_local(input_sec,  in_q,  sizeof(in_q));
    if (has_out) (void)clar_extract_first_quoted_span_local(output_sec, out_q, sizeof(out_q));

    if (!in_q[0] && !out_q[0]) {
        // no explicit literals -> keep NONE
        return;
    }

    // escape for flat EDIT_PARAMETERS
    char in_q_esc[1024]  = {0};
    char out_q_esc[1024] = {0};

    if (in_q[0])  clar_escape_ep_text(in_q,  in_q_esc,  sizeof(in_q_esc));
    if (out_q[0]) clar_escape_ep_text(out_q, out_q_esc, sizeof(out_q_esc));

    const char *in_v  = in_q[0]  ? (in_q_esc[0]  ? in_q_esc  : in_q)  : NULL;
    const char *out_v = out_q[0] ? (out_q_esc[0] ? out_q_esc : out_q) : NULL;

    // Build Variant A (flat, grouped). Write all headers once in order.
    char string_part[2048]; string_part[0] = 0;
    if (in_v && out_v) {
        _snprintf_s(string_part, sizeof(string_part), _TRUNCATE,
            "TEXT: input_text: \"%s\"\n"
            "TEXT: output_text: \"%s\"\n",
            in_v, out_v
        );
    } else if (out_v) {
        _snprintf_s(string_part, sizeof(string_part), _TRUNCATE,
            "TEXT: output_text: \"%s\"\n",
            out_v
        );
    } else if (in_v) {
        _snprintf_s(string_part, sizeof(string_part), _TRUNCATE,
            "TEXT: input_text: \"%s\"\n",
            in_v
        );
    }

    char buf[4096]; buf[0] = 0;
    if (string_part[0]) {
        _snprintf_s(buf, sizeof(buf), _TRUNCATE,
            "EDIT_PARAMETERS:\n"
            "SCALAR:\n"
            "NONE\n"
            "\n"
            "STRING:\n"
            "%s"
            "\n"
            "HARDWARE:\n"
            "NONE\n"
            "\n"
            "NETWORK:\n"
            "NONE\n"
            "\n"
            "DOMAIN:\n"
            "NONE\n",
            string_part
        );
    } else {
        // should not happen because we already checked at least one literal exists,
        // but keep deterministic fallback
        _snprintf_s(buf, sizeof(buf), _TRUNCATE,
            "EDIT_PARAMETERS:\n"
            "SCALAR:\n"
            "NONE\n"
            "\n"
            "STRING:\n"
            "NONE\n"
            "\n"
            "HARDWARE:\n"
            "NONE\n"
            "\n"
            "NETWORK:\n"
            "NONE\n"
            "\n"
            "DOMAIN:\n"
            "NONE\n"
        );
    }

    // In-place replace [pos, tail) with buf
    size_t head_len = (size_t)(pos - spec);
    size_t buf_len  = strlen(buf);
    size_t tail_len = strlen(tail);

    size_t need = head_len + buf_len + tail_len + 1;
    if (need > spec_sz) {
        // not enough room -> keep NONE
        return;
    }

    memmove(spec + head_len + buf_len, tail, tail_len + 1);
    memcpy(spec + head_len, buf, buf_len);
}

// Обрезка пробелов/переводов строк по краям.
static void clar_trim(char *s) {
    if (!s || !*s) return;

    // слева
    char *start = s;
    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n') {
        start++;
    }
    if (start != s) {
        memmove(s, start, strlen(start) + 1);
    }

    // справа
    size_t len = strlen(s);
    while (len > 0) {
        char c = s[len - 1];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            s[--len] = '\0';
        } else {
            break;
        }
    }
}

// Крупная эвристика — есть ли кириллица в тексте (UTF-8)
static int clar_is_russian(const char *text) {
    if (!text) return 0;
    while (*text) {
        unsigned char c = (unsigned char)*text;
        if (c >= 0xD0 && c <= 0xD3) { // диапазоны первых байт кириллицы в UTF-8
            return 1;
        }
        text++;
    }
    return 0;
}

static int clar_path_join3(char *out, size_t out_sz,
                           const char *a, const char *b, const char *c)
{
    if (!out || out_sz == 0) return 0;
    if (!a) a = "";
	if (!b) b = "";
	if (!c) c = "";

    size_t la = strlen(a), lb = strlen(b), lc = strlen(c);
    if (la + lb + lc + 1 > out_sz) return 0;

    memcpy(out, a, la);
    memcpy(out + la, b, lb);
    memcpy(out + la + lb, c, lc);
    out[la + lb + lc] = '\0';
    return 1;
}

// ----------------------------
// TLS / поиск сертификата CA
// ----------------------------

// Получить директорию .exe
static int clar_get_exe_dir(char *out, size_t out_size) {
    if (!out || out_size == 0) return 0;

    DWORD rc = GetModuleFileNameA(NULL, out, (DWORD)out_size);
    if (rc == 0 || rc >= out_size) {
        out[0] = '\0';
        return 0;
    }

    // Обрезаем до каталога
    char *last_bs = strrchr(out, '\\');
    if (!last_bs) {
        // странный путь, но оставим как есть
        return 1;
    }

    *last_bs = '\0';
    return 1;
}

// Проверка существования файла
static int clar_file_exists(const char *path) {
    if (!path || !*path) return 0;
    return (_access(path, 0) == 0);
}

// Поиск CA bundle: env -> exe_dir\certs\*.pem/*.crt -> .\certs\*.pem/*.crt
static int clar_find_ca_bundle_path(char *out_path, size_t out_size) {
    if (!out_path || out_size == 0) return 0;
    out_path[0] = '\0';

    // 1) Явный путь из переменной окружения
    char env_buf[1024];
    DWORD env_rc = GetEnvironmentVariableA("SKYNET_GEMINI_CAINFO",
                                           env_buf,
                                           (DWORD)sizeof(env_buf));
    if (env_rc > 0 && env_rc < sizeof(env_buf)) {
		if (clar_file_exists(env_buf)) {
			size_t n = strnlen(env_buf, out_size);
			if (n >= out_size) {
				fprintf(stderr,
					"[CLARIFY][TLS] CA path from env too long for buffer (%llu >= %llu), ignoring.\n",
					(unsigned long long)n,
					(unsigned long long)out_size);
			} else {
				memcpy(out_path, env_buf, n + 1); // copy with '\0'
				return 1;
			}
		}
    }

    // 2) exe_dir\certs\*.pem / *.crt
    char exe_dir[MAX_PATH];
    if (clar_get_exe_dir(exe_dir, sizeof(exe_dir))) {
        WIN32_FIND_DATAA fdata;
        HANDLE hFind;
        char pattern[MAX_PATH * 2];

        // *.pem
        snprintf(pattern, sizeof(pattern), "%s\\certs\\*.pem", exe_dir);
        hFind = FindFirstFileA(pattern, &fdata);
        if (hFind != INVALID_HANDLE_VALUE) {
            FindClose(hFind);
            out_path[0] = '\0';
			strncat(out_path, exe_dir, out_size - 1);
			strncat(out_path, "\\certs\\", out_size - 1 - strlen(out_path));
			strncat(out_path, fdata.cFileName, out_size - 1 - strlen(out_path));
            return clar_file_exists(out_path);
        }

        // *.crt
        snprintf(pattern, sizeof(pattern), "%s\\certs\\*.crt", exe_dir);
        hFind = FindFirstFileA(pattern, &fdata);
        if (hFind != INVALID_HANDLE_VALUE) {
            FindClose(hFind);
			if (clar_path_join3(out_path, out_size, exe_dir, "\\certs\\", fdata.cFileName)) {
				return clar_file_exists(out_path);
			}
        }
    }

    // 3) Относительный каталог .\certs (если exe запущен из корня проекта)
    WIN32_FIND_DATAA fdata;
    HANDLE hFind;

    hFind = FindFirstFileA(".\\certs\\*.pem", &fdata);
    if (hFind != INVALID_HANDLE_VALUE) {
        FindClose(hFind);
        snprintf(out_path, out_size, ".\\certs\\%s", fdata.cFileName);
        return clar_file_exists(out_path);
    }

    hFind = FindFirstFileA(".\\certs\\*.crt", &fdata);
    if (hFind != INVALID_HANDLE_VALUE) {
        FindClose(hFind);
        snprintf(out_path, out_size, ".\\certs\\%s", fdata.cFileName);
        return clar_file_exists(out_path);
    }

    return 0;
}

// Настройка TLS для curl: CA bundle + (опционально) небезопасный режим через env
static void clar_configure_curl_tls(CURL *curl) {
    if (!curl) return;

    // Dev-переключатель: отключить проверку сертификатов (ТОЛЬКО для локального теста)
    char insecure_buf[64];
    DWORD insecure_rc = GetEnvironmentVariableA("SKYNET_GEMINI_SSL_INSECURE",
                                                insecure_buf,
                                                (DWORD)sizeof(insecure_buf));
    if (insecure_rc > 0 && insecure_rc < sizeof(insecure_buf)) {
        fprintf(stderr,
                "[CLARIFY][TLS] WARNING: SSL verification disabled via SKYNET_GEMINI_SSL_INSECURE.\n");
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
        return;
    }

    char ca_path[MAX_PATH * 2];
    if (clar_find_ca_bundle_path(ca_path, sizeof(ca_path))) {
        fprintf(stderr, "[CLARIFY][TLS] Using CA bundle: %s\n", ca_path);
        curl_easy_setopt(curl, CURLOPT_CAINFO, ca_path);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    } else {
        fprintf(stderr,
                "[CLARIFY][TLS] CA bundle not found in ./certs or exe_dir/certs; "
                "falling back to system store.\n");
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    }
}

// -------------------------
// Построение промпта LLM
// -------------------------

int rag_structurize_task_description_only(
      const char *initial_desc,
      const char *syntax_hint,
      int         ring_hint,
      int         use_external_llm,
      char       *out_desc,
      size_t      out_desc_size)
{
    if (!initial_desc || !out_desc || out_desc_size == 0) {
        return -1;
    }

        out_desc[0] = '\0';

    // Structurize без Clarify: всегда через единый публичный API (baseline по умолчанию).
    return rag_structurize_task_description(
        initial_desc ? initial_desc : "",
        (syntax_hint ? syntax_hint : ""),
        ring_hint,
        use_external_llm,
        out_desc,
        out_desc_size
    );
}

// ============================================================================
// Structurize v2.0 Variant B (Hybrid) - Fix/Normalize CLARIFIED_SPEC by evidence
// ============================================================================

#define HYBRID_FIX_MAX_EVIDENCE   12
#define HYBRID_FIX_SNIP_RADIUS    140
#define HYBRID_FIX_MAX_LINE       256
#define HYBRID_FIX_MAX_CURRENT    12000

typedef struct HybridFixEvidence {
    char   fact[256];
    char   snippet[512];
    int    is_conflict; // 0=missing, 1=conflict
    unsigned long long h; // dedup hash
} HybridFixEvidence;

static const char *clar_strcasestr_local(const char *haystack, const char *needle) {
    if (!haystack || !needle || !needle[0]) return haystack;
    size_t nlen = strlen(needle);
    for (const char *h = haystack; *h; ++h) {
        size_t i = 0;
        while (i < nlen) {
            unsigned char hc = (unsigned char)h[i];
            unsigned char nc = (unsigned char)needle[i];
            if (!hc) return NULL;
            if (tolower(hc) != tolower(nc)) break;
            ++i;
        }
        if (i == nlen) return h;
    }
    return NULL;
}

static void hybrid_copy_snippet(const char *src, size_t src_len, size_t center_pos, char *out, size_t out_sz) {
    if (!out || out_sz == 0) return;
    out[0] = '\0';
    if (!src || src_len == 0) return;

    size_t start = 0;
    size_t end = src_len;

    if (center_pos > HYBRID_FIX_SNIP_RADIUS) start = center_pos - HYBRID_FIX_SNIP_RADIUS;
    if (center_pos + HYBRID_FIX_SNIP_RADIUS < end) end = center_pos + HYBRID_FIX_SNIP_RADIUS;

    // Snap to line boundaries if possible (avoid huge mid-line truncation)
    while (start > 0 && src[start] != '\n' && (center_pos - start) < (HYBRID_FIX_SNIP_RADIUS + 40)) start--;
    while (end < src_len && src[end] != '\n' && (end - center_pos) < (HYBRID_FIX_SNIP_RADIUS + 40)) end++;

    if (end <= start) return;

    size_t n = end - start;
    if (n >= out_sz) n = out_sz - 1;

    memcpy(out, src + start, n);
    out[n] = '\0';
    clar_trim(out);
}

static int hybrid_add_evidence(HybridFixEvidence *ev, int *ev_n, int cap,
                               const char *fact, const char *snippet, int is_conflict)
{
    if (!ev || !ev_n || cap <= 0 || !fact || !fact[0]) return 0;

    unsigned long long h = struct_ml_hash_str(fact);
    if (h == 0ULL) return 0;

    for (int i = 0; i < *ev_n; ++i) {
        if (ev[i].h == h) return 0;
    }
    if (*ev_n >= cap) return 0;

    _snprintf_s(ev[*ev_n].fact, sizeof(ev[*ev_n].fact), _TRUNCATE, "%s", fact);
    if (snippet && snippet[0]) _snprintf_s(ev[*ev_n].snippet, sizeof(ev[*ev_n].snippet), _TRUNCATE, "%s", snippet);
    else ev[*ev_n].snippet[0] = '\0';
    ev[*ev_n].is_conflict = is_conflict ? 1 : 0;
    ev[*ev_n].h = h;

    (*ev_n)++;
    return 1;
}

static int hybrid_line_starts_with_req(const char *line) {
    if (!line) return 0;
    while (*line == ' ' || *line == '\t') line++;
    if (!*line) return 0;
    if (*line == '-' || *line == '*' || *line == '+') return 1;
    if (isdigit((unsigned char)*line)) return 1; // "1)", "1.", etc.
    return 0;
}

static int hybrid_extract_first_keyval(const char *text,
                                       const char *key_a,
                                       const char *key_b,
                                       char *out_val, size_t out_val_sz,
                                       size_t *out_center_pos)
{
    if (out_val && out_val_sz) out_val[0] = '\0';
    if (out_center_pos) *out_center_pos = 0;

    if (!text || !out_val || out_val_sz == 0) return 0;
    const char *p = text;
    size_t base_off = 0;

    while (*p) {
        const char *ls = p;
        const char *le = strchr(ls, '\n');
        if (!le) le = ls + strlen(ls);

        size_t line_len = (size_t)(le - ls);
        char line[HYBRID_FIX_MAX_LINE];
        size_t copy_n = line_len;
        if (copy_n >= sizeof(line)) copy_n = sizeof(line) - 1;
        memcpy(line, ls, copy_n);
        line[copy_n] = '\0';

        int has_key = 0;
        if (key_a && key_a[0] && clar_strcasestr_local(line, key_a)) has_key = 1;
        if (!has_key && key_b && key_b[0] && clar_strcasestr_local(line, key_b)) has_key = 1;

        if (has_key) {
            char *colon = strchr(line, ':');
            if (colon) {
                colon++;
                while (*colon == ' ' || *colon == '\t') colon++;
                _snprintf_s(out_val, out_val_sz, _TRUNCATE, "%s", colon);
                clar_trim(out_val);

                if (out_center_pos) {
                    *out_center_pos = base_off + (size_t)(colon - line);
                }
                if (out_val[0]) return 1;
            }
        }

        if (*le == '\n') le++;
        base_off = (size_t)(le - text);
        p = le;
    }
    return 0;
}

static int hybrid_extract_env_value(const char *clarified, const char *key, char *out_val, size_t out_val_sz) {
    if (out_val && out_val_sz) out_val[0] = '\0';
    if (!clarified || !key || !key[0] || !out_val || out_val_sz == 0) return 0;

    const char *p = clarified;
    while (*p) {
        const char *ls = p;
        const char *le = strchr(ls, '\n');
        if (!le) le = ls + strlen(ls);

        size_t line_len = (size_t)(le - ls);
        char line[HYBRID_FIX_MAX_LINE];
        size_t copy_n = line_len;
        if (copy_n >= sizeof(line)) copy_n = sizeof(line) - 1;
        memcpy(line, ls, copy_n);
        line[copy_n] = '\0';

        // Match KEY at line start (after optional spaces/bullets) followed by ':'
        char *s = line;
        while (*s == ' ' || *s == '\t' || *s == '*' || *s == '-') s++;

        size_t K = strlen(key);
        if (K > 0 && _strnicmp(s, key, K) == 0) {
            char *t = s + K;
            while (*t == ' ' || *t == '\t') t++;
            if (*t == ':') {
                t++;
                while (*t == ' ' || *t == '\t') t++;
                _snprintf_s(out_val, out_val_sz, _TRUNCATE, "%s", t);
                clar_trim(out_val);
                if (out_val[0]) return 1;
            }
        }
        if (*le == '\n') le++;
        p = le;
    }
    return 0;
}

static int hybrid_extract_ring_num_from_text(const char *text, int *out_ring, size_t *out_pos) {
    if (out_pos) *out_pos = 0;
    if (out_ring) *out_ring = 0;
    if (!text || !text[0]) return 0;

    const char *p = text;
    while (*p) {
        // word boundary before "ring"
        if ((p == text || !isalnum((unsigned char)p[-1])) &&
            tolower((unsigned char)p[0]) == 'r' &&
            tolower((unsigned char)p[1]) == 'i' &&
            tolower((unsigned char)p[2]) == 'n' &&
            tolower((unsigned char)p[3]) == 'g' &&
            !isalnum((unsigned char)p[4]))
        {
            const char *q = p + 4;
            while (*q == ' ' || *q == '\t' || *q == ':' || *q == '=' || *q == '-' ) q++;

            int sign = 1;
            if (*q == '-') { sign = -1; q++; }

            if (isdigit((unsigned char)*q)) {
                long v = 0;
                while (isdigit((unsigned char)*q)) {
                    v = v * 10 + (*q - '0');
                    q++;
                    if (v > 999) break;
                }
                v *= sign;

                // допустимый диапазон подсказки колец: [-3..3] (можно расширить позже)
                if (v >= -3 && v <= 3) {
                    if (out_ring) *out_ring = (int)v;
                    if (out_pos) *out_pos = (size_t)(p - text);
                    return 1;
                }
            }
        }
        p++;
    }
    return 0;
}

static int hybrid_is_strict_int(const char *s) {
    if (!s) return 0;
    while (*s == ' ' || *s == '\t') s++;
    if (!*s) return 0;
    if (*s == '-') s++;
    if (!isdigit((unsigned char)*s)) return 0;
    while (*s) {
        if (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') break;
        if (!isdigit((unsigned char)*s)) return 0;
        s++;
    }
    return 1;
}

static int hybrid_values_equal_ci(const char *a, const char *b) {
    if (!a) a = "";
    if (!b) b = "";
    while (*a == ' ' || *a == '\t' || *a == '\r' || *a == '\n') a++;
    while (*b == ' ' || *b == '\t' || *b == '\r' || *b == '\n') b++;
    size_t la = strlen(a), lb = strlen(b);
    while (la > 0 && (a[la-1] == ' ' || a[la-1] == '\t' || a[la-1] == '\r' || a[la-1] == '\n')) la--;
    while (lb > 0 && (b[lb-1] == ' ' || b[lb-1] == '\t' || b[lb-1] == '\r' || b[lb-1] == '\n')) lb--;
    if (la != lb) return 0;
    for (size_t i = 0; i < la; ++i) {
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return 0;
    }
    return 1;
}

static int hybrid_strip_code_fences(char *s) {
    if (!s || !s[0]) return 0;
    // remove leading ```... line
    if (strncmp(s, "```", 3) == 0) {
        char *nl = strchr(s, '\n');
        if (nl) {
            memmove(s, nl + 1, strlen(nl + 1) + 1);
        } else {
            s[0] = '\0';
            return 1;
        }
    }
    // remove trailing ``` fence if present
    char *tail = strstr(s, "\n```");
    if (tail) {
        *tail = '\0';
    }
    clar_trim(s);
    return 1;
}

static int hybrid_extract_clarified_only(const char *llm_out, char *dst, size_t dst_sz) {
    if (!dst || dst_sz == 0) return 0;
    dst[0] = '\0';
    if (!llm_out || !llm_out[0]) return 0;

    // Work on a temp buffer so we can strip fences safely.
    char tmp[65536];
    _snprintf_s(tmp, sizeof(tmp), _TRUNCATE, "%s", llm_out);
    clar_trim(tmp);
    hybrid_strip_code_fences(tmp);

    const char *p = tmp;

    // Prefer explicit markers.
    const char *begin = strstr(p, "BEGIN_CLARIFIED_SPEC");
    const char *end   = strstr(p, "END_CLARIFIED_SPEC");

    if (begin && end && end > begin) {
        const char *start = begin;
        // move to next line after marker
        const char *nl = strchr(begin, '\n');
        if (nl) start = nl + 1;
        size_t n = (size_t)(end - start);
        if (n >= dst_sz) n = dst_sz - 1;
        memcpy(dst, start, n);
        dst[n] = '\0';
        clar_trim(dst);
        hybrid_strip_code_fences(dst);
        return (dst[0] != '\0');
    }

    // Fallback: find first CLARIFIED_SPEC: and take the rest.
    const char *cs = strstr(p, "CLARIFIED_SPEC:");
    if (!cs) {
        // Sometimes models output lowercase or spaces; try case-insensitive scan for "clarified_spec:"
        const char *q = p;
        while (*q) {
            if (tolower((unsigned char)q[0]) == 'c') {
                if (_strnicmp(q, "clarified_spec:", 15) == 0) { cs = q; break; }
            }
            q++;
        }
    }
    if (!cs) return 0;

    _snprintf_s(dst, dst_sz, _TRUNCATE, "%s", cs);
    clar_trim(dst);
    hybrid_strip_code_fences(dst);
    return (dst[0] != '\0');
}

static int hybrid_count_line_starts_with(const char *text, const char *hdr) {
    if (!text || !hdr || !hdr[0]) return 0;
    int count = 0;
    size_t H = strlen(hdr);

    const char *p = text;

    // check start-of-string
    if (strncmp(p, hdr, H) == 0) count++;

    while (*p) {
        if (*p == '\n') {
            const char *q = p + 1;
            if (strncmp(q, hdr, H) == 0) count++;
        }
        p++;
    }
    return count;
}

static int hybrid_validate_clarified_output(const char *out_struct) {
    if (!out_struct || !out_struct[0]) return 0;

    const char *p = out_struct;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;

    if (strncmp(p, "CLARIFIED_SPEC:", strlen("CLARIFIED_SPEC:")) != 0) return 0;
    if (hybrid_count_line_starts_with(out_struct, "CLARIFIED_SPEC:") != 1) return 0;
    
    const int c_goal = hybrid_count_line_starts_with(out_struct, "GOAL:");
    const int c_req  = hybrid_count_line_starts_with(out_struct, "REQUIREMENTS:");
	
	// Project S v2: не распознаём TASK вообще. Валидируем только наличие GOAL+REQUIREMENTS.
	if (c_goal > 1 || c_req > 1) return 0;

	const int is_new = (c_goal == 1 && c_req == 1);
	if (!is_new) return 0;

    // Common required sections
    if (hybrid_count_line_starts_with(out_struct, "INPUT:") != 1) return 0;
    if (hybrid_count_line_starts_with(out_struct, "OUTPUT:") != 1) return 0;
    if (hybrid_count_line_starts_with(out_struct, "CONSTRAINTS:") != 1) return 0;
    if (hybrid_count_line_starts_with(out_struct, "ENVIRONMENT:") != 1) return 0;
    if (hybrid_count_line_starts_with(out_struct, "EDIT_PARAMETERS:") != 1) return 0;
    if (hybrid_count_line_starts_with(out_struct, "NON_GOALS:") != 1) return 0;
	
	// Require CLARIFIED_SPEC header exactly once if present in your canonical format
	if (hybrid_count_line_starts_with(out_struct, "CLARIFIED_SPEC:") != 1) return 0;

    // Order check
    if (is_new) {
        const char *h_goal = strstr(out_struct, "\nGOAL:");            if (!h_goal) return 0; h_goal++;
        const char *h_req  = strstr(out_struct, "\nREQUIREMENTS:");    if (!h_req)  return 0; h_req++;
        const char *h_in   = strstr(out_struct, "\nINPUT:");           if (!h_in)   return 0; h_in++;
        const char *h_out  = strstr(out_struct, "\nOUTPUT:");          if (!h_out)  return 0; h_out++;
        const char *h_cons = strstr(out_struct, "\nCONSTRAINTS:");     if (!h_cons) return 0; h_cons++;
        const char *h_env  = strstr(out_struct, "\nENVIRONMENT:");     if (!h_env)  return 0; h_env++;
        const char *h_edit = strstr(out_struct, "\nEDIT_PARAMETERS:"); if (!h_edit) return 0; h_edit++;
        const char *h_ng   = strstr(out_struct, "\nNON_GOALS:");       if (!h_ng)   return 0; h_ng++;

        if (!(p < h_goal && h_goal < h_req && h_req < h_in && h_in < h_out && h_out < h_cons && h_cons < h_env && h_env < h_edit && h_edit < h_ng)) return 0;
    }

    // EDIT_PARAMETERS must not contain ENVIRONMENT.* keys (or SYNTAX/RING/OS lines)
    {
        const char *s_edit = strstr(out_struct, "\nEDIT_PARAMETERS:");
        const char *s_ng2  = strstr(out_struct, "\nNON_GOALS:");
        if (s_edit && s_ng2 && s_edit < s_ng2) {
            const char *q = s_edit + 1;
            while (q < s_ng2) {
                const char *e = strchr(q, '\n');
                size_t n = e ? (size_t)(e - q) : (size_t)(s_ng2 - q);
                char line[HYBRID_FIX_MAX_LINE];
                if (n >= sizeof(line)) n = sizeof(line) - 1;
                memcpy(line, q, n);
                line[n] = '\0';
                clar_trim(line);

                if (_strnicmp(line, "EDIT_PARAMETERS:", (int)strlen("EDIT_PARAMETERS:")) != 0) {
                    char *s = line;
                    while (*s == ' ' || *s == '\t' || *s == '*' || *s == '-') s++;

                    if (_strnicmp(s, "ENVIRONMENT.", (int)strlen("ENVIRONMENT.")) == 0) return 0;
                    if (_strnicmp(s, "SYNTAX:", (int)strlen("SYNTAX:")) == 0) return 0;
                    if (_strnicmp(s, "RING:",   (int)strlen("RING:"))   == 0) return 0;
                    if (_strnicmp(s, "OS:",     (int)strlen("OS:"))     == 0) return 0;
                }

                if (!e) break;
                q = e + 1;
            }
        }
    }

    // ENVIRONMENT must include literal SYNTAX and numeric RING (без RING_HINT)
    char ringv[128]; ringv[0] = '\0';
    if (!hybrid_extract_env_value(out_struct, "RING", ringv, sizeof(ringv))) return 0;
    if (!hybrid_is_strict_int(ringv)) return 0;

    char sx[128]; sx[0] = '\0';
    if (!hybrid_extract_env_value(out_struct, "SYNTAX", sx, sizeof(sx))) return 0;
    clar_trim(sx);
    if (!sx[0]) return 0;

    return 1;
}

static int hybrid_llm_generate(int use_external_llm, const char *prompt, char *out, size_t out_sz) {
    if (!out || out_sz == 0) return 0;
    out[0] = '\0';

    if (use_external_llm) {
        return clar_structurize_with_gemini(prompt, out, out_sz);
    }

    if (!g_llama_model_gen) {
        fprintf(stderr, "[SKYNET][STRUCTURIZE][HYBRID_FIX] No local LLM model available.\n");
        return 0;
    }

    if (!llama_client_generate(g_llama_model_gen, prompt, out, out_sz - 1, 0.2f, 0)) {
        return 0;
    }
    out[out_sz - 1] = '\0';
    return 1;
}

int rag_structurize_fix_hybrid(
    const char *original_spec,
    const char *current_clarified,
    const char *syntax_hint,
    int         ring_hint,
    int         use_external_llm,
    char       *out,
    size_t      out_sz)
{
    if (!out || out_sz == 0) return 0;
    out[0] = '\0';

    const char *orig = original_spec ? original_spec : "";
    const char *cur  = current_clarified ? current_clarified : "";
    // Force mode (>=2) allows a single LLM "normalize" pass even when evidence-delta is empty (Fix-gate only).
	int force_empty_delta_llm = (use_external_llm >= 2) ? 1 : 0;

	// Keep-output source for no-op/fallback paths (must be valid for whole function scope).
	const char *fallback_keep  = cur ? cur : "";
	const char *cur_prompt_src = cur ? cur : "";

	// Deterministic canonicalization buffer for empty-delta paths (lifetime: whole function).
	char empty_delta_norm[65536];
	empty_delta_norm[0] = '\0';

    _snprintf_s(out, out_sz, _TRUNCATE, "%s", cur);
    // Policy (Project S): ONLINE=ON must never trigger local LLM implicitly.
    // We compute an effective mode here to avoid ONLINE/use_external_arg desync.
    int use_external_effective = (use_external_llm ? 1 : 0);
    if (g_skynet_online_enabled) {
        // ONLINE means "API Models Active" (external), never local by fallback.
        use_external_effective = 1;

        // If ONLINE is ON but Gemini key is missing, refuse local and no-op.
        if (!GEMINI_API_KEY || !GEMINI_API_KEY[0]) {
            fprintf(stderr,
                "[SKYNET][STRUCTURIZE][HYBRID_FIX] ONLINE=ON but GEMINI_API_KEY is empty; refusing local by policy (no-op).\n");
            if (out && out_sz > 0) {
                strncpy(out, fallback_keep ? fallback_keep : "", out_sz - 1);
                out[out_sz - 1] = '\0';
            }
            return 1;
        }
    }
	fprintf(stderr,
		"[SKYNET][STRUCTURIZE][HYBRID_FIX] debug: online_flag=%d use_external_arg=%d use_external_effective=%d\n",
		g_skynet_online_enabled, use_external_llm, use_external_effective);

    HybridFixEvidence ev[HYBRID_FIX_MAX_EVIDENCE];
    int ev_n = 0;

    // 1) Key conflicts: SYNTAX
    {
        char orig_syntax[128]; orig_syntax[0] = '\0';
        size_t pos = 0;
        if (hybrid_extract_first_keyval(orig, "SYNTAX", "СИНТАКСИС", orig_syntax, sizeof(orig_syntax), &pos)) {
            char cur_syntax[128]; cur_syntax[0] = '\0';
            if (hybrid_extract_env_value(cur, "SYNTAX", cur_syntax, sizeof(cur_syntax))) {
                if (!hybrid_values_equal_ci(orig_syntax, cur_syntax)) {
                    char snip[512];
                    hybrid_copy_snippet(orig, strlen(orig), pos, snip, sizeof(snip));
                    char fact[256];
                    _snprintf_s(fact, sizeof(fact), _TRUNCATE, "ENVIRONMENT.SYNTAX must be \"%s\" (current=\"%s\")", orig_syntax, cur_syntax);
                    hybrid_add_evidence(ev, &ev_n, HYBRID_FIX_MAX_EVIDENCE, fact, snip, 1);
                }
            } else {
                char snip[512];
                hybrid_copy_snippet(orig, strlen(orig), pos, snip, sizeof(snip));
                char fact[256];
                _snprintf_s(fact, sizeof(fact), _TRUNCATE, "ENVIRONMENT.SYNTAX must include \"%s\"", orig_syntax);
                hybrid_add_evidence(ev, &ev_n, HYBRID_FIX_MAX_EVIDENCE, fact, snip, 0);
            }
        }
    }

   // 2) Key conflicts: RING (must be strict numeric). Extract from ORIGINAL by finding "Ring <n>" safely.
	{
    int orig_ring_num = 0;
    size_t pos = 0;

    if (hybrid_extract_ring_num_from_text(orig, &orig_ring_num, &pos)) {
        char want[32];
        _snprintf_s(want, sizeof(want), _TRUNCATE, "%d", orig_ring_num);

        char cur_ring[128]; cur_ring[0] = '\0';
        if (hybrid_extract_env_value(cur, "RING", cur_ring, sizeof(cur_ring))) {
            // If current isn't strict int OR differs -> conflict
            if (!hybrid_is_strict_int(cur_ring) || !hybrid_values_equal_ci(want, cur_ring)) {
                char snip[512];
                hybrid_copy_snippet(orig, strlen(orig), pos, snip, sizeof(snip));
                char fact[256];
                _snprintf_s(fact, sizeof(fact), _TRUNCATE,
                    "ENVIRONMENT.RING must be \"%s\" (minimal required numeric ring). Current=\"%s\"",
                    want, cur_ring);
                hybrid_add_evidence(ev, &ev_n, HYBRID_FIX_MAX_EVIDENCE, fact, snip, 1);
            }
        } else {
            char snip[512];
            hybrid_copy_snippet(orig, strlen(orig), pos, snip, sizeof(snip));
            char fact[256];
            _snprintf_s(fact, sizeof(fact), _TRUNCATE,
                "ENVIRONMENT.RING must be \"%s\" (minimal required numeric ring)", want);
            hybrid_add_evidence(ev, &ev_n, HYBRID_FIX_MAX_EVIDENCE, fact, snip, 0);
        }
    } else {
        // No explicit ring number found in ORIGINAL: do not force any ring changes by evidence.
        (void)ring_hint;
    }
}

    // 3) Requirement-like lines from ORIGINAL that appear missing in CURRENT (token overlap heuristic)
    {
        const char *p = orig;
        size_t base_off = 0;
        while (*p && ev_n < HYBRID_FIX_MAX_EVIDENCE) {
            const char *ls = p;
            const char *le = strchr(ls, '\n');
            if (!le) le = ls + strlen(ls);

            size_t line_len = (size_t)(le - ls);
            if (line_len > 12 && line_len < 240) {
                char line[HYBRID_FIX_MAX_LINE];
                size_t copy_n = line_len;
                if (copy_n >= sizeof(line)) copy_n = sizeof(line) - 1;
                memcpy(line, ls, copy_n);
                line[copy_n] = '\0';
                clar_trim(line);

                if (hybrid_line_starts_with_req(line)) {
                    char tok[3][32];
                    int tok_n = 0;

                    const char *t = line;
                    while (*t && tok_n < 3) {
                        while (*t && !isalnum((unsigned char)*t)) t++;
                        if (!*t) break;
                        char buf[32];
                        int bn = 0;
                        while (*t && isalnum((unsigned char)*t) && bn < (int)sizeof(buf) - 1) {
                            buf[bn++] = *t++;
                        }
                        buf[bn] = '\0';
                        if (bn >= 4) {
                            _snprintf_s(tok[tok_n], sizeof(tok[tok_n]), _TRUNCATE, "%s", buf);
                            tok_n++;
                        }
                    }

                    int hit = 0;
                    for (int i = 0; i < tok_n; ++i) {
                        if (clar_strcasestr_local(cur, tok[i])) hit++;
                    }

                    if (tok_n >= 2 && hit < 2) {
                        char snip[512];
                        hybrid_copy_snippet(orig, strlen(orig), base_off + 1, snip, sizeof(snip));
                        char fact[256];
                        _snprintf_s(fact, sizeof(fact), _TRUNCATE, "Missing requirement: %s", line);
                        hybrid_add_evidence(ev, &ev_n, HYBRID_FIX_MAX_EVIDENCE, fact, snip, 0);
                    }
                }
            }

            if (*le == '\n') le++;
            base_off = (size_t)(le - orig);
            p = le;
        }
    }

    int miss_n = 0;
    int conf_n = 0;
    for (int i = 0; i < ev_n; ++i) {
        if (ev[i].is_conflict) conf_n++;
        else miss_n++;
    }

    fprintf(stderr, "[SKYNET][STRUCTURIZE][HYBRID_FIX] delta built: missing=%d conflict=%d (snippets=%d).\n",
            miss_n, conf_n, ev_n);

	if (ev_n == 0) {
		fprintf(stderr, "[SKYNET][STRUCTURIZE][HYBRID_FIX] No evidence delta; structural normalization only.\n");

		// Always do deterministic canonicalization first.
		strncpy(empty_delta_norm, cur ? cur : "", sizeof(empty_delta_norm) - 1);
		empty_delta_norm[sizeof(empty_delta_norm) - 1] = '\0';

		int any = 0;
		if (tr_has_bullet_lines(empty_delta_norm)) { tr_apply_strip_bullets(empty_delta_norm, sizeof(empty_delta_norm)); any = 1; }
		if (tr_has_duplicate_headers(empty_delta_norm) || !hybrid_validate_clarified_output(empty_delta_norm)) {
			tr_apply_canon_headers(empty_delta_norm, sizeof(empty_delta_norm));
			any = 1;
		}
		if (tr_edit_has_env_keys(empty_delta_norm)) { tr_apply_env_literal(empty_delta_norm, sizeof(empty_delta_norm)); any = 1; }
		(void)any;

		// Default behavior: no LLM when evidence-delta is empty.
		if (!force_empty_delta_llm) {
			strncpy(out, empty_delta_norm, out_sz - 1);
			out[out_sz - 1] = '\0';
			return 1;
		}

		// Fix-gate forced behavior: run ONE LLM normalize pass even with empty evidence.
		fprintf(stderr, "[SKYNET][STRUCTURIZE][HYBRID_FIX] Empty delta: forced LLM normalize enabled.\n");
		cur_prompt_src = empty_delta_norm;
		fallback_keep  = empty_delta_norm;
		// continue to prompt build with empty evidence-delta
	}

    char prompt[32768];
    prompt[0] = '\0';

    // CURRENT may be long; cap it
    char cur_cap[HYBRID_FIX_MAX_CURRENT + 64];
    size_t cur_len = strlen(cur_prompt_src);
    size_t cur_take = (cur_len > (size_t)HYBRID_FIX_MAX_CURRENT) ? (size_t)HYBRID_FIX_MAX_CURRENT : cur_len;
    _snprintf_s(cur_cap, sizeof(cur_cap), _TRUNCATE, "%.*s", (int)cur_take, cur_prompt_src);

    int is_ru = clar_is_russian(cur_cap);
    const char *lang_hint = is_ru ? "Use Russian for section content." : "Use English for section content.";
    const char *syntax_ctx = (syntax_hint && syntax_hint[0]) ? syntax_hint : "UNKNOWN";
    // Project S (Concept v2.0): HYBRID_FIX не имеет qa_log; используем безопасные фоллбеки.
    // Эти переменные нужны только потому, что ты добавил их как аргументы в _snprintf_s/snprintf.
    const char *orig_for_prompt     = orig ? orig : "";
    const char *qa_for_prompt       = "";   // CLARIFICATION_LOG в этой функции отсутствует
    const char *evidence_for_prompt = "";   // evidence формируется ниже (strncat), либо оставляем пустым

    _snprintf_s(prompt, sizeof(prompt), _TRUNCATE,
        "You are a CLARIFIED_SPEC normalizer for Project S (Structurize -> Planner).\n"
        "Purpose: Turn CURRENT_CLARIFIED_SPEC into one canonical CLARIFIED_SPEC.\n"
        "Return ONLY the corrected CLARIFIED_SPEC between markers. No explanations. No markdown. No code fences.\n"
        "\n"
        "Canonical output format (always include every header; use NONE only when truly empty):\n"
        "BEGIN_CLARIFIED_SPEC\n"
        "CLARIFIED_SPEC:\n"
        "GOAL:\n"
        "REQUIREMENTS:\n"
        "INPUT:\n"
        "OUTPUT:\n"
        "CONSTRAINTS:\n"
        "ENVIRONMENT:\n"
        "EDIT_PARAMETERS:\n"
        "NON_GOALS:\n"
        "END_CLARIFIED_SPEC\n"
        "\n"
        "How to normalize (evidence-first, lossless):\n"
        "1) Keep all explicit facts that are present in CURRENT.\n"
        "2) Restore missing facts only if explicitly present in ORIGINAL or CLARIFICATION_LOG or MISSING_FACTS.\n"
        "3) Do not invent requirements, outputs, tools, platforms, APIs, filenames, identifiers, or values.\n"
        "4) Do not change numbers, units, identifiers, filenames, API/function/register/port names.\n"
        "5) Remove duplicates inside the same section (keep the most specific line).\n"
        "\n"
        "GOAL / REQUIREMENTS:\n"
        "- GOAL is short intent only (1-3 lines). No constants here.\n"
        "- REQUIREMENTS contains all detailed requirements and facts as separate plain lines.\n"
        "- Legacy conversion: if CURRENT contains TASK instead of GOAL/REQUIREMENTS, convert it.\n"
        "  Put 1-3 concise intent lines into GOAL, and move the remaining explicit lines into REQUIREMENTS.\n"
        "- If CURRENT contains both TASK and GOAL/REQUIREMENTS, keep GOAL/REQUIREMENTS as primary and move TASK lines into REQUIREMENTS (deduplicate).\n"
        "\n"
        "INPUT:\n"
        "- INPUT is what the solution needs/uses: runtime inputs and required dependencies (files, args, devices, APIs, DLLs).\n"
        "- If dependencies are mentioned (e.g., DLLs/APIs/devices) and INPUT is NONE, move those lines into INPUT.\n"
        "- After moving, remove duplicates from REQUIREMENTS.\n"
        "\n"
        "OUTPUT:\n"
        "- OUTPUT is only user-observable results/effects (artifacts, visible behavior, created resources, exit behavior).\n"
        "- If GOAL/REQUIREMENTS contains observable effects and OUTPUT is NONE, move those effects into OUTPUT.\n"
        "- Do not put prohibitions into OUTPUT; those belong to CONSTRAINTS.\n"
        "- Do not invent a numeric success exit code. If success code is not explicitly specified, say only that the program exits successfully.\n"
        "- Do not list internal implementation steps (e.g., driver load/unload) as OUTPUT unless explicitly stated as a user-observable requirement.\n"
        "\n"
        "CONSTRAINTS:\n"
        "- CONSTRAINTS is only explicit prohibitions/limits (must not / forbidden / prohibited / do not). If none -> NONE.\n"
        "- If a line is not a prohibition/limit, it belongs in REQUIREMENTS.\n"
        "\n"
        "ENVIRONMENT:\n"
        "- Use key OS: (Latin). If CURRENT has ОС:, normalize it to OS:.\n"
        "- ENVIRONMENT is immutable execution context and hard assumptions only (OS/SYNTAX/RING/ARCH/privileges).\n"
        "- Start ENVIRONMENT with these literal lines in this exact order (then add other hard assumptions if present):\n"
        "SYNTAX: %s\n"
        "RING: %d\n"
        "OS: ANY\n"
		"ARCH: ANY\n"
        "- Keep SYNTAX and RING exactly from CONTEXT.\n"
        "- OS/ARCH may be updated only if CURRENT explicitly names a target OS/arch.\n"
        "\n"
        "EDIT_PARAMETERS:\n"
        "- If concrete tunable values exist (sizes, colors, durations, frequencies, coordinates, port addresses), do not leave EDIT_PARAMETERS as NONE.\n"
        "- Replace only the concrete value with {{scope.key}} and define that key here.\n"
        "- Keep existing {{...}} placeholder names unchanged.\n"
        "- Do not put execution context (OS/SYNTAX/RING/ARCH/privileges) into EDIT_PARAMETERS.\n"
        "- Keep it flat; no nesting; no indentation.\n"
        "- Always output ALL group headers in this exact order. If a group has no entries, output NONE on the next line:\n"
        "SCALAR:\n"
        "STRING:\n"
        "HARDWARE:\n"
        "NETWORK:\n"
        "DOMAIN:\n"
		"- Canonical entries ONLY:\n"
		"  In SCALAR:  each entry MUST be: SCALAR: scope.key: <number>\n"
		"  In STRING:  each entry MUST be: STRING: scope.key: \"<string>\"\n"
		"- CRITICAL: Every entry line MUST begin with the group prefix at column 0 (not only the header).\n"
		"- Do NOT output bare \"key: value\" lines.\n"
		"- Do NOT use INT:/TEXT:/INTEGER:/HEX_COLOR:/FILE_PATH:/FOLDER_PATH: prefixes.\n"
		"- Strings use double quotes. No arrays.\n"
		"\n"
		"Normalization rule (LLM FIX, required): convert abstract values into concrete editable constants:\n"
		"- Named colors (red/blue/yellow/etc. and Russian equivalents) MUST be converted to \"#RRGGBB\" in STRING.\n"
		"- Qualitative sizes (small/medium/large, маленький/средний/большой, etc.): if they affect a numeric parameter and no number is given, choose a reasonable concrete number consistent with the rest of the spec; if ambiguous, keep it as STRING.\n"
		"- Units: store only numbers in SCALAR (px, ms, Hz). Strip unit suffixes.\n"
		"- Paths/folders: keep a clean literal path or a clean folder id in STRING (avoid phrases like \"моей папки\").\n"
		"- Be consistent inside one spec.\n"
        "\n"
        "NON_GOALS:\n"
        "- Only explicit out-of-scope statements. If none -> NONE.\n"
        "\n"
        "Style for deterministic parsing:\n"
        "- Write plain lines only. No '-', '*', bullets, or leading indentation.\n"
        "- Do not use backticks or markdown formatting.\n"
        "- Prefer one fact per line in GOAL/REQUIREMENTS/INPUT/OUTPUT/CONSTRAINTS/NON_GOALS.\n"
        "\n"
        "Language rule:\n"
        "%s\n"
        "\n"
        "ORIGINAL:\n%.24000s\n"
        "\n"
        "CLARIFICATION_LOG:\n%.24000s\n"
        "\n"
        "CURRENT_CLARIFIED_SPEC:\n%.24000s\n"
        "\n"
        "EVIDENCE_DELTA:\n%.24000s\n"
        "\n",
        syntax_ctx,
        ring_hint,
        lang_hint,
        orig_for_prompt,
        qa_for_prompt,
        cur_cap,
        evidence_for_prompt
    );

    strncat(prompt, "MISSING_FACTS:\n", sizeof(prompt) - strlen(prompt) - 1);
    int idx = 1;
    for (int i = 0; i < ev_n; ++i) {
        if (ev[i].is_conflict) continue;
        char line[900];
        _snprintf_s(line, sizeof(line), _TRUNCATE,
            "%d) %s\nEVIDENCE_SNIPPET: \"%s\"\n",
            idx++, ev[i].fact, ev[i].snippet[0] ? ev[i].snippet : "(snippet missing)"
        );
        strncat(prompt, line, sizeof(prompt) - strlen(prompt) - 1);
    }

    strncat(prompt, "\nCONFLICT_FACTS:\n", sizeof(prompt) - strlen(prompt) - 1);
    idx = 1;
    for (int i = 0; i < ev_n; ++i) {
        if (!ev[i].is_conflict) continue;
        char line[900];
        _snprintf_s(line, sizeof(line), _TRUNCATE,
            "%d) %s\nEVIDENCE_SNIPPET: \"%s\"\n",
            idx++, ev[i].fact, ev[i].snippet[0] ? ev[i].snippet : "(snippet missing)"
        );
        strncat(prompt, line, sizeof(prompt) - strlen(prompt) - 1);
    }
    strncat(prompt, "\nOUTPUT:\nReturn ONLY the corrected CLARIFIED_SPEC.\n", sizeof(prompt) - strlen(prompt) - 1);

    char repaired[65536];
    repaired[0] = '\0';
    
	fprintf(stderr,
    "[SKYNET][STRUCTURIZE][HYBRID_FIX] debug: online_flag=%d use_external_arg=%d delta_n=%d\n",
    g_skynet_online_enabled, use_external_llm, ev_n);

    fprintf(stderr, "[SKYNET][STRUCTURIZE][HYBRID_FIX] Calling LLM repair (external=%d)...\n", use_external_effective ? 1 : 0);

    if (!hybrid_llm_generate(use_external_effective, prompt, repaired, sizeof(repaired))) {
        fprintf(stderr,
            "[SKYNET][STRUCTURIZE][HYBRID_FIX] LLM generation failed; keeping current CLARIFIED_SPEC (no-op).\n");
        if (out && out_sz > 0) {
            strncpy(out, fallback_keep ? fallback_keep : "", out_sz - 1);
            out[out_sz - 1] = '\0';
        }
        return 1;
    }
	clar_trim(repaired);

	char extracted[65536];
	extracted[0] = '\0';

	if (!hybrid_extract_clarified_only(repaired, extracted, sizeof(extracted))) {
		fprintf(stderr, "[SKYNET][STRUCTURIZE][HYBRID_FIX] Could not extract CLARIFIED_SPEC from LLM output; rejecting.\n");
		// minimal debug prefix to avoid guessing
		fprintf(stderr, "[SKYNET][STRUCTURIZE][HYBRID_FIX] LLM raw prefix: %.220s\n", repaired);
		return 0;
	} 
    // Preservation guard: do not erase non-empty INPUT/OUTPUT from current spec.
        // Deterministic pre-validate canonicalization (Project S: канонизация до планирования).
    // IMPORTANT: do not "hide" forbidden headers by rebuilding; let validator reject them.
    {
        int has_forbidden =
            clar_struct_has_header_loose(extracted, "RING_HINT") ||
            clar_struct_has_header_loose(extracted, "IO") ||
            clar_struct_has_header_loose(extracted, "PREDICTED_LABELS");

        if (!has_forbidden) {
            int changed = 0;

            if (tr_has_bullet_lines(extracted)) {
                tr_apply_strip_bullets(extracted, sizeof(extracted));
                changed = 1;
            }
            if (tr_has_duplicate_headers(extracted)) {
                tr_apply_canon_headers(extracted, sizeof(extracted)); // also removes duplicates by rebuild
                changed = 1;
            }
            if (tr_edit_has_env_keys(extracted)) {
                tr_apply_env_literal(extracted, sizeof(extracted));
                changed = 1;
            }

            // Hard guarantee: if still invalid (order/missing), rebuild canon (but only when no forbidden headers).
            if (!hybrid_validate_clarified_output(extracted)) {
                tr_apply_canon_headers(extracted, sizeof(extracted));
                changed = 1;
            }
            if (changed) {
                fprintf(stderr, "[SKYNET][STRUCTURIZE][HYBRID_FIX] pre-validate canonicalize applied.\n");
            }
        } else {
            // forbidden headers must be visible to validator; no canonicalization here
        }
    }
    {
        // helper: check if section between header and next header has any non-empty payload line
        int cur_in_has = 0, out_in_has = 0;
        int cur_out_has = 0, out_out_has = 0;

        const char *a = strstr(cur, "\nINPUT:");        if (!a && strncmp(cur, "INPUT:", 6) == 0) a = cur;
        const char *b = a ? strstr(a, "\nOUTPUT:") : NULL;
        if (a && b && a < b) {
            const char *q = strchr(a, '\n');
            if (q) q++;
            while (q && q < b) {
                const char *e = strchr(q, '\n');
                size_t n = e ? (size_t)(e - q) : (size_t)(b - q);
                while (n > 0 && (q[0] == ' ' || q[0] == '\t' || q[0] == '\r')) { q++; n--; }
                if (n > 0 && !(n == 4 && _strnicmp(q, "NONE", 4) == 0)) { cur_in_has = 1; break; }
                if (!e) break;
                q = e + 1;
            }
        }

        a = strstr(extracted, "\nINPUT:");              if (!a && strncmp(extracted, "INPUT:", 6) == 0) a = extracted;
        b = a ? strstr(a, "\nOUTPUT:") : NULL;
        if (a && b && a < b) {
            const char *q = strchr(a, '\n');
            if (q) q++;
            while (q && q < b) {
                const char *e = strchr(q, '\n');
                size_t n = e ? (size_t)(e - q) : (size_t)(b - q);
                while (n > 0 && (q[0] == ' ' || q[0] == '\t' || q[0] == '\r')) { q++; n--; }
                if (n > 0 && !(n == 4 && _strnicmp(q, "NONE", 4) == 0)) { out_in_has = 1; break; }
                if (!e) break;
                q = e + 1;
            }
        }

        a = strstr(cur, "\nOUTPUT:");                   if (!a && strncmp(cur, "OUTPUT:", 7) == 0) a = cur;
        b = a ? strstr(a, "\nCONSTRAINTS:") : NULL;
        if (a && b && a < b) {
            const char *q = strchr(a, '\n');
            if (q) q++;
            while (q && q < b) {
                const char *e = strchr(q, '\n');
                size_t n = e ? (size_t)(e - q) : (size_t)(b - q);
                while (n > 0 && (q[0] == ' ' || q[0] == '\t' || q[0] == '\r')) { q++; n--; }
                if (n > 0 && !(n == 4 && _strnicmp(q, "NONE", 4) == 0)) { cur_out_has = 1; break; }
                if (!e) break;
                q = e + 1;
            }
        }

        a = strstr(extracted, "\nOUTPUT:");             if (!a && strncmp(extracted, "OUTPUT:", 7) == 0) a = extracted;
        b = a ? strstr(a, "\nCONSTRAINTS:") : NULL;
        if (a && b && a < b) {
            const char *q = strchr(a, '\n');
            if (q) q++;
            while (q && q < b) {
                const char *e = strchr(q, '\n');
                size_t n = e ? (size_t)(e - q) : (size_t)(b - q);
                while (n > 0 && (q[0] == ' ' || q[0] == '\t' || q[0] == '\r')) { q++; n--; }
                if (n > 0 && !(n == 4 && _strnicmp(q, "NONE", 4) == 0)) { out_out_has = 1; break; }
                if (!e) break;
                q = e + 1;
            }
        }

        if ((cur_in_has && !out_in_has) || (cur_out_has && !out_out_has)) {
            fprintf(stderr,
                "[SKYNET][STRUCTURIZE][HYBRID_FIX] Preservation guard: repair erased INPUT/OUTPUT; keeping current (no-op).\n");
            if (out && out_sz > 0) {
                strncpy(out, fallback_keep ? fallback_keep : "", out_sz - 1);
                out[out_sz - 1] = '\0';
            }
            return 1;
        }
    }

	if (!hybrid_validate_clarified_output(extracted)) {
		hybrid_debug_dump_reject_reasons(extracted);

		if (ev_n == 0) {
			fprintf(stderr,
				"[SKYNET][STRUCTURIZE][HYBRID_FIX] Invalid extracted output on empty-delta normalize; keeping current CLARIFIED_SPEC (no-op).\n");
			strncpy(out, fallback_keep ? fallback_keep : "", out_sz - 1);
			out[out_sz - 1] = '\0';
			return 1;
		}

		fprintf(stderr, "[SKYNET][STRUCTURIZE][HYBRID_FIX] Invalid extracted output; rejecting.\n");
		fprintf(stderr,
			"[SKYNET][STRUCTURIZE][HYBRID_FIX] hdr_counts: cs=%d GOAL=%d REQ=%d IN=%d OUT=%d CONS=%d ENV=%d EDIT=%d NG=%d\n",
			hybrid_count_line_starts_with(extracted, "CLARIFIED_SPEC:"),
			hybrid_count_line_starts_with(extracted, "GOAL:"),
			hybrid_count_line_starts_with(extracted, "REQUIRMENTS:"),
			hybrid_count_line_starts_with(extracted, "INPUT:"),
			hybrid_count_line_starts_with(extracted, "OUTPUT:"),
			hybrid_count_line_starts_with(extracted, "CONSTRAINTS:"),
			hybrid_count_line_starts_with(extracted, "ENVIRONMENT:"),
			hybrid_count_line_starts_with(extracted, "EDIT_PARAMETERS:"),
			hybrid_count_line_starts_with(extracted, "NON_GOALS:")
		);
		fprintf(stderr, "[SKYNET][STRUCTURIZE][HYBRID_FIX] extracted prefix: %.220s\n", extracted);
		return 0;
	}
	_snprintf_s(out, out_sz, _TRUNCATE, "%s", extracted);

	fprintf(stderr, "[SKYNET][STRUCTURIZE][HYBRID_FIX] Repair OK.\n");
	return 1;
}

// ============================================================================
// Structurize Demo Bridge
// Thin public wrapper for website/backend demo.
// Uses per-session SQLite DB opened by backend wrapper.
// ============================================================================

int rag_structurize_demo_open_db(const char *db_path)
{
    if (!db_path || !db_path[0]) {
        fprintf(stderr, "[SKYNET][STRUCTURIZE][DEMO] db_path is empty.\n");
        return 0;
    }

    if (g_db_structurize) {
        sqlite3_close(g_db_structurize);
        g_db_structurize = NULL;
    }

    if (sqlite3_open(db_path, &g_db_structurize) != SQLITE_OK) {
        fprintf(stderr,
                "[SKYNET][STRUCTURIZE][DEMO] sqlite3_open failed: %s\n",
                g_db_structurize ? sqlite3_errmsg(g_db_structurize) : "(null)");
        if (g_db_structurize) {
            sqlite3_close(g_db_structurize);
            g_db_structurize = NULL;
        }
        return 0;
    }

    if (!structurize_ops_ensure_schema(g_db_structurize)) {
        fprintf(stderr, "[SKYNET][STRUCTURIZE][DEMO] ensure_schema failed.\n");
        sqlite3_close(g_db_structurize);
        g_db_structurize = NULL;
        return 0;
    }

    return 1;
}

void rag_structurize_demo_close_db(void)
{
    if (g_db_structurize) {
        sqlite3_close(g_db_structurize);
        g_db_structurize = NULL;
    }
}

int rag_structurize_demo_generate(const char *input_text,
                                  const char *syntax_hint,
                                  int ring_hint,
                                  char *out_struct,
                                  size_t out_struct_size)
{
    if (!g_db_structurize) {
        fprintf(stderr, "[SKYNET][STRUCTURIZE][DEMO] DB is not open.\n");
        return 0;
    }
    if (!input_text || !input_text[0]) {
        fprintf(stderr, "[SKYNET][STRUCTURIZE][DEMO] input_text is empty.\n");
        return 0;
    }
    if (!out_struct || out_struct_size == 0) {
        return 0;
    }

    out_struct[0] = '\0';

    // IMPORTANT:
    // Website demo should use deterministic AUTO mode, not external LLM by default.
    // This is the best recruiter-facing mode for "learning by correction" demo.
    return rag_structurize_task_description(
               input_text,
               syntax_hint ? syntax_hint : "",
               ring_hint,
               STRUCTURIZE_MODE_AUTO,
               out_struct,
               out_struct_size) > 0;
}

int rag_structurize_demo_teach_and_generate(const char *input_text,
                                            const char *syntax_hint,
                                            int ring_hint,
                                            const char *corrected_struct,
                                            char *out_struct,
                                            size_t out_struct_size)
{
    if (!g_db_structurize) {
        fprintf(stderr, "[SKYNET][STRUCTURIZE][DEMO] DB is not open.\n");
        return 0;
    }
    if (!input_text || !input_text[0]) {
        fprintf(stderr, "[SKYNET][STRUCTURIZE][DEMO] input_text is empty.\n");
        return 0;
    }
    if (!corrected_struct || !corrected_struct[0]) {
        fprintf(stderr, "[SKYNET][STRUCTURIZE][DEMO] corrected_struct is empty.\n");
        return 0;
    }
    if (!out_struct || out_struct_size == 0) {
        return 0;
    }

    out_struct[0] = '\0';

    if (!rag_structurize_learn_commit(
            input_text,
            syntax_hint ? syntax_hint : "",
            ring_hint,
            corrected_struct))
    {
        fprintf(stderr, "[SKYNET][STRUCTURIZE][DEMO] learn_commit failed.\n");
        return 0;
    }

    return rag_structurize_demo_generate(
        input_text,
        syntax_hint,
        ring_hint,
        out_struct,
        out_struct_size);
}
