#include "jobs.h"
#include "applog.h"
#include "ampr_sprx_asset.h"
#include "convert.h"
#include "platform.h"
#include "smp.h"
#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define QMAX 16
#define AMPR_EMBED_SPRX_DIR "/data/ampr-lz4"
#define AMPR_SPRX_MAGIC "AMPRPAK4" /* formato de pacote que o núcleo gera */
#define HMAX 10

typedef struct {
  int job;
  char id[16], title[128], dest_drive[8];
  int level;
} jobreq_t;

typedef struct {
  int job;
  char id[16], title[128];
  const char *result;   /* done|error|cancelled */
  char error[256];
  uint64_t src_bytes, out_bytes;
  int seconds;
} hist_t;

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static jobreq_t queue[QMAX];
static int qn, next_job = 1;
static hist_t hist[HMAX];
static int hn;                       /* total já escrito (anel) */

static int cur_active;
static jobreq_t cur;
static conv_progress_t prog;
static int cur_finished;             /* worker já passou pela etapa FINISH */
static char cur_error[256];
static time_t cur_start, cur_end;
static int cur_valid;                /* cur/prog descrevem um job (rodando ou terminado) */
/* amostra para velocidade */
static int smp_stage = -1;
static uint64_t smp_done;
static double smp_t, speed_ema;

static double now_s(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static const char *stage_name(int s) {
  switch (s) {
    case CONV_STAGE_MOUNT: return "mount";
    case CONV_STAGE_SCAN: return "scan";
    case CONV_STAGE_PACK: return "pack";
    case CONV_STAGE_COPY: return "copy";
    case CONV_STAGE_VERIFY: return "verify";
    case CONV_STAGE_FINISH: return "finish";
    case CONV_STAGE_DONE: return "done";
    case CONV_STAGE_ERROR: return "error";
    case CONV_STAGE_CANCELLED: return "cancelled";
    default: return "idle";
  }
}

static void fmt_gb(char *o, size_t n, uint64_t b) { snprintf(o, n, "%.2f GB", (double)b / 1073741824.0); }

static int exists_dir(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISDIR(st.st_mode); }

/* 1 se o arquivo contém magic (busca em blocos com sobreposição); 0 se não; -1 se não abre. */
static int file_has_magic(const char *path, const char *magic) {
  FILE *f = fopen(path, "rb");
  if (!f) return -1;
  size_t ml = strlen(magic), keep = 0, n;
  unsigned char buf[65536 + 16];
  int found = 0;
  while (!found && (n = fread(buf + keep, 1, sizeof buf - keep, f)) > 0) {
    size_t have = keep + n;
    for (size_t i = 0; i + ml <= have; i++)
      if (buf[i] == (unsigned char)magic[0] && !memcmp(buf + i, magic, ml)) { found = 1; break; }
    keep = have < ml - 1 ? have : ml - 1;
    memmove(buf, buf + have - keep, keep);
  }
  fclose(f);
  return found;
}

/* 1 se path já tem exatamente data/len. */
static int same_content(const char *path, const unsigned char *data, size_t len) {
  struct stat st;
  if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || (uint64_t)st.st_size != (uint64_t)len) return 0;
  FILE *f = fopen(path, "rb");
  if (!f) return 0;
  unsigned char buf[65536];
  size_t off = 0, n;
  int same = 1;
  while (same && (n = fread(buf, 1, sizeof buf, f)) > 0) {
    if (off + n > len || memcmp(buf, data + off, n)) same = 0;
    off += n;
  }
  fclose(f);
  return same && off == len;
}

/* Grava o libSceAmpr.sprx embutido em /data/ampr-lz4/ (se ainda não estiver lá). 0 ok. */
static int embedded_sprx_path(char *out, size_t n, char *err, size_t en) {
  char dir[512], tmp[560];
  plat_path(dir, sizeof dir, AMPR_EMBED_SPRX_DIR);
  snprintf(out, n, "%s/libSceAmpr.sprx", dir);
  if (same_content(out, libSceAmpr_sprx, libSceAmpr_sprx_len)) return 0;
  if (mkdir(dir, 0777) != 0 && !exists_dir(dir)) { snprintf(err, en, "não foi possível criar %s", dir); return -1; }
  snprintf(tmp, sizeof tmp, "%s.tmp", out);
  FILE *f = fopen(tmp, "wb");
  if (!f) { snprintf(err, en, "não foi possível gravar %s", tmp); return -1; }
  int ok = fwrite(libSceAmpr_sprx, 1, libSceAmpr_sprx_len, f) == libSceAmpr_sprx_len;
  ok = (fflush(f) == 0) && ok;
  ok = (fsync(fileno(f)) == 0) && ok;
  ok = (fclose(f) == 0) && ok;
  if (!ok || rename(tmp, out) != 0) { unlink(tmp); snprintf(err, en, "falha ao gravar %s", out); return -1; }
  return 0;
}

