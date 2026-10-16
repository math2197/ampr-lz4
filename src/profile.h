/* Perfis de empacotamento por jogo/engine (TOML do ampr_pack/MkPFS) embutidos no binário. GPL-3.0-or-later.
 *
 * Os perfis em profiles/ vêm do MkPFS.CSharp (danthespal, GPL-3.0), distribuídos pelo
 * theghostonline/PS5-LZ4-Compression (Husam Osman, GPL-3.0-or-later); o formato é o do ampr_pack.py do
 * ampr_emu (drakmor). Ver THIRD_PARTY.md. */
#pragma once
#include <stddef.h>
#include <stdint.h>

enum { PROF_ACT_PACK = 0, PROF_ACT_LOOSE = 1 };

/* Limiares de economia do LZ4 por bloco (min_savings_* / io_neutral_min_savings_* do ampr_pack.py).
 * Razões em pontos-base (1/10000): 0.01 = 100, 0.125 = 1250, 0.0025 = 25. */
typedef struct {
  uint32_t min_bytes;
  uint32_t io_min_bytes;
  uint16_t min_bp;
  uint16_t io_min_bp;
} prof_savings_t;

#define PROF_SAVINGS_DEFAULT ((prof_savings_t){64u, 8192u, 100u, 1250u})
/* Regras de áudio "random/hot" do perfil automático da referência (ampr_pack_profile.c). */
#define PROF_SAVINGS_AUDIO ((prof_savings_t){16u, 2048u, 25u, 1250u})

typedef struct {
  uint8_t action;       /* PROF_ACT_* ("store" é tratado como loose: o empacotador não grava STORE_ONLY) */
  uint8_t block_shift;  /* 14..20 */
  uint8_t file_flags;   /* AMPR_FILE_FLAG_HOT / AMPR_FILE_FLAG_RANDOM_ACCESS */
  prof_savings_t sav;
  char **include;
  size_t n_include;
  char **exclude;
  size_t n_exclude;
} prof_rule_t;

typedef struct {
  char id[32];          /* "insomniac", "unity", ... (vazio = sem perfil) */
  char label[64];
  char match[16];       /* "title-id", "name" ou "layout" */
  prof_rule_t def;      /* regra padrão: default_action / default_block_size / limiares de [pack] */
  prof_rule_t *rules;   /* a última regra que casa vence (select_rule do ampr_pack.py) */
  size_t n_rules;
  int unknown_keys;     /* chaves ignoradas (ex.: "ayout" em astrobot.toml) */
} prof_t;

/* Parser TOML mínimo: só o subconjunto usado pelos perfis (comentários, [tabela], [[rule]], chave = string |
 * inteiro | float | bool | array de strings, inclusive em várias linhas). 0 ok; -1 com err. */
int prof_parse_toml(const char *text, size_t len, prof_t *out, char *err, size_t errlen);
void prof_free(prof_t *p);

/* Regra vencedora para rel (caminho relativo, '/'), ou &p->def se nenhuma casar. */
const prof_rule_t *prof_select(const prof_t *p, const char *rel);

/* Glob do ampr_pack.py (fnmatchcase), sem distinção de maiúsculas (exFAT/chaves dobradas do AMPR):
 * '*' casa qualquer sequência, inclusive '/' (logo "**" equivale a "*"), '?' casa um caractere. */
int prof_glob(const char *pat, const char *s);

/* Lê titleId e o nome do título (titleName do idioma padrão) de <src>/sce_sys/param.json. 0 ok. */
int prof_read_param(const char *src_dir, char *title_id, size_t tn, char *name, size_t nn);

/* Escolhe um perfil embutido (profiles/index.json): Title ID, depois nome, depois layout_engine
 * ("insomniac", "unity" ou ""). Retorna 1 e preenche out se achou; 0 se nenhum; -1 erro (err). */
int prof_choose_bundled(const char *title_id, const char *name, const char *layout_engine, prof_t *out, char *err,
                        size_t errlen);

/* Carrega um perfil embutido pelo nome do arquivo (ex.: "insomniac.toml"). 0 ok. */
int prof_load_bundled(const char *file, prof_t *out, char *err, size_t errlen);
/* Número de perfis embutidos e o nome do i-ésimo arquivo (para testes). */
size_t prof_bundled_count(void);
const char *prof_bundled_name(size_t i);
