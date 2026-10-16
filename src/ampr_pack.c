/* Empacotador AMPRPAK4 + volumes AMPRDAT3 + sidecars AMPRCRC1/AMPRCFG1.
 * Segue tools/ampr_pack.py / tools/ampr_pack_format.py do ampr_emu (layout "mixed", grupo único,
 * blocos de 64 KiB ou 16 KiB, io_page 64 KiB, alinhamento de chunk 64 B). GPL-3.0. */
#include "ampr_pack.h"
#include "ampr_index.h"
#include "util.h"
#include "lz4.h"
#include "lz4hc.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define BATCH_BYTES (16u << 20)
#define BATCH_MAX_BLOCKS 16384u
#define VOL_BUF_BYTES (8u << 20)
#define PAYLOAD_OFFSET AMPR_IO_PAGE_SIZE /* align_up(64, max(payload_alignment, io_page)) */

/* auto-loose (valores padrão do ampr_pack.py) */
#define AUTO_LOOSE_MIN_SIZE (64ull << 20)
#define AUTO_LOOSE_SAMPLE_BLOCKS 32u
#define AUTO_LOOSE_SAMPLE_BYTES (16ull << 20)

/* ------------------------------------------------------------------ decisão de codec */

int ampr_keep_lz4_ex(uint32_t raw, uint32_t comp, uint32_t io_page, uint32_t min_bytes, uint32_t min_bp,
                     uint32_t io_min_bytes, uint32_t io_min_bp) {
  if (comp == 0 || comp >= raw) return 0;
  uint64_t saved = raw - comp;
  uint64_t req_bytes = min_bytes, req_bp = min_bp;
  /* Layout não-streaming: se não economiza página de E/S, exige os limiares "io_neutral". */
  uint64_t raw_pages = (raw + (uint64_t)io_page - 1) / io_page;
  uint64_t comp_pages = (comp + (uint64_t)io_page - 1) / io_page;
  if (comp_pages >= raw_pages) {
    req_bytes = io_min_bytes;
    req_bp = io_min_bp;
  }
  if (saved < req_bytes) return 0;
  if (saved * 10000u < (uint64_t)raw * req_bp) return 0; /* ratio < required (exato em inteiros) */
  return 1;
}

int ampr_keep_lz4(uint32_t raw, uint32_t comp, uint32_t io_page) {
  /* min_savings_bytes=64, min_savings_ratio=0.01, io_neutral 8 KiB / 0.125 */
  return ampr_keep_lz4_ex(raw, comp, io_page, 64, 100, 8192, 1250);
}

void ampr_runtime_encode(uint8_t rt[AMPR_CFG1_SIZE], const uint8_t build_id[16], uint64_t manifest_len,
                         uint64_t idx_len) {
  /* runtime_budget() de theghostonline/PS5-LZ4-Compression src/ampr_pack.c (GPL-3.0-or-later,
   * Copyright (C) 2026 Husam Osman): reserva os buffers fixos do runtime, o manifesto e o AMPRIDX3
   * residentes e divide o resto do pool: 1/4 (até 64 MiB) para o cache físico, o resto (até 256 MiB)
   * para o cache decodificado; ambos múltiplos de 16 KiB. */
  const uint64_t MiB = 1ull << 20;
  uint64_t fixed = 32 * 2 * MiB + (uint64_t)AMPR_RT_WORKERS * MiB + 32 * MiB;
  uint64_t used = fixed + manifest_len + idx_len + 16 * MiB;
  uint64_t avail = AMPR_RT_POOL_BYTES > used ? AMPR_RT_POOL_BYTES - used : 0;
  uint64_t physical = avail / 4;
  if (physical > 64 * MiB) physical = 64 * MiB;
  uint64_t decoded = avail - physical;
  if (decoded > 256 * MiB) decoded = 256 * MiB;
  physical = align_down_u64(physical, 16384);
  decoded = align_down_u64(decoded, 16384);
  memset(rt, 0, AMPR_CFG1_SIZE);
  memcpy(rt, "AMPRCFG1", 8);
  le32_put(rt + 8, 1);
  le32_put(rt + 12, AMPR_CFG1_SIZE);
  memcpy(rt + 16, build_id, 16);
  le64_put(rt + 32, decoded);
  le64_put(rt + 40, physical);
  le32_put(rt + 48, AMPR_RT_WORKERS);
  le32_put(rt + 52, AMPR_RT_RESERVE_WORKERS);
  le32_put(rt + 56, crc32_update(0, rt, AMPR_CFG1_SIZE));
}

/* ------------------------------------------------------------------ compressão */

typedef struct {
  void *state;
  int level;
} lz4ctx_t;

static int lz4ctx_init(lz4ctx_t *c, int level) {
  c->level = level;
  c->state = malloc(level <= 4 ? (size_t)LZ4_sizeofState() : (size_t)LZ4_sizeofStateHC());
  return c->state ? 0 : -1;
}

static int lz4ctx_compress(lz4ctx_t *c, const uint8_t *src, uint32_t len, uint8_t *dst, uint32_t cap) {
  if (cap == 0) return 0;
  if (c->level <= 4)
    return LZ4_compress_fast_extState(c->state, (const char *)src, (char *)dst, (int)len, (int)cap, 5 - c->level);
  return LZ4_compress_HC_extStateHC(c->state, (const char *)src, (char *)dst, (int)len, (int)cap, c->level);
}

/* Comprime um bloco; devolve codec e tamanho armazenado (dst recebe o LZ4 se codec==LZ4). */
static uint8_t compress_block(lz4ctx_t *c, const scan_file_t *f, const uint8_t *raw, uint32_t len, uint8_t *dst,
                              uint32_t *stored) {
  int n = lz4ctx_compress(c, raw, len, dst, len - 1);
  if (n > 0 && ampr_keep_lz4_ex(len, (uint32_t)n, AMPR_IO_PAGE_SIZE, f->sav_min_bytes, f->sav_min_bp,
                                f->sav_io_min_bytes, f->sav_io_min_bp)) {
    *stored = (uint32_t)n;
    return AMPR_CHUNK_CODEC_LZ4;
  }
  *stored = len;
  return AMPR_CHUNK_CODEC_RAW;
}

