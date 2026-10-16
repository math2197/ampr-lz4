/* Substituto do núcleo, só para `make host-stub` / `make ps5-stub`: simula conv_run com progresso falso. */
#include "convert.h"
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void (*conv_notify_hook)(const char *msg) = NULL;

void conv_notify(const char *fmt, ...) {
  char msg[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);
  if (conv_notify_hook) conv_notify_hook(msg);
  printf("[notify] %s\n", msg);
}

int conv_check_source(const char *src_dir, char *why, int why_len) {
  struct stat st;
  if (stat(src_dir, &st) != 0 || !S_ISDIR(st.st_mode)) { snprintf(why, (size_t)why_len, "pasta de origem inexistente"); return 1; }
  return 0;
}

static void step(conv_progress_t *p, int stage, uint64_t total, int ms_total) {
  atomic_store(&p->stage, stage);
  atomic_store(&p->bytes_total, total);
  atomic_store(&p->bytes_done, 0);
  const int ticks = ms_total / 100;
  for (int i = 1; i <= ticks && !atomic_load(&p->cancel); i++) {
    usleep(100000);
    atomic_store(&p->bytes_done, total / (uint64_t)ticks * (uint64_t)i);
  }
}

int conv_run(const conv_opts_t *o, conv_progress_t *p) {
  const uint64_t src = 4ull << 30;
  atomic_store(&p->src_bytes, src);
  step(p, CONV_STAGE_SCAN, src, 1000);
  step(p, CONV_STAGE_PACK, src, 8000);
  step(p, CONV_STAGE_COPY, src * 6 / 10, 2000);
  step(p, CONV_STAGE_VERIFY, src * 6 / 10, 2000);
  if (atomic_load(&p->cancel)) { atomic_store(&p->stage, CONV_STAGE_CANCELLED); return -1; }
  char tmp[1024];
  mkdir(o->dst_dir, 0777);
  snprintf(tmp, sizeof tmp, "%s/ampr_assets.index", o->dst_dir);
  FILE *f = fopen(tmp, "w");
  if (!f) { snprintf(p->error, sizeof p->error, "stub: não consegui gravar %s: %s", tmp, strerror(errno)); atomic_store(&p->stage, CONV_STAGE_ERROR); return -1; }
  fprintf(f, "stub level=%d workers=%d fakelib=%s\n", o->level, o->workers, o->fakelib_sprx ? o->fakelib_sprx : "(none)");
  fclose(f);
  atomic_store(&p->out_bytes, src * 6 / 10);
  atomic_store(&p->stage, CONV_STAGE_DONE);
  return 0;
}
