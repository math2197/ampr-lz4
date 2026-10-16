/* Servidor HTTP/1.1 mínimo: uma thread por conexão, Connection: close.
 * Padrão de socket/accept/thread igual ao ftpsrv do ps5-payload-dev (comprovado em payload PS5):
 * socket(AF_INET, SOCK_STREAM, 0) + SO_REUSEADDR + bind + listen(SOMAXCONN) + accept bloqueante +
 * pthread_create(attr NULL) + pthread_detach. Cada evento da conexão vira uma linha no log. */
#include "httpd.h"
#include "applog.h"
#include "jobs.h"
#include "json.h"
#include "platform.h"
#include "smp.h"
#include "ui_assets.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MAX_REQ 16384
#define MAX_CONN 32
#define HEARTBEAT_S 30
#define SOCK_TIMEOUT_S 5

#ifdef MSG_NOSIGNAL
#define SEND_FLAGS MSG_NOSIGNAL
#else
#define SEND_FLAGS 0
#endif

static atomic_int g_conns;
static atomic_uint g_accepted;
static int g_port;

/* Estado de uma requisição: o que foi respondido (para a linha de log da resposta). */
typedef struct {
  int fd;
  unsigned id;
  int status;      /* 0 = nada enviado */
  size_t clen;     /* Content-Length anunciado */
  size_t sent;     /* bytes efetivamente enviados (cabeçalho + corpo) */
  size_t want;     /* bytes que deveriam ser enviados */
  int send_err;    /* errno do primeiro send que falhou (0 = ok) */
} req_t;

static const char *ip_str(struct in_addr a, char *out, size_t n) {
  if (!inet_ntop(AF_INET, &a, out, (socklen_t)n)) snprintf(out, n, "?");
  return out;
}

/* Envia tudo, tratando envio parcial e EINTR. Retorna bytes enviados; *err = errno da falha. */
static size_t send_all(int fd, const void *buf, size_t n, int *err) {
  const char *p = buf;
  size_t done = 0;
  while (done < n) {
    ssize_t w = send(fd, p + done, n - done, SEND_FLAGS);
    if (w < 0 && errno == EINTR) continue;
    if (w <= 0) { if (err && !*err) *err = w < 0 ? errno : EPIPE; break; }
    done += (size_t)w;
  }
  return done;
}

static void respond(req_t *q, int code, const char *ctype, const void *body, size_t len) {
  const char *msg = code == 200 ? "OK" : code == 400 ? "Bad Request" : code == 403 ? "Forbidden" : code == 404 ? "Not Found" :
                    code == 405 ? "Method Not Allowed" : code == 409 ? "Conflict" : code == 413 ? "Payload Too Large" :
                    code == 503 ? "Service Unavailable" : "Error";
  char h[256];
  int hl = snprintf(h, sizeof h, "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n",
                    code, msg, ctype, len);
  q->status = code;
  q->clen = len;
  q->want = (size_t)hl + len;
  q->sent = send_all(q->fd, h, (size_t)hl, &q->send_err);
  if (len && !q->send_err) q->sent += send_all(q->fd, body, len, &q->send_err);
}

static void respond_json_sb(req_t *q, int code, sb_t *b) {
  respond(q, code, "application/json; charset=utf-8", b->p ? b->p : "{}", b->p ? b->n : 2);
}

static void respond_err(req_t *q, int code, const char *msg) {
  sb_t b; sb_init(&b);
  sb_puts(&b, "{\"ok\":false,\"error\":"); sb_json_str(&b, msg); sb_puts(&b, "}");
  respond_json_sb(q, code, &b);
  sb_free(&b);
}

/* Rede local/loopback. `a` vem de sockaddr_in.sin_addr (ordem de rede); AF_INET puro, sem IPv4-mapped. */
static int peer_allowed(struct in_addr a) {
  uint32_t ip = ntohl(a.s_addr);
  return (ip >> 24) == 127 || (ip >> 24) == 10 || (ip >> 20) == 0xAC1 /* 172.16/12 */ ||
         (ip >> 16) == 0xC0A8 /* 192.168/16 */ || (ip >> 16) == 0xA9FE /* 169.254/16 */;
}