typedef struct {
  uint32_t file;     /* índice em r->files */
  uint32_t off;      /* deslocamento em in/out do lote */
  uint32_t raw_len;
  uint32_t stored_len;
  uint32_t crc;
  uint8_t codec;
  uint8_t fp[16];
} blk_t;

typedef struct {
  uint8_t *in, *out;
  blk_t *blk;
  const scan_file_t *files; /* limiares de economia por arquivo */
  uint32_t n, used;
  _Atomic uint32_t next, done;
} batch_t;

typedef struct {
  pthread_mutex_t mu;
  pthread_cond_t cv_work, cv_done;
  batch_t *cur;
  uint64_t gen;
  int stop;
  int level, dedup;
  pthread_t th[16];
  int nth;
  _Atomic int init_fail;
} pool_t;

static void process_block(lz4ctx_t *c, batch_t *b, uint32_t i, int dedup) {
  blk_t *k = &b->blk[i];
  const uint8_t *raw = b->in + k->off;
  k->crc = crc32_update(0, raw, k->raw_len);
  if (dedup) {
    uint8_t d[32];
    sha256(raw, k->raw_len, d);
    memcpy(k->fp, d, 16);
  }
  k->codec = compress_block(c, &b->files[k->file], raw, k->raw_len, b->out + k->off, &k->stored_len);
}

static void *pool_worker(void *arg) {
  pool_t *p = (pool_t *)arg;
  lz4ctx_t c;
  if (lz4ctx_init(&c, p->level) != 0) {
    atomic_store(&p->init_fail, 1);
    c.state = NULL;
  }
  uint64_t seen = 0;
  for (;;) {
    pthread_mutex_lock(&p->mu);
    while (!p->stop && (p->cur == NULL || seen == p->gen)) pthread_cond_wait(&p->cv_work, &p->mu);
    if (p->stop) { pthread_mutex_unlock(&p->mu); break; }
    batch_t *b = p->cur;
    seen = p->gen;
    pthread_mutex_unlock(&p->mu);
    for (;;) {
      uint32_t i = atomic_fetch_add(&b->next, 1);
      if (i >= b->n) break;
      if (c.state) process_block(&c, b, i, p->dedup);
      else { b->blk[i].codec = 0xFF; } /* sinaliza falha de memória */
      if (atomic_fetch_add(&b->done, 1) + 1 == b->n) {
        pthread_mutex_lock(&p->mu);
        pthread_cond_broadcast(&p->cv_done);
        pthread_mutex_unlock(&p->mu);
      }
    }
  }
  free(c.state);
  return NULL;
}

static int pool_start(pool_t *p, int n, int level, int dedup) {
  memset(p, 0, sizeof *p);
  pthread_mutex_init(&p->mu, NULL);
  pthread_cond_init(&p->cv_work, NULL);
  pthread_cond_init(&p->cv_done, NULL);
  p->level = level;
  p->dedup = dedup;
  pthread_attr_t at;
  pthread_attr_init(&at);
  pthread_attr_setstacksize(&at, 512u << 10); /* pilha padrão do PS5 é pequena; não depender dela */
  for (int i = 0; i < n; i++) {
    if (pthread_create(&p->th[i], &at, pool_worker, p) != 0) break;
    p->nth++;
  }
  pthread_attr_destroy(&at);
  if (p->nth == 0 && pthread_create(&p->th[0], NULL, pool_worker, p) == 0) p->nth = 1; /* pilha padrão */
  return p->nth > 0 ? 0 : -1;
}

static void pool_stop(pool_t *p) {
  pthread_mutex_lock(&p->mu);
  p->stop = 1;
  pthread_cond_broadcast(&p->cv_work);
  pthread_mutex_unlock(&p->mu);
  for (int i = 0; i < p->nth; i++) pthread_join(p->th[i], NULL);
  pthread_mutex_destroy(&p->mu);
  pthread_cond_destroy(&p->cv_work);
  pthread_cond_destroy(&p->cv_done);
}

static void pool_submit(pool_t *p, batch_t *b) {
  atomic_store(&b->next, 0);
  atomic_store(&b->done, 0);
  pthread_mutex_lock(&p->mu);
  p->cur = b;
  p->gen++;
  pthread_cond_broadcast(&p->cv_work);
  pthread_mutex_unlock(&p->mu);
}

static void pool_wait(pool_t *p, batch_t *b) {
  pthread_mutex_lock(&p->mu);
  while (atomic_load(&b->done) < b->n) pthread_cond_wait(&p->cv_done, &p->mu);
  pthread_mutex_unlock(&p->mu);
}

/* ------------------------------------------------------------------ volumes */

typedef struct {
  int fd;
  uint64_t pos;    /* posição lógica (inclui o que está no buffer) */
  uint8_t *buf;
  size_t blen;
  uint64_t padding;
  char tmp_path[2048];
} vol_t;

static int vol_flush(vol_t *v) {
  if (v->blen && write_full(v->fd, v->buf, v->blen) != 0) return -1;
  v->blen = 0;
  return 0;
}

static int vol_put(vol_t *v, const uint8_t *data, uint64_t n) { /* data NULL = zeros */
  while (n) {
    size_t room = VOL_BUF_BYTES - v->blen;
    size_t k = n < room ? (size_t)n : room;
    if (data) { memcpy(v->buf + v->blen, data, k); data += k; }
    else memset(v->buf + v->blen, 0, k);
    v->blen += k;
    v->pos += k;
    n -= k;
    if (v->blen == VOL_BUF_BYTES && vol_flush(v) != 0) return -1;
  }
  return 0;
}

/* placement_for() do ampr_pack.py para layout random/mixed */
static uint64_t place_offset(uint64_t pos, uint32_t stored, uint32_t *flags) {
  const uint64_t page = AMPR_IO_PAGE_SIZE;
  uint64_t off;
  if (stored >= page) {
    off = align_up_u64(pos, page);
  } else {
    off = align_up_u64(pos, AMPR_CHUNK_ALIGN);
    uint64_t page_end = align_up_u64(off + 1, page);
    if (off + stored > page_end) off = align_up_u64(off, page);
  }
  uint32_t fl = 0;
  if (stored <= page && off / page == (off + stored - 1) / page) fl |= AMPR_CHUNK_FLAG_PAGE_CONTAINED;
  if (off % page == 0) fl |= AMPR_CHUNK_FLAG_PAGE_ALIGNED;
  *flags = fl;
  return off;
}

