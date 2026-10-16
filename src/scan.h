/* Varredura da origem e classificação loose/pack. GPL-3.0. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "convert.h"
#include "profile.h"

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
  uint8_t locked;       /* segurança/sempre-em-disco: keep_loose, perfis e engine não mudam a ação */
  uint8_t file_flags;   /* AMPR_FILE_FLAG_HOT | AMPR_FILE_FLAG_RANDOM_ACCESS (só PACK) */
  uint32_t sav_min_bytes, sav_io_min_bytes; /* limiares de economia LZ4 (ver prof_savings_t) */
  uint16_t sav_min_bp, sav_io_min_bp;
  const char *reason;   /* motivo da classificação (estático) */
} scan_file_t;

typedef struct {
  scan_file_t *files;   /* ordenado por key; inclui não indexados */
  size_t n_files;
  char **dirs;          /* diretórios relativos (para recriar no destino) */
  size_t n_dirs;
  uint64_t total_bytes; /* soma dos tamanhos de todos os arquivos copiados/empacotados */
  size_t n_indexed;
  int engine;           /* conv_engine_t detectado */
  uint64_t packed_bytes;/* estimativa: soma dos PACK indexados (antes do auto-loose) */
  uint64_t loose_bytes; /* total_bytes - packed_bytes */
  char profile[32];     /* id do perfil embutido aplicado ("" = nenhum) */
  char profile_match[16]; /* "title-id", "name" ou "layout" */
  char title_id[16];    /* de sce_sys/param.json */
  char profile_err[200]; /* perfil embutido que casou mas não carregou (segue sem perfil) */
} scan_result_t;

/* Varre src_dir recursivamente (sem seguir symlinks: symlink = erro).
 * fakelib_sprx != NULL: inclui/substitui fakelib/libSceAmpr.sprx (loose) lendo desse caminho.
 * Retorna 0 ou -1 com mensagem em err. */
int scan_tree(const char *src_dir, const char *fakelib_sprx, scan_result_t *out, char *err, size_t errlen);
/* Igual a scan_tree, aplicando também as regras extras keep_loose (ver conv_opts_t.keep_loose). */
int scan_tree_ex(const char *src_dir, const char *fakelib_sprx, const char *keep_loose, scan_result_t *out, char *err,
                 size_t errlen);
/* Publica engine/packed_bytes/loose_bytes da varredura em p (estimativa exibida pela plataforma). */
void scan_publish_estimate(const scan_result_t *r, conv_progress_t *p);

/* Detecta a engine pelos caminhos relativos da lista (CONV_ENGINE_*). */
int scan_detect_engine(const scan_file_t *files, size_t n);
/* Layout de engine coberto pelos perfis embutidos: "insomniac" (toc, dag e d/ na raiz), "unity"
 * (StreamingAssets ao lado dos Data/ do Unity, na raiz ou em Media/) ou "". */
const char *scan_layout_engine(const scan_result_t *r);
/* Aplica as regras na ordem de precedência: segurança e sempre-em-disco (scan_classify, travadas) >
 * keep_loose do usuário > perfil do jogo (prof, pode ser NULL) > perfil da engine > regras padrão. */
void scan_apply_rules(scan_result_t *r, const char *keep_loose, const prof_t *prof);
/* 1 se rel é de um arquivo que precisa ficar no disco (PlayGo, boot do Ghost of Yōtei...). */
int scan_keep_on_disk(const char *rel, uint64_t size);
/* 1 se rel casa com alguma regra da lista keep_loose (extensão, nome ou prefixo de caminho). */
int scan_keep_loose_match(const char *rel, const char *keep_loose);
void scan_free(scan_result_t *r);

/* Classificação estática (sem amostragem): segurança/sempre-em-disco (locked) e regras padrão.
 * Preenche action/exec/block_shift/locked/file_flags/limiares/reason. */
void scan_classify(const char *rel, uint64_t size, scan_file_t *f);

/* 1 se o caminho relativo é um dos arquivos de runtime excluídos do índice e da cópia. */
int scan_is_excluded(const char *rel);
