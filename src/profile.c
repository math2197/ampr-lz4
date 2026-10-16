/* Perfis de empacotamento por jogo/engine. GPL-3.0-or-later.
 *
 * Seleção (Title ID, nome, layout) adaptada de theghostonline/PS5-LZ4-Compression, src/lzc_api.c
 * (bundled_profile_match, detect_game_engine, csv_has, csv_found_in):
 *   SPDX-License-Identifier: GPL-3.0-or-later
 *   Copyright (C) 2026 Husam Osman.
 * Semântica das regras (última que casa vence, glob fnmatch com '*' casando '/', layout "auto" + hot =
 * random) segue tools/ampr_pack.py do ampr_emu (drakmor). Ver THIRD_PARTY.md. */
#include "profile.h"
#include "ampr_pack.h"
#include "json.h"
#include "profiles_asset.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define NPROF_FILES (sizeof k_prof_files / sizeof k_prof_files[0])

size_t prof_bundled_count(void) {
  size_t n = 0;
  for (size_t i = 0; i < NPROF_FILES; i++)
    if (strcmp(k_prof_files[i].name, "index.json") != 0) n++;
  return n;
}

const char *prof_bundled_name(size_t i) {
  for (size_t k = 0; k < NPROF_FILES; k++) {
    if (!strcmp(k_prof_files[k].name, "index.json")) continue;
    if (i-- == 0) return k_prof_files[k].name;
  }
  return NULL;
}

static const char *bundled(const char *name, size_t *len) {
  for (size_t i = 0; i < NPROF_FILES; i++)
    if (!strcmp(k_prof_files[i].name, name)) {
      *len = k_prof_files[i].len;
      return (const char *)k_prof_files[i].data;
    }
  return NULL;
}

/* ------------------------------------------------------------------ glob */

int prof_glob(const char *p, const char *s) {
  const char *star_p = NULL, *star_s = NULL;
  while (*s) {
    if (*p == '*') {
      while (*p == '*') p++;
      star_p = p;
      star_s = s;
      continue;
    }
    if (*p && (*p == '?' || tolower((unsigned char)*p) == tolower((unsigned char)*s))) {
      p++;
      s++;
      continue;
    }
    if (star_p) {
      p = star_p;
      s = ++star_s;
      continue;
    }
    return 0;
  }
  while (*p == '*') p++;
  return *p == 0;
}

static int glob_any(char *const *pats, size_t n, const char *rel) {
  for (size_t i = 0; i < n; i++) {
    const char *pt = pats[i];
    while (*pt == '/') pt++;
    if (prof_glob(pt, rel)) return 1;
  }
  return 0;
}

const prof_rule_t *prof_select(const prof_t *p, const char *rel) {
  const prof_rule_t *sel = &p->def;
  while (*rel == '/') rel++;
  for (size_t i = 0; i < p->n_rules; i++) {
    const prof_rule_t *r = &p->rules[i];
    if (glob_any(r->include, r->n_include, rel) && !glob_any(r->exclude, r->n_exclude, rel)) sel = r;
  }
  return sel;
}

/* ------------------------------------------------------------------ TOML mínimo */

typedef enum { TV_NONE, TV_STR, TV_NUM, TV_BOOL, TV_ARR } tv_kind;

typedef struct {
  tv_kind kind;
  char str[256];
  double num;
  int is_int;
  long long ival;
  int b;
  char **arr;
  size_t n_arr;
} tval_t;

typedef struct {
  const char *p, *end;
  int line;
  char *err;
  size_t errlen;
} tparse_t;

static int terr(tparse_t *t, const char *msg) {
  snprintf(t->err, t->errlen, "TOML linha %d: %s", t->line, msg);
  return -1;
}

/* pula espaços, comentários e (se nl) quebras de linha */
static void tskip(tparse_t *t, int nl) {
  while (t->p < t->end) {
    char c = *t->p;
    if (c == ' ' || c == '\t' || c == '\r') t->p++;
    else if (c == '#') { while (t->p < t->end && *t->p != '\n') t->p++; }
    else if (c == '\n' && nl) { t->line++; t->p++; }
    else break;
  }
}

