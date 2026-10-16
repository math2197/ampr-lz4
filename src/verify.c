/* Verificação pós-gravação de um conjunto AMPRPAK4. Espelha load_manifest/validate_manifest/
 * verify_packs do ampr_emu (tools/ampr_pack_format.py, tools/ampr_pack.py). GPL-3.0. */
#include "verify.h"
#include "ampr_pack.h"
#include "util.h"
#include "lz4.h"

#include <errno.h>
#include <stdarg.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define WIN_BYTES (16u << 20)

static int verr(char *err, size_t errlen, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static int verr(char *err, size_t errlen, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(err, errlen, fmt, ap);
  va_end(ap);
  return -1;
}

static int read_whole(const char *path, uint8_t **out, size_t *len) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) return -1;
  struct stat st;
  if (fstat(fd, &st) != 0) { close(fd); return -1; }
  uint8_t *b = (uint8_t *)malloc(st.st_size ? (size_t)st.st_size : 1);
  if (!b) { close(fd); errno = ENOMEM; return -1; }
  if (st.st_size && pread_full(fd, b, (size_t)st.st_size, 0) != 0) { free(b); close(fd); return -1; }
  close(fd);
  *out = b;
  *len = (size_t)st.st_size;
  return 0;
}

typedef struct {
  int fd;
  uint64_t file_size, payload_off;
  uint32_t page;
} vpack_t;

