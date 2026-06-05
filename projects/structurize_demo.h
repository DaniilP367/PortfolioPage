#ifndef STRUCTURIZE_DEMO_H
#define STRUCTURIZE_DEMO_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int  rag_structurize_demo_open_db(const char *db_path);
void rag_structurize_demo_close_db(void);

int rag_structurize_demo_generate(const char *input_text,
                                  const char *syntax_hint,
                                  int ring_hint,
                                  char *out_struct,
                                  size_t out_struct_size);

int rag_structurize_demo_teach_and_generate(const char *input_text,
                                            const char *syntax_hint,
                                            int ring_hint,
                                            const char *corrected_struct,
                                            char *out_struct,
                                            size_t out_struct_size);

#ifdef __cplusplus
}
#endif

#endif
