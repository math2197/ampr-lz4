/* Cliente do ShadowMountPlus 1.7+.
 *
 * CONFIRMADO (branch 1.7, https://github.com/drakmor/ShadowMountPlus/blob/1.7/docs/api.md):
 *  - Endpoint padrão 127.0.0.1:10101; chaves api_port / api_bind_address em
 *    /data/shadowmount/config.ini (api.md linhas 6-9). Toda operação JSON é POST com um objeto
 *    JSON, Content-Type: application/json, corpo <= 4096 B, Connection: close (l.33-37).
 *  - POST /api/v1/version -> {"status":0,"shadowmount_version":"1.7",...} (l.109-118).
 *  - POST /api/v1/games {"include_size":bool} -> {"status","count","size_included","games":[Game]}
 *    com Game.{path,runtime_path,source_type(folder|image|pkg),image_type,title_id,title_name,
 *    size_bytes,mounted,fakelib_mode,...} (docs/openapi.yaml, schema Game, l.1590-1710).
 *  - POST /api/v1/games/info {"title_id"} -> Game plano + status/size_status (openapi l.1728).
 *  - POST /api/v1/games/mount {"title_id","mode":"ro"} -> 501/ENOTSUP quando o modo não é
 *    suportado, 409/EBUSY se jogo ativo (api.md l.236-246). A resposta de mount NÃO traz
 *    runtime_path (openapi MountResponse l.1872: status,title_id,mounted,mode); por isso o
 *    runtime_path é lido depois via /games/info (Game.runtime_path).
 *    O mount é por TITLE_ID (não por caminho de imagem) e exige o jogo já gerenciado pelo SMP.
 *  - POST /api/v1/games/unmount {"title_id"} (api.md l.66).
 *  - POST /api/v1/games/fakelib {"title_id","mode":"full|emulators|disabled|default"} (l.63, 224).
 *    Leitura do modo atual: campo Game.fakelib_mode (resolvido) de /games ou /games/info.
 *  - GET /api/v1/games/icon?title_id=X[&size=thumb] -> PNG (l.64).
 *  - Erros: {"status":<errno>,"error":"..."} com HTTP 4xx/5xx (l.103-107).
 *  - libSceAmpr.sprx: o SMP guarda emuladores em `emulators_path` (padrão
 *    /data/shadowmount/emus; README 1.7 l.82-84, include/sm_paths.h DEFAULT_EMULATORS_PATH) e
 *    baixa de ampr_update_url (github.com/drakmor/ampr_emu). SUPOSIÇÃO: o arquivo fica em
 *    <emulators_path>/libSceAmpr.sprx (nome confirmado em README l.191; local exato não
 *    confirmado no código de update).
 *
 * SUPOSIÇÕES isoladas nas constantes abaixo. */
#include "smp.h"
#include "applog.h"
#include "json.h"
#include "platform.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#define SMP_DEFAULT_HOST "127.0.0.1"
#define SMP_DEFAULT_PORT 10101
#define SMP_CONFIG_PATH "/data/shadowmount/config.ini"
#define SMP_DEFAULT_EMUS_DIR "/data/shadowmount/emus"
#define SMP_AMPR_FILE "libSceAmpr.sprx"
#define SMP_MOUNT_MODE "ro"
#define SMP_LIST_INCLUDE_SIZE 1      /* tamanho físico; para pastas percorre o diretório */
#define SMP_ICON_QUERY "&size=thumb"
#define SMP_TIMEOUT_S 15
#define SMP_CONNECT_MS 2000          /* connect no loopback: 2 s bastam */
#define SMP_UI_IDLE_S 5              /* chamadas feitas por requisições da UI: 5 s sem resposta = erro */
#ifdef MSG_NOSIGNAL
#define SMP_SEND_FLAGS MSG_NOSIGNAL
#else
#define SMP_SEND_FLAGS 0
#endif
#define SMP_MOUNT_TIMEOUT_S 180
#define SMP_MAX_RESP (16u << 20)

static char g_host[64] = SMP_DEFAULT_HOST;
static int g_port = SMP_DEFAULT_PORT;
static char g_emus[256] = SMP_DEFAULT_EMUS_DIR;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static char *trim(char *s) {
  while (*s == ' ' || *s == '\t') s++;
  char *e = s + strlen(s);
  while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
  return s;
}

