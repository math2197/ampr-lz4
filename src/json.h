/* Mini helpers de JSON: leitura de chaves de nível superior e construtor de saída. */
#pragma once
#include <stddef.h>
#include <stdint.h>

/* Leitura. `obj` aponta para '{'. Retorna ponteiro para o início do valor da chave (nível 1) ou NULL. */
const char *json_get(const char *obj, const char *key);
/* Ponteiro logo após o valor que começa em v (string, objeto, array ou literal); NULL se malformado. */
const char *json_skip(const char *v);
/* Decodifica string JSON em out (UTF-8, truncando). 0 = ok, -1 = não é string. */
int json_str(const char *v, char *out, size_t n);
int json_int(const char *v, long long *out);   /* 0 = ok */
int json_bool(const char *v, int *out);        /* 0 = ok */
/* Atalhos: lêem a chave de obj; retornam 0 se existir e tiver o tipo certo. */
int json_get_str(const char *obj, const char *key, char *out, size_t n);
int json_get_int(const char *obj, const char *key, long long *out);
int json_get_bool(const char *obj, const char *key, int *out);

/* Construtor de string dinâmica (saída JSON). */
typedef struct { char *p; size_t n, cap; int oom; } sb_t;
void sb_init(sb_t *b);
void sb_free(sb_t *b);
void sb_puts(sb_t *b, const char *s);
void sb_printf(sb_t *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void sb_json_str(sb_t *b, const char *s);   /* escreve "..." com escape */
