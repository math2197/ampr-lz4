/* Cliente HTTP do ShadowMountPlus 1.7+ (API em /api/v1). Ver comentários em smp.c. */
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct {
  char title_id[16];
  char title[128];
  char path[512];          /* caminho físico da imagem/pasta */
  char runtime_path[512];  /* diretório montado (vazio se desmontado) */
  char source_type[12];    /* folder|image|pkg */
  char image_type[12];     /* ufs|exfatfs|pfs|pfsc|... */
  char fakelib_mode[16];   /* full|emulators|disabled */
  uint64_t size;
  int mounted;
} smp_game_t;

/* Todas retornam 0 em sucesso; em erro, !=0 e mensagem em err (se não NULL). */
int smp_version(char *out, size_t n);
int smp_list_games(smp_game_t **out, int *count, char *err, size_t en); /* free(*out) */
int smp_game_info(const char *title_id, smp_game_t *g, char *err, size_t en);
int smp_icon(const char *title_id, uint8_t **png, size_t *len);        /* free(*png) */
/* Monta (modo ro; se HTTP 501 repete sem modo) e devolve o diretório raiz do jogo em runtime. */
int smp_mount(const char *title_id, char *runtime_path, size_t n, char *err, size_t en);
int smp_unmount(const char *title_id, char *err, size_t en);
int smp_get_fakelib(const char *title_id, char *mode, size_t n);
int smp_set_fakelib(const char *title_id, const char *mode, char *err, size_t en);
/* Caminho (já com prefixo de host) do libSceAmpr.sprx instalado pelo SMP, ou "" se não existir. */
void smp_ampr_sprx_path(char *out, size_t n);
/* Pasta lógica de emuladores do SMP (emulators_path), para mensagens. */
void smp_emus_dir(char *out, size_t n);