/* Escolhe o libSceAmpr.sprx a gravar em fakelib/ do destino:
 *  (a) o do SMP (emulators_path) se existir e suportar pacotes (string AMPRPAK4);
 *  (b) senão o embutido (ampr_emu 0.4.2.1), gravado em /data/ampr-lz4/libSceAmpr.sprx.
 * O da fakelib/ da origem nunca é usado: o núcleo o substitui pelo escolhido (scan_tree). */
static int choose_sprx(char *out, size_t n, char *err, size_t en) {
  char smp[512];
  smp_ampr_sprx_path(smp, sizeof smp);
  if (smp[0]) {
    int h = file_has_magic(smp, AMPR_SPRX_MAGIC);
    if (h == 1) { snprintf(out, n, "%s", smp); alog("job: sprx do SMP com " AMPR_SPRX_MAGIC ": %s", smp); return 0; }
    alog("job: sprx do SMP %s sem " AMPR_SPRX_MAGIC " (%d); usando o embutido", smp, h);
  }
  if (embedded_sprx_path(out, n, err, en) != 0) return -1;
  alog("job: usando libSceAmpr.sprx embutido: %s", out);
  return 0;
}

static void set_stage(int s) {
  pthread_mutex_lock(&mu);
  atomic_store(&prog.stage, s);
  pthread_mutex_unlock(&mu);
}

static void run_job(const jobreq_t *r) {
  char err[256] = "", runtime[512] = "", why[200] = "", dst[640], hb[600], root_real[400], logical[300];
  int ok = 0, mounted = 0, cancelled = 0;

  pthread_mutex_lock(&mu);
  cur = *r;
  cur_active = 1; cur_valid = 1; cur_finished = 0; cur_error[0] = 0;
  memset(&prog, 0, sizeof prog);
  atomic_store(&prog.stage, CONV_STAGE_MOUNT);
  cur_start = time(NULL);
  smp_stage = -1; speed_ema = 0;
  pthread_mutex_unlock(&mu);

  conv_notify("AMPR-LZ4: iniciando %s", r->title[0] ? r->title : r->id);

  const char *root = plat_drive_root(r->dest_drive);
  if (!root) { snprintf(err, sizeof err, "drive de destino inválido"); goto out; }
  plat_path(root_real, sizeof root_real, root);
  if (!exists_dir(root_real)) { snprintf(err, sizeof err, "drive %s não está disponível", r->dest_drive); goto out; }
  snprintf(logical, sizeof logical, "%s/homebrew/%s-AMPR", root, r->id);
  plat_path(dst, sizeof dst, logical);
  snprintf(hb, sizeof hb, "%s/homebrew", root_real);
  if (!exists_dir(hb) && mkdir(hb, 0777) != 0 && !exists_dir(hb)) { snprintf(err, sizeof err, "não foi possível criar %s", hb); goto out; }
  {
    struct stat st;
    if (stat(dst, &st) == 0) { snprintf(err, sizeof err, "o destino já existe: %s", logical); goto out; }
  }

  {
    /* Se o SMP já mantém a imagem montada (jogo pronto para iniciar), reaproveita e não desmonta no fim. */
    smp_game_t gi;
    memset(&gi, 0, sizeof gi);
    if (smp_game_info(r->id, &gi, NULL, 0) == 0 && gi.mounted && gi.runtime_path[0]) {
      snprintf(runtime, sizeof runtime, "%s", gi.runtime_path);
    } else {
      if (smp_mount(r->id, runtime, sizeof runtime, err, sizeof err)) goto out;
      mounted = 1;
    }
  }
  if (atomic_load(&prog.cancel)) { cancelled = 1; goto out; }

  if (conv_check_source(runtime, why, sizeof why)) {
    snprintf(err, sizeof err, "jogo incompatível: %s", why[0] ? why : "não importa libSceAmpr");
    goto out;
  }

  char sprx[512];
  if (choose_sprx(sprx, sizeof sprx, err, sizeof err) != 0) goto out;
  conv_opts_t o = {runtime, dst, sprx, r->level, g_cfg.workers};
  atomic_store(&prog.stage, CONV_STAGE_SCAN);
  int rc = conv_run(&o, &prog);
  if (rc == 0) ok = 1;
  else if (atomic_load(&prog.stage) == CONV_STAGE_CANCELLED) cancelled = 1;
  else snprintf(err, sizeof err, "%s", prog.error[0] ? prog.error : "falha na conversão");

out:
  set_stage(CONV_STAGE_FINISH);
  if (ok) {
    char mode[16] = "";
    if (smp_get_fakelib(r->id, mode, sizeof mode) == 0 && !strcmp(mode, "disabled")) {
      char e2[128];
      if (smp_set_fakelib(r->id, "emulators", e2, sizeof e2)) conv_notify("AMPR-LZ4: aviso, fakelib não ajustado (%s)", e2);
    }
  }
  if (mounted) {
    char e3[128];
    if (smp_unmount(r->id, e3, sizeof e3)) conv_notify("AMPR-LZ4: aviso, falha ao desmontar (%s)", e3);
  }

  int final = ok ? CONV_STAGE_DONE : cancelled ? CONV_STAGE_CANCELLED : CONV_STAGE_ERROR;
  uint64_t src = atomic_load(&prog.src_bytes), outb = atomic_load(&prog.out_bytes);
  if (ok) {
    char a[32], b[32];
    fmt_gb(a, sizeof a, src); fmt_gb(b, sizeof b, outb);
    conv_notify("AMPR-LZ4: %s concluído: %s -> %s (%.0f%%)", r->title[0] ? r->title : r->id, a, b, src ? 100.0 * (double)outb / (double)src : 0.0);
  } else if (final == CONV_STAGE_ERROR) conv_notify("AMPR-LZ4: erro em %s: %s", r->id, err);
  else conv_notify("AMPR-LZ4: %s cancelado", r->id);

  pthread_mutex_lock(&mu);
  hist_t *h = &hist[hn % HMAX];
  hn++;
  memset(h, 0, sizeof *h);
  h->job = r->job;
  snprintf(h->id, sizeof h->id, "%s", r->id);
  snprintf(h->title, sizeof h->title, "%s", r->title);
  h->result = stage_name(final);
  snprintf(h->error, sizeof h->error, "%s", err);
  h->src_bytes = src; h->out_bytes = outb;
  h->seconds = (int)(time(NULL) - cur_start);
  atomic_store(&prog.stage, final);
  if (!ok && final == CONV_STAGE_ERROR) snprintf(prog.error, sizeof prog.error, "%s", err);
  cur_finished = 1;
  cur_end = time(NULL);
  cur_active = 0;
  pthread_mutex_unlock(&mu);
}