static int valid_id(const char *s) {
  size_t l = strlen(s);
  if (!l || l > 15) return 0;
  for (; *s; s++)
    if (!((*s >= '0' && *s <= '9') || (*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z') || *s == '_' || *s == '-')) return 0;
  return 1;
}

static int has_suffix(const char *s, const char *suf) {
  size_t l = strlen(s), k = strlen(suf);
  return l >= k && !strcasecmp(s + l - k, suf);
}

/* Jogos elegíveis: imagens .exfat/.ffpfsc sob /data ou /mnt/ext0|1. */
static int eligible(const smp_game_t *g, const char **fmt, const char **drive) {
  if (strcmp(g->source_type, "image")) return 0;
  if (has_suffix(g->path, ".exfat")) *fmt = "exfat";
  else if (has_suffix(g->path, ".ffpfsc")) *fmt = "ffpfsc";
  else return 0;
  *drive = plat_drive_of(g->path);
  return *drive != NULL && g->title_id[0];
}

/* Procura TITLEID-AMPR/ampr_assets.index em qualquer drive; retorna drive ou NULL. */
static const char *find_converted(const char *tid, char *dest, size_t n) {
  static const char *drives[] = {"data", "ext0", "ext1"};
  for (int i = 0; i < 3; i++) {
    char logical[300], real[400];
    snprintf(logical, sizeof logical, "%s/homebrew/%s-AMPR", plat_drive_root(drives[i]), tid);
    plat_path(real, sizeof real, logical);
    strncat(real, "/ampr_assets.index", sizeof real - strlen(real) - 1);
    struct stat st;
    if (stat(real, &st) == 0) { snprintf(dest, n, "%s", logical); return drives[i]; }
  }
  return NULL;
}

static void api_games(req_t *q) {
  smp_game_t *g; int n; char err[256];
  if (smp_list_games(&g, &n, err, sizeof err)) { respond_err(q, 503, err); return; }
  sb_t b; sb_init(&b);
  sb_puts(&b, "{\"games\":[");
  int first = 1;
  for (int i = 0; i < n; i++) {
    const char *fmt, *drive;
    if (!eligible(&g[i], &fmt, &drive)) continue;
    char dest[300];
    const char *cd = find_converted(g[i].title_id, dest, sizeof dest);
    if (!cd) snprintf(dest, sizeof dest, "%s/homebrew/%s-AMPR", plat_drive_root(drive), g[i].title_id);
    sb_puts(&b, first ? "{" : ",{"); first = 0;
    sb_puts(&b, "\"id\":"); sb_json_str(&b, g[i].title_id);
    sb_puts(&b, ",\"title\":"); sb_json_str(&b, g[i].title[0] ? g[i].title : g[i].title_id);
    sb_puts(&b, ",\"title_id\":"); sb_json_str(&b, g[i].title_id);
    sb_printf(&b, ",\"format\":\"%s\",\"drive\":\"%s\",\"image_path\":", fmt, drive); sb_json_str(&b, g[i].path);
    sb_printf(&b, ",\"size_bytes\":%llu,\"converted\":%s,\"compatible\":null,\"dest_path\":", (unsigned long long)g[i].size, cd ? "true" : "false");
    sb_json_str(&b, dest);
    {
      const char *eng = jobs_engine_of(g[i].title_id); /* conhecida só depois de um SCAN nesta sessão */
      sb_puts(&b, ",\"engine\":");
      if (eng) sb_json_str(&b, eng);
      else sb_puts(&b, "null");
    }
    sb_puts(&b, "}");
  }
  sb_puts(&b, "]}");
  free(g);
  respond_json_sb(q, 200, &b);
  sb_free(&b);
}

static void api_icon(req_t *q, const char *query) {
  char id[32] = "";
  const char *p = strstr(query, "id=");
  if (p) {
    p += 3;
    size_t i = 0;
    while (*p && *p != '&' && i < sizeof id - 1) id[i++] = *p++;
    id[i] = 0;
  }
  uint8_t *png; size_t len;
  if (!valid_id(id) || smp_icon(id, &png, &len)) { respond(q, 404, "text/plain", "sem ícone", 10); return; }
  respond(q, 200, "image/png", png, len);
  free(png);
}

static void api_info(req_t *q) {
  char ip[64], sv[64] = "";
  plat_local_ip(ip, sizeof ip);
  if (smp_version(sv, sizeof sv)) sv[0] = 0; /* UI mostra "SMP não encontrado" quando vazio */
  sb_t b; sb_init(&b);
  sb_puts(&b, "{\"version\":"); sb_json_str(&b, AMPR_VERSION);
  sb_puts(&b, ",\"smp_version\":"); sb_json_str(&b, sv);
  sb_puts(&b, ",\"ip\":"); sb_json_str(&b, ip);
  sb_printf(&b, ",\"port\":%d}", g_port);
  respond_json_sb(q, 200, &b);
  sb_free(&b);
}

/* keep_loose: até 255 caracteres de [A-Za-z0-9._/,-]. */
static int valid_keep_loose(const char *s) {
  size_t n = strlen(s);
  if (n > 255) return 0;
  for (size_t i = 0; i < n; i++) {
    char c = s[i];
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
          c == '/' || c == ',' || c == '-'))
      return 0;
  }
  return 1;
}

static void api_convert(req_t *q, const char *body) {
  char id[32] = "", drive[16] = "", keep[512] = "";
  long long level = g_cfg.level;
  int del = 0, pack_all = 0;
  if (json_get_str(body, "id", id, sizeof id) || !valid_id(id)) { respond_err(q, 400, "id inválido"); return; }
  if (json_get_str(body, "dest_drive", drive, sizeof drive) || !plat_drive_root(drive)) { respond_err(q, 400, "dest_drive inválido (data|ext0|ext1)"); return; }
  if (json_get(body, "level") && (json_get_int(body, "level", &level) || level < 1 || level > 12)) { respond_err(q, 400, "level deve ser 1..12"); return; }
  if (json_get(body, "keep_loose") && (json_get_str(body, "keep_loose", keep, sizeof keep) || !valid_keep_loose(keep))) {
    respond_err(q, 400, "keep_loose: até 255 caracteres de A-Z a-z 0-9 . _ / , -");
    return;
  }
  if (json_get(body, "delete_source") && json_get_bool(body, "delete_source", &del)) { respond_err(q, 400, "delete_source deve ser true ou false"); return; }
  if (json_get(body, "pack_all") && json_get_bool(body, "pack_all", &pack_all)) { respond_err(q, 400, "pack_all deve ser true ou false"); return; }
  smp_game_t *g; int n; char err[256];
  if (smp_list_games(&g, &n, err, sizeof err)) { respond_err(q, 503, err); return; }
  const char *title = NULL; char tbuf[128] = "";
  for (int i = 0; i < n; i++) {
    const char *fmt, *dr;
    if (!strcmp(g[i].title_id, id) && eligible(&g[i], &fmt, &dr)) { snprintf(tbuf, sizeof tbuf, "%s", g[i].title[0] ? g[i].title : id); title = tbuf; break; }
  }
  free(g);
  if (!title) { respond_err(q, 404, "jogo não encontrado"); return; }
  int j = jobs_enqueue(id, title, drive, (int)level, keep, del, pack_all, err, sizeof err);
  if (j < 0) { respond_err(q, 409, err); return; }
  char out[64];
  snprintf(out, sizeof out, "{\"ok\":true,\"job\":%d}", j);
  respond(q, 200, "application/json", out, strlen(out));
}

static void api_cancel(req_t *q, const char *body) {
  long long job = 0;
  if (json_get(body, "job") && json_get_int(body, "job", &job)) { respond_err(q, 400, "job inválido"); return; }
  if (jobs_cancel((int)job)) { respond_err(q, 404, "nada para cancelar"); return; }
  respond(q, 200, "application/json", "{\"ok\":true}", 11);
}

static void api_status(req_t *q) {
  sb_t b; sb_init(&b);
  jobs_status_json(&b);
  respond_json_sb(q, 200, &b);
  sb_free(&b);
}

#define REQ_DEADLINE_S 5 /* leitura feita no laço do accept: prazo curto para cabeçalho + corpo */

/* Requisição já lida (no laço do accept). */
typedef struct {
  req_t q;
  struct timeval t0;
  char *buf;          /* malloc(MAX_REQ + 1) */
  char method[8];
  char path[256];
  char *query;
  char *body;
} conn_t;

static int deadline_passed(const struct timeval *t0) {
  struct timeval now;
  gettimeofday(&now, NULL);
  return now.tv_sec - t0->tv_sec >= REQ_DEADLINE_S;
}

/* Motivo de saída de recv() em texto (timeout, EOF, erro). */
static void recv_why(ssize_t r, int e, char *out, size_t n) {
  if (r == 0) snprintf(out, n, "EOF");
  else if (e == EAGAIN || e == EWOULDBLOCK) snprintf(out, n, "timeout (%d s)", SOCK_TIMEOUT_S);
  else snprintf(out, n, "erro errno %d (%s)", e, strerror(e));
}

/* Lê cabeçalho (até \r\n\r\n, não até EOF) e corpo. 0 = pronto para despachar; -1 = já respondido/abandonar. */
static int read_request(conn_t *c) {
  req_t *q = &c->q;
  char *req = c->buf;
  size_t n = 0;
  char *hend = NULL;
  char why[96] = "";
  req[0] = 0;
  while (n < MAX_REQ) {
    ssize_t r = recv(q->fd, req + n, MAX_REQ - n, 0);
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) { recv_why(r, errno, why, sizeof why); break; }
    n += (size_t)r;
    req[n] = 0;
    if ((hend = strstr(req, "\r\n\r\n"))) { snprintf(why, sizeof why, "cabeçalho completo"); break; }
    if (deadline_passed(&c->t0)) { snprintf(why, sizeof why, "prazo de %d s", REQ_DEADLINE_S); break; }
  }
  if (!hend && n >= MAX_REQ) snprintf(why, sizeof why, "limite de %d B", MAX_REQ);
  alog("conn #%u: leitura %zu B, fim: %s", q->id, n, why);
  if (!hend) {
    if (n) respond_err(q, n >= MAX_REQ ? 413 : 400, n >= MAX_REQ ? "requisição grande demais" : "requisição inválida");
    return -1; /* n == 0: nada chegou, não há a quem responder */
  }
  *hend = 0;
  char *body = hend + 4;
  size_t hdr = (size_t)(body - req);
  size_t have = n - hdr;
  size_t clen = 0;

  if (sscanf(req, "%7s %255s", c->method, c->path) != 2) { alog("conn #%u: linha de requisição inválida", q->id); respond_err(q, 400, "requisição inválida"); return -1; }
  alog("conn #%u: %s %s", q->id, c->method, c->path);

  for (char *p = strchr(req, '\n'); p; p = strchr(p + 1, '\n'))
    if (!strncasecmp(p + 1, "content-length:", 15)) {
      char *e;
      long long v = strtoll(p + 16, &e, 10);
      if (e == p + 16 || v < 0 || (unsigned long long)v > MAX_REQ) { respond_err(q, v < 0 || e == p + 16 ? 400 : 413, "Content-Length inválido"); return -1; }
      clen = (size_t)v;
    }
  if (clen > MAX_REQ - hdr) { respond_err(q, 413, "corpo grande demais"); return -1; }
  if (have > clen) have = clen;
  while (have < clen) {
    if (deadline_passed(&c->t0)) { alog("conn #%u: corpo: prazo esgotado (%zu/%zu B)", q->id, have, clen); respond_err(q, 400, "corpo incompleto"); return -1; }
    ssize_t r = recv(q->fd, body + have, clen - have, 0);
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) {
      recv_why(r, errno, why, sizeof why);
      alog("conn #%u: corpo incompleto %zu/%zu B, fim: %s", q->id, have, clen, why);
      respond_err(q, 400, "corpo incompleto");
      return -1;
    }
    have += (size_t)r;
  }
  body[clen] = 0;
  c->body = body;
  c->query = strchr(c->path, '?');
  if (c->query) *c->query++ = 0; else c->query = c->path + strlen(c->path);
  return 0;
}

