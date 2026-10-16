/* Utilitários do núcleo de conversão AMPR. GPL-3.0. */
#include "util.h"
#include "convert.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ hashes */

void ampr_fold_path(char *s, size_t len) {
  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char)s[i];
    if (c == '\\') c = '/';
    if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + 0x20);
    s[i] = (char)c;
  }
}

uint64_t ampr_path_hash(const char *path, size_t len, uint64_t basis) {
  uint64_t h = basis;
  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char)path[i];
    if (c == '\\') c = '/';
    if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + 0x20);
    h ^= c;
    h *= FNV64_PRIME;
  }
  return h ? h : 1;
}

/* ------------------------------------------------------------------ CRC32 (slicing-by-8) */

static uint32_t g_crc_tab[8][256];
static _Atomic int g_crc_ready;

static void crc_init(void) {
  if (atomic_load(&g_crc_ready)) return;
  for (uint32_t i = 0; i < 256; i++) {
    uint32_t c = i;
    for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    g_crc_tab[0][i] = c;
  }
  for (uint32_t i = 0; i < 256; i++) {
    uint32_t c = g_crc_tab[0][i];
    for (int t = 1; t < 8; t++) {
      c = g_crc_tab[0][c & 0xFF] ^ (c >> 8);
      g_crc_tab[t][i] = c;
    }
  }
  /* Idempotente: corridas entre threads escrevem os mesmos valores. */
  atomic_store(&g_crc_ready, 1);
}

