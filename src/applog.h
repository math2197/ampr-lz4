/* Log de diagnóstico da plataforma: /data/ampr-lz4/ampr-lz4.log (prefixo de host aplicado) + stdout.
 * Uma linha por chamada, com timestamp; fflush + fsync a cada linha (sobrevive a um travamento). */
#pragma once

#define ALOG_DIR "/data/ampr-lz4"
#define ALOG_FILE ALOG_DIR "/ampr-lz4.log"

/* Cria a pasta e abre o log (append; zera se passar de 1 MiB). Liga conv_notify ao log.
 * Retorna 0 ou errno da falha (o log continua indo para stdout). */
int alog_init(void);
void alog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Loga e envia notificação no PS5 (conv_notify). */
void alog_notify(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Instala handlers de SIGSEGV/SIGBUS/SIGILL/SIGFPE/SIGABRT/SIGSYS/SIGTRAP (como o websrv do ps5-payload-dev):
 * gravam "FATAL: sinal N" no log/stdout, notificam e encerram. Sem isso o payload morre em silêncio. */
void alog_install_crash_handlers(void);