/* Rotas que só usam memória estática: servidas no próprio laço do accept, sem thread. */
static int serve_static(conn_t *c) {
  req_t *q = &c->q;
  if (strcmp(c->method, "GET")) return 0;
  const char *p = c->path;
  if (!strcmp(p, "/ping")) respond(q, 200, "text/plain; charset=utf-8", "pong", 4);
  else if (!strcmp(p, "/")) respond(q, 200, "text/html; charset=utf-8", ui_index_html, ui_index_html_len);
  else if (!strcmp(p, "/app.css")) respond(q, 200, "text/css; charset=utf-8", ui_app_css, ui_app_css_len);
  else if (!strcmp(p, "/app.js")) respond(q, 200, "application/javascript; charset=utf-8", ui_app_js, ui_app_js_len);
  else return 0;
  return 1;
}

/* Rotas de API em /api/ (podem falar com o SMP: até ~7 s). Rodam numa thread para não segurar o accept. */
static void serve_api(conn_t *c) {
  req_t *q = &c->q;
  const char *p = c->path;
  int get = !strcmp(c->method, "GET"), post = !strcmp(c->method, "POST");
  if (!get && !post) respond_err(q, 405, "método não permitido");
  else if (get && !strcmp(p, "/api/games")) api_games(q);
  else if (get && !strcmp(p, "/api/icon")) api_icon(q, c->query);
  else if (get && !strcmp(p, "/api/status")) api_status(q);
  else if (get && !strcmp(p, "/api/info")) api_info(q);
  else if (post && !strcmp(p, "/api/convert")) api_convert(q, c->body);
  else if (post && !strcmp(p, "/api/cancel")) api_cancel(q, c->body);
  else respond_err(q, 404, "não encontrado");
}