static int tstring(tparse_t *t, char *out, size_t n) {
  if (t->p >= t->end || (*t->p != '"' && *t->p != '\'')) return terr(t, "string esperada");
  char q = *t->p++;
  size_t k = 0;
  while (t->p < t->end && *t->p != q) {
    char c = *t->p++;
    if (c == '\n') return terr(t, "string sem fim");
    if (c == '\\' && q == '"') {
      if (t->p >= t->end) return terr(t, "escape incompleto");
      char e = *t->p++;
      c = e == 'n' ? '\n' : e == 't' ? '\t' : e == '\\' ? '\\' : e == '"' ? '"' : 0;
      if (!c) return terr(t, "escape não suportado");
    }
    if (k + 1 >= n) return terr(t, "string longa demais");
    out[k++] = c;
  }
  if (t->p >= t->end) return terr(t, "string sem fim");
  t->p++;
  out[k] = 0;
  return 0;
}

static void tval_free(tval_t *v) {
  for (size_t i = 0; i < v->n_arr; i++) free(v->arr[i]);
  free(v->arr);
  v->arr = NULL;
  v->n_arr = 0;
}

static int tvalue(tparse_t *t, tval_t *v) {
  memset(v, 0, sizeof *v);
  if (t->p >= t->end) return terr(t, "valor esperado");
  char c = *t->p;
  if (c == '"' || c == '\'') {
    v->kind = TV_STR;
    return tstring(t, v->str, sizeof v->str);
  }
  if (c == '[') {
    v->kind = TV_ARR;
    t->p++;
    for (;;) {
      tskip(t, 1);
      if (t->p >= t->end) return terr(t, "array sem fim");
      if (*t->p == ']') { t->p++; return 0; }
      char s[512];
      if (tstring(t, s, sizeof s) != 0) return -1;
      char **na = (char **)realloc(v->arr, (v->n_arr + 1) * sizeof *na);
      if (!na) return terr(t, "sem memória");
      v->arr = na;
      if (!(v->arr[v->n_arr] = strdup(s))) return terr(t, "sem memória");
      v->n_arr++;
      tskip(t, 1);
      if (t->p < t->end && *t->p == ',') t->p++;
      else {
        tskip(t, 1);
        if (t->p >= t->end || *t->p != ']') return terr(t, "',' ou ']' esperado no array");
      }
    }
  }
  if (t->end - t->p >= 4 && !strncmp(t->p, "true", 4)) { v->kind = TV_BOOL; v->b = 1; t->p += 4; return 0; }
  if (t->end - t->p >= 5 && !strncmp(t->p, "false", 5)) { v->kind = TV_BOOL; v->b = 0; t->p += 5; return 0; }
  char num[64];
  size_t k = 0;
  while (t->p < t->end && k + 1 < sizeof num &&
         (isdigit((unsigned char)*t->p) || *t->p == '.' || *t->p == '-' || *t->p == '+' || *t->p == '_' ||
          *t->p == 'e' || *t->p == 'E')) {
    if (*t->p != '_') num[k++] = *t->p;
    t->p++;
  }
  num[k] = 0;
  if (!k) return terr(t, "valor não suportado");
  char *e;
  v->kind = TV_NUM;
  if (!strpbrk(num, ".eE")) {
    errno = 0;
    v->ival = strtoll(num, &e, 10);
    if (*e || errno) return terr(t, "inteiro inválido");
    v->is_int = 1;
    v->num = (double)v->ival;
  } else {
    v->num = strtod(num, &e);
    if (*e) return terr(t, "número inválido");
  }
  return 0;
}

/* tamanho: inteiro ou "64KiB"/"1MiB"/"16B" */
static int tsize(const tval_t *v, uint64_t *out) {
  if (v->kind == TV_NUM && v->is_int && v->ival >= 0) { *out = (uint64_t)v->ival; return 0; }
  if (v->kind != TV_STR) return -1;
  char *e;
  errno = 0;
  unsigned long long n = strtoull(v->str, &e, 10);
  if (e == v->str || errno) return -1;
  while (*e == ' ') e++;
  uint64_t mul = 1;
  if (!*e || !strcasecmp(e, "B")) mul = 1;
  else if (!strcasecmp(e, "KiB") || !strcasecmp(e, "K") || !strcasecmp(e, "KB")) mul = 1024;
  else if (!strcasecmp(e, "MiB") || !strcasecmp(e, "M") || !strcasecmp(e, "MB")) mul = 1024 * 1024;
  else if (!strcasecmp(e, "GiB") || !strcasecmp(e, "G") || !strcasecmp(e, "GB")) mul = 1024ull * 1024 * 1024;
  else return -1;
  *out = (uint64_t)n * mul;
  return 0;
}

