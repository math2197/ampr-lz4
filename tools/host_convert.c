/* CLI host (macOS/Linux) para testar o núcleo de conversão AMPR. GPL-3.0.
 * uso: host_convert [-l nivel] [-w workers] [-f fakelib.sprx] [-c] [-x ms] <origem> <destino>
 *   -c  só roda conv_check_source; -x  seta cancel após ms (teste) */
#include "../src/convert.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

static const char *stage_name(int s) {
  static const char *n[] = {"IDLE", "MOUNT", "SCAN", "PACK", "COPY", "VERIFY", "FINISH", "DONE", "ERROR", "CANCELLED"};
  return (s >= 0 && s <= CONV_STAGE_CANCELLED) ? n[s] : "?";
}

typedef struct {
  conv_opts_t o;
  conv_progress_t p;
  int rc;
  volatile int finished;
} job_t;

static void *runner(void *arg) {
  job_t *j = (job_t *)arg;
  j->rc = conv_run(&j->o, &j->p);
  j->finished = 1;
  return NULL;
}

static double now_s(void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (double)tv.tv_sec + tv.tv_usec / 1e6;
}

int main(int argc, char **argv) {
  static job_t j;
  j.o.level = 9;
  j.o.workers = 4;
  int check_only = 0, ch, cancel_ms = -1;
  while ((ch = getopt(argc, argv, "l:w:f:cx:")) != -1) {
    switch (ch) {
      case 'l': j.o.level = atoi(optarg); break;
      case 'w': j.o.workers = atoi(optarg); break;
      case 'f': j.o.fakelib_sprx = optarg; break;
      case 'c': check_only = 1; break;
      case 'x': cancel_ms = atoi(optarg); break; /* teste: cancela após N ms */
      default:
        fprintf(stderr, "uso: %s [-l nivel] [-w workers] [-f fakelib.sprx] [-c] <origem> [destino]\n", argv[0]);
        return 2;
    }
  }
  if (argc - optind < (check_only ? 1 : 2)) {
    fprintf(stderr, "uso: %s [-l nivel] [-w workers] [-f fakelib.sprx] [-c] <origem> [destino]\n", argv[0]);
    return 2;
  }
  j.o.src_dir = argv[optind];
  char why[256];
  int c = conv_check_source(j.o.src_dir, why, sizeof why);
  printf("check_source: %d (%s)\n", c, why);
  if (check_only) return c ? 1 : 0;
  j.o.dst_dir = argv[optind + 1];

  double t0 = now_s();
  pthread_t th;
  pthread_create(&th, NULL, runner, &j);
  int last_stage = -1;
  double last_print = 0;
  while (!j.finished) {
    usleep(cancel_ms >= 0 ? 10 * 1000 : 100 * 1000);
    int s = atomic_load(&j.p.stage);
    double t = now_s();
    if (cancel_ms >= 0 && (t - t0) * 1000.0 >= cancel_ms) atomic_store(&j.p.cancel, 1);
    if (s != last_stage || t - last_print >= 2.0) {
      uint64_t tot = atomic_load(&j.p.bytes_total), done = atomic_load(&j.p.bytes_done);
      printf("[%7.1fs] %-8s %6.1f%%  %llu/%llu MiB\n", t - t0, stage_name(s), tot ? 100.0 * done / tot : 0.0,
             (unsigned long long)(done >> 20), (unsigned long long)(tot >> 20));
      fflush(stdout);
      last_stage = s;
      last_print = t;
    }
  }
  pthread_join(th, NULL);
  double el = now_s() - t0;
  uint64_t src = atomic_load(&j.p.src_bytes), out = atomic_load(&j.p.out_bytes);
  printf("resultado: rc=%d stage=%s tempo=%.2fs origem=%llu bytes saída=%llu bytes razão=%.2f%%%s%s\n", j.rc,
         stage_name(atomic_load(&j.p.stage)), el, (unsigned long long)src, (unsigned long long)out,
         src ? 100.0 * out / src : 0.0, j.rc ? " erro=" : "", j.rc ? j.p.error : "");
  return j.rc ? 1 : 0;
}