static void log_response(const req_t *q) {
  if (!q->status) return;
  if (q->send_err)
    alog("conn #%u: resposta %d Content-Length %zu, enviados %zu/%zu B, erro send errno %d (%s)", q->id, q->status, q->clen,
         q->sent, q->want, q->send_err, strerror(q->send_err));
  else
    alog("conn #%u: resposta %d Content-Length %zu, enviados %zu/%zu B", q->id, q->status, q->clen, q->sent, q->want);
}

/* Registra a resposta, fecha o socket e libera a conexão. */
static void conn_finish(conn_t *c, int counted) {
  log_response(&c->q);
  shutdown(c->q.fd, SHUT_RDWR);
  int rc = close(c->q.fd);
  int e = errno;
  int left = counted ? atomic_fetch_sub(&g_conns, 1) - 1 : atomic_load(&g_conns);
  struct timeval t1;
  gettimeofday(&t1, NULL);
  long ms = (long)(t1.tv_sec - c->t0.tv_sec) * 1000 + (long)(t1.tv_usec - c->t0.tv_usec) / 1000;
  if (rc) alog("conn #%u: close falhou errno %d (threads ativas %d, %ld ms)", c->q.id, e, left, ms);
  else alog("conn #%u: close (threads ativas %d, %ld ms)", c->q.id, left, ms);
  free(c->buf);
  free(c);
}