static int tblock_shift(tparse_t *t, const tval_t *v, uint8_t *shift) {
  uint64_t n;
  if (tsize(v, &n) != 0) return terr(t, "block_size inválido");
  for (uint8_t s = 14; s <= 20; s++)
    if (n == (1ull << s)) { *shift = s; return 0; }
  return terr(t, "block_size precisa ser potência de 2 entre 16KiB e 1MiB");
}

static int tratio(tparse_t *t, const tval_t *v, uint16_t *bp) {
  if (v->kind != TV_NUM || v->num < 0 || v->num > 1) return terr(t, "razão inválida (0..1)");
  *bp = (uint16_t)(v->num * 10000.0 + 0.5);
  return 0;
}

static int tbytes32(tparse_t *t, const tval_t *v, uint32_t *out) {
  uint64_t n;
  if (tsize(v, &n) != 0 || n > 0xFFFFFFFFull) return terr(t, "tamanho inválido");
  *out = (uint32_t)n;
  return 0;
}

static void rule_free(prof_rule_t *r) {
  for (size_t i = 0; i < r->n_include; i++) free(r->include[i]);
  for (size_t i = 0; i < r->n_exclude; i++) free(r->exclude[i]);
  free(r->include);
  free(r->exclude);
  r->include = r->exclude = NULL;
  r->n_include = r->n_exclude = 0;
}

void prof_free(prof_t *p) {
  for (size_t i = 0; i < p->n_rules; i++) rule_free(&p->rules[i]);
  free(p->rules);
  rule_free(&p->def);
  memset(p, 0, sizeof *p);
}

/* estado auxiliar por regra (resolvido no fim: layout/hot/streaming -> flags) */
typedef struct {
  int layout;   /* 0 auto, 1 mixed, 2 random, 3 streaming */
  int hot, streaming, has_shift, has_sav[4];
} rule_aux_t;

static int apply_savings_key(tparse_t *t, const char *k, const tval_t *v, prof_savings_t *s, int *seen) {
  if (!strcmp(k, "min_savings_bytes")) { seen[0] = 1; return tbytes32(t, v, &s->min_bytes); }
  if (!strcmp(k, "min_savings_ratio")) { seen[1] = 1; return tratio(t, v, &s->min_bp); }
  if (!strcmp(k, "io_neutral_min_savings_bytes")) { seen[2] = 1; return tbytes32(t, v, &s->io_min_bytes); }
  if (!strcmp(k, "io_neutral_min_savings_ratio")) { seen[3] = 1; return tratio(t, v, &s->io_min_bp); }
  return 1; /* não é chave de limiar */
}

static int action_of(tparse_t *t, const tval_t *v, uint8_t *act) {
  if (v->kind != TV_STR) return terr(t, "action precisa ser string");
  if (!strcmp(v->str, "compress")) *act = PROF_ACT_PACK;
  else if (!strcmp(v->str, "loose") || !strcmp(v->str, "store")) *act = PROF_ACT_LOOSE;
  else return terr(t, "action desconhecida");
  return 0;
}