/* ------------------------------------------------------------------ contexto */

typedef struct {
  uint64_t location;   /* offset | pack_id << 48 */
  uint32_t descriptor; /* (stored-1) | codec<<20 | flags<<22 */
} chunk_t;

typedef struct {
  scan_result_t *r;
  const ampr_pack_opts_t *o;
  ampr_pack_result_t *res;
  char *err;
  size_t errlen;
  /* leitura */
  size_t fi;
  uint64_t foff;
  int fd;
  /* saída */
  vol_t *vols;
  uint32_t n_vols, cap_vols;
  uint64_t max_volume;
  chunk_t *chunks;
  uint32_t *crcs;
  uint8_t (*fps)[16];
  uint32_t *raw_lens;
  uint64_t n_chunks, cap_chunks;
  uint32_t *first_chunk, *chunk_count; /* por r->files */
  /* dedup: tabela aberta de índice+1 */
  uint32_t *dd;
  uint64_t dd_mask;
  /* progresso */
  uint64_t done_bytes, total_bytes;
} pctx_t;

static int perr(pctx_t *c, const char *fmt, const char *a, int e) {
  char m[256];
  snprintf(m, sizeof m, fmt, a ? a : "");
  if (e) snprintf(c->err, c->errlen, "%s: %s", m, e == ENOSPC ? "sem espaço no destino" : strerror(e));
  else snprintf(c->err, c->errlen, "%s", m);
  return -1;
}

static int cancelled(pctx_t *c) { return c->o->p && atomic_load(&c->o->p->cancel); }

static int new_volume(pctx_t *c) {
  if (c->n_vols >= 0xFFFFu) return perr(c, "volumes demais", NULL, 0);
  if (c->n_vols == c->cap_vols) {
    uint32_t nc = c->cap_vols ? c->cap_vols * 2 : 4;
    vol_t *nv = (vol_t *)realloc(c->vols, nc * sizeof *nv);
    if (!nv) return perr(c, "sem memória", NULL, 0);
    c->vols = nv;
    c->cap_vols = nc;
  }
  if (c->n_vols) { /* fecha a cauda do volume anterior (rollover) */
    vol_t *pv = &c->vols[c->n_vols - 1];
    uint64_t tail = align_up_u64(pv->pos, AMPR_IO_PAGE_SIZE) - pv->pos;
    if (vol_put(pv, NULL, tail) != 0 || vol_flush(pv) != 0) return perr(c, "escrita em %s", pv->tmp_path, errno);
    pv->padding += tail;
    free(pv->buf);
    pv->buf = NULL;
  }
  vol_t *v = &c->vols[c->n_vols];
  memset(v, 0, sizeof *v);
  v->fd = -1;
  char name[64];
  snprintf(name, sizeof name, AMPR_PACK_NAME_FMT ".tmp", c->n_vols);
  if (path_join(v->tmp_path, sizeof v->tmp_path, c->o->dst_dir, name) != 0) return perr(c, "caminho longo", NULL, 0);
  c->n_vols++;
  v->buf = (uint8_t *)malloc(VOL_BUF_BYTES);
  if (!v->buf) return perr(c, "sem memória", NULL, 0);
  v->fd = open(v->tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0777);
  if (v->fd < 0) return perr(c, "não criou %s", v->tmp_path, errno);
  /* 0777 explícito (umask): ver o comentário em convert.c sobre SIGBUS/CE-108255-1. */
  if (fchmod(v->fd, 0777) != 0) return perr(c, "chmod %s", v->tmp_path, errno);
  if (vol_put(v, NULL, PAYLOAD_OFFSET) != 0) return perr(c, "escrita em %s", v->tmp_path, errno);
  v->padding = PAYLOAD_OFFSET - AMPR_DAT3_HEADER_SIZE;
  return 0;
}

static int push_chunk(pctx_t *c, uint64_t loc, uint32_t desc, uint32_t crc, const uint8_t *fp, uint32_t raw_len) {
  if (c->n_chunks >= 0xFFFFFFFFull) return perr(c, "chunks demais para AMPRPAK4", NULL, 0);
  if (c->n_chunks == c->cap_chunks) {
    uint64_t nc = c->cap_chunks ? c->cap_chunks * 2 : 4096;
    chunk_t *a = (chunk_t *)realloc(c->chunks, nc * sizeof *a);
    if (a) c->chunks = a;
    uint32_t *b = (uint32_t *)realloc(c->crcs, nc * sizeof *b);
    if (b) c->crcs = b;
    uint8_t(*d)[16] = (uint8_t(*)[16])realloc(c->fps, nc * 16);
    if (d) c->fps = d;
    uint32_t *e = (uint32_t *)realloc(c->raw_lens, nc * sizeof *e);
    if (e) c->raw_lens = e;
    if (!a || !b || !d || !e) return perr(c, "sem memória", NULL, 0);
    c->cap_chunks = nc;
  }
  c->chunks[c->n_chunks].location = loc;
  c->chunks[c->n_chunks].descriptor = desc;
  c->crcs[c->n_chunks] = crc;
  if (fp) memcpy(c->fps[c->n_chunks], fp, 16);
  else memset(c->fps[c->n_chunks], 0, 16);
  c->raw_lens[c->n_chunks] = raw_len;
  c->n_chunks++;
  return 0;
}

static uint64_t dd_hash(const uint8_t *fp, uint32_t raw_len, uint8_t codec) {
  uint64_t h = le64_get(fp) ^ ((uint64_t)raw_len << 8) ^ codec;
  return h * 0x9E3779B97F4A7C15ull;
}

