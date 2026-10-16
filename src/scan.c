/* Varredura da origem e classificação loose/pack. GPL-3.0. */
#include "scan.h"
#include "util.h"
#include "ampr_pack.h"
#include "profile.h"

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

/* ---------- segurança: sempre soltos, acima de qualquer regra (keep_loose, perfil, engine) ----------
 * Extensões: as nossas + a lista "safety exclusions" de theghostonline/PS5-LZ4-Compression
 * (src/ampr_pack_profile.c, k_safety_exclusions; GPL-3.0-or-later, Copyright (C) 2026 Husam Osman). */
static const char *k_loose_ext[] = {
    /* executáveis/módulos */
    ".elf", ".self", ".prx", ".sprx",
    /* metadados/dados pequenos e configuração lidos por caminhos não-APR */
    ".bin", ".dat", ".json", ".xml", ".ini", ".cfg", ".txt", ".bak", ".utoc",
    /* mídia (streaming por decodificadores do sistema) */
    ".mp4", ".ogg", ".mp3", ".bk2", ".usm", ".webm", ".wav", ".at9", ".ivf", ".flac", ".aac", ".opus", ".m4a",
    ".avi", ".mkv", ".mov", ".wmv", ".flv", ".m4v",
    /* áudio Unity/FMOD e bancos de som: lidos com open/read, fora da API AMPR */
    ".resource", ".bank", ".fsb", ".wem", ".bnk",
    /* imagens/texturas soltas (lidas pelo sistema ou por decodificadores próprios) */
    ".png", ".jpg", ".jpeg", ".webp", ".gif", ".bmp", ".ico", ".tga", ".tif", ".tiff", ".exr", ".hdr", ".psd",
    ".dds", ".ktx", ".ktx2", ".astc", ".basis", ".gnf", ".gnfp", ".jxm", ".vtf",
    /* já comprimidos / imagens de disco */
    ".zip", ".7z", ".rar", ".pak", ".gz", ".xz", ".bz2", ".zst", ".lz4", ".pfs", ".img", NULL};
static const char *k_exec_ext[] = {".elf", ".self", ".prx", ".sprx", NULL};
/* pastas de sistema/runtime: soltas em QUALQUER profundidade */
static const char *k_loose_dirs[] = {"sce_sys", "sce_module", "fakelib", "system", "save", "mods", "trophy2",
                                     "uds", "decrypted", NULL};
/* nomes de saída AMPR (em qualquer pasta) */
static const char *k_loose_names[] = {"eboot.bin", "ampr_emu.index", "ampr_assets.index", "ampr_assets.index.crc",
                                      "ampr_assets.index.runtime", NULL};

/* Arquivos que o PS5 lê fora do processo do jogo (onde o APR-EMU não mostra os pacotes) ou que o boot
 * precisa no disco: marcadores do PlayGo, arquivos de boot do Ghost of Yōtei (PPSA26344). Lista de
 * ampr_pack_keep_on_disk() em theghostonline/PS5-LZ4-Compression src/ampr_pack.c
 * (GPL-3.0-or-later, Copyright (C) 2026 Husam Osman). O packagedefinition.txt do Glacier (HITMAN 3,
 * 007 First Light) já fica solto pela extensão .txt; arquivos vazios também (scan_classify). */
static const char *k_keep_on_disk[] = {
    "cache_ps5/pgc_*_dummy_file",
    "cache_ps5/playgo.pgm",
    "cache_ps5/load_menu_catalog.log",
    "cache_ps5/all_shaders.texmeshman",
    "cache_ps5/game.sprig.packman",
    "cache_ps5/game.sprig.texmeshman",
    "cache_ps5/pulse.sprig.texmeshman",
    "sounds/scream/_generated/buss_config.bus",
    "sounds/scream/_generated/distance_model.dml",
    "sounds/scream/_generated/group_mix.gmx",
    "*/packagedefinition.txt",
    "packagedefinition.txt",
    NULL};

/* ---------- regras padrão (abaixo de keep_loose, perfil e engine) ---------- */
/* Pastas de áudio (qualquer profundidade): blocos de 16 KiB e HOT|RANDOM_ACCESS, como as regras de áudio do
 * perfil automático da referência (audio_globs + soundbank.*). */