int prof_parse_toml(const char *text, size_t len, prof_t *out, char *err, size_t errlen) {
  memset(out, 0, sizeof *out);
  out->def.action = PROF_ACT_LOOSE; /* ampr_pack.py: default_action = "loose" */
  out->def.block_shift = 16;
  out->def.sav = PROF_SAVINGS_DEFAULT;
  tparse_t t = {text, text + len, 1, err, errlen};
  enum { SEC_ROOT, SEC_PACK, SEC_RULE, SEC_OTHER } sec = SEC_ROOT;
  rule_aux_t *aux = NULL;
  int pack_seen[4] = {0};
  int rc = -1;
  tval_t v;
  memset(&v, 0, sizeof v);

  for (;;) {
    tskip(&t, 1);
    if (t.p >= t.end) break;
    if (*t.p == '[') {
      int arr = t.end - t.p >= 2 && t.p[1] == '[';
      t.p += arr ? 2 : 1;
      const char *b = t.p;
      while (t.p < t.end && *t.p != ']' && *t.p != '\n') t.p++;
      size_t nl = (size_t)(t.p - b);
      if (t.p >= t.end || *t.p != ']' || (arr && (t.end - t.p < 2 || t.p[1] != ']'))) { terr(&t, "cabeçalho de tabela inválido"); goto out; }
      t.p += arr ? 2 : 1;
      while (nl && b[nl - 1] == ' ') nl--;
      while (nl && *b == ' ') { b++; nl--; }
      if (arr && nl == 4 && !strncmp(b, "rule", 4)) {
        prof_rule_t *nr = (prof_rule_t *)realloc(out->rules, (out->n_rules + 1) * sizeof *nr);
        if (!nr) { terr(&t, "sem memória"); goto out; }
        out->rules = nr;
        rule_aux_t *na = (rule_aux_t *)realloc(aux, (out->n_rules + 1) * sizeof *na);
        if (!na) { terr(&t, "sem memória"); goto out; }
        aux = na;
        memset(&out->rules[out->n_rules], 0, sizeof *nr);
        memset(&aux[out->n_rules], 0, sizeof *na);
        out->rules[out->n_rules].action = PROF_ACT_PACK; /* Rule.action padrão do ampr_pack.py: "compress" */
        out->n_rules++;
        sec = SEC_RULE;
      } else if (!arr && nl == 4 && !strncmp(b, "pack", 4)) {
        sec = SEC_PACK;
      } else {
        sec = SEC_OTHER; /* [runtime], [groups.*], [[...]] desconhecidos: ignorados */
      }
      tskip(&t, 0);
      if (t.p < t.end && *t.p != '\n') { terr(&t, "lixo após cabeçalho"); goto out; }
      continue;
    }
    /* chave */
    char key[64];
    size_t k = 0;
    while (t.p < t.end && (isalnum((unsigned char)*t.p) || *t.p == '_' || *t.p == '-' || *t.p == '.')) {
      if (k + 1 < sizeof key) key[k++] = *t.p;
      t.p++;
    }
    key[k] = 0;
    if (!k) { terr(&t, "chave esperada"); goto out; }
    tskip(&t, 0);
    if (t.p >= t.end || *t.p != '=') { terr(&t, "'=' esperado"); goto out; }
    t.p++;
    tskip(&t, 0);
    if (tvalue(&t, &v) != 0) goto out;
    tskip(&t, 0);
    if (t.p < t.end && *t.p != '\n') { terr(&t, "lixo após valor"); goto out; }

    if (sec == SEC_PACK) {
      int r;
      if (!strcmp(key, "default_action")) {
        if (action_of(&t, &v, &out->def.action) != 0) goto out;
      } else if (!strcmp(key, "default_block_size")) {
        if (tblock_shift(&t, &v, &out->def.block_shift) != 0) goto out;
      } else if ((r = apply_savings_key(&t, key, &v, &out->def.sav, pack_seen)) < 0) {
        goto out;
      } else if (r > 0) {
        /* demais chaves de [pack] (nomes, workers, nível, dedup...) não se aplicam a este empacotador */
      }
    } else if (sec == SEC_RULE) {
      prof_rule_t *ru = &out->rules[out->n_rules - 1];
      rule_aux_t *ax = &aux[out->n_rules - 1];
      int r;
      if (!strcmp(key, "action")) {
        if (action_of(&t, &v, &ru->action) != 0) goto out;
      } else if (!strcmp(key, "include") || !strcmp(key, "exclude")) {
        if (v.kind != TV_ARR) { terr(&t, "include/exclude precisa ser array de strings"); goto out; }
        if (key[0] == 'i') { ru->include = v.arr; ru->n_include = v.n_arr; }
        else { ru->exclude = v.arr; ru->n_exclude = v.n_arr; }
        v.arr = NULL;
        v.n_arr = 0;
      } else if (!strcmp(key, "block_size")) {
        if (tblock_shift(&t, &v, &ru->block_shift) != 0) goto out;
        ax->has_shift = 1;
      } else if (!strcmp(key, "layout")) {
        if (v.kind != TV_STR) { terr(&t, "layout precisa ser string"); goto out; }
        if (!strcmp(v.str, "auto")) ax->layout = 0;
        else if (!strcmp(v.str, "mixed")) ax->layout = 1;
        else if (!strcmp(v.str, "random")) ax->layout = 2;
        else if (!strcmp(v.str, "streaming")) ax->layout = 3;
        else { terr(&t, "layout desconhecido"); goto out; }
      } else if (!strcmp(key, "hot") || !strcmp(key, "streaming")) {
        if (v.kind != TV_BOOL) { terr(&t, "hot/streaming precisa ser bool"); goto out; }
        if (key[0] == 'h') ax->hot = v.b;
        else ax->streaming = v.b;
      } else if ((r = apply_savings_key(&t, key, &v, &ru->sav, ax->has_sav)) < 0) {
        goto out;
      } else if (r > 0 && strcmp(key, "group") != 0 && strcmp(key, "force_pack") != 0) {
        out->unknown_keys++; /* ex.: "ayout" em astrobot.toml */
      }
    }
    tval_free(&v);
  }

  /* resolve cada regra: herda bloco/limiares de [pack]; layout auto + hot = random (Rule.resolved_layout) */
  for (size_t i = 0; i < out->n_rules; i++) {
    prof_rule_t *ru = &out->rules[i];
    rule_aux_t *ax = &aux[i];
    if (!ax->has_shift) ru->block_shift = out->def.block_shift;
    prof_savings_t s = ru->sav;
    ru->sav = out->def.sav;
    if (ax->has_sav[0]) ru->sav.min_bytes = s.min_bytes;
    if (ax->has_sav[1]) ru->sav.min_bp = s.min_bp;
    if (ax->has_sav[2]) ru->sav.io_min_bytes = s.io_min_bytes;
    if (ax->has_sav[3]) ru->sav.io_min_bp = s.io_min_bp;
    int layout = ax->layout;
    if (layout == 0) layout = ax->streaming ? 3 : ax->hot ? 2 : 1;
    ru->file_flags = 0;
    /* Este empacotador só grava o layout "mixed" (chunks sempre page-safe): "streaming" vira mixed, que o
     * runtime aceita para qualquer arquivo; "random" ganha RANDOM_ACCESS (exige page-safe, que já temos). */
    if (layout == 2) ru->file_flags |= AMPR_FILE_FLAG_RANDOM_ACCESS;
    if (ax->hot) ru->file_flags |= AMPR_FILE_FLAG_HOT;
    if (!ru->n_include) { terr(&t, "[[rule]] sem include"); goto out; }
  }
  rc = 0;
out:
  tval_free(&v);
  free(aux);
  if (rc != 0) prof_free(out);
  return rc;
}