/* Grava (ou reaproveita) o bloco k do lote no volume corrente. */
static int place_block(pctx_t *c, batch_t *b, blk_t *k) {
  const uint8_t *data = (k->codec == AMPR_CHUNK_CODEC_LZ4) ? b->out + k->off : b->in + k->off;
  uint32_t stored = k->stored_len;
  ampr_pack_result_t *res = c->res;

  if (c->dd) {
    uint64_t pos = dd_hash(k->fp, k->raw_len, k->codec) & c->dd_mask;
    for (;;) {
      uint32_t v = c->dd[pos];
      if (!v) break;
      uint64_t ci = v - 1;
      const chunk_t *pr = &c->chunks[ci];
      uint32_t pcodec = (pr->descriptor >> 20) & 3u;
      if (c->raw_lens[ci] == k->raw_len && pcodec == k->codec && memcmp(c->fps[ci], k->fp, 16) == 0) {
        uint32_t pflags = (pr->descriptor >> 22) & 0xFFu;
        uint32_t flags = AMPR_CHUNK_FLAG_SHARED |
                         (pflags & (AMPR_CHUNK_FLAG_PAGE_CONTAINED | AMPR_CHUNK_FLAG_PAGE_ALIGNED));
        uint32_t desc = (pr->descriptor & 0x3FFFFFu) | (flags << 22);
        res->chunks_shared++;
        if (k->codec == AMPR_CHUNK_CODEC_LZ4) res->chunks_lz4++;
        else res->chunks_raw++;
        return push_chunk(c, pr->location, desc, k->crc, k->fp, k->raw_len);
      }
      pos = (pos + 1) & c->dd_mask;
    }
  }

  if (c->n_vols == 0 && new_volume(c) != 0) return -1;
  vol_t *v = &c->vols[c->n_vols - 1];
  uint32_t flags;
  uint64_t off = place_offset(v->pos, stored, &flags);
  if (align_up_u64(off + stored, AMPR_IO_PAGE_SIZE) > c->max_volume) {
    if (new_volume(c) != 0) return -1;
    v = &c->vols[c->n_vols - 1];
    off = place_offset(v->pos, stored, &flags);
  }
  /* não-streaming exige posicionamento seguro de página */
  int page_safe = stored <= AMPR_IO_PAGE_SIZE ? (flags & AMPR_CHUNK_FLAG_PAGE_CONTAINED) != 0
                                              : (flags & AMPR_CHUNK_FLAG_PAGE_ALIGNED) != 0;
  if (!page_safe) return perr(c, "chunk sem posicionamento seguro de página (bug)", NULL, 0);
  uint64_t pad = off - v->pos;
  if (vol_put(v, NULL, pad) != 0 || vol_put(v, data, stored) != 0)
    return perr(c, "escrita em %s", v->tmp_path, errno);
  v->padding += pad;
  uint32_t pack_id = c->n_vols - 1;
  uint64_t loc = off | ((uint64_t)pack_id << 48);
  uint32_t desc = (stored - 1) | ((uint32_t)k->codec << 20) | (flags << 22);
  if (push_chunk(c, loc, desc, k->crc, c->dd ? k->fp : NULL, k->raw_len) != 0) return -1;
  res->stored_bytes += stored;
  if (k->codec == AMPR_CHUNK_CODEC_LZ4) res->chunks_lz4++;
  else res->chunks_raw++;
  if (c->dd) {
    uint64_t pos = dd_hash(k->fp, k->raw_len, k->codec) & c->dd_mask;
    while (c->dd[pos]) pos = (pos + 1) & c->dd_mask;
    c->dd[pos] = (uint32_t)c->n_chunks; /* índice + 1 */
  }
  return 0;
}

/* Preenche um lote com blocos dos próximos arquivos PACK (ordem de fileId). */
static int fill_batch(pctx_t *c, batch_t *b) {
  b->n = 0;
  b->used = 0;
  scan_result_t *r = c->r;
  b->files = r->files;
  while (c->fi < r->n_files) {
    scan_file_t *f = &r->files[c->fi];
    if (f->action != SCAN_ACT_PACK) { c->fi++; continue; }
    if (c->fd < 0) {
      c->fd = open(f->src, O_RDONLY);
      if (c->fd < 0) return perr(c, "não abriu %s", f->src, errno);
      struct stat st;
      if (fstat(c->fd, &st) != 0 || (uint64_t)st.st_size != f->size)
        return perr(c, "arquivo mudou durante a conversão: %s", f->rel, 0);
      c->foff = 0;
    }
    uint32_t bs = 1u << f->block_shift;
    uint64_t left = f->size - c->foff;
    uint32_t space = BATCH_BYTES - b->used;
    uint32_t need = left < bs ? (uint32_t)left : bs;
    if (space < need || b->n == BATCH_MAX_BLOCKS) break;
    uint64_t span = (uint64_t)(space / bs) * bs;
    uint64_t max_blocks = BATCH_MAX_BLOCKS - b->n;
    if (span > max_blocks * bs) span = max_blocks * bs;
    if (span == 0 || span > left) span = left;
    if (pread_full(c->fd, b->in + b->used, (size_t)span, (off_t)c->foff) != 0)
      return perr(c, "leitura de %s", f->rel, errno);
    for (uint64_t o = 0; o < span; o += bs) {
      blk_t *k = &b->blk[b->n++];
      k->file = (uint32_t)c->fi;
      k->off = b->used + (uint32_t)o;
      k->raw_len = (uint32_t)((span - o) < bs ? (span - o) : bs);
    }
    b->used += (uint32_t)span;
    c->foff += span;
    if (c->foff == f->size) {
      close(c->fd);
      c->fd = -1;
      c->fi++;
    }
  }
  return 0;
}

static int place_batch(pctx_t *c, batch_t *b) {
  for (uint32_t i = 0; i < b->n; i++) {
    blk_t *k = &b->blk[i];
    if (k->codec == 0xFF) return perr(c, "sem memória para estado LZ4", NULL, 0);
    if (c->chunk_count[k->file] == 0) c->first_chunk[k->file] = (uint32_t)c->n_chunks;
    if (place_block(c, b, k) != 0) return -1;
    c->chunk_count[k->file]++;
    c->res->logical_bytes += k->raw_len;
    c->done_bytes += k->raw_len;
  }
  if (c->o->p) atomic_store(&c->o->p->bytes_done, c->done_bytes);
  return 0;
}