static const char *k_audio_dirs[] = {"audio", "sound", "sounds", "sfx", "bgm", "music", "voice", "voices",
                                     "snd", "vo", "dialog", "dialogue", NULL};
/* Contêineres de streaming do perfil automático da referência (streaming_containers): soltos. */
static const char *k_stream_containers[] = {"*/movie", "*/movie_*", "*/movies", "*/movies_*", "*/soundbank",
                                            "*/soundbank_*", "*/screenreaderwem", "*/screenreaderwem.*",
                                            "*/wem", "*/wem_*", "*/wem.*", NULL};

static int ends_with_ci(const char *s, const char *suf) {
  size_t a = strlen(s), b = strlen(suf);
  return a >= b && strcasecmp(s + a - b, suf) == 0;
}

static int in_list_ci(const char *s, size_t len, const char **list) {
  for (int i = 0; list[i]; i++)
    if (strlen(list[i]) == len && strncasecmp(s, list[i], len) == 0) return 1;
  return 0;
}

static int glob_list(const char **list, const char *rel) {
  for (int i = 0; list[i]; i++)
    if (prof_glob(list[i], rel)) return 1;
  return 0;
}

int scan_is_excluded(const char *rel) {
  static const char *ex[] = {"ampr_commands.bin", "apr_emu.log", "ampr_emu.index",
                             "ampr_emu.index.tmp", "ampr_emu.index.bak", NULL};
  for (int i = 0; ex[i]; i++)
    if (strcasecmp(rel, ex[i]) == 0) return 1;
  return 0;
}

int scan_keep_on_disk(const char *rel, uint64_t size) {
  return size == 0 || glob_list(k_keep_on_disk, rel);
}

/* 1 se algum diretório de rel (não o nome do arquivo) está na lista (sem distinção de maiúsculas). */
static int any_dir_in(const char *rel, const char **list) {
  const char *p = rel;
  for (;;) {
    const char *e = strchr(p, '/');
    if (!e) return 0;
    if (in_list_ci(p, (size_t)(e - p), list)) return 1;
    p = e + 1;
  }
}

static void set_savings(scan_file_t *f, prof_savings_t s) {
  f->sav_min_bytes = s.min_bytes;
  f->sav_min_bp = s.min_bp;
  f->sav_io_min_bytes = s.io_min_bytes;
  f->sav_io_min_bp = s.io_min_bp;
}

/* Regras padrão (camada mais baixa). */
static void default_classify(const char *rel, const char *base, scan_file_t *f) {
  f->block_shift = 16;
  f->file_flags = 0;
  set_savings(f, PROF_SAVINGS_DEFAULT);
  if (!strchr(rel, '/')) { f->action = SCAN_ACT_LOOSE; f->reason = "root"; return; }
  if (glob_list(k_stream_containers, rel)) { f->action = SCAN_ACT_LOOSE; f->reason = "stream-container"; return; }
  f->action = SCAN_ACT_PACK;
  f->reason = "pack";
  if (any_dir_in(rel, k_audio_dirs) || strncasecmp(base, "soundbank.", 10) == 0) {
    /* leituras pequenas e aleatórias de bancos de som: blocos de 16 KiB, HOT|RANDOM_ACCESS (nunca STREAMING:
     * o runtime rejeita STREAMING com RANDOM_ACCESS) e limiares de economia menores, como na referência. */
    f->block_shift = 14;
    f->file_flags = AMPR_FILE_FLAG_HOT | AMPR_FILE_FLAG_RANDOM_ACCESS;
    set_savings(f, PROF_SAVINGS_AUDIO);
    f->reason = "pack-audio";
  }
}

