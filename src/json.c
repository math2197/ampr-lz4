#include "json.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *ws(const char *p) {
  while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
  return p;
}

static const char *skip_string(const char *p) { /* p em '"' */
  if (*p != '"') return NULL;
  p++;
  while (*p) {
    if (*p == '\\') { if (!p[1]) return NULL; p += 2; continue; }
    if (*p == '"') return p + 1;
    p++;
  }
  return NULL;
}

const char *json_skip(const char *v) {
  if (!v) return NULL;
  v = ws(v);
  if (*v == '"') return skip_string(v);
  if (*v == '{' || *v == '[') {
    int depth = 0;
    const char *p = v;
    while (*p) {
      if (*p == '"') { p = skip_string(p); if (!p) return NULL; continue; }
      if (*p == '{' || *p == '[') depth++;
      else if (*p == '}' || *p == ']') { if (--depth == 0) return p + 1; }
      p++;
    }
    return NULL;
  }
  while (*v && *v != ',' && *v != '}' && *v != ']' && !isspace((unsigned char)*v)) v++;
  return v;
}

const char *json_get(const char *obj, const char *key) {
  if (!obj) return NULL;
  const char *p = ws(obj);
  if (*p != '{') return NULL;
  p = ws(p + 1);
  size_t kl = strlen(key);
  while (*p && *p != '}') {
    const char *ks = p;
    const char *ke = skip_string(p);
    if (!ke) return NULL;
    p = ws(ke);
    if (*p != ':') return NULL;
    p = ws(p + 1);
    int match = (size_t)(ke - ks) == kl + 2 && strncmp(ks + 1, key, kl) == 0;
    if (match) return p;
    p = json_skip(p);
    if (!p) return NULL;
    p = ws(p);
    if (*p == ',') p = ws(p + 1);
  }
  return NULL;
}

int json_str(const char *v, char *out, size_t n) {
  if (!v || *v != '"' || n == 0) return -1;
  v++;
  size_t o = 0;
  while (*v && *v != '"') {
    unsigned c = (unsigned char)*v++;
    if (c == '\\') {
      char e = *v++;
      switch (e) {
        case 'n': c = '\n'; break;
        case 't': c = '\t'; break;
        case 'r': c = '\r'; break;
        case 'b': c = '\b'; break;
        case 'f': c = '\f'; break;
        case 'u': {
          unsigned u = 0;
          for (int i = 0; i < 4 && isxdigit((unsigned char)*v); i++, v++)
            u = u * 16 + (unsigned)(isdigit((unsigned char)*v) ? *v - '0' : (tolower(*v) - 'a' + 10));
          /* UTF-8 (sem pares substitutos: viram '?') */
          char tmp[4]; int tl;
          if (u < 0x80) { tmp[0] = (char)u; tl = 1; }
          else if (u < 0x800) { tmp[0] = (char)(0xC0 | (u >> 6)); tmp[1] = (char)(0x80 | (u & 63)); tl = 2; }
          else if (u >= 0xD800 && u < 0xE000) { tmp[0] = '?'; tl = 1; }
          else { tmp[0] = (char)(0xE0 | (u >> 12)); tmp[1] = (char)(0x80 | ((u >> 6) & 63)); tmp[2] = (char)(0x80 | (u & 63)); tl = 3; }
          for (int i = 0; i < tl; i++) if (o + 1 < n) out[o++] = tmp[i];
          continue;
        }
        case 0: v--; continue;
        default: c = (unsigned char)e;
      }
    }
    if (o + 1 < n) out[o++] = (char)c;
  }
  out[o] = 0;
  return 0;
}

int json_int(const char *v, long long *out) {
  if (!v) return -1;
  char *end;
  long long x = strtoll(v, &end, 10);
  if (end == v) return -1;
  *out = x;
  return 0;
}

int json_bool(const char *v, int *out) {
  if (!v) return -1;
  if (!strncmp(v, "true", 4)) { *out = 1; return 0; }
  if (!strncmp(v, "false", 5)) { *out = 0; return 0; }
  return -1;
}

int json_get_str(const char *o, const char *k, char *out, size_t n) { return json_str(json_get(o, k), out, n); }
int json_get_int(const char *o, const char *k, long long *out) { return json_int(json_get(o, k), out); }
int json_get_bool(const char *o, const char *k, int *out) { return json_bool(json_get(o, k), out); }

void sb_init(sb_t *b) { b->p = NULL; b->n = b->cap = 0; b->oom = 0; }
void sb_free(sb_t *b) { free(b->p); sb_init(b); }

static int sb_grow(sb_t *b, size_t add) {
  if (b->oom) return -1;
  if (b->n + add + 1 <= b->cap) return 0;
  size_t nc = b->cap ? b->cap : 256;
  while (nc < b->n + add + 1) nc *= 2;
  char *np = realloc(b->p, nc);
  if (!np) { b->oom = 1; return -1; }
  b->p = np; b->cap = nc;
  return 0;
}

void sb_puts(sb_t *b, const char *s) {
  size_t l = strlen(s);
  if (sb_grow(b, l)) return;
  memcpy(b->p + b->n, s, l + 1);
  b->n += l;
}

void sb_printf(sb_t *b, const char *fmt, ...) {
  va_list ap, ap2;
  va_start(ap, fmt);
  va_copy(ap2, ap);
  int l = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  if (l >= 0 && sb_grow(b, (size_t)l) == 0) {
    vsnprintf(b->p + b->n, (size_t)l + 1, fmt, ap2);
    b->n += (size_t)l;
  }
  va_end(ap2);
}

void sb_json_str(sb_t *b, const char *s) {
  sb_puts(b, "\"");
  for (; *s; s++) {
    unsigned char c = (unsigned char)*s;
    if (c == '"') sb_puts(b, "\\\"");
    else if (c == '\\') sb_puts(b, "\\\\");
    else if (c == '\n') sb_puts(b, "\\n");
    else if (c == '\r') sb_puts(b, "\\r");
    else if (c == '\t') sb_puts(b, "\\t");
    else if (c < 0x20) sb_printf(b, "\\u%04x", c);
    else { char t[2] = {(char)c, 0}; sb_puts(b, t); }
  }
  sb_puts(b, "\"");
}
