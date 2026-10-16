/* Utilitários do núcleo de conversão AMPR: hashes, CRC32, SHA-256, E/S e arquivos.
 * Portável: PS5 (ps5-payload-sdk, FreeBSD-like) e macOS (testes). GPL-3.0. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <sys/types.h>

/* ---- hashes ---- */
#define FNV64_BASIS_PAK4 0xCBF29CE484222325ull /* AMPRPAK4 path_hash (FNV-1a padrão) */
#define FNV64_BASIS_IDX3 1469598103934665603ull /* AMPRIDX3 slots (constante truncada do runtime) */
#define FNV64_PRIME 1099511628211ull

/* FNV-1a 64 sobre os bytes de path com dobra AMPR ('\\'->'/', A-Z->a-z); 0 vira 1. */
uint64_t ampr_path_hash(const char *path, size_t len, uint64_t basis);
/* Dobra AMPR in-place (mesmas regras). */
void ampr_fold_path(char *s, size_t len);

/* CRC-32 compatível com zlib (crc32(seed, data)). Use seed 0 para iniciar. */
uint32_t crc32_update(uint32_t seed, const void *data, size_t len);

typedef struct {
  uint32_t h[8];
  uint64_t len;
  uint8_t buf[64];
  size_t buf_len;
} sha256_ctx;
void sha256_init(sha256_ctx *c);
void sha256_update(sha256_ctx *c, const void *data, size_t len);
void sha256_final(sha256_ctx *c, uint8_t out[32]);
void sha256(const void *data, size_t len, uint8_t out[32]);

/* ---- little-endian ---- */
static inline void le16_put(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void le32_put(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static inline void le64_put(uint8_t *p, uint64_t v) { le32_put(p, (uint32_t)v); le32_put(p + 4, (uint32_t)(v >> 32)); }
static inline uint16_t le16_get(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t le32_get(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint64_t le64_get(const uint8_t *p) { return (uint64_t)le32_get(p) | ((uint64_t)le32_get(p + 4) << 32); }

static inline uint64_t align_up_u64(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }
static inline uint64_t align_down_u64(uint64_t v, uint64_t a) { return v & ~(a - 1); }

/* ---- E/S ---- */
/* Escreve/lê tudo (repete em EINTR/curtas). 0 ok, -1 erro (errno). pread_full: -1 também se EOF antes. */
int write_full(int fd, const void *buf, size_t len);
int pwrite_full(int fd, const void *buf, size_t len, off_t off);
int pread_full(int fd, void *buf, size_t len, off_t off);

/* Grava buf inteiro em "<path>.tmp", fsync, rename para path. mode p/ open. 0 ok. */
int write_file_atomic(const char *path, const void *buf, size_t len, int mode);
/* Copia src -> dst (via dst.tmp + fsync + rename), chmod mode. Soma bytes copiados em *progress
 * se não NULL; aborta se *cancel != 0. 0 ok, -1 erro (errno), -2 cancelado. */
int copy_file_atomic(const char *src, const char *dst, int mode, _Atomic uint64_t *progress,
                     _Atomic int *cancel);
/* fsync de diretório (best-effort). */
void fsync_dir(const char *dir);
/* mkdir -p (componentes intermediários). 0 ok. */
int mkdir_p(const char *path, int mode);
/* rm -rf sem seguir symlinks. 0 ok. */
int rm_rf(const char *path);
/* Busca binária de needle no arquivo (leitura em blocos com sobreposição). 1 achou, 0 não, -1 erro. */
int file_contains(const char *path, const void *needle, size_t needle_len);

/* snprintf seguro para path; retorna 0 se coube. */
int path_join(char *out, size_t outsz, const char *a, const char *b);

uint64_t now_ms(void);
