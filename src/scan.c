/* Varredura da origem e classificação loose/pack. GPL-3.0. */
#include "scan.h"
#include "util.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define APP0_PATH_MAX 1000 /* "/app0/..." < SCE_KERNEL_PATH_MAX (1024) com folga */

typedef struct {
  scan_result_t *r;
  size_t cap_files, cap_dirs;
  const char *root;
  char *err;
  size_t errlen;
} scan_ctx;

static const char *k_loose_ext[] = {
    /* executáveis/módulos */
    ".elf", ".self", ".prx", ".sprx",
    /* metadados/dados pequenos lidos por caminhos não-APR */
    ".bin", ".dat", ".json", ".xml",
    /* mídia (streaming por decodificadores do sistema) */
    ".mp4", ".ogg", ".mp3", ".bk2", ".usm", ".webm", ".wav", ".at9",
    /* áudio Unity/FMOD e bancos de som: lidos com open/read, fora da API AMPR */
    ".resource", ".bank", ".fsb", ".wem", ".bnk",
    /* já comprimidos */
    ".zip", ".7z", ".rar", ".pak", ".gz", ".xz", ".zst", NULL};
static const char *k_exec_ext[] = {".elf", ".self", ".prx", ".sprx", NULL};
static const char *k_loose_top[] = {"sce_sys", "sce_module", "fakelib", "system", NULL};
static const char *k_audio_dirs[] = {"audio", "sound", "sounds", "sfx", "bgm", "music",
                                     "voice", "voices", "snd", NULL};

static int ends_with_ci(const char *s, const char *suf) {
  size_t a = strlen(s), b = strlen(suf);
  return a >= b && strcasecmp(s + a - b, suf) == 0;
}

static int in_list_ci(const char *s, size_t len, const char **list) {
  for (int i = 0; list[i]; i++)
    if (strlen(list[i]) == len && strncasecmp(s, list[i], len) == 0) return 1;
  return 0;
}

int scan_is_excluded(const char *rel) {
  static const char *ex[] = {"ampr_commands.bin", "apr_emu.log", "ampr_emu.index",
                             "ampr_emu.index.tmp", "ampr_emu.index.bak", NULL};
  for (int i = 0; ex[i]; i++)
    if (strcasecmp(rel, ex[i]) == 0) return 1;
  return 0;
}

void scan_classify(const char *rel, uint64_t size, scan_file_t *f) {
  const char *slash = strchr(rel, '/');
  const char *base = strrchr(rel, '/');
  base = base ? base + 1 : rel;
  f->exec = 0;
  f->block_shift = 16;
  f->action = SCAN_ACT_LOOSE;
  for (int i = 0; k_exec_ext[i]; i++)
    if (ends_with_ci(base, k_exec_ext[i])) f->exec = 1;
  if (strcasecmp(base, "eboot.bin") == 0) f->exec = 1;

  if (!slash) { f->reason = "root"; return; }
  if (in_list_ci(rel, (size_t)(slash - rel), k_loose_top)) { f->reason = "system-dir"; return; }
  if (strcasecmp(base, "eboot.bin") == 0) { f->reason = "eboot"; return; }
  for (int i = 0; k_loose_ext[i]; i++)
    if (ends_with_ci(base, k_loose_ext[i])) { f->reason = "extension"; return; }
  if (size == 0) { f->reason = "empty"; return; }

  f->action = SCAN_ACT_PACK;
  f->reason = "pack";
  /* Pastas de áudio: blocos de 16 KiB (leituras pequenas e aleatórias de bancos de som). */
  const char *p = rel;
  while (p < base) {
    const char *e = strchr(p, '/');
    if (!e || e >= base) break;
    if (in_list_ci(p, (size_t)(e - p), k_audio_dirs)) { f->block_shift = 14; break; }
    p = e + 1;
  }
}