/* ------------------------------------------------------------------ param.json e seleção */

static char *read_small(const char *path, size_t max) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) return NULL;
  struct stat st;
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || (uint64_t)st.st_size > max) { close(fd); return NULL; }
  char *b = (char *)malloc((size_t)st.st_size + 1);
  size_t got = 0;
  while (b && got < (size_t)st.st_size) {
    ssize_t n = read(fd, b + got, (size_t)st.st_size - got);
    if (n <= 0) { free(b); b = NULL; break; }
    got += (size_t)n;
  }
  close(fd);
  if (b) b[got] = 0;
  return b;
}

static const char *skip_ws(const char *p) {
  while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
  return p;
}

int prof_read_param(const char *src_dir, char *tid, size_t tn, char *name, size_t nn) {
  char path[2048];
  if (tn) tid[0] = 0;
  if (nn) name[0] = 0;
  if (snprintf(path, sizeof path, "%s/sce_sys/param.json", src_dir) >= (int)sizeof path) return -1;
  char *b = read_small(path, 4u << 20);
  if (!b) return -1;
  const char *o = skip_ws(b);
  if (*o == '{') {
    json_get_str(o, "titleId", tid, tn);
    const char *lp = json_get(o, "localizedParameters");
    if (lp && *lp == '{') {
      char lang[32] = "";
      json_get_str(lp, "defaultLanguage", lang, sizeof lang);
      const char *lo = lang[0] ? json_get(lp, lang) : NULL;
      if (!lo || *lo != '{') lo = json_get(lp, "en-US");
      if (lo && *lo == '{') json_get_str(lo, "titleName", name, nn);
    }
  }
  free(b);
  return 0;
}