/* _sample_block_indices + _auto_loose_decision do ampr_pack.py */
static int auto_loose(pctx_t *c, scan_file_t *f, lz4ctx_t *lz, uint8_t *in, uint8_t *out, int *loose) {
  *loose = 0;
  /* auto_loose_hot_files = false (padrão do ampr_pack.py): arquivos HOT nunca saem por amostragem. */
  if (f->size < AUTO_LOOSE_MIN_SIZE || (f->file_flags & AMPR_FILE_FLAG_HOT)) return 0;
  uint64_t bs = 1ull << f->block_shift;
  uint64_t total = (f->size + bs - 1) / bs;
  uint64_t by_bytes = AUTO_LOOSE_SAMPLE_BYTES / bs;
  if (by_bytes < 1) by_bytes = 1;
  uint64_t samples = AUTO_LOOSE_SAMPLE_BLOCKS < by_bytes ? AUTO_LOOSE_SAMPLE_BLOCKS : by_bytes;
  int fd = open(f->src, O_RDONLY);
  if (fd < 0) return perr(c, "não abriu %s", f->src, errno);
  uint64_t sampled = 0, stored_sum = 0, raw_blocks = 0, count = 0, prev = UINT64_MAX;
  for (uint64_t s = 0; s < samples; s++) {
    uint64_t idx;
    if (total <= samples) idx = s;
    else if (samples == 1) idx = total / 2;
    else idx = (s * (total - 1)) / (samples - 1);
    if (idx >= total) break;
    if (idx == prev) continue; /* conjunto (sem repetição) */
    prev = idx;
    uint64_t off = idx * bs;
    uint32_t len = (uint32_t)((f->size - off) < bs ? (f->size - off) : bs);
    if (pread_full(fd, in, len, (off_t)off) != 0) { close(fd); return perr(c, "leitura de %s", f->rel, errno); }
    uint32_t st;
    uint8_t codec = compress_block(lz, f, in, len, out, &st);
    sampled += len;
    stored_sum += st;
    raw_blocks += codec == AMPR_CHUNK_CODEC_RAW;
    count++;
  }
  close(fd);
  if (!count || !sampled) return 0;
  double savings = 1.0 - (double)stored_sum / (double)sampled;
  double raw_ratio = (double)raw_blocks / (double)count;
  *loose = savings < 0.05 || raw_ratio >= 0.90;
  return 0;
}

/* Estimativa do espaço de saída (pré-checagem de espaço livre): comprime uma amostra espalhada dos arquivos
 * PACK com o mesmo compressor/limiares do empacotador e extrapola. Arquivos >= 64 MiB: até 64 blocos cada;
 * menores: amostra combinada (primeiro bloco de arquivos espaçados). Orçamento total ~256 MiB. Sem pack_all,
 * arquivo grande que cairia no auto-loose conta pelo tamanho cheio (vira solto). */
#define EST_BLOCKS_PER_FILE 64u
#define EST_BUDGET_BYTES (256ull << 20)
#define EST_SMALL_BUDGET (32ull << 20)

static int est_sample(int fd, const scan_file_t *f, uint64_t nblk, lz4ctx_t *lz, uint8_t *in, uint8_t *out,
                      uint64_t *sampled, uint64_t *stored_sum, uint64_t *raw_blocks, uint64_t *count) {
  uint64_t bs = 1ull << f->block_shift;
  uint64_t total = (f->size + bs - 1) / bs;
  uint64_t prev = UINT64_MAX;
  for (uint64_t s = 0; s < nblk; s++) {
    uint64_t idx = total <= nblk ? s : nblk == 1 ? total / 2 : (s * (total - 1)) / (nblk - 1);
    if (idx >= total) break;
    if (idx == prev) continue;
    prev = idx;
    uint64_t off = idx * bs;
    uint32_t len = (uint32_t)((f->size - off) < bs ? (f->size - off) : bs);
    if (pread_full(fd, in, len, (off_t)off) != 0) return -1;
    uint32_t st;
    uint8_t codec = compress_block(lz, f, in, len, out, &st);
    *sampled += len;
    *stored_sum += st;
    *raw_blocks += codec == AMPR_CHUNK_CODEC_RAW;
    (*count)++;
  }
  return 0;
}

int ampr_pack_estimate(const scan_result_t *r, int level, int pack_all, ampr_estimate_t *est, char *err,
                       size_t errlen) {
  memset(est, 0, sizeof *est);
  level = level < 1 ? 1 : level > 12 ? 12 : level;
  uint64_t maxbs = 1, n_big = 0, small_bytes = 0, n_small = 0, chunks = 0, n_packed_files = 0;
  for (size_t i = 0; i < r->n_files; i++) {
    const scan_file_t *f = &r->files[i];
    if (!f->indexed || f->action != SCAN_ACT_PACK) { est->loose_bytes += f->size; continue; }
    uint64_t bs = 1ull << f->block_shift;
    if (bs > maxbs) maxbs = bs;
    est->packed_src_bytes += f->size;
    chunks += (f->size + bs - 1) / bs;
    n_packed_files++;
    if (f->size >= AUTO_LOOSE_MIN_SIZE) n_big++;
    else { small_bytes += f->size; n_small++; }
  }
  /* manifesto/CRC/índice: ~16 B por chunk (+ CRC 4 B) e ~160 B por arquivo (nome incluído, com folga) */
  est->overhead_bytes = chunks * 24 + n_packed_files * 160 + (1ull << 20);
  uint8_t *in = (uint8_t *)malloc(maxbs), *out = (uint8_t *)malloc(maxbs);
  lz4ctx_t lz = {NULL, level};
  int rc = -1;
  if (!in || !out || lz4ctx_init(&lz, level) != 0) { snprintf(err, errlen, "sem memória"); goto done; }

  uint64_t big_budget = EST_BUDGET_BYTES - (n_small ? EST_SMALL_BUDGET : 0);
  uint64_t big_blocks = n_big ? big_budget / maxbs / n_big : 0;
  if (big_blocks > EST_BLOCKS_PER_FILE) big_blocks = EST_BLOCKS_PER_FILE;
  if (big_blocks < 2) big_blocks = 2;
  uint64_t small_stride = 1;
  if (n_small) {
    uint64_t want = EST_SMALL_BUDGET / maxbs;
    if (want < 1) want = 1;
    small_stride = (n_small + want - 1) / want;
  }
  uint64_t ss = 0, sst = 0, k = 0; /* amostra combinada dos pequenos */
  for (size_t i = 0; i < r->n_files; i++) {
    const scan_file_t *f = &r->files[i];
    if (!f->indexed || f->action != SCAN_ACT_PACK) continue;
    int big = f->size >= AUTO_LOOSE_MIN_SIZE;
    if (!big && (k++ % small_stride) != 0) continue;
    if (f->size == 0) continue;
    int fd = open(f->src, O_RDONLY);
    if (fd < 0) { snprintf(err, errlen, "não abriu %s: %s", f->src, strerror(errno)); goto done; }
    uint64_t sa = 0, st = 0, rb = 0, ct = 0;
    int e = est_sample(fd, f, big ? big_blocks : 1, &lz, in, out, &sa, &st, &rb, &ct);
    int eno = errno;
    close(fd);
    if (e != 0) { snprintf(err, errlen, "leitura de %s: %s", f->rel, strerror(eno)); goto done; }
    est->sampled_bytes += sa;
    if (!big) { ss += sa; sst += st; continue; }
    if (!sa) continue;
    double savings = 1.0 - (double)st / (double)sa;
    double raw_ratio = (double)rb / (double)ct;
    if (!pack_all && !(f->file_flags & AMPR_FILE_FLAG_HOT) && (savings < 0.05 || raw_ratio >= 0.90)) {
      est->loose_bytes += f->size; /* auto-loose: copiado inteiro */
      est->auto_loose_bytes += f->size;
    } else {
      est->packed_est_bytes += (uint64_t)((double)f->size * (double)st / (double)sa) + 1;
    }
  }
  /* pequenos: razão combinada aplicada a todos (a decisão de auto-loose só vale para >= 64 MiB) */
  double sratio = ss ? (double)sst / (double)ss : 1.0;
  est->packed_est_bytes += (uint64_t)((double)small_bytes * sratio) + (n_small ? 1 : 0);
  rc = 0;
done:
  free(in);
  free(out);
  free(lz.state);
  return rc;
}