uint32_t crc32_update(uint32_t seed, const void *data, size_t len) {
  crc_init();
  const uint8_t *p = (const uint8_t *)data;
  uint32_t c = seed ^ 0xFFFFFFFFu;
  while (len && ((uintptr_t)p & 7)) {
    c = g_crc_tab[0][(c ^ *p++) & 0xFF] ^ (c >> 8);
    len--;
  }
  while (len >= 8) {
    uint32_t lo = c ^ le32_get(p);
    uint32_t hi = le32_get(p + 4);
    c = g_crc_tab[7][lo & 0xFF] ^ g_crc_tab[6][(lo >> 8) & 0xFF] ^ g_crc_tab[5][(lo >> 16) & 0xFF] ^
        g_crc_tab[4][lo >> 24] ^ g_crc_tab[3][hi & 0xFF] ^ g_crc_tab[2][(hi >> 8) & 0xFF] ^
        g_crc_tab[1][(hi >> 16) & 0xFF] ^ g_crc_tab[0][hi >> 24];
    p += 8;
    len -= 8;
  }
  while (len--) c = g_crc_tab[0][(c ^ *p++) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

/* ------------------------------------------------------------------ SHA-256 */

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

#define ROR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(uint32_t h[8], const uint8_t *p) {
  uint32_t w[64];
  for (int i = 0; i < 16; i++)
    w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) | ((uint32_t)p[4 * i + 2] << 8) | p[4 * i + 3];
  for (int i = 16; i < 64; i++) {
    uint32_t s0 = ROR32(w[i - 15], 7) ^ ROR32(w[i - 15], 18) ^ (w[i - 15] >> 3);
    uint32_t s1 = ROR32(w[i - 2], 17) ^ ROR32(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
  for (int i = 0; i < 64; i++) {
    uint32_t S1 = ROR32(e, 6) ^ ROR32(e, 11) ^ ROR32(e, 25);
    uint32_t ch = (e & f) ^ (~e & g);
    uint32_t t1 = hh + S1 + ch + K256[i] + w[i];
    uint32_t S0 = ROR32(a, 2) ^ ROR32(a, 13) ^ ROR32(a, 22);
    uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
    uint32_t t2 = S0 + mj;
    hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
  }
  h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

void sha256_init(sha256_ctx *c) {
  static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  memcpy(c->h, iv, sizeof iv);
  c->len = 0;
  c->buf_len = 0;
}

void sha256_update(sha256_ctx *c, const void *data, size_t len) {
  const uint8_t *p = (const uint8_t *)data;
  c->len += len;
  if (c->buf_len) {
    size_t n = 64 - c->buf_len;
    if (n > len) n = len;
    memcpy(c->buf + c->buf_len, p, n);
    c->buf_len += n; p += n; len -= n;
    if (c->buf_len == 64) { sha256_block(c->h, c->buf); c->buf_len = 0; }
  }
  while (len >= 64) { sha256_block(c->h, p); p += 64; len -= 64; }
  if (len) { memcpy(c->buf, p, len); c->buf_len = len; }
}

void sha256_final(sha256_ctx *c, uint8_t out[32]) {
  uint64_t bits = c->len * 8;
  uint8_t pad = 0x80;
  sha256_update(c, &pad, 1);
  uint8_t z = 0;
  while (c->buf_len != 56) sha256_update(c, &z, 1);
  uint8_t lb[8];
  for (int i = 0; i < 8; i++) lb[i] = (uint8_t)(bits >> (56 - 8 * i));
  sha256_update(c, lb, 8);
  for (int i = 0; i < 8; i++) {
    out[4 * i] = (uint8_t)(c->h[i] >> 24); out[4 * i + 1] = (uint8_t)(c->h[i] >> 16);
    out[4 * i + 2] = (uint8_t)(c->h[i] >> 8); out[4 * i + 3] = (uint8_t)c->h[i];
  }
}

void sha256(const void *data, size_t len, uint8_t out[32]) {
  sha256_ctx c;
  sha256_init(&c);
  sha256_update(&c, data, len);
  sha256_final(&c, out);
}

/* ------------------------------------------------------------------ E/S */

int write_full(int fd, const void *buf, size_t len) {
  const uint8_t *p = (const uint8_t *)buf;
  while (len) {
    ssize_t n = write(fd, p, len);
    if (n < 0) { if (errno == EINTR) continue; return -1; }
    if (n == 0) { errno = EIO; return -1; }
    p += n; len -= (size_t)n;
  }
  return 0;
}

int pwrite_full(int fd, const void *buf, size_t len, off_t off) {
  const uint8_t *p = (const uint8_t *)buf;
  while (len) {
    ssize_t n = pwrite(fd, p, len, off);
    if (n < 0) { if (errno == EINTR) continue; return -1; }
    if (n == 0) { errno = EIO; return -1; }
    p += n; len -= (size_t)n; off += n;
  }
  return 0;
}

int pread_full(int fd, void *buf, size_t len, off_t off) {
  uint8_t *p = (uint8_t *)buf;
  while (len) {
    ssize_t n = pread(fd, p, len, off);
    if (n < 0) { if (errno == EINTR) continue; return -1; }
    if (n == 0) { errno = EIO; return -1; } /* EOF inesperado */
    p += n; len -= (size_t)n; off += n;
  }
  return 0;
}

int path_join(char *out, size_t outsz, const char *a, const char *b) {
  int n;
  if (!b || !*b) n = snprintf(out, outsz, "%s", a);
  else n = snprintf(out, outsz, "%s/%s", a, b);
  if (n < 0 || (size_t)n >= outsz) { errno = ENAMETOOLONG; return -1; }
  return 0;
}

void fsync_dir(const char *dir) {
  int fd = open(dir, O_RDONLY | O_DIRECTORY);
  if (fd >= 0) { (void)fsync(fd); close(fd); }
}

int write_file_atomic(const char *path, const void *buf, size_t len, int mode) {
  char tmp[2048];
  if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp) { errno = ENAMETOOLONG; return -1; }
  int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, mode);
  if (fd < 0) return -1;
  if (write_full(fd, buf, len) != 0 || fsync(fd) != 0) {
    int e = errno; close(fd); unlink(tmp); errno = e; return -1;
  }
  if (close(fd) != 0 || rename(tmp, path) != 0) { int e = errno; unlink(tmp); errno = e; return -1; }
  return 0;
}

int copy_file_atomic(const char *src, const char *dst, int mode, _Atomic uint64_t *progress,
                     _Atomic int *cancel) {
  char tmp[2048];
  if (snprintf(tmp, sizeof tmp, "%s.tmp", dst) >= (int)sizeof tmp) { errno = ENAMETOOLONG; return -1; }
  int in = open(src, O_RDONLY);
  if (in < 0) return -1;
  int out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (out < 0) { int e = errno; close(in); errno = e; return -1; }
  size_t bufsz = 8u << 20;
  uint8_t *buf = (uint8_t *)malloc(bufsz);
  int rc = 0;
  if (!buf) { errno = ENOMEM; rc = -1; }
  off_t off = 0;
  while (rc == 0) {
    if (cancel && atomic_load(cancel)) { rc = -2; break; }
    ssize_t n = pread(in, buf, bufsz, off);
    if (n < 0) { if (errno == EINTR) continue; rc = -1; break; }
    if (n == 0) break;
    if (write_full(out, buf, (size_t)n) != 0) { rc = -1; break; }
    off += n;
    if (progress) atomic_fetch_add(progress, (uint64_t)n);
  }
  int e = errno;
  free(buf);
  close(in);
  if (rc == 0 && (fchmod(out, (mode_t)mode) != 0 || fsync(out) != 0)) { rc = -1; e = errno; }
  if (close(out) != 0 && rc == 0) { rc = -1; e = errno; }
  if (rc == 0 && rename(tmp, dst) != 0) { rc = -1; e = errno; }
  if (rc != 0) unlink(tmp);
  errno = e;
  return rc;
}

int mkdir_p(const char *path, int mode) {
  char buf[2048];
  size_t len = strlen(path);
  if (len >= sizeof buf) { errno = ENAMETOOLONG; return -1; }
  memcpy(buf, path, len + 1);
  for (size_t i = 1; i <= len; i++) {
    if (buf[i] == '/' || buf[i] == '\0') {
      char saved = buf[i];
      buf[i] = '\0';
      if (mkdir(buf, (mode_t)mode) != 0 && errno != EEXIST) return -1;
      buf[i] = saved;
    }
  }
  struct stat st;
  if (stat(path, &st) != 0) return -1;
  if (!S_ISDIR(st.st_mode)) { errno = ENOTDIR; return -1; }
  return 0;
}

int rm_rf(const char *path) {
  struct stat st;
  if (lstat(path, &st) != 0) return errno == ENOENT ? 0 : -1;
  if (!S_ISDIR(st.st_mode)) return unlink(path);
  DIR *d = opendir(path);
  if (!d) return -1;
  int rc = 0;
  struct dirent *de;
  char child[2048];
  while ((de = readdir(d)) != NULL) {
    if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
    if (path_join(child, sizeof child, path, de->d_name) != 0 || rm_rf(child) != 0) rc = -1;
  }
  closedir(d);
  if (rmdir(path) != 0) rc = -1;
  return rc;
}

int file_contains(const char *path, const void *needle, size_t nlen) {
  if (nlen == 0) return 1;
  int fd = open(path, O_RDONLY);
  if (fd < 0) return -1;
  size_t bufsz = 4u << 20;
  uint8_t *buf = (uint8_t *)malloc(bufsz + nlen);
  if (!buf) { close(fd); return -1; }
  size_t keep = 0;
  off_t off = 0;
  int found = 0;
  for (;;) {
    ssize_t n = pread(fd, buf + keep, bufsz, off);
    if (n < 0) { if (errno == EINTR) continue; found = -1; break; }
    if (n == 0) break;
    off += n;
    size_t have = keep + (size_t)n;
    if (have >= nlen) {
      const uint8_t *first = (const uint8_t *)needle;
      for (size_t i = 0; i + nlen <= have; i++) {
        if (buf[i] == first[0] && memcmp(buf + i, needle, nlen) == 0) { found = 1; break; }
      }
      if (found) break;
      keep = nlen - 1;
      memmove(buf, buf + have - keep, keep);
    } else {
      keep = have;
    }
  }
  free(buf);
  close(fd);
  return found;
}

uint64_t now_ms(void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (uint64_t)tv.tv_sec * 1000u + (uint64_t)tv.tv_usec / 1000u;
}

/* ------------------------------------------------------------------ notificação */

#ifdef __PROSPERO__
typedef struct notify_request {
  char useless1[45];
  char message[3075];
} notify_request_t;
int sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int);
#endif

void (*conv_notify_hook)(const char *msg) = NULL;

void conv_notify(const char *fmt, ...) {
  char msg[3075];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);
  if (conv_notify_hook) conv_notify_hook(msg);
#ifdef __PROSPERO__
  notify_request_t req;
  memset(&req, 0, sizeof req);
  strncpy(req.message, msg, sizeof req.message - 1);
  sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
#else
  if (!conv_notify_hook) { /* com hook (payload no host), a mensagem já foi para o log/stdout */
    printf("[notify] %s\n", msg);
    fflush(stdout);
  }
#endif
}
