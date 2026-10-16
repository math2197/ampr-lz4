/* Camada de plataforma: caminhos-base, config global, IP local. (plataforma, não núcleo) */
#pragma once
#include <stddef.h>

#define AMPR_VERSION "0.1.0"

typedef struct {
  int port;      /* porta HTTP (padrão 8095) */
  int workers;   /* threads de compressão (padrão 4) */
  int level;     /* nível LZ4 padrão (padrão 6) */
  int shortcut;  /* obsoleto, ignorado (o atalho é o payload separado ampr-shortcut.elf) */
} plat_cfg_t;
extern plat_cfg_t g_cfg;

/* Prefixo de raiz: no host, env AMPR_ROOT_PREFIX redireciona /data e /mnt/extN. No PS5 é "". */
const char *plat_prefix(void);
/* Escreve prefixo + caminho absoluto em out. */
void plat_path(char *out, size_t n, const char *abs_path);
/* Raiz lógica de um drive ("data"|"ext0"|"ext1") ou NULL se inválido. */
const char *plat_drive_root(const char *drive);
/* Descobre o drive a partir de um caminho de imagem (/data/..., /mnt/ext0/...); NULL se fora. */
const char *plat_drive_of(const char *path);
/* IP local em texto. */
void plat_local_ip(char *out, size_t n);
void plat_load_config(void);