static void *api_thread(void *arg) {
  conn_t *c = arg;
  alog("conn #%u: thread em execução", c->q.id);
  serve_api(c);
  conn_finish(c, 1);
  return NULL;
}

/* Timeouts e SIGPIPE por socket. struct timeval do FreeBSD/PS5 = {time_t, suseconds_t} (16 B), igual ao host. */
static void conn_sockopts(int fd, unsigned id) {
  struct timeval tv = {SOCK_TIMEOUT_S, 0};
  if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv)) alog("conn #%u: SO_RCVTIMEO falhou errno %d", id, errno);
  if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv)) alog("conn #%u: SO_SNDTIMEO falhou errno %d", id, errno);
#ifdef SO_NOSIGPIPE
  int one = 1;
  if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one)) alog("conn #%u: SO_NOSIGPIPE falhou errno %d", id, errno);
#endif
}

int httpd_listen(int port) {
  g_port = port;
  struct rlimit rl;
  if (getrlimit(RLIMIT_NOFILE, &rl) == 0)
    alog("httpd: RLIMIT_NOFILE cur=%lld max=%lld", (long long)rl.rlim_cur, (long long)rl.rlim_max);
  int s = socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0) { int e = errno; alog("httpd: socket() falhou errno %d", e); errno = e; return -1; }
  alog("httpd: socket criado (fd %d)", s);
  int one = 1;
  if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) != 0) alog("httpd: SO_REUSEADDR falhou errno %d (seguindo)", errno);
  struct sockaddr_in a;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_ANY);
  a.sin_port = htons((uint16_t)port);
  if (bind(s, (struct sockaddr *)&a, sizeof a) != 0) {
    int e = errno;
    alog("httpd: bind 0.0.0.0:%d falhou errno %d (%s)", port, e, strerror(e));
    close(s);
    errno = e;
    return -1;
  }
  alog("httpd: bind 0.0.0.0:%d ok", port);
  if (listen(s, SOMAXCONN) != 0) {
    int e = errno;
    alog("httpd: listen falhou errno %d (%s)", e, strerror(e));
    close(s);
    errno = e;
    return -1;
  }
  alog("httpd: listen ok");
  return s;
}