static void init_once(void) {
  char path[512], line[512];
  plat_path(path, sizeof path, SMP_CONFIG_PATH);
  FILE *f = fopen(path, "r");
  if (f) {
    while (fgets(line, sizeof line, f)) {
      char *s = trim(line);
      if (*s == '#' || *s == ';' || !*s) continue;
      char *eq = strchr(s, '=');
      if (!eq) continue;
      *eq = 0;
      char *k = trim(s), *v = trim(eq + 1);
      if (!strcmp(k, "api_port") && atoi(v) > 0 && atoi(v) < 65536) g_port = atoi(v);
      else if (!strcmp(k, "api_bind_address") && *v) {
        /* 0.0.0.0 = escuta em tudo; de dentro do PS5 conectamos em loopback */
        snprintf(g_host, sizeof g_host, "%s", strcmp(v, "0.0.0.0") ? v : "127.0.0.1");
      } else if (!strcmp(k, "emulators_path") && *v) snprintf(g_emus, sizeof g_emus, "%s", v);
    }
    fclose(f);
  }
  const char *e = getenv("AMPR_SMP_PORT"); /* override para testes no host */
  if (e && atoi(e) > 0) g_port = atoi(e);
}

/* Conecta com timeout (connect não bloqueante + poll). No FreeBSD/PS5, connect() bloqueante IGNORA
 * SO_SNDTIMEO (só o Linux usa), então sem isso um SMP travado prende a thread para sempre. */
static int connect_timeout(int s, const struct sockaddr_in *a, int ms, char *why, size_t wn) {
  int fl = fcntl(s, F_GETFL, 0);
  if (fl < 0 || fcntl(s, F_SETFL, fl | O_NONBLOCK) < 0) { snprintf(why, wn, "fcntl errno %d", errno); return -1; }
  int rc = connect(s, (const struct sockaddr *)a, sizeof *a);
  if (rc != 0 && errno != EINPROGRESS) { snprintf(why, wn, "connect errno %d (%s)", errno, strerror(errno)); return -1; }
  if (rc != 0) {
    struct pollfd p = {s, POLLOUT, 0};
    int pr;
    do pr = poll(&p, 1, ms); while (pr < 0 && errno == EINTR);
    if (pr == 0) { snprintf(why, wn, "connect: sem resposta em %d ms", ms); return -1; }
    if (pr < 0) { snprintf(why, wn, "poll errno %d", errno); return -1; }
    int e = 0;
    socklen_t el = sizeof e;
    if (getsockopt(s, SOL_SOCKET, SO_ERROR, &e, &el) != 0) e = errno;
    if (e) { snprintf(why, wn, "connect errno %d (%s)", e, strerror(e)); return -1; }
  }
  if (fcntl(s, F_SETFL, fl) < 0) { snprintf(why, wn, "fcntl errno %d", errno); return -1; }
  return 0;
}

