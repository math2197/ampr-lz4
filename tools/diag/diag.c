/* ampr-diag.elf: payload mínimo para isolar problemas de ambiente (loader, notificação, /data, rede).
 * Linkado exatamente como o samples/hello_world do SDK (sem bibliotecas extras). GPL-3.0.
 *  1) notificação "diag 1"
 *  2) grava /data/ampr-lz4/diag.txt
 *  3) socket + bind em 0.0.0.0:8095 (mesmo padrão do ampr-lz4.elf/ftpsrv) e notifica o resultado
 *  4) por 120 s responde "ok" a qualquer GET; GET /thread responde de uma thread (pthread_create+detach,
 *     como o ampr-lz4.elf), para separar "rede" de "threads". Cada conexão é logada em stdout e em
 *     /data/ampr-lz4/diag.txt; a primeira gera notificação. */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define DIAG_PORT 8095
#define DIAG_SECONDS 120
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

typedef struct notify_request {
  char useless1[45];
  char message[3075];
} notify_request_t;

int sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int);

static void notify(const char *fmt, ...) {
  notify_request_t req;
  va_list ap;
  memset(&req, 0, sizeof req);
  va_start(ap, fmt);
  vsnprintf(req.message, sizeof req.message, fmt, ap);
  va_end(ap);
  sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
  printf("[ampr-diag] %s\n", req.message);
  fflush(stdout);
}

static FILE *g_log;

static void logf_(const char *fmt, ...) {
  char m[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(m, sizeof m, fmt, ap);
  va_end(ap);
  printf("[ampr-diag] %s\n", m);
  fflush(stdout);
  if (g_log) { fprintf(g_log, "%s\n", m); fflush(g_log); }
}

static const char RESP[] = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 3\r\nConnection: close\r\n\r\nok\n";

static int send_all(int fd, const char *p, size_t n) {
  size_t done = 0;
  while (done < n) {
    ssize_t w = send(fd, p + done, n - done, MSG_NOSIGNAL);
    if (w < 0 && errno == EINTR) continue;
    if (w <= 0) return -1;
    done += (size_t)w;
  }
  return 0;
}

static void *reply_thread(void *arg) {
  int c = (int)(long)arg;
  int rc = send_all(c, RESP, sizeof RESP - 1);
  logf_("conn fd %d (thread): send %s errno %d", c, rc ? "falhou" : "ok", rc ? errno : 0);
  close(c);
  return NULL;
}

int main(void) {
  signal(SIGPIPE, SIG_IGN);
  notify("diag 1");

  int e = 0;
  if (mkdir("/data/ampr-lz4", 0777) != 0 && errno != EEXIST) e = errno;
  g_log = fopen("/data/ampr-lz4/diag.txt", "w");
  if (g_log) {
    fprintf(g_log, "ampr-diag ok, time=%lld pid=%d\n", (long long)time(NULL), (int)getpid());
    fflush(g_log);
    notify("diag 2: /data/ampr-lz4/diag.txt gravado");
  } else {
    notify("diag 2: falha ao gravar diag.txt (mkdir errno %d, fopen errno %d)", e, errno);
  }

  int s = socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0) {
    notify("diag 3: socket falhou errno %d", errno);
    return 1;
  }
  int one = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  struct sockaddr_in a;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_ANY);
  a.sin_port = htons(DIAG_PORT);
  if (bind(s, (struct sockaddr *)&a, sizeof a) != 0) {
    notify("diag 3: bind 0.0.0.0:%d falhou errno %d", DIAG_PORT, errno);
    close(s);
    return 1;
  }
  if (listen(s, SOMAXCONN) != 0) {
    notify("diag 3: listen falhou errno %d", errno);
    close(s);
    return 1;
  }
  notify("diag 3: bind 0.0.0.0:%d ok; responde 'ok' por %d s", DIAG_PORT, DIAG_SECONDS);

  time_t end = time(NULL) + DIAG_SECONDS;
  int served = 0;
  while (time(NULL) < end) {
    fd_set rs;
    FD_ZERO(&rs);
    FD_SET(s, &rs);
    struct timeval tv = {1, 0};
    if (select(s + 1, &rs, NULL, NULL, &tv) <= 0) continue;
    struct sockaddr_in pa;
    memset(&pa, 0, sizeof pa);
    socklen_t pl = sizeof pa;
    int c = accept(s, (struct sockaddr *)&pa, &pl);
    if (c < 0) { logf_("accept falhou errno %d", errno); continue; }
    char ip[INET_ADDRSTRLEN] = "?";
    inet_ntop(AF_INET, &pa.sin_addr, ip, sizeof ip);
    if (!served) notify("diag: conexão de %s", ip);
    struct timeval rt = {3, 0};
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &rt, sizeof rt);
    setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &rt, sizeof rt);
#ifdef SO_NOSIGPIPE
    setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    char req[2048];
    size_t n = 0;
    req[0] = 0;
    while (n < sizeof req - 1 && !strstr(req, "\r\n\r\n")) {
      ssize_t r = recv(c, req + n, sizeof req - 1 - n, 0);
      if (r <= 0) break;
      n += (size_t)r;
      req[n] = 0;
    }
    char method[8] = "", path[64] = "";
    sscanf(req, "%7s %63s", method, path);
    logf_("conn fd %d de %s:%u: leu %zu B, %s %s", c, ip, (unsigned)ntohs(pa.sin_port), n, method, path);
    served++;
    if (!strcmp(path, "/thread")) {
      pthread_t t;
      int rc = pthread_create(&t, NULL, reply_thread, (void *)(long)c);
      if (rc == 0) { pthread_detach(t); continue; }
      logf_("conn fd %d: pthread_create falhou rc=%d", c, rc);
    }
    int rc = send_all(c, RESP, sizeof RESP - 1);
    logf_("conn fd %d: send %s errno %d", c, rc ? "falhou" : "ok", rc ? errno : 0);
    close(c);
  }
  close(s);
  notify("diag 4: fim (%d requisições atendidas)", served);
  if (g_log) fclose(g_log);
  return 0;
}