/* Laço igual ao do ampr-diag.elf (comprovado no PS5): select de 1 s + accept, leitura e rotas estáticas
 * na própria thread principal; heartbeat também daqui (se ele parar, o laço travou ou o processo morreu). */
void httpd_serve(int lfd) {
  unsigned accept_errs = 0;
  int warned_thr = 0;
  time_t last_hb = time(NULL);
  for (;;) {
    time_t now = time(NULL);
    if (now - last_hb >= HEARTBEAT_S) {
      last_hb = now;
      alog("vivo: conexões ativas %d, aceitas total %u", atomic_load(&g_conns), atomic_load(&g_accepted));
    }
    fd_set rs;
    FD_ZERO(&rs);
    FD_SET(lfd, &rs);
    struct timeval stv = {1, 0};
    int sr = select(lfd + 1, &rs, NULL, NULL, &stv);
    if (sr < 0 && errno != EINTR) { alog("httpd: select falhou errno %d", errno); usleep(100000); }
    if (sr <= 0) continue;

    struct sockaddr_in pa;
    memset(&pa, 0, sizeof pa);
    socklen_t pl = sizeof pa;
    int fd = accept(lfd, (struct sockaddr *)&pa, &pl);
    if (fd < 0) {
      int e = errno;
      if (e == EINTR || e == EAGAIN || e == EWOULDBLOCK) continue;
      accept_errs++;
      if (accept_errs <= 5 || accept_errs % 100 == 0) alog("httpd: accept falhou errno %d (%s), falhas %u", e, strerror(e), accept_errs);
      usleep(100000);
      continue;
    }
    unsigned id = atomic_fetch_add(&g_accepted, 1) + 1;
    char ip[INET_ADDRSTRLEN];
    ip_str(pa.sin_addr, ip, sizeof ip);
    int allowed = peer_allowed(pa.sin_addr);
    alog("conn #%u: accept fd %d de %s:%u (family %d, addrlen %u) peer_allowed=%d", id, fd, ip, (unsigned)ntohs(pa.sin_port),
         (int)pa.sin_family, (unsigned)pl, allowed);
    if (id == 1) alog_notify("AMPR-LZ4: conexão de %s", ip);
    conn_sockopts(fd, id);

    conn_t *c = calloc(1, sizeof *c);
    char *buf = c ? malloc(MAX_REQ + 1) : NULL;
    if (!buf) { alog("conn #%u: sem memória, close", id); free(c); close(fd); continue; }
    c->buf = buf;
    c->q.fd = fd;
    c->q.id = id;
    gettimeofday(&c->t0, NULL);

    if (!allowed) { respond(&c->q, 403, "text/plain", "forbidden", 9); conn_finish(c, 0); continue; }
    if (read_request(c) != 0 || serve_static(c)) { conn_finish(c, 0); continue; }
    if (strncmp(c->path, "/api/", 5)) { respond_err(&c->q, 404, "não encontrado"); conn_finish(c, 0); continue; }

    if (atomic_load(&g_conns) >= MAX_CONN) { respond(&c->q, 503, "text/plain", "busy", 4); conn_finish(c, 0); continue; }
    atomic_fetch_add(&g_conns, 1);
    pthread_t t;
    int rc = pthread_create(&t, NULL, api_thread, c); /* como ftpsrv: pilha padrão + detach */
    if (rc == 0) {
      int drc = pthread_detach(t);
      alog("conn #%u: thread criada rc=0 detach=%d", id, drc);
      continue;
    }
    alog("conn #%u: pthread_create falhou rc=%d (%s); atendendo no laço principal", id, rc, strerror(rc));
    if (!warned_thr++) alog_notify("AMPR-LZ4: falha ao criar thread de conexão (erro %d)", rc);
    serve_api(c);
    conn_finish(c, 1);
  }
}