/* ------------------------------------------------------------------ montagem */

static void put_file_rec(uint8_t *p, uint64_t hash, uint64_t size, int64_t mtime, uint32_t first, uint32_t count,
                         uint32_t poff, uint32_t plen, uint32_t flags, uint8_t shift) {
  le64_put(p, hash);
  le64_put(p + 8, size);
  le64_put(p + 16, (uint64_t)mtime);
  le32_put(p + 24, first);
  le32_put(p + 28, count);
  le32_put(p + 32, poff);
  le32_put(p + 36, plen);
  le32_put(p + 40, flags);
  p[44] = shift;
  p[45] = 0; /* packing_class: índice do grupo "default" */
  le16_put(p + 46, 0);
}

static void sha_part(sha256_ctx *s, const void *data, uint64_t len) {
  uint8_t l[8];
  le64_put(l, len);
  sha256_update(s, l, 8);
  sha256_update(s, data, (size_t)len);
}

static int assemble(pctx_t *c) {
  scan_result_t *r = c->r;
  ampr_pack_result_t *res = c->res;
  uint64_t nf = r->n_indexed, nc = c->n_chunks, np = c->n_vols;

  /* strings: caminhos na ordem de fileId, depois nomes dos volumes */
  uint64_t ssize = 0;
  for (size_t i = 0; i < r->n_files; i++)
    if (r->files[i].indexed) ssize += r->files[i].app0_len + 1u;
  char names[64];
  uint64_t names_size = 0;
  for (uint32_t i = 0; i < np; i++) names_size += (uint64_t)snprintf(names, sizeof names, AMPR_PACK_NAME_FMT, i) + 1;
  ssize += names_size;
  if (ssize > 0xFFFFFFFFull) return perr(c, "tabela de strings grande demais", NULL, 0);

  uint64_t files_off = AMPR_PAK4_HEADER_SIZE;
  uint64_t chunks_off = files_off + nf * AMPR_PAK4_FILE_REC;
  uint64_t packs_off = chunks_off + nc * AMPR_PAK4_CHUNK_REC;
  uint64_t strings_off = packs_off + np * AMPR_PAK4_PACK_REC;
  uint64_t total = strings_off + ssize;
  uint8_t *m = (uint8_t *)calloc(1, (size_t)total);
  if (!m) return perr(c, "sem memória (manifesto)", NULL, 0);

  uint8_t *str = m + strings_off;
  uint64_t spos = 0, fidx = 0;
  for (size_t i = 0; i < r->n_files; i++) {
    scan_file_t *f = &r->files[i];
    if (!f->indexed) continue;
    memcpy(str + spos, f->app0, f->app0_len);
    uint64_t hash = ampr_path_hash(f->app0, f->app0_len, FNV64_BASIS_PAK4);
    uint8_t *rec = m + files_off + fidx * AMPR_PAK4_FILE_REC;
    if (f->action == SCAN_ACT_PACK)
      put_file_rec(rec, hash, f->size, f->mtime, c->first_chunk[i], c->chunk_count[i], (uint32_t)spos, f->app0_len,
                   AMPR_FILE_FLAG_PACKED | f->file_flags, f->block_shift);
    else
      put_file_rec(rec, hash, f->size, f->mtime, 0, 0, (uint32_t)spos, f->app0_len, 0, 0);
    spos += f->app0_len + 1u;
    fidx++;
  }
  for (uint64_t i = 0; i < nc; i++) {
    uint8_t *p = m + chunks_off + i * AMPR_PAK4_CHUNK_REC;
    le64_put(p, c->chunks[i].location);
    le32_put(p + 8, c->chunks[i].descriptor);
  }
  for (uint32_t i = 0; i < np; i++) {
    vol_t *v = &c->vols[i];
    int nl = snprintf(names, sizeof names, AMPR_PACK_NAME_FMT, i);
    memcpy(str + spos, names, (size_t)nl);
    uint8_t *p = m + packs_off + (uint64_t)i * AMPR_PAK4_PACK_REC;
    le64_put(p, v->pos - PAYLOAD_OFFSET);
    le64_put(p + 8, v->pos);
    le32_put(p + 16, (uint32_t)spos);
    le32_put(p + 20, (uint32_t)nl);
    le32_put(p + 24, AMPR_PACK_FLAG_IO_PAGE_LAYOUT);
    le32_put(p + 28, AMPR_IO_PAGE_SIZE);
    spos += (uint64_t)nl + 1;
  }

  /* payload do .crc */
  uint8_t *crcf = (uint8_t *)malloc((size_t)(AMPR_CRC1_HEADER_SIZE + nc * 4));
  if (!crcf) { free(m); return perr(c, "sem memória (crc)", NULL, 0); }
  uint8_t *crcp = crcf + AMPR_CRC1_HEADER_SIZE;
  for (uint64_t i = 0; i < nc; i++) le32_put(crcp + i * 4, c->crcs[i]);

  /* build_id = SHA-256("AMPRPACK4\0" + (len u64 + bytes) de cada parte)[:16]
   * partes: config canônica, files, chunks, crcs, packs, strings (como deterministic_build_id). */
  char cfg[256];
  int cl = snprintf(cfg, sizeof cfg,
                    "{\"tool\":\"ampr-lz4-c\",\"version\":1,\"layout\":\"mixed\",\"io_page_size\":%u,"
                    "\"chunk_alignment\":%u,\"level\":%d,\"dedup\":%d}",
                    AMPR_IO_PAGE_SIZE, AMPR_CHUNK_ALIGN, c->o->level, c->o->dedup ? 1 : 0);
  sha256_ctx s;
  sha256_init(&s);
  sha256_update(&s, "AMPRPACK4\0", 10);
  sha_part(&s, cfg, (uint64_t)cl);
  sha_part(&s, m + files_off, nf * AMPR_PAK4_FILE_REC);
  sha_part(&s, m + chunks_off, nc * AMPR_PAK4_CHUNK_REC);
  sha_part(&s, crcp, nc * 4);
  sha_part(&s, m + packs_off, np * AMPR_PAK4_PACK_REC);
  sha_part(&s, str, ssize);
  uint8_t dig[32];
  sha256_final(&s, dig);
  memcpy(res->build_id, dig, 16);

  /* cabeçalho AMPRPAK4 */
  memcpy(m, "AMPRPAK4", 8);
  le32_put(m + 8, 4);
  le32_put(m + 12, AMPR_PAK4_HEADER_SIZE);
  le32_put(m + 16, 0);
  le32_put(m + 20, 0x01020304u);
  memcpy(m + 24, res->build_id, 16);
  le64_put(m + 40, nf);
  le64_put(m + 48, nc);
  le32_put(m + 56, (uint32_t)np);
  le32_put(m + 60, AMPR_PAK4_FILE_REC);
  le32_put(m + 64, AMPR_PAK4_CHUNK_REC);
  le32_put(m + 68, AMPR_PAK4_PACK_REC);
  le64_put(m + 72, files_off);
  le64_put(m + 80, chunks_off);
  le64_put(m + 88, packs_off);
  le64_put(m + 96, strings_off);
  le64_put(m + 104, ssize);
  le32_put(m + 112, crc32_update(0, m + AMPR_PAK4_HEADER_SIZE, (size_t)(total - AMPR_PAK4_HEADER_SIZE)));
  le32_put(m + 116, 0);
  le64_put(m + 120, 0);
  le32_put(m + 116, crc32_update(0, m, AMPR_PAK4_HEADER_SIZE));
  res->manifest = m;
  res->manifest_len = (size_t)total;

  /* AMPRCRC1 */
  memcpy(crcf, "AMPRCRC1", 8);
  le32_put(crcf + 8, 1);
  le32_put(crcf + 12, AMPR_CRC1_HEADER_SIZE);
  memcpy(crcf + 16, res->build_id, 16);
  le64_put(crcf + 32, nc);
  le32_put(crcf + 40, crc32_update(0, crcp, (size_t)(nc * 4)));
  le32_put(crcf + 44, 0);
  le32_put(crcf + 44, crc32_update(0, crcf, AMPR_CRC1_HEADER_SIZE));
  res->crc = crcf;
  res->crc_len = (size_t)(AMPR_CRC1_HEADER_SIZE + nc * 4);

  /* AMPRCFG1: orçamento automático (manifesto + AMPRIDX3 residentes) */
  ampr_runtime_encode(res->runtime, res->build_id, total, ampr_idx3_size(r));

  /* cabeçalhos AMPRDAT3 e fechamento dos volumes */
  for (uint32_t i = 0; i < np; i++) {
    vol_t *v = &c->vols[i];
    uint8_t h[AMPR_DAT3_HEADER_SIZE];
    memset(h, 0, sizeof h);
    memcpy(h, "AMPRDAT3", 8);
    le32_put(h + 8, 3);
    le32_put(h + 12, AMPR_DAT3_HEADER_SIZE);
    le32_put(h + 16, i);
    le32_put(h + 20, AMPR_PACK_FLAG_IO_PAGE_LAYOUT);
    memcpy(h + 24, res->build_id, 16);
    le64_put(h + 40, PAYLOAD_OFFSET);
    le64_put(h + 48, v->pos - PAYLOAD_OFFSET);
    le32_put(h + 56, crc32_update(0, h, sizeof h));
    if (pwrite_full(v->fd, h, sizeof h, 0) != 0 || fsync(v->fd) != 0) return perr(c, "finalizando %s", v->tmp_path, errno);
    if (close(v->fd) != 0) { v->fd = -1; return perr(c, "fechando %s", v->tmp_path, errno); }
    v->fd = -1;
    res->volume_bytes += v->pos;
    res->padding_bytes += v->padding;
  }
  res->n_packs = (uint32_t)np;
  res->chunks = nc;
  return 0;
}

