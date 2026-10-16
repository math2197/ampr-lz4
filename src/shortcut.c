/* Instala o app-atalho "AMPR-LZ4" (AMLZ00001) na aba Mídia, a partir do próprio ampr-lz4.elf.
 *
 * Padrão do ftpsrv (ps5-payload-dev/ftpsrv, install-ps5.c): link com -lSceIpmi -lSceAppInstUtil (nessa ordem),
 * sceAppInstUtilInitialize(), gravar /user/app/<TID>/sce_sys/{param.json,icon0.png} e
 * sceAppInstUtilAppInstallTitleDir(TID, "/user/app/", 0), resolvida por kernel_dynlib_handle +
 * kernel_dynlib_resolve com o NID Wudg3Xe3heE (fallback: sceAppInstUtilAppInstallAll).
 * Porte de tools/shortcut/shortcut.c (que funcionou como payload separado). */
#include "shortcut.h"
#include "applog.h"
#include "platform.h"

#ifdef AMPR_PS5
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <ps5/kernel.h>

#include "icon_asset.h"

int sceAppInstUtilInitialize(void);
int sceAppInstUtilTerminate(void);
int sceAppInstUtilAppInstallAll(void *);

#define TITLE_ID "AMLZ00001"
#define APP_ROOT "/user/app"
#define APP_DIR APP_ROOT "/" TITLE_ID
#define SYS_DIR APP_DIR "/sce_sys"
#define PARAM_PATH SYS_DIR "/param.json"
#define ICON_PATH SYS_DIR "/icon0.png"
#define NID_INSTALL_TITLEDIR "Wudg3Xe3heE"

/* 1 se o arquivo existe com exatamente estes bytes. */
static int same_file(const char *path, const void *data, size_t len) {
  FILE *f = fopen(path, "rb");
  if (!f) return 0;
  unsigned char *buf = malloc(len + 1);
  if (!buf) { fclose(f); return 0; }
  size_t n = fread(buf, 1, len + 1, f);
  int same = n == len && memcmp(buf, data, len) == 0;
  free(buf);
  fclose(f);
  return same;
}

static int write_file(const char *path, const void *data, size_t len) {
  FILE *f = fopen(path, "wb");
  if (!f) return -1;
  int ok = fwrite(data, 1, len, f) == len;
  ok = (fclose(f) == 0) && ok;
  return ok ? 0 : -1;
}

static int mk(const char *path) { return (mkdir(path, 0755) != 0 && errno != EEXIST) ? -1 : 0; }

static void *shortcut_thread(void *arg) {
  (void)arg;
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
      "}\n", TITLE_ID, g_cfg.port);
  if (jl <= 0 || jl >= (int)sizeof json) {
    alog_notify("AMPR-LZ4: atalho falhou (param.json 0x%08X)", 0u);
    return NULL;
  }

  struct stat st;
  int existed = stat(APP_DIR, &st) == 0;
  if (existed && same_file(PARAM_PATH, json, (size_t)jl) && same_file(ICON_PATH, icon0_png, icon0_png_len)) {
    alog("atalho: já instalado (%s)", APP_DIR);
    return NULL;
  }
  alog("atalho: %s (porta %d)", existed ? "atualizando" : "primeira instalação", g_cfg.port);

  alog("atalho: vou criar as pastas %s", SYS_DIR);
  if (mk(APP_DIR) || mk(SYS_DIR)) {
    alog_notify("AMPR-LZ4: atalho falhou (mkdir 0x%08X)", (unsigned)errno);
    return NULL;
  }
  alog("atalho: vou gravar param.json");
  if (write_file(PARAM_PATH, json, (size_t)jl)) {
    alog_notify("AMPR-LZ4: atalho falhou (param.json 0x%08X)", (unsigned)errno);
    return NULL;
  }
  alog("atalho: vou gravar icon0.png (%u bytes)", icon0_png_len);
  if (write_file(ICON_PATH, icon0_png, icon0_png_len)) {
    alog_notify("AMPR-LZ4: atalho falhou (icon0.png 0x%08X)", (unsigned)errno);
    return NULL;
  }

  int (*install_titledir)(const char *, const char *, void *) = 0;
  uint32_t handle = 0;
  alog("atalho: vou chamar kernel_dynlib_handle (libSceAppInstUtil.sprx)");
  if (!kernel_dynlib_handle(-1, "libSceAppInstUtil.sprx", &handle)) {
    alog("atalho: vou chamar kernel_dynlib_resolve (handle %u)", handle);
    install_titledir = (void *)kernel_dynlib_resolve(-1, handle, NID_INSTALL_TITLEDIR);
  }
  alog("atalho: TitleDir %s", install_titledir ? "resolvida" : "não resolvida (usarei InstallAll)");

  alog("atalho: vou chamar sceAppInstUtilInitialize");
  int err = sceAppInstUtilInitialize();
  alog("atalho: sceAppInstUtilInitialize -> 0x%08X", (unsigned)err);
  if (err) {
    alog_notify("AMPR-LZ4: atalho falhou (Initialize 0x%08X)", (unsigned)err);
    return NULL;
  }

  alog("atalho: vou registrar o app (%s)", install_titledir ? "InstallTitleDir" : "InstallAll");
  err = install_titledir ? install_titledir(TITLE_ID, APP_ROOT "/", 0) : sceAppInstUtilAppInstallAll(0);
  alog("atalho: registro -> 0x%08X", (unsigned)err);

  alog("atalho: vou chamar sceAppInstUtilTerminate");
  int terr = sceAppInstUtilTerminate();
  alog("atalho: sceAppInstUtilTerminate -> 0x%08X", (unsigned)terr);

  if (err) {
    alog_notify("AMPR-LZ4: atalho falhou (instalar 0x%08X)", (unsigned)err);
    return NULL;
  }
  if (!existed) alog_notify("AMPR-LZ4: atalho em Mídia criado");
  else alog("atalho: atualizado");
  return NULL;
}

int shortcut_start(void) {
  if (!g_cfg.shortcut) {
    alog("atalho: desligado (shortcut=0)");
    return 0;
  }
  pthread_t t;
  alog("atalho: vou criar a thread");
  int e = pthread_create(&t, NULL, shortcut_thread, NULL);
  if (e) {
    alog_notify("AMPR-LZ4: atalho falhou (thread 0x%08X)", (unsigned)e);
    return e;
  }
  pthread_detach(t);
  return 0;
}

#else /* host */

int shortcut_start(void) {
  alog("atalho: ignorado no host (%s)", g_cfg.shortcut ? "ligado" : "shortcut=0");
  return 0;
}
#endif
