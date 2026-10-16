/* Varredura da origem e classificação loose/pack. GPL-3.0. */
#pragma once
#include <stddef.h>
#include <stdint.h>

enum { SCAN_ACT_PACK = 0, SCAN_ACT_LOOSE = 1 };

typedef struct {
  char *rel;            /* caminho relativo à raiz, separador '/', bytes como no disco */
  char *src;            /* caminho absoluto de leitura (origem, ou fakelib_sprx) */
  char *app0;           /* "/app0/<rel>" (grafia original) */
  char *key;            /* app0 dobrado (ordenação/colisões) */
  uint32_t app0_len;
  uint64_t size;
  int64_t mtime;        /* segundos Unix (int(st_mtime)) */
  uint8_t action;       /* SCAN_ACT_* (auto-loose pode mudar PACK->LOOSE depois) */
  uint8_t indexed;      /* entra no AMPRIDX3/AMPRPAK4 */
  uint8_t exec;         /* executável: chmod 0755 */
  uint8_t block_shift;  /* 14..20 para PACK */
  const char *reason;   /* motivo da classificação (estático) */
} scan_file_t;

typedef struct {
  scan_file_t *files;   /* ordenado por key; inclui não indexados */
  size_t n_files;
  char **dirs;          /* diretórios relativos (para recriar no destino) */
  size_t n_dirs;
  uint64_t total_bytes; /* soma dos tamanhos de todos os arquivos copiados/empacotados */
  size_t n_indexed;
} scan_result_t;

/* Varre src_dir recursivamente (sem seguir symlinks: symlink = erro).
 * fakelib_sprx != NULL: inclui/substitui fakelib/libSceAmpr.sprx (loose) lendo desse caminho.
 * Retorna 0 ou -1 com mensagem em err. */
int scan_tree(const char *src_dir, const char *fakelib_sprx, scan_result_t *out, char *err, size_t errlen);
void scan_free(scan_result_t *r);

/* Classificação estática de um caminho relativo (sem amostragem). Preenche action/exec/block_shift/reason. */
void scan_classify(const char *rel, uint64_t size, scan_file_t *f);

/* 1 se o caminho relativo é um dos arquivos de runtime excluídos do índice e da cópia. */
int scan_is_excluded(const char *rel);