void scan_classify(const char *rel, uint64_t size, scan_file_t *f) {
  const char *base = strrchr(rel, '/');
  base = base ? base + 1 : rel;
  f->exec = 0;
  for (int i = 0; k_exec_ext[i]; i++)
    if (ends_with_ci(base, k_exec_ext[i])) f->exec = 1;
  if (strcasecmp(base, "eboot.bin") == 0) f->exec = 1;
  default_classify(rel, base, f);

  /* segurança e sempre-em-disco: travados */
  const char *why = NULL;
  const char *slash = strchr(rel, '/');
  if (any_dir_in(rel, k_loose_dirs) || (slash && (size_t)(slash - rel) == 8 && !strncasecmp(rel, "_DUPLEX_", 8)))
    why = "system-dir";
  else if (in_list_ci(base, strlen(base), k_loose_names)) why = strcasecmp(base, "eboot.bin") ? "ampr-output" : "eboot";
  else if (size == 0) why = "empty";
  else if (glob_list(k_keep_on_disk, rel)) why = "keep-on-disk";
  else {
    for (int i = 0; k_loose_ext[i]; i++)
      if (ends_with_ci(base, k_loose_ext[i])) { why = "extension"; break; }
  }
  if (why) {
    f->action = SCAN_ACT_LOOSE;
    f->locked = 1;
    f->file_flags = 0;
    f->reason = why;
  } else {
    f->locked = 0;
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
    if (ends_with_ci(base, ".utoc") || ends_with_ci(base, ".ucas") || ends_with_ci(base, ".uproject") ||
        ends_with_ci(base, ".upluginmanifest") || (ends_with_ci(base, ".pak") && has_dir_ci(rel, "Paks")))
      unreal = 1;
    else if (ends_with_ci(base, ".rmdtoc"))
      north = 1;
    else if (strcasecmp(base, "globalgamemanagers") == 0 || strcasecmp(base, "globalgamemanagers.assets") == 0 ||
             ((ends_with_ci(base, ".assets") || ends_with_ci(base, ".resS")) && has_dir_ci(rel, "Media")))
      unity = 1;
  }
  return unreal ? CONV_ENGINE_UNREAL : north ? CONV_ENGINE_NORTHLIGHT : unity ? CONV_ENGINE_UNITY : CONV_ENGINE_UNKNOWN;
}

static int has_file_ci(const scan_result_t *r, const char *rel) {
  for (size_t i = 0; i < r->n_files; i++)
    if (strcasecmp(r->files[i].rel, rel) == 0) return 1;
  return 0;
}

static int has_dir_entry_ci(const scan_result_t *r, const char *rel) {
  for (size_t i = 0; i < r->n_dirs; i++)
    if (strcasecmp(r->dirs[i], rel) == 0) return 1;
  return 0;
}

/* detect_game_engine() de theghostonline/PS5-LZ4-Compression src/lzc_api.c (GPL-3.0-or-later,
 * Copyright (C) 2026 Husam Osman), sobre a lista da varredura e sem distinção de maiúsculas. */
