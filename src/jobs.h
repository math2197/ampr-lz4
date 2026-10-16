/* Fila de jobs de conversão (uma thread de trabalho). */
#pragma once
#include "json.h"

/* Cria a thread de trabalho. 0 ok; !=0 se pthread_create falhou (também com a pilha padrão). */
int jobs_init(void);
/* Enfileira. Retorna número do job (>0) ou <0 com mensagem em err. */
/* keep_loose: regras extras (já validadas; "" ou NULL = nenhuma). delete_source: apagar a imagem original
 * depois de sucesso verificado (só a imagem .exfat/.ffpfsc do SMP, com várias checagens; ver jobs.c).
 * pack_all: 1 = sem auto-loose (vale também se config.ini tiver pack_all=1). */
int jobs_enqueue(const char *title_id, const char *title, const char *dest_drive, int level, const char *keep_loose,
                 int delete_source, int pack_all, char *err, size_t en);
/* Engine já detectada para o jogo ("unreal", "unity", ...) ou NULL se ainda desconhecida. */
const char *jobs_engine_of(const char *title_id);
/* job==0 cancela o atual. Retorna 0 se algo foi cancelado. */
int jobs_cancel(int job);
/* Escreve o JSON de /api/status. */
void jobs_status_json(sb_t *b);