static int64_t mono_ms(void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/* Requisição HTTP com prazos: connect <= SMP_CONNECT_MS, no máximo idle_s sem receber nada e
 * total_s no total. body pode ser NULL (GET). resp é malloc'ado (corpo, NUL-terminado). */
static int http_raw(const char *method, const char *path, const char *body, int idle_s, int total_s, int *status,
                    char **resp, size_t *rlen, char *why, size_t wn) {
  pthread_once(&g_once, init_once);
  *resp = NULL; *rlen = 0; *status = 0; why[0] = 0;
  int64_t t0 = mono_ms();
  struct sockaddr_in a;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_port = htons((uint16_t)g_port);
  if (inet_pton(AF_INET, g_host, &a.sin_addr) != 1) { snprintf(why, wn, "host inválido %s", g_host); return -1; }
  int s = socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0) { snprintf(why, wn, "socket errno %d", errno); return -1; }
#ifdef SO_NOSIGPIPE
  int one = 1;
  setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
  struct timeval tv = {idle_s, 0};
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
  if (connect_timeout(s, &a, SMP_CONNECT_MS, why, wn)) { close(s); return -1; }
  char head[1024];
  size_t bl = body ? strlen(body) : 0;
  int hl = snprintf(head, sizeof head,
                    "%s %s HTTP/1.1\r\nHost: %s:%d\r\nConnection: close\r\nContent-Type: application/json\r\n"
                    "Content-Length: %zu\r\n\r\n", method, path, g_host, g_port, bl);
  if (send(s, head, (size_t)hl, SMP_SEND_FLAGS) != hl || (bl && send(s, body, bl, SMP_SEND_FLAGS) != (ssize_t)bl)) {
    snprintf(why, wn, "send errno %d", errno);
    close(s);
    return -1;
  }
  size_t cap = 4096, n = 0;
  char *buf = malloc(cap);
  if (!buf) { close(s); snprintf(why, wn, "sem memória"); return -1; }
  for (;;) {
    if (n + 1024 + 1 > cap) {
      if (cap >= SMP_MAX_RESP) break;
      cap *= 2;
      char *nb = realloc(buf, cap);
      if (!nb) { free(buf); close(s); snprintf(why, wn, "sem memória"); return -1; }
      buf = nb;
    }
    int64_t left = (int64_t)total_s * 1000 - (mono_ms() - t0);
    if (left <= 0) { free(buf); close(s); snprintf(why, wn, "prazo total de %d s esgotado (%zu B recebidos)", total_s, n); return -1; }
    int wait = left < (int64_t)idle_s * 1000 ? (int)left : idle_s * 1000;
    struct pollfd p = {s, POLLIN, 0};
    int pr;
    do pr = poll(&p, 1, wait); while (pr < 0 && errno == EINTR);
    if (pr == 0) {
      free(buf); close(s);
      snprintf(why, wn, "sem resposta em %d s (%zu B recebidos)", wait / 1000, n);
      return -1;
    }
    ssize_t r = recv(s, buf + n, cap - n - 1, 0);
    if (r < 0 && errno == EINTR) continue;
    if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      free(buf); close(s);
      snprintf(why, wn, "sem resposta em %d s (%zu B recebidos)", idle_s, n);
      return -1;
    }
    if (r <= 0) break;
    n += (size_t)r;
  }
  close(s);
  buf[n] = 0;
  char *he = strstr(buf, "\r\n\r\n");
  if (n < 12 || strncmp(buf, "HTTP/", 5) || !he) { free(buf); snprintf(why, wn, "resposta HTTP inválida (%zu B)", n); return -1; }
  *status = atoi(buf + 9);
  char *b = he + 4;
  size_t blen = n - (size_t)(b - buf);
  *he = 0;
  int chunked = 0;
  for (char *p = buf; *p; p++)
    if ((p == buf || p[-1] == '\n') && !strncasecmp(p, "transfer-encoding:", 18) && strcasestr(p, "chunked")) chunked = 1;
  if (chunked) {
    char *w = b, *rp = b, *end = b + blen;
    while (rp < end) {
      char *nl = memchr(rp, '\n', (size_t)(end - rp));
      if (!nl) break;
      size_t cl = strtoul(rp, NULL, 16);
      rp = nl + 1;
      if (cl == 0) break;
      if (cl > (size_t)(end - rp)) cl = (size_t)(end - rp);
      memmove(w, rp, cl); w += cl; rp += cl;
      if (rp < end && *rp == '\r') rp++;
      if (rp < end && *rp == '\n') rp++;
    }
    blen = (size_t)(w - b);
    b[blen] = 0;
  }
  memmove(buf, b, blen + 1);
  *resp = buf;
  *rlen = blen;
  return 0;
}

/* http_raw + uma linha de log por chamada (resultado e duração). */
static int http(const char *method, const char *path, const char *body, int idle_s, int total_s, int *status,
                char **resp, size_t *rlen, char *why, size_t wn) {
  int64_t t0 = mono_ms();
  int rc = http_raw(method, path, body, idle_s, total_s, status, resp, rlen, why, wn);
  long ms = (long)(mono_ms() - t0);
  if (rc) alog("smp: %s %s falhou: %s (%ld ms)", method, path, why, ms);
  else alog("smp: %s %s -> HTTP %d, %zu B (%ld ms)", method, path, *status, *rlen, ms);
  return rc;
}

static void fill_err(char *err, size_t en, const char *resp, int status, const char *what) {
  if (!err || !en) return;
  char m[200] = "";
  if (resp && *resp == '{') json_get_str(resp, "error", m, sizeof m);
  if (status == 0) snprintf(err, en, "SMP inacessível (%s: %s)", what, resp ? resp : "?");
  else if (*m) snprintf(err, en, "SMP: %s (HTTP %d)", m, status);
  else snprintf(err, en, "SMP: HTTP %d em %s", status, what);
}