int ampr_verify(const char *dst_dir, const char *manifest_path, const char *crc_path, conv_progress_t *p,
                char *err, size_t errlen) {
  uint8_t *m = NULL, *cf = NULL, *win = NULL, *dec = NULL;
  size_t mlen = 0, clen = 0;
  vpack_t *packs = NULL;
  uint32_t np = 0;
  int rc = -1;

  if (read_whole(manifest_path, &m, &mlen) != 0) { verr(err, errlen, "verificação: não leu %s: %s", manifest_path, strerror(errno)); goto out; }
  if (read_whole(crc_path, &cf, &clen) != 0) { verr(err, errlen, "verificação: não leu %s: %s", crc_path, strerror(errno)); goto out; }
  if (mlen < AMPR_PAK4_HEADER_SIZE || memcmp(m, "AMPRPAK4", 8) || le32_get(m + 8) != 4 ||
      le32_get(m + 12) != AMPR_PAK4_HEADER_SIZE || le32_get(m + 16) != 0 || le32_get(m + 20) != 0x01020304u ||
      le32_get(m + 60) != AMPR_PAK4_FILE_REC || le32_get(m + 64) != AMPR_PAK4_CHUNK_REC ||
      le32_get(m + 68) != AMPR_PAK4_PACK_REC || le64_get(m + 120) != 0) {
    verr(err, errlen, "verificação: cabeçalho AMPRPAK4 inválido");
    goto out;
  }
  {
    uint8_t h[AMPR_PAK4_HEADER_SIZE];
    memcpy(h, m, sizeof h);
    le32_put(h + 116, 0);
    if (crc32_update(0, h, sizeof h) != le32_get(m + 116) ||
        crc32_update(0, m + AMPR_PAK4_HEADER_SIZE, mlen - AMPR_PAK4_HEADER_SIZE) != le32_get(m + 112)) {
      verr(err, errlen, "verificação: CRC do manifesto não confere");
      goto out;
    }
  }
  const uint8_t *build_id = m + 24;
  uint64_t nf = le64_get(m + 40), nc = le64_get(m + 48);
  np = le32_get(m + 56);
  uint64_t files_off = le64_get(m + 72), chunks_off = le64_get(m + 80), packs_off = le64_get(m + 88);
  uint64_t strings_off = le64_get(m + 96), ssize = le64_get(m + 104);
  if (files_off != AMPR_PAK4_HEADER_SIZE || chunks_off != files_off + nf * AMPR_PAK4_FILE_REC ||
      packs_off != chunks_off + nc * AMPR_PAK4_CHUNK_REC || strings_off != packs_off + (uint64_t)np * AMPR_PAK4_PACK_REC ||
      strings_off + ssize != mlen) {
    verr(err, errlen, "verificação: layout de seções inválido");
    goto out;
  }
  const char *strs = (const char *)m + strings_off;

  /* CRC1 */
  if (clen < AMPR_CRC1_HEADER_SIZE || memcmp(cf, "AMPRCRC1", 8) || le32_get(cf + 8) != 1 ||
      le32_get(cf + 12) != AMPR_CRC1_HEADER_SIZE || memcmp(cf + 16, build_id, 16) || le64_get(cf + 32) != nc ||
      clen != AMPR_CRC1_HEADER_SIZE + nc * 4) {
    verr(err, errlen, "verificação: sidecar CRC1 inválido ou de outro build");
    goto out;
  }
  {
    uint8_t h[AMPR_CRC1_HEADER_SIZE];
    memcpy(h, cf, sizeof h);
    le32_put(h + 44, 0);
    if (crc32_update(0, h, sizeof h) != le32_get(cf + 44) ||
        crc32_update(0, cf + AMPR_CRC1_HEADER_SIZE, (size_t)(nc * 4)) != le32_get(cf + 40)) {
      verr(err, errlen, "verificação: CRC do sidecar não confere");
      goto out;
    }
  }

  /* volumes */
  packs = (vpack_t *)calloc(np ? np : 1, sizeof *packs);
  if (!packs) { verr(err, errlen, "sem memória"); goto out; }
  for (uint32_t i = 0; i < np; i++) packs[i].fd = -1;
  for (uint32_t i = 0; i < np; i++) {
    const uint8_t *pr = m + packs_off + (uint64_t)i * AMPR_PAK4_PACK_REC;
    uint64_t payload = le64_get(pr), fsz = le64_get(pr + 8);
    uint32_t noff = le32_get(pr + 16), nlen = le32_get(pr + 20), flags = le32_get(pr + 24), page = le32_get(pr + 28);
    if ((uint64_t)noff + nlen >= ssize || strs[noff + nlen] != 0 || nlen == 0 || nlen > 255 ||
        memchr(strs + noff, '/', nlen) || flags != AMPR_PACK_FLAG_IO_PAGE_LAYOUT || page < 4096 || page > (1u << 20) ||
        (page & (page - 1)) || payload > fsz || fsz % page || (fsz - payload) % page ||
        fsz - payload < AMPR_DAT3_HEADER_SIZE) {
      verr(err, errlen, "verificação: registro de volume %u inválido", i);
      goto out;
    }
    char name[256], path[2048];
    memcpy(name, strs + noff, nlen);
    name[nlen] = 0;
    if (path_join(path, sizeof path, dst_dir, name) != 0) { verr(err, errlen, "caminho longo"); goto out; }
    packs[i].fd = open(path, O_RDONLY);
    if (packs[i].fd < 0) { verr(err, errlen, "verificação: não abriu %s: %s", path, strerror(errno)); goto out; }
    struct stat st;
    uint8_t h[AMPR_DAT3_HEADER_SIZE], z[AMPR_DAT3_HEADER_SIZE];
    if (fstat(packs[i].fd, &st) != 0 || (uint64_t)st.st_size != fsz ||
        pread_full(packs[i].fd, h, sizeof h, 0) != 0) {
      verr(err, errlen, "verificação: tamanho/leitura de %s não confere", name);
      goto out;
    }
    memcpy(z, h, sizeof z);
    le32_put(z + 56, 0);
    if (memcmp(h, "AMPRDAT3", 8) || le32_get(h + 8) != 3 || le32_get(h + 12) != AMPR_DAT3_HEADER_SIZE ||
        le32_get(h + 16) != i || le32_get(h + 20) != flags || memcmp(h + 24, build_id, 16) ||
        le64_get(h + 40) != fsz - payload || le64_get(h + 48) != payload || le32_get(h + 60) != 0 ||
        crc32_update(0, z, sizeof z) != le32_get(h + 56)) {
      verr(err, errlen, "verificação: cabeçalho AMPRDAT3 inválido em %s", name);
      goto out;
    }
    packs[i].file_size = fsz;
    packs[i].payload_off = fsz - payload;
    packs[i].page = page;
  }

  /* total lógico para progresso */
  uint64_t total = 0, done = 0;
  for (uint64_t f = 0; f < nf; f++) {
    const uint8_t *fr = m + files_off + f * AMPR_PAK4_FILE_REC;
    if (le32_get(fr + 40) & AMPR_FILE_FLAG_PACKED) total += le64_get(fr + 8);
  }
  if (p) { atomic_store(&p->bytes_total, total); atomic_store(&p->bytes_done, 0); }

  win = (uint8_t *)malloc(WIN_BYTES);
  dec = (uint8_t *)malloc(1u << 20);
  if (!win || !dec) { verr(err, errlen, "sem memória"); goto out; }
  int64_t win_pack = -1;
  uint64_t win_start = 0, win_len = 0;
  const uint8_t *crcs = cf + AMPR_CRC1_HEADER_SIZE;

  for (uint64_t f = 0; f < nf; f++) {
    const uint8_t *fr = m + files_off + f * AMPR_PAK4_FILE_REC;
    uint64_t hash = le64_get(fr), size = le64_get(fr + 8);
    uint32_t first = le32_get(fr + 24), count = le32_get(fr + 28), poff = le32_get(fr + 32), plen = le32_get(fr + 36);
    uint32_t flags = le32_get(fr + 40);
    uint8_t shift = fr[44];
    if ((uint64_t)poff + plen >= ssize || strs[poff + plen] != 0 || plen < 7 ||
        strncmp(strs + poff, "/app0/", 6) != 0 ||
        ampr_path_hash(strs + poff, plen, FNV64_BASIS_PAK4) != hash || fr[45] != 0 || le16_get(fr + 46) != 0) {
      verr(err, errlen, "verificação: registro de arquivo %llu inválido", (unsigned long long)f + 1);
      goto out;
    }
    if (!(flags & AMPR_FILE_FLAG_PACKED)) {
      if (first || count || shift || flags) { verr(err, errlen, "verificação: loose %llu com chunks", (unsigned long long)f + 1); goto out; }
      continue;
    }
    uint64_t bs = 1ull << shift;
    /* flags aceitos: PACKED, opcionalmente HOT e RANDOM_ACCESS (nunca STREAMING: só gravamos layout mixed) */
    if (shift < 14 || shift > 20 ||
        (flags & ~(uint32_t)(AMPR_FILE_FLAG_PACKED | AMPR_FILE_FLAG_HOT | AMPR_FILE_FLAG_RANDOM_ACCESS)) != 0 ||
        (uint64_t)first + count > nc ||
        count != (size + bs - 1) / bs) {
      verr(err, errlen, "verificação: geometria inválida no arquivo %llu", (unsigned long long)f + 1);
      goto out;
    }
    for (uint32_t k = 0; k < count; k++) {
      if (p && atomic_load(&p->cancel)) { rc = -2; goto out; }
      uint64_t ci = (uint64_t)first + k;
      const uint8_t *cr = m + chunks_off + ci * AMPR_PAK4_CHUNK_REC;
      uint64_t loc = le64_get(cr);
      uint32_t desc = le32_get(cr + 8);
      uint64_t off = loc & ((1ull << 48) - 1);
      uint32_t pid = (uint32_t)(loc >> 48);
      uint32_t stored = (desc & 0xFFFFFu) + 1, codec = (desc >> 20) & 3u, cfl = desc >> 22;
      uint64_t raw = (size - k * bs) < bs ? (size - k * bs) : bs;
      if (pid >= np || (cfl & ~0xFu) || (cfl & AMPR_CHUNK_FLAG_STREAMING) || codec > 1 || stored > bs ||
          (codec == 0 && stored != raw)) {
        verr(err, errlen, "verificação: chunk %llu inválido", (unsigned long long)ci);
        goto out;
      }
      vpack_t *pk = &packs[pid];
      uint64_t page = pk->page;
      int contained = (cfl & AMPR_CHUNK_FLAG_PAGE_CONTAINED) != 0, aligned = (cfl & AMPR_CHUNK_FLAG_PAGE_ALIGNED) != 0;
      if (off < pk->payload_off || off + stored > pk->file_size || off % AMPR_CHUNK_ALIGN ||
          (contained && (stored > page || off / page != (off + stored - 1) / page)) || (aligned && off % page) ||
          (stored <= page && !contained) || (stored > page && !aligned)) {
        verr(err, errlen, "verificação: posicionamento inválido do chunk %llu", (unsigned long long)ci);
        goto out;
      }
      if (win_pack != (int64_t)pid || off < win_start || off + stored > win_start + win_len) {
        uint64_t len = pk->file_size - off;
        if (len > WIN_BYTES) len = WIN_BYTES;
        if (pread_full(pk->fd, win, (size_t)len, (off_t)off) != 0) {
          verr(err, errlen, "verificação: leitura do volume %u: %s", pid, strerror(errno));
          goto out;
        }
        win_pack = pid;
        win_start = off;
        win_len = len;
      }
      const uint8_t *src = win + (off - win_start);
      const uint8_t *data = src;
      if (codec == AMPR_CHUNK_CODEC_LZ4) {
        int n = LZ4_decompress_safe((const char *)src, (char *)dec, (int)stored, (int)raw);
        if (n < 0 || (uint64_t)n != raw) {
          verr(err, errlen, "verificação: LZ4 inválido no chunk %llu", (unsigned long long)ci);
          goto out;
        }
        data = dec;
      }
      if (crc32_update(0, data, (size_t)raw) != le32_get(crcs + ci * 4)) {
        verr(err, errlen, "verificação: CRC do bloco decodificado não confere (chunk %llu, %.*s)",
             (unsigned long long)ci, (int)plen, strs + poff);
        goto out;
      }
      done += raw;
      if (p && (k & 63) == 0) atomic_store(&p->bytes_done, done);
    }
  }
  if (p) atomic_store(&p->bytes_done, done);
  rc = 0;
out:
  if (packs)
    for (uint32_t i = 0; i < np; i++)
      if (packs[i].fd >= 0) close(packs[i].fd);
  free(packs);
  free(m);
  free(cf);
  free(win);
  free(dec);
  return rc;
}
