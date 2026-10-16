/* Fila de jobs de conversão (uma thread de trabalho). */
#pragma once
#include "json.h"

/* Cria a thread de trabalho. 0 ok; !=0 se pthread_create falhou (também com a pilha padrão). */
int jobs_init(void);
/* Enfileira. Retorna número do job (>0) ou <0 com mensagem em err. */
int jobs_enqueue(const char *title_id, const char *title, const char *dest_drive, int level, char *err, size_t en);
/* job==0 cancela o atual. Retorna 0 se algo foi cancelado. */
int jobs_cancel(int job);
/* Escreve o JSON de /api/status. */
void jobs_status_json(sb_t *b);