/* config_normalize da referência: só letras e dígitos, em minúsculas. */
static void normalize(const char *in, char *out, size_t n) {
  size_t k = 0;
  for (; in && *in && k + 1 < n; in++)
    if (isalnum((unsigned char)*in)) out[k++] = (char)tolower((unsigned char)*in);
  if (n) out[k] = 0;
}

/* item de array de strings JSON == value (sem distinção de maiúsculas) ou, se substr, item (>= 4 chars)
 * contido em value. */
static int json_arr_has(const char *arr, const char *value, int substr) {
  if (!arr || *arr != '[' || !value || !*value) return 0;
  const char *p = skip_ws(arr + 1);
  while (*p && *p != ']') {
    char s[128];
    if (json_str(p, s, sizeof s) == 0) {
      if (substr ? (strlen(s) >= 4 && strstr(value, s)) : !strcasecmp(s, value)) return 1;
    }
    p = json_skip(p);
    if (!p) return 0;
    p = skip_ws(p);
    if (*p == ',') p = skip_ws(p + 1);
  }
  return 0;
}

int prof_load_bundled(const char *file, prof_t *out, char *err, size_t errlen) {
  size_t len;
  const char *data = bundled(file, &len);
  if (!data) { snprintf(err, errlen, "perfil embutido ausente: %s", file); return -1; }
  char e2[200];
  if (prof_parse_toml(data, len, out, e2, sizeof e2) != 0) { snprintf(err, errlen, "%s: %s", file, e2); return -1; }
  return 0;
}

int prof_choose_bundled(const char *title_id, const char *name, const char *layout_engine, prof_t *out, char *err,
                        size_t errlen) {
  size_t len;
  const char *raw = bundled("index.json", &len);
  memset(out, 0, sizeof *out);
  if (!raw) return 0;
  char *idx = (char *)malloc(len + 1);
  if (!idx) { snprintf(err, errlen, "sem memória"); return -1; }
  memcpy(idx, raw, len);
  idx[len] = 0;
  char norm[256];
  normalize(name, norm, sizeof norm);
  const char *hit = NULL, *how = NULL;
  /* passada 0: Title ID ou nome (feito/testado para o jogo); passada 1: layout da engine */
  for (int pass = 0; pass < 2 && !hit; pass++) {
    const char *p = skip_ws(idx);
    if (*p != '[') break;
    p = skip_ws(p + 1);
    while (*p == '{' && !hit) {
      if (pass == 0) {
        if (title_id && *title_id && json_arr_has(json_get(p, "titleIds"), title_id, 0)) { hit = p; how = "title-id"; }
        else if (norm[0] && json_arr_has(json_get(p, "names"), norm, 1)) { hit = p; how = "name"; }
      } else if (layout_engine && *layout_engine) {
        char eng[32];
        if (json_get_str(p, "engine", eng, sizeof eng) == 0 && !strcmp(eng, layout_engine)) { hit = p; how = "layout"; }
      }
      if (hit) break;
      p = json_skip(p);
      if (!p) break;
      p = skip_ws(p);
      if (*p == ',') p = skip_ws(p + 1);
    }
  }
  int rc = 0;
  if (hit) {
    char file[64], id[32], label[64];
    if (json_get_str(hit, "file", file, sizeof file) != 0 || json_get_str(hit, "id", id, sizeof id) != 0) {
      snprintf(err, errlen, "profiles/index.json: entrada sem id/file");
      rc = -1;
    } else if (prof_load_bundled(file, out, err, errlen) != 0) {
      rc = -1;
    } else {
      if (json_get_str(hit, "label", label, sizeof label) != 0) snprintf(label, sizeof label, "%s", id);
      snprintf(out->id, sizeof out->id, "%s", id);
      snprintf(out->label, sizeof out->label, "%s", label);
      snprintf(out->match, sizeof out->match, "%s", how);
      rc = 1;
    }
  }
  free(idx);
  return rc;
}