static void *worker(void *arg) {
  (void)arg;
  alog("jobs: thread de conversão em execução"); /* prova de que threads com pilha custom rodam */
  for (;;) {
    pthread_mutex_lock(&mu);
    while (qn == 0) pthread_cond_wait(&cv, &mu);
    jobreq_t r = queue[0];
    memmove(&queue[0], &queue[1], (size_t)(--qn) * sizeof queue[0]);
    pthread_mutex_unlock(&mu);
    run_job(&r);
  }
  return NULL;
}

int jobs_init(void) {
  pthread_t t;
  pthread_attr_t a;
  pthread_attr_init(&a);
  pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
  int rs = pthread_attr_setstacksize(&a, 1u << 20); /* walk() recursivo usa ~6 KiB de pilha por nível */
  int rc = pthread_create(&t, &a, worker, NULL);
  pthread_attr_destroy(&a);
  if (rc != 0) {
    alog("jobs: pthread_create (pilha 1 MiB, setstacksize=%d) falhou: %d; tentando pilha padrão", rs, rc);
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    rc = pthread_create(&t, &a, worker, NULL);
    pthread_attr_destroy(&a);
    if (rc != 0) alog("jobs: pthread_create (pilha padrão) falhou: %d", rc);
  }
  return rc;
}

int jobs_enqueue(const char *id, const char *title, const char *drive, int level, char *err, size_t en) {
  pthread_mutex_lock(&mu);
  int dup = cur_active && !strcmp(cur.id, id);
  for (int i = 0; i < qn; i++) if (!strcmp(queue[i].id, id)) dup = 1;
  if (dup) { snprintf(err, en, "este jogo já está na fila"); pthread_mutex_unlock(&mu); return -1; }
  if (qn >= QMAX) { snprintf(err, en, "fila cheia"); pthread_mutex_unlock(&mu); return -1; }
  jobreq_t *r = &queue[qn++];
  memset(r, 0, sizeof *r);
  r->job = next_job++;
  snprintf(r->id, sizeof r->id, "%s", id);
  snprintf(r->title, sizeof r->title, "%s", title);
  snprintf(r->dest_drive, sizeof r->dest_drive, "%s", drive);
  r->level = level;
  int j = r->job;
  pthread_cond_signal(&cv);
  pthread_mutex_unlock(&mu);
  return j;
}

