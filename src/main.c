#include "applog.h"
#include "convert.h"
#include "httpd.h"
#include "jobs.h"
#include "platform.h"
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef AMPR_PS5
#include <sys/syscall.h>
#endif

static void on_exit_log(void) { alog("main: exit() chamado - processo encerrando"); }

int main(void) {
  /* 1a ação: notificação. Se ela não aparecer, o payload nem chegou aqui (loader/libs). */
  conv_notify("AMPR-LZ4 %s iniciando...", AMPR_VERSION);
  /* stdout pode ser o socket do loader, já fechado; send() também usa MSG_NOSIGNAL/SO_NOSIGPIPE */
  int sigpipe_ok = signal(SIGPIPE, SIG_IGN) != SIG_ERR;
  setvbuf(stdout, NULL, _IOLBF, 0);
#ifdef AMPR_PS5
  syscall(SYS_thr_set_name, -1, "ampr-lz4.elf");
#endif
  int lerr = alog_init();
  alog("main: entrou (versão %s, pid %d)", AMPR_VERSION, (int)getpid());
  alog_install_crash_handlers();
  atexit(on_exit_log);
  alog("main: SIGPIPE ignorado=%d, handlers de falha instalados", sigpipe_ok);
  if (lerr) alog_notify("AMPR-LZ4: aviso, log %s indisponível (errno %d)", ALOG_FILE, lerr);

  plat_load_config();
  const char *ep = getenv("AMPR_PORT"); /* override para testes no host */
  if (ep && atoi(ep) > 0) g_cfg.port = atoi(ep);
  alog("config: port=%d workers=%d level=%d", g_cfg.port, g_cfg.workers, g_cfg.level);

  int fd = -1;
  for (int tries = 0; tries < 5 && fd < 0; tries++) {
    if (tries) sleep(1);
    fd = httpd_listen(g_cfg.port); /* loga socket/bind/listen com errno */
    if (fd < 0 && errno != EADDRINUSE) break;
  }
  if (fd < 0) {
    int e = errno;
    if (e == EADDRINUSE) alog_notify("AMPR-LZ4 já está em execução (porta %d em uso)", g_cfg.port);
    else alog_notify("AMPR-LZ4: falha ao abrir a porta %d (errno %d: %s)", g_cfg.port, e, strerror(e));
    alog("main: saindo (sem socket)");
    return 1;
  }

  if (jobs_init() != 0) alog_notify("AMPR-LZ4: falha ao criar a thread de conversão (conversões indisponíveis)");
  else alog("jobs: thread de conversão criada");

  char ip[64];
  plat_local_ip(ip, sizeof ip);
  alog("ip local: %s", ip);
  alog_notify("AMPR-LZ4 pronto: http://%s:%d", ip, g_cfg.port);
  alog("servidor: aguardando conexões em 0.0.0.0:%d", g_cfg.port);
  httpd_serve(fd);
  alog("main: httpd_serve retornou (inesperado)");
  return 0;
}