int ampr_pack_build(scan_result_t *r, const ampr_pack_opts_t *o, ampr_pack_result_t *res, char *err,
                    size_t errlen) {
  memset(res, 0, sizeof *res);
  pctx_t c;
  memset(&c, 0, sizeof c);
  c.r = r;
  c.o = o;
  c.res = res;
  c.err = err;
  c.errlen = errlen;
  c.fd = -1;
  c.max_volume = o->max_volume ? o->max_volume : AMPR_MAX_VOLUME_BYTES;
  int level = o->level < 1 ? 1 : o->level > 12 ? 12 : o->level;
  int workers = o->workers < 1 ? 1 : o->workers > 16 ? 16 : o->workers;
  int rc = -1;
  batch_t bt[2];
  memset(bt, 0, sizeof bt);
  pool_t pool;
  int pool_on = 0;
  lz4ctx_t lz = {NULL, level};

  c.first_chunk = (uint32_t *)calloc(r->n_files ? r->n_files : 1, sizeof(uint32_t));
  c.chunk_count = (uint32_t *)calloc(r->n_files ? r->n_files : 1, sizeof(uint32_t));
  for (int i = 0; i < 2; i++) {
    bt[i].in = (uint8_t *)malloc(BATCH_BYTES);
    bt[i].out = (uint8_t *)malloc(BATCH_BYTES);
    bt[i].blk = (blk_t *)malloc(BATCH_MAX_BLOCKS * sizeof(blk_t));
  }
  if (!c.first_chunk || !c.chunk_count || !bt[0].in || !bt[0].out || !bt[0].blk || !bt[1].in || !bt[1].out ||
      !bt[1].blk || lz4ctx_init(&lz, level) != 0) {
    perr(&c, "sem memória", NULL, 0);
    goto out;
  }

  /* Planejamento: não indexados ficam loose; auto-loose por amostragem (desligado com pack_all). */
  if (o->p) atomic_store(&o->p->auto_loose_bytes, 0);
  uint64_t bound_chunks = 0;
  for (size_t i = 0; i < r->n_files; i++) {
    scan_file_t *f = &r->files[i];
    if (!f->indexed) f->action = SCAN_ACT_LOOSE;
    if (f->action != SCAN_ACT_PACK) continue;
    if (cancelled(&c)) { rc = -2; goto out; }
    int loose = 0;
    if (!o->pack_all && auto_loose(&c, f, &lz, bt[0].in, bt[0].out, &loose) != 0) goto out;
    if (loose) {
      f->action = SCAN_ACT_LOOSE;
      f->reason = "auto-loose";
      res->files_auto_loose++;
      res->auto_loose_bytes += f->size;
      if (o->p) atomic_store(&o->p->auto_loose_bytes, res->auto_loose_bytes);
      continue;
    }
    c.total_bytes += f->size;
    bound_chunks += (f->size + (1ull << f->block_shift) - 1) >> f->block_shift;
  }
  for (size_t i = 0; i < r->n_files; i++) {
    if (!r->files[i].indexed) continue;
    if (r->files[i].action == SCAN_ACT_PACK) res->files_packed++;
    else res->files_loose++;
  }
  if (o->p) {
    atomic_store(&o->p->bytes_total, c.total_bytes);
    atomic_store(&o->p->bytes_done, 0);
  }
  if (o->dedup && bound_chunks) {
    uint64_t slots = 1024;
    while (slots < bound_chunks * 2) slots <<= 1;
    c.dd = (uint32_t *)calloc((size_t)slots, sizeof(uint32_t));
    if (!c.dd) { perr(&c, "sem memória (dedup)", NULL, 0); goto out; }
    c.dd_mask = slots - 1;
  }

  if (pool_start(&pool, workers, level, o->dedup) != 0) { perr(&c, "pthread_create falhou", NULL, 0); goto out; }
  pool_on = 1;

  /* Pipeline: lê o lote N+1 enquanto o pool comprime o lote N; grava na ordem. */
  c.fi = 0;
  if (fill_batch(&c, &bt[0]) != 0) goto out;
  int cur = 0;
  if (bt[cur].n) pool_submit(&pool, &bt[cur]);
  while (bt[cur].n) {
    int nxt = cur ^ 1;
    int frc = cancelled(&c) ? 0 : fill_batch(&c, &bt[nxt]);
    if (cancelled(&c)) bt[nxt].n = 0;
    pool_wait(&pool, &bt[cur]);
    if (frc != 0) goto out;
    if (cancelled(&c)) { rc = -2; goto out; }
    if (place_batch(&c, &bt[cur]) != 0) goto out;
    if (bt[nxt].n) pool_submit(&pool, &bt[nxt]);
    cur = nxt;
  }
  if (cancelled(&c)) { rc = -2; goto out; }

  /* consistência por arquivo */
  for (size_t i = 0; i < r->n_files; i++) {
    scan_file_t *f = &r->files[i];
    if (f->action != SCAN_ACT_PACK) continue;
    uint64_t want = (f->size + (1ull << f->block_shift) - 1) >> f->block_shift;
    if (c.chunk_count[i] != want) { perr(&c, "contagem de chunks inconsistente em %s", f->rel, 0); goto out; }
  }

  /* cauda de cada volume até a página; descarrega buffers */
  for (uint32_t i = 0; i < c.n_vols; i++) {
    vol_t *v = &c.vols[i];
    if (!v->buf) continue; /* já finalizado no rollover */
    uint64_t end = align_up_u64(v->pos, AMPR_IO_PAGE_SIZE);
    uint64_t tail = end - v->pos;
    if (vol_put(v, NULL, tail) != 0 || vol_flush(v) != 0) { perr(&c, "escrita em %s", v->tmp_path, errno); goto out; }
    v->padding += tail;
  }
  if (assemble(&c) != 0) goto out;
  if (o->p) atomic_store(&o->p->bytes_done, c.total_bytes);
  rc = 0;

out:
  if (pool_on) pool_stop(&pool);
  if (c.fd >= 0) close(c.fd);
  for (uint32_t i = 0; i < c.n_vols; i++) {
    if (c.vols[i].fd >= 0) close(c.vols[i].fd);
    free(c.vols[i].buf);
  }
  free(c.vols);
  free(c.chunks);
  free(c.crcs);
  free(c.fps);
  free(c.raw_lens);
  free(c.first_chunk);
  free(c.chunk_count);
  free(c.dd);
  free(lz.state);
  for (int i = 0; i < 2; i++) { free(bt[i].in); free(bt[i].out); free(bt[i].blk); }
  if (rc != 0) {
    res->n_packs = c.n_vols; /* para o chamador apagar os .tmp */
    free(res->manifest); res->manifest = NULL;
    free(res->crc); res->crc = NULL;
  }
  return rc;
}

void ampr_pack_result_free(ampr_pack_result_t *res) {
  free(res->manifest);
  free(res->crc);
  res->manifest = NULL;
  res->crc = NULL;
}