/* POST JSON; sucesso = HTTP 2xx e status==0. */
static int post(const char *path, const char *body, int idle_s, int total_s, char **resp, int *http_status, char *err, size_t en) {
  size_t rl;
  int st = 0;
  char why[128];
  if (http("POST", path, body, idle_s, total_s, &st, resp, &rl, why, sizeof why) != 0) {
    fill_err(err, en, why, 0, path);
    if (http_status) *http_status = 0;
    return -1;
  }
  if (http_status) *http_status = st;
  long long code = 0;
  if (st < 200 || st > 299 || (json_get_int(*resp, "status", &code) == 0 && code != 0)) {
    fill_err(err, en, *resp, st, path);
    return -1;
  }
  return 0;
}

int smp_version(char *out, size_t n) {
  char *r = NULL;
  int hs;
  char err[64];
  if (post("/api/v1/version", "{}", SMP_UI_IDLE_S, SMP_UI_IDLE_S, &r, &hs, err, sizeof err)) { free(r); return -1; }
  int rc = json_get_str(r, "shadowmount_version", out, n);
  free(r);
  return rc;
}

static void parse_game(const char *o, smp_game_t *g) {
  long long v;
  int b;
  memset(g, 0, sizeof *g);
  json_get_str(o, "title_id", g->title_id, sizeof g->title_id);
  json_get_str(o, "title_name", g->title, sizeof g->title);
  json_get_str(o, "path", g->path, sizeof g->path);
  json_get_str(o, "runtime_path", g->runtime_path, sizeof g->runtime_path);
  json_get_str(o, "source_type", g->source_type, sizeof g->source_type);
  json_get_str(o, "image_type", g->image_type, sizeof g->image_type);
  json_get_str(o, "fakelib_mode", g->fakelib_mode, sizeof g->fakelib_mode);
  if (json_get_int(o, "size_bytes", &v) == 0 && v > 0) g->size = (uint64_t)v;
  else if (json_get_int(o, "app_db_size_bytes", &v) == 0 && v > 0) g->size = (uint64_t)v;
  if (json_get_bool(o, "mounted", &b) == 0) g->mounted = b;
}

int smp_list_games(smp_game_t **out, int *count, char *err, size_t en) {
  char *r = NULL;
  int hs;
  *out = NULL; *count = 0;
  if (post("/api/v1/games", SMP_LIST_INCLUDE_SIZE ? "{\"include_size\":true}" : "{}", SMP_UI_IDLE_S, SMP_TIMEOUT_S * 4, &r, &hs, err, en)) {
    free(r);
    return -1;
  }
  const char *arr = json_get(r, "games");
  if (!arr || *arr != '[') { free(r); if (err) snprintf(err, en, "SMP: resposta sem 'games'"); return -1; }
  int cap = 64, n = 0;
  smp_game_t *v = malloc((size_t)cap * sizeof *v);
  const char *p = arr + 1;
  while (v) {
    while (*p == ' ' || *p == ',' || *p == '\n' || *p == '\r' || *p == '\t') p++;
    if (*p != '{') break;
    const char *e = json_skip(p);
    if (!e) break;
    if (n == cap) {
      cap *= 2;
      smp_game_t *nv = realloc(v, (size_t)cap * sizeof *v);
      if (!nv) { free(v); v = NULL; break; }
      v = nv;
    }
    parse_game(p, &v[n++]);
    p = e;
  }
  free(r);
  if (!v) { if (err) snprintf(err, en, "sem memória"); return -1; }
  *out = v; *count = n;
  return 0;
}

static int valid_tid(const char *t) {
  if (!*t || strlen(t) > 15) return 0;
  for (; *t; t++)
    if (!((*t >= '0' && *t <= '9') || (*t >= 'A' && *t <= 'Z') || (*t >= 'a' && *t <= 'z') || *t == '_' || *t == '-')) return 0;
  return 1;
}