const char *scan_layout_engine(const scan_result_t *r) {
  static const char *bases[] = {"", "Media/"};
  static const char *markers[] = {"Data/globalgamemanagers", "Data/data.unity3d",
                                  "Data/Managed/Metadata/global-metadata.dat"};
  char a[256];
  if (has_file_ci(r, "toc") && has_file_ci(r, "dag") && has_dir_entry_ci(r, "d")) return "insomniac";
  for (size_t i = 0; i < 2; i++) {
    snprintf(a, sizeof a, "%sStreamingAssets", bases[i]);
    if (!has_dir_entry_ci(r, a)) continue;
    for (size_t k = 0; k < 2; k++)
      for (size_t m = 0; m < 3; m++) {
        snprintf(a, sizeof a, "%s%s", bases[k], markers[m]);
        if (has_file_ci(r, a)) return "unity";
      }
  }
  return "";
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

/* Unreal: o .ucas (IoStore, dados dos assets) e .uasset/.ubulk vão para os pacotes, como no
 * theghostonline/PS5-LZ4-Compression. Ficam soltos: os índices/containers lidos com I/O comum pelo
 * FPlatformFile (.utoc, .pak, .sig — .utoc e .pak já estão na lista de segurança), o projeto e os manifestos
 * de plugins (.uproject/.uplugin/.upluginmanifest), a configuração (pastas Config/ em qualquer nível), a
 * localização (.locres/.locmeta) e os dados do ICU (Internationalization/, .icu). O SIGSEGV antigo do
 * Stellar Blade ("Failed to find /Engine/...") veio de um build que empacotava também Config/, .ini e
 * .uproject; o container global (global.ucas/global.utoc, em qualquer pasta) fica solto por precaução. */
static const char *unreal_loose(const char *rel, const char *base) {
  static const char *ext[] = {".utoc", ".pak", ".sig", ".uproject", ".upluginmanifest", ".uplugin",
                              ".locres", ".locmeta", ".icu", NULL};
  for (int i = 0; ext[i]; i++)
    if (ends_with_ci(base, ext[i])) return "engine-unreal";
  if (strcasecmp(base, "global.ucas") == 0 || strcasecmp(base, "global.utoc") == 0) return "engine-unreal";
  if (has_dir_ci(rel, "Config") || has_dir_ci(rel, "Internationalization")) return "engine-unreal";
  return NULL;
}

void scan_apply_rules(scan_result_t *r, const char *keep_loose, const prof_t *prof) {
  r->engine = scan_detect_engine(r->files, r->n_files);
  for (size_t i = 0; i < r->n_files; i++) {
    scan_file_t *f = &r->files[i];
    if (f->locked) continue; /* 1) segurança e sempre-em-disco */
    const char *base = base_of(f->rel);
    if (scan_keep_loose_match(f->rel, keep_loose)) { /* 2) keep_loose do usuário */
      f->action = SCAN_ACT_LOOSE;
      f->file_flags = 0;
      f->reason = "keep-loose";
      continue;
    }
    if (prof && prof->id[0]) { /* 3) perfil do jogo: decide tudo o que sobrou (inclui default_action) */
      const prof_rule_t *ru = prof_select(prof, f->rel);
      f->reason = ru == &prof->def ? "profile-default" : "profile";
      if (ru->action == PROF_ACT_PACK) {
        f->action = SCAN_ACT_PACK;
        f->block_shift = ru->block_shift;
        f->file_flags = ru->file_flags;
        set_savings(f, ru->sav);
      } else {
        f->action = SCAN_ACT_LOOSE;
        f->file_flags = 0;
      }
      continue;
    }
    if (f->action != SCAN_ACT_PACK) continue; /* 5) padrão já decidiu solto */
    const char *why = NULL; /* 4) perfil da engine */
    if (r->engine == CONV_ENGINE_UNREAL) why = unreal_loose(f->rel, base);
    else if (r->engine == CONV_ENGINE_NORTHLIGHT && ends_with_ci(base, ".rmdtoc"))
      why = "engine-northlight"; /* ampr_emu issue #7: Control quebra com .rmdtoc empacotado. */
    if (why) {
      f->action = SCAN_ACT_LOOSE;
      f->file_flags = 0;
      f->reason = why;
    }
  }
}

void scan_publish_estimate(const scan_result_t *r, conv_progress_t *p) {
  snprintf(p->profile, sizeof p->profile, "%s", r->profile);
  atomic_store(&p->engine, r->engine);
  atomic_store(&p->auto_loose_bytes, 0);
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
  /* perfil embutido: Title ID / nome (sce_sys/param.json), depois layout */
  char name[256] = "", perr[256] = "";
  prof_read_param(src_dir, out->title_id, sizeof out->title_id, name, sizeof name);
  prof_t prof;
  int pr = prof_choose_bundled(out->title_id, name, scan_layout_engine(out), &prof, perr, sizeof perr);
  if (pr < 0) {
    /* perfil embutido inválido: segue sem perfil (as regras de segurança continuam valendo) */
    snprintf(out->profile_err, sizeof out->profile_err, "%s", perr);
    memset(&prof, 0, sizeof prof);
  } else if (pr > 0) {
    snprintf(out->profile, sizeof out->profile, "%s", prof.id);
    snprintf(out->profile_match, sizeof out->profile_match, "%s", prof.match);
  }
  scan_apply_rules(out, keep_loose, pr > 0 ? &prof : NULL);
  if (pr > 0) prof_free(&prof);
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