static int fail(scan_ctx *c, const char *fmt, const char *a, int e) {
  char m[256];
  snprintf(m, sizeof m, fmt, a);
  if (e) snprintf(c->err, c->errlen, "%s: %s", m, strerror(e));
  else snprintf(c->err, c->errlen, "%s", m);
  return -1;
}

static int add_dir(scan_ctx *c, const char *rel) {
  if (c->r->n_dirs == c->cap_dirs) {
    size_t nc = c->cap_dirs ? c->cap_dirs * 2 : 64;
    char **nd = (char **)realloc(c->r->dirs, nc * sizeof *nd);
    if (!nd) return fail(c, "sem memória (%s)", rel, 0);
    c->r->dirs = nd;
    c->cap_dirs = nc;
  }
  if (!(c->r->dirs[c->r->n_dirs] = strdup(rel))) return fail(c, "sem memória (%s)", rel, 0);
  c->r->n_dirs++;
  return 0;
}

static int add_file(scan_ctx *c, const char *rel, const char *src, const struct stat *st) {
  if (c->r->n_files == c->cap_files) {
    size_t nc = c->cap_files ? c->cap_files * 2 : 256;
    scan_file_t *nf = (scan_file_t *)realloc(c->r->files, nc * sizeof *nf);
    if (!nf) return fail(c, "sem memória (%s)", rel, 0);
    c->r->files = nf;
    c->cap_files = nc;
  }
  scan_file_t *f = &c->r->files[c->r->n_files];
  memset(f, 0, sizeof *f);
  size_t rl = strlen(rel);
  if (rl + 6 > APP0_PATH_MAX) return fail(c, "caminho longo demais: %s", rel, 0);
  f->rel = strdup(rel);
  f->src = strdup(src);
  f->app0 = (char *)malloc(rl + 7);
  f->key = (char *)malloc(rl + 7);
  if (!f->rel || !f->src || !f->app0 || !f->key) {
    free(f->rel); free(f->src); free(f->app0); free(f->key);
    return fail(c, "sem memória (%s)", rel, 0);
  }
  memcpy(f->app0, "/app0/", 6);
  memcpy(f->app0 + 6, rel, rl + 1);
  f->app0_len = (uint32_t)(rl + 6);
  memcpy(f->key, f->app0, rl + 7);
  ampr_fold_path(f->key, f->app0_len);
  f->size = (uint64_t)st->st_size;
  f->mtime = (int64_t)st->st_mtime;
  scan_classify(rel, f->size, f);
  /* Igual ao build_ampr_index.py: caminhos com TAB/CR/LF ficam fora do índice. */
  f->indexed = strpbrk(rel, "\t\r\n") == NULL;
  c->r->n_files++;
  c->r->total_bytes += f->size;
  return 0;
}

static int walk(scan_ctx *c, const char *rel) {
  char abs[2048];
  if (path_join(abs, sizeof abs, c->root, rel) != 0) return fail(c, "caminho longo demais: %s", rel, 0);
  DIR *d = opendir(abs);
  if (!d) return fail(c, "não abriu diretório %s", abs, errno);
  struct dirent *de;
  int rc = 0;
  while (rc == 0 && (de = readdir(d)) != NULL) {
    if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
    char crel[2048], cabs[2048];
    if (*rel) {
      if (snprintf(crel, sizeof crel, "%s/%s", rel, de->d_name) >= (int)sizeof crel) {
        rc = fail(c, "caminho longo demais em %s", rel, 0);
        break;
      }
    } else if (snprintf(crel, sizeof crel, "%s", de->d_name) >= (int)sizeof crel) {
      rc = fail(c, "caminho longo demais em %s", rel, 0);
      break;
    }
    if (path_join(cabs, sizeof cabs, c->root, crel) != 0) { rc = fail(c, "caminho longo demais: %s", crel, 0); break; }
    /* '\\' vira separador nos caminhos AMPR (canonical_asset_path); exFAT nem permite. */
    if (strchr(de->d_name, '\\')) { rc = fail(c, "nome com '\\' não suportado: %s", crel, 0); break; }
    struct stat st;
    if (lstat(cabs, &st) != 0) { rc = fail(c, "lstat falhou em %s", cabs, errno); break; }
    if (S_ISLNK(st.st_mode)) { rc = fail(c, "symlink não suportado: %s", crel, 0); break; }
    if (S_ISDIR(st.st_mode)) {
      rc = add_dir(c, crel);
      if (rc == 0) rc = walk(c, crel);
    } else if (S_ISREG(st.st_mode)) {
      if (!*rel && scan_is_excluded(crel)) continue;
      rc = add_file(c, crel, cabs, &st);
    } else {
      rc = fail(c, "tipo de arquivo não suportado: %s", crel, 0);
    }
  }
  closedir(d);
  return rc;
}

