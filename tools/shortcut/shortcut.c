/* ampr-shortcut.elf: payload separado que instala o app-atalho "AMPR-LZ4" na aba Midia e sai.
 * Roda fora do ampr-lz4.elf: se carregar libSceAppInstUtil derrubar este processo, o servidor nao cai.
 *
 * Padrao copiado de https://github.com/ps5-payload-dev/ftpsrv (Makefile.ps5, alvo ftpsrv-ps5-install.elf,
 * e install-ps5.c): link com -lSceIpmi -lSceAppInstUtil (nessa ordem), sceAppInstUtilInitialize(),
 * gravar /user/app/<TID>/sce_sys/{param.json,icon0.png} e sceAppInstUtilAppInstallTitleDir(TID, "/user/app/", 0),
 * resolvida por kernel_dynlib_handle + kernel_dynlib_resolve com o NID Wudg3Xe3heE (fallback: InstallAll).
 * GPL-3.0, como o projeto de origem. */
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <ps5/kernel.h>

#include "icon_asset.h"

int sceAppInstUtilInitialize(void);
int sceAppInstUtilAppInstallAll(void *);

typedef struct notify_request {
  char useless1[45];
  char message[3075];
} notify_request_t;
int sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int);

#define TITLE_ID "AMLZ00001"
#define APP_ROOT "/user/app"
#define APP_DIR APP_ROOT "/" TITLE_ID
#define SYS_DIR APP_DIR "/sce_sys"
#define LOG_DIR "/data/ampr-lz4"
#define LOG_PATH LOG_DIR "/ampr-shortcut.log"
#define CFG_PATH LOG_DIR "/config.ini"
#define NID_INSTALL_TITLEDIR "Wudg3Xe3heE"

static FILE *g_log;

/* Uma linha por chamada; fflush + fsync para sobreviver a um crash logo depois. */
static void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf_(const char *fmt, ...) {
  char msg[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);
  puts(msg);
  fflush(stdout);
  if (g_log) {
    fprintf(g_log, "%s\n", msg);
    fflush(g_log);
    fsync(fileno(g_log));
  }
}

static void notify(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void notify(const char *fmt, ...) {
  notify_request_t req;
  char msg[300];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);
  logf_("notificacao: %s", msg);
  memset(&req, 0, sizeof req);
  strncpy(req.message, msg, sizeof req.message - 1);
  sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
}

static int read_port(void) {
  int port = 8095;
  char line[256];
  FILE *f = fopen(CFG_PATH, "r");
  if (!f) return port;
  while (fgets(line, sizeof line, f)) {
    if (strncmp(line, "port", 4) != 0) continue;
    char *p = line + 4;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '=') continue;
    int x = atoi(p + 1);
    if (x > 0 && x < 65536) port = x;
  }
  fclose(f);
  return port;
}

static int write_file(const char *path, const void *data, size_t len) {
  FILE *f = fopen(path, "wb");
  if (!f) return -1;
  int ok = fwrite(data, 1, len, f) == len;
  ok = (fclose(f) == 0) && ok;
  return ok ? 0 : -1;
}

static int mk(const char *path) {
  return (mkdir(path, 0755) != 0 && errno != EEXIST) ? -1 : 0;
}

int main(void) {
  mkdir(LOG_DIR, 0777);
  g_log = fopen(LOG_PATH, "a");
  logf_("ampr-shortcut: inicio (log %s)", g_log ? "ok" : "indisponivel");
  notify("AMPR-LZ4: instalando atalho em Midia...");

  int port = read_port();
  char json[768];
  int jl = snprintf(json, sizeof json,
      "{\n"
      "  \"titleId\": \"%s\",\n"
      "  \"applicationCategoryType\": 65536,\n"
      "  \"deeplinkUri\": \"http://127.0.0.1:%d/\",\n"
      "  \"localizedParameters\": {\n"
      "    \"defaultLanguage\": \"en-US\",\n"
      "    \"en-US\": { \"titleName\": \"AMPR-LZ4\" },\n"
      "    \"pt-BR\": { \"titleName\": \"AMPR-LZ4\" }\n"
      "  }\n"
      "}\n", TITLE_ID, port);
  if (jl <= 0 || jl >= (int)sizeof json) { notify("AMPR-LZ4: atalho falhou (param.json)"); return 1; }
  logf_("porta lida do config: %d", port);

  logf_("vou criar pastas %s", SYS_DIR);
  if (mk(APP_DIR) || mk(SYS_DIR)) {
    int e = errno;
    notify("AMPR-LZ4: atalho falhou (mkdir errno %d)", e);
    return 1;
  }
  logf_("vou gravar param.json");
  if (write_file(SYS_DIR "/param.json", json, (size_t)jl)) {
    int e = errno;
    notify("AMPR-LZ4: atalho falhou (param.json errno %d)", e);
    return 1;
  }
  logf_("vou gravar icon0.png (%u bytes)", icon0_png_len);
  if (write_file(SYS_DIR "/icon0.png", icon0_png, icon0_png_len)) {
    int e = errno;
    notify("AMPR-LZ4: atalho falhou (icon0.png errno %d)", e);
    return 1;
  }

  int (*install_titledir)(const char *, const char *, void *) = 0;
  uint32_t handle = 0;
  logf_("vou resolver sceAppInstUtilAppInstallTitleDir (kernel_dynlib_handle)");
  if (!kernel_dynlib_handle(-1, "libSceAppInstUtil.sprx", &handle)) {
    logf_("vou chamar kernel_dynlib_resolve (handle %u)", handle);
    install_titledir = (void *)kernel_dynlib_resolve(-1, handle, NID_INSTALL_TITLEDIR);
  }
  logf_("TitleDir %s", install_titledir ? "resolvida" : "nao resolvida (usarei InstallAll)");

  logf_("vou chamar sceAppInstUtilInitialize");
  int err = sceAppInstUtilInitialize();
  logf_("sceAppInstUtilInitialize -> 0x%08X", (unsigned)err);
  if (err) {
    notify("AMPR-LZ4: atalho falhou (Initialize 0x%08X)", (unsigned)err);
    return 1;
  }

  logf_("vou registrar o app (%s)", install_titledir ? "InstallTitleDir" : "InstallAll");
  err = install_titledir ? install_titledir(TITLE_ID, APP_ROOT "/", 0) : sceAppInstUtilAppInstallAll(0);
  logf_("registro -> 0x%08X", (unsigned)err);
  if (err) {
    notify("AMPR-LZ4: atalho falhou (instalar 0x%08X)", (unsigned)err);
    return 1;
  }
  notify("AMPR-LZ4: atalho em Midia instalado (porta %d)", port);
  return 0;
}
