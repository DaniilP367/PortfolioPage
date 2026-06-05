#include "include/sqlite3.h"

sqlite3 *g_db_structurize = NULL;

void *g_llama_model_gen = NULL;
void *g_llama_model_en  = NULL;
void *g_llama_model_ru  = NULL;

const char *GEMINI_API_KEY = NULL;

int g_skynet_online_enabled = 0;
int g_skynet_debug_enabled  = 0;