static int cmp_key(const void *a, const void *b) {
  const scan_file_t *x = (const scan_file_t *)a, *y = (const scan_file_t *)b;
  size_t n = x->app0_len < y->app0_len ? x->app0_len : y->app0_len;
  int c = memcmp(x->key, y->key, n);
  if (c) return c;
  return x->app0_len < y->app0_len ? -1 : x->app0_len > y->app0_len;
}

int scan_tree(const char *src_dir, const char *fakelib_sprx, scan_result_t *out, char *err, size_t errlen) {
  memset(out, 0, sizeof *out);
  scan_ctx c = {out, 0, 0, src_dir, err, errlen};
  if (walk(&c, "") != 0) { scan_free(out); return -1; }

  if (fakelib_sprx) {
    struct stat st;
    if (stat(fakelib_sprx, &st) != 0 || !S_ISREG(st.st_mode)) {
      snprintf(err, errlen, "fakelib_sprx inválido: %s", fakelib_sprx);
      scan_free(out);
      return -1;
    }
    const char *rel = "fakelib/libSceAmpr.sprx";
    size_t i;
    for (i = 0; i < out->n_files; i++)
      if (strcasecmp(out->files[i].rel, rel) == 0) break;
    if (i < out->n_files) { /* substitui a cópia da origem */
      scan_file_t *f = &out->files[i];
      out->total_bytes -= f->size;
      free(f->src);
      f->src = strdup(fakelib_sprx);
      f->size = (uint64_t)st.st_size;
      f->mtime = (int64_t)st.st_mtime;
      out->total_bytes += f->size;
      if (!f->src) { snprintf(err, errlen, "sem memória"); scan_free(out); return -1; }
    } else {
      int have_dir = 0;
      for (size_t k = 0; k < out->n_dirs; k++)
        if (strcasecmp(out->dirs[k], "fakelib") == 0) have_dir = 1;
      if ((!have_dir && add_dir(&c, "fakelib") != 0) || add_file(&c, rel, fakelib_sprx, &st) != 0) {
        scan_free(out);
        return -1;
      }
    }
  }

  qsort(out->files, out->n_files, sizeof *out->files, cmp_key);
  for (size_t i = 0; i < out->n_files; i++) {
    if (i && cmp_key(&out->files[i - 1], &out->files[i]) == 0) {
      snprintf(err, errlen, "colisão de caminho sem distinção de maiúsculas: %s <-> %s",
               out->files[i - 1].rel, out->files[i].rel);
      scan_free(out);
      return -1;
    }
    if (out->files[i].indexed) out->n_indexed++;
  }
  if (out->n_indexed == 0) {
    snprintf(err, errlen, "origem sem arquivos");
    scan_free(out);
    return -1;
  }
  return 0;
}

void scan_free(scan_result_t *r) {
  for (size_t i = 0; i < r->n_files; i++) {
    free(r->files[i].rel); free(r->files[i].src); free(r->files[i].app0); free(r->files[i].key);
  }
  for (size_t i = 0; i < r->n_dirs; i++) free(r->dirs[i]);
  free(r->files);
  free(r->dirs);
  memset(r, 0, sizeof *r);
}
