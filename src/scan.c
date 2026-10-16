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

/* ---------- perfis por engine e regras extras ----------
 * O ampr_emu padrão só atende leituras feitas pela API AMPR (APR). Arquivos que a engine lê com
 * open/read/pread/AIO comuns ou via mmap precisam ficar soltos (docs/ASSET_PACKS.md do ampr_emu,
 * "The default production integration intercepts APR read-only opens"). */

/* 1 se algum diretório de rel (não o nome do arquivo) é `name` (sem distinção de maiúsculas). */
static int has_dir_ci(const char *rel, const char *name) {
  size_t nl = strlen(name);
  const char *p = rel;
  for (;;) {
    const char *e = strchr(p, '/');
    if (!e) return 0;
    if ((size_t)(e - p) == nl && strncasecmp(p, name, nl) == 0) return 1;
    p = e + 1;
  }
}

static const char *base_of(const char *rel) {
  const char *b = strrchr(rel, '/');
  return b ? b + 1 : rel;
}

int scan_detect_engine(const scan_file_t *files, size_t n) {
  int unreal = 0, north = 0, unity = 0;
  for (size_t i = 0; i < n; i++) {
    const char *rel = files[i].rel, *base = base_of(rel);
    if (ends_with_ci(base, ".utoc") || ends_with_ci(base, ".ucas") ||
        (ends_with_ci(base, ".pak") && has_dir_ci(rel, "Paks")))
      unreal = 1;
    else if (ends_with_ci(base, ".rmdtoc"))
      north = 1;
    else if (strcasecmp(base, "globalgamemanagers") == 0 || strcasecmp(base, "globalgamemanagers.assets") == 0 ||
             ((ends_with_ci(base, ".assets") || ends_with_ci(base, ".resS")) && has_dir_ci(rel, "Media")))
      unity = 1;
  }
  return unreal ? CONV_ENGINE_UNREAL : north ? CONV_ENGINE_NORTHLIGHT : unity ? CONV_ENGINE_UNITY : CONV_ENGINE_UNKNOWN;
}

int scan_keep_loose_match(const char *rel, const char *list) {
  if (!list) return 0;
  const char *base = base_of(rel);
  const char *p = list;
  while (*p) {
    const char *e = strchr(p, ',');
    size_t len = e ? (size_t)(e - p) : strlen(p);
    char tok[256];
    while (len && (*p == ' ' || *p == '\t')) { p++; len--; }
    while (len && (p[len - 1] == ' ' || p[len - 1] == '\t')) len--;
    if (len && len < sizeof tok) {
      memcpy(tok, p, len);
      tok[len] = 0;
      const char *t = tok;
      if (strncasecmp(t, "/app0/", 6) == 0) t += 6;
      while (*t == '/') t++;
      size_t tl = strlen(t);
      if (!tl) {
        /* "/" ou "/app0/" sozinho: ignora (não manda o jogo inteiro para fora dos pacotes) */
      } else if (strchr(t, '/')) {
        /* caminho relativo: o próprio arquivo ou um diretório-prefixo */
        if (strncasecmp(rel, t, tl) == 0 && (rel[tl] == 0 || rel[tl] == '/' || t[tl - 1] == '/')) return 1;
      } else if (t[0] == '.') {
        if (ends_with_ci(base, t)) return 1;
      } else if (strcasecmp(base, t) == 0) {
        return 1;
      }
    }
    if (!e) break;
    p = e + 1;
  }
  return 0;
}

void scan_apply_rules(scan_result_t *r, const char *keep_loose) {
  r->engine = scan_detect_engine(r->files, r->n_files);
  for (size_t i = 0; i < r->n_files; i++) {
    scan_file_t *f = &r->files[i];
    if (f->action != SCAN_ACT_PACK) continue;
    const char *base = base_of(f->rel);
    if (r->engine == CONV_ENGINE_UNREAL &&
        (ends_with_ci(base, ".ucas") || ends_with_ci(base, ".utoc") || has_dir_ci(f->rel, "Paks"))) {
      /* IoStore/pak: o FPlatformFile do Unreal lê .ucas/.utoc/.pak/.sig com I/O comum. */
      f->action = SCAN_ACT_LOOSE;
      f->reason = "engine-unreal";
    } else if (r->engine == CONV_ENGINE_NORTHLIGHT && ends_with_ci(base, ".rmdtoc")) {
      /* ampr_emu issue #7: Control quebra com .rmdtoc empacotado. */
      f->action = SCAN_ACT_LOOSE;
      f->reason = "engine-northlight";
    } else if (scan_keep_loose_match(f->rel, keep_loose)) {
      f->action = SCAN_ACT_LOOSE;
      f->reason = "keep-loose";
    }
  }
}

void scan_publish_estimate(const scan_result_t *r, conv_progress_t *p) {
  atomic_store(&p->engine, r->engine);
  atomic_store(&p->packed_bytes, r->packed_bytes);
  atomic_store(&p->loose_bytes, r->loose_bytes);
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
  return scan_tree_ex(src_dir, fakelib_sprx, NULL, out, err, errlen);
}

int scan_tree_ex(const char *src_dir, const char *fakelib_sprx, const char *keep_loose, scan_result_t *out, char *err,
                 size_t errlen) {
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
  scan_apply_rules(out, keep_loose);
  for (size_t i = 0; i < out->n_files; i++)
    if (out->files[i].action == SCAN_ACT_PACK && out->files[i].indexed) out->packed_bytes += out->files[i].size;
  out->loose_bytes = out->total_bytes - out->packed_bytes;
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