int jobs_cancel(int job) {
  int rc = -1;
  pthread_mutex_lock(&mu);
  if (cur_active && (job == 0 || cur.job == job)) {
    atomic_store(&prog.cancel, 1);
    rc = 0;
  } else if (job > 0) {
    for (int i = 0; i < qn; i++)
      if (queue[i].job == job) {
        hist_t *h = &hist[hn % HMAX];
        hn++;
        memset(h, 0, sizeof *h);
        h->job = job;
        snprintf(h->id, sizeof h->id, "%s", queue[i].id);
        snprintf(h->title, sizeof h->title, "%s", queue[i].title);
        h->result = "cancelled";
        memmove(&queue[i], &queue[i + 1], (size_t)(qn - i - 1) * sizeof queue[0]);
        qn--;
        rc = 0;
        break;
      }
  }
  pthread_mutex_unlock(&mu);
  return rc;
}

void jobs_status_json(sb_t *b) {
  pthread_mutex_lock(&mu);
  sb_puts(b, "{\"current\":");
  if (!cur_valid || (!cur_active && time(NULL) - cur_end > 20)) sb_puts(b, "null");
  else {
    int st = atomic_load(&prog.stage);
    /* o núcleo pode marcar DONE/ERROR antes de a plataforma terminar FINISH */
    if ((st == CONV_STAGE_DONE || st == CONV_STAGE_ERROR || st == CONV_STAGE_CANCELLED) && !cur_finished) st = CONV_STAGE_FINISH;
    uint64_t done = atomic_load(&prog.bytes_done), total = atomic_load(&prog.bytes_total);
    double t = now_s();
    if (!cur_active) { smp_stage = -1; speed_ema = 0; }
    else if (st != smp_stage) { smp_stage = st; smp_done = done; smp_t = t; speed_ema = 0; }
    else if (t - smp_t >= 0.5) {
      double inst = done >= smp_done ? (double)(done - smp_done) / (t - smp_t) : 0;
      speed_ema = speed_ema > 0 ? 0.6 * speed_ema + 0.4 * inst : inst;
      smp_done = done; smp_t = t;
    }
    long long eta = (speed_ema > 1 && total > done) ? (long long)((double)(total - done) / speed_ema) : -1;
    sb_printf(b, "{\"job\":%d,\"id\":", cur.job); sb_json_str(b, cur.id);
    sb_puts(b, ",\"title\":"); sb_json_str(b, cur.title);
    sb_printf(b, ",\"stage\":\"%s\",\"bytes_done\":%llu,\"bytes_total\":%llu,\"src_bytes\":%llu,\"out_bytes\":%llu,\"speed_bps\":%.0f,\"eta_s\":%lld,\"error\":",
              stage_name(st), (unsigned long long)done, (unsigned long long)total,
              (unsigned long long)atomic_load(&prog.src_bytes), (unsigned long long)atomic_load(&prog.out_bytes), cur_active ? speed_ema : 0.0, cur_active ? eta : 0LL);
    sb_json_str(b, st == CONV_STAGE_ERROR ? prog.error : "");
    sb_puts(b, "}");
  }
  sb_puts(b, ",\"queue\":[");
  for (int i = 0; i < qn; i++) {
    sb_printf(b, "%s{\"job\":%d,\"id\":", i ? "," : "", queue[i].job); sb_json_str(b, queue[i].id);
    sb_puts(b, ",\"title\":"); sb_json_str(b, queue[i].title);
    sb_printf(b, ",\"dest_drive\":\"%s\",\"level\":%d}", queue[i].dest_drive, queue[i].level);
  }
  sb_puts(b, "],\"history\":[");
  int cnt = hn < HMAX ? hn : HMAX;
  for (int i = 0; i < cnt; i++) {   /* mais recente primeiro */
    hist_t *h = &hist[(hn - 1 - i) % HMAX];
    sb_printf(b, "%s{\"job\":%d,\"id\":", i ? "," : "", h->job); sb_json_str(b, h->id);
    sb_puts(b, ",\"title\":"); sb_json_str(b, h->title);
    sb_printf(b, ",\"result\":\"%s\",\"src_bytes\":%llu,\"out_bytes\":%llu,\"seconds\":%d,\"error\":", h->result,
              (unsigned long long)h->src_bytes, (unsigned long long)h->out_bytes, h->seconds);
    sb_json_str(b, h->error);
    sb_puts(b, "}");
  }
  sb_puts(b, "]}");
  pthread_mutex_unlock(&mu);
}
