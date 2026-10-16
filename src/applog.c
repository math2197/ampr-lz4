#include "applog.h"
#include "convert.h"
#include "platform.h"
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static FILE *g_f;
static int g_stdout_sock; /* no PS5 o stdout costuma ser o socket do loader */
static int g_stdout_dead;

#ifdef MSG_NOSIGNAL
#define ALOG_SEND_FLAGS (MSG_DONTWAIT | MSG_NOSIGNAL)
#else
#define ALOG_SEND_FLAGS MSG_DONTWAIT
#endif

static void write_line(const char *msg) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  time_t t = tv.tv_sec;
  struct tm tm;
  char ts[32];
  if (localtime_r(&t, &tm)) strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm);
  else snprintf(ts, sizeof ts, "%lld", (long long)t);
  pthread_mutex_lock(&g_mu);
  if (g_stdout_sock) {
    /* nunca bloquear: se quem enviou o payload parou de ler o socket, a linha é descartada */
    char line[1200];
    int l = snprintf(line, sizeof line, "[ampr-lz4 %s.%03d] %s\n", ts, (int)(tv.tv_usec / 1000), msg);
    if (l > 0 && send(1, line, (size_t)l < sizeof line ? (size_t)l : sizeof line - 1, ALOG_SEND_FLAGS) < 0 && errno != EINTR) {
      g_stdout_sock = 0;
      g_stdout_dead = 1; /* ninguém lendo (ou fechado): para de escrever no stdout, o arquivo de log continua */
    }
  } else if (!g_stdout_dead) {
    printf("[ampr-lz4 %s.%03d] %s\n", ts, (int)(tv.tv_usec / 1000), msg);
    fflush(stdout);
  }
  if (g_f) {
    fprintf(g_f, "%s.%03d %s\n", ts, (int)(tv.tv_usec / 1000), msg);
    fflush(g_f);
    fsync(fileno(g_f));
  }
  pthread_mutex_unlock(&g_mu);
}

static void notify_hook(const char *msg) {
  char b[600];
  snprintf(b, sizeof b, "notificação: %s", msg);
  write_line(b);
}

int alog_init(void) {
  char dir[512], path[512];
  plat_path(dir, sizeof dir, ALOG_DIR);
  plat_path(path, sizeof path, ALOG_FILE);
  struct stat so;
  g_stdout_sock = fstat(1, &so) == 0 && S_ISSOCK(so.st_mode);
  if (g_stdout_sock) { /* MSG_DONTWAIT não basta em todo kernel: limite duro de 100 ms por escrita */
    struct timeval tv = {0, 100000};
    setsockopt(1, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(1, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
  }
  int err = 0;
  if (mkdir(dir, 0777) != 0 && errno != EEXIST) err = errno;
  struct stat st;
  const char *mode = (stat(path, &st) == 0 && st.st_size > (1 << 20)) ? "w" : "a";
  FILE *f = fopen(path, mode);
  if (!f && !err) err = errno ? errno : EIO;
  pthread_mutex_lock(&g_mu);
  g_f = f;
  pthread_mutex_unlock(&g_mu);
  conv_notify_hook = notify_hook;
  return err;
}

void alog(const char *fmt, ...) {
  char msg[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);
  write_line(msg);
}

void alog_notify(const char *fmt, ...) {
  char msg[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);
  conv_notify("%s", msg); /* o hook grava no log */
}

static const char *sig_name(int sig) {
  switch (sig) {
  case SIGSEGV: return "SIGSEGV";
  case SIGBUS: return "SIGBUS";
  case SIGILL: return "SIGILL";
  case SIGFPE: return "SIGFPE";
  case SIGABRT: return "SIGABRT";
  case SIGSYS: return "SIGSYS";
  case SIGTRAP: return "SIGTRAP";
  default: return "?";
  }
}

static void on_fatal(int sig) {
  /* sem mutex/stdio: o sinal pode ter chegado com g_mu travado */
  char m[128];
  int l = snprintf(m, sizeof m, "FATAL: sinal %d (%s) - processo encerrando\n", sig, sig_name(sig));
  if (l > 0) {
    ssize_t w = write(1, m, (size_t)l);
    if (g_f) w = write(fileno(g_f), m, (size_t)l);
    (void)w;
    if (g_f) fsync(fileno(g_f));
  }
  conv_notify_hook = NULL;
  conv_notify("AMPR-LZ4 caiu: sinal %d (%s)", sig, sig_name(sig));
  _exit(128 + sig);
}

void alog_install_crash_handlers(void) {
  static const int sigs[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGSYS, SIGTRAP};
  for (size_t i = 0; i < sizeof sigs / sizeof sigs[0]; i++) signal(sigs[i], on_fatal);
}