int smp_game_info(const char *tid, smp_game_t *g, char *err, size_t en) {
  if (!valid_tid(tid)) { if (err) snprintf(err, en, "title_id inválido"); return -1; }
  char body[96], *r = NULL;
  snprintf(body, sizeof body, "{\"title_id\":\"%s\"}", tid);
  int hs;
  if (post("/api/v1/games/info", body, SMP_TIMEOUT_S * 4, SMP_TIMEOUT_S * 4, &r, &hs, err, en)) { free(r); return -1; }
  parse_game(r, g);
  free(r);
  return 0;
}

int smp_icon(const char *tid, uint8_t **png, size_t *len) {
  if (!valid_tid(tid)) return -1;
  char path[160], *r;
  snprintf(path, sizeof path, "/api/v1/games/icon?title_id=%s" SMP_ICON_QUERY, tid);
  int st;
  char why[128];
  if (http("GET", path, NULL, SMP_UI_IDLE_S, SMP_TIMEOUT_S, &st, &r, len, why, sizeof why) != 0) return -1;
  if (st != 200 || *len < 8 || memcmp(r, "\x89PNG", 4)) { free(r); return -1; }
  *png = (uint8_t *)r;
  return 0;
}

int smp_mount(const char *tid, char *runtime, size_t n, char *err, size_t en) {
  if (!valid_tid(tid)) { if (err) snprintf(err, en, "title_id inválido"); return -1; }
  char body[128], *r = NULL;
  int hs = 0;
  snprintf(body, sizeof body, "{\"title_id\":\"%s\",\"mode\":\"%s\"}", tid, SMP_MOUNT_MODE);
  int rc = post("/api/v1/games/mount", body, SMP_MOUNT_TIMEOUT_S, SMP_MOUNT_TIMEOUT_S, &r, &hs, err, en);
  free(r);
  if (rc && hs == 501) { /* modo não suportado: repete sem modo */
    snprintf(body, sizeof body, "{\"title_id\":\"%s\"}", tid);
    r = NULL;
    rc = post("/api/v1/games/mount", body, SMP_MOUNT_TIMEOUT_S, SMP_MOUNT_TIMEOUT_S, &r, &hs, err, en);
    free(r);
  }
  if (rc) return -1;
  smp_game_t g;
  memset(&g, 0, sizeof g);
  if (smp_game_info(tid, &g, err, en) || !g.runtime_path[0]) {
    if (err && !g.runtime_path[0]) snprintf(err, en, "SMP montou mas não informou runtime_path");
    smp_unmount(tid, NULL, 0); /* não deixar o jogo montado quando falhamos aqui */
    return -1;
  }
  snprintf(runtime, n, "%s", g.runtime_path);
  return 0;
}

int smp_unmount(const char *tid, char *err, size_t en) {
  if (!valid_tid(tid)) return -1;
  char body[96], *r = NULL;
  snprintf(body, sizeof body, "{\"title_id\":\"%s\"}", tid);
  int hs;
  int rc = post("/api/v1/games/unmount", body, SMP_MOUNT_TIMEOUT_S, SMP_MOUNT_TIMEOUT_S, &r, &hs, err, en);
  free(r);
  return rc;
}

int smp_get_fakelib(const char *tid, char *mode, size_t n) {
  smp_game_t g;
  if (smp_game_info(tid, &g, NULL, 0)) return -1;
  snprintf(mode, n, "%s", g.fakelib_mode);
  return 0;
}

int smp_set_fakelib(const char *tid, const char *mode, char *err, size_t en) {
  if (!valid_tid(tid)) return -1;
  char body[128], *r = NULL;
  snprintf(body, sizeof body, "{\"title_id\":\"%s\",\"mode\":\"%s\"}", tid, mode);
  int hs;
  int rc = post("/api/v1/games/fakelib", body, SMP_TIMEOUT_S, SMP_TIMEOUT_S, &r, &hs, err, en);
  free(r);
  return rc;
}

void smp_ampr_sprx_path(char *out, size_t n) {
  pthread_once(&g_once, init_once);
  char logical[400], real[512];
  snprintf(logical, sizeof logical, "%s/%s", g_emus, SMP_AMPR_FILE);
  plat_path(real, sizeof real, logical);
  struct stat st;
  if (stat(real, &st) == 0 && S_ISREG(st.st_mode)) snprintf(out, n, "%s", real);
  else out[0] = 0;
}

void smp_emus_dir(char *out, size_t n) {
  pthread_once(&g_once, init_once);
  snprintf(out, n, "%s", g_emus);
}
