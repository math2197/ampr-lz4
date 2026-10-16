/* Orquestração da conversão para AMPR (contrato em convert.h). GPL-3.0. */
#include "convert.h"
#include "ampr_index.h"
#include "ampr_pack.h"
#include "scan.h"
#include "util.h"
#include "verify.h"

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <unistd.h>

#define SPACE_MARGIN (256ull << 20)

static void set_why(char *why, int len, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void set_why(char *why, int len, const char *fmt, ...) {
  if (!why || len <= 0) return;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(why, (size_t)len, fmt, ap);
  va_end(ap);
}

static int ends_ci(const char *s, const char *suf) {
  size_t a = strlen(s), b = strlen(suf);
  return a >= b && strcasecmp(s + a - b, suf) == 0;
}

int conv_check_source(const char *src_dir, char *why, int why_len) {
  static const char needle[] = "libSceAmpr";
  char path[2048];
  struct stat st;
  if (!src_dir || stat(src_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
    set_why(why, why_len, "origem não é um diretório: %s", src_dir ? src_dir : "(null)");
    return 1;
  }
  if (path_join(path, sizeof path, src_dir, AMPR_PAK4_NAME) == 0 && lstat(path, &st) == 0) {
    set_why(why, why_len, "jogo já está em AMPR (%s existe)", AMPR_PAK4_NAME);
    return 2;
  }
  if (path_join(path, sizeof path, src_dir, "eboot.bin") != 0 || stat(path, &st) != 0) {
    set_why(why, why_len, "eboot.bin não encontrado em %s", src_dir);
    return 3;
  }
  int r = file_contains(path, needle, sizeof needle - 1);
  if (r < 0) { set_why(why, why_len, "não leu eboot.bin: %s", strerror(errno)); return 4; }
  if (r == 1) { set_why(why, why_len, "ok: eboot.bin importa libSceAmpr"); return 0; }

  char mod[2048];
  if (path_join(mod, sizeof mod, src_dir, "sce_module") == 0) {
    DIR *d = opendir(mod);
    if (d) {
      struct dirent *de;
      while ((de = readdir(d)) != NULL) {
        if (!ends_ci(de->d_name, ".prx") && !ends_ci(de->d_name, ".sprx")) continue;
        if (path_join(path, sizeof path, mod, de->d_name) != 0) continue;
        if (lstat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        if (file_contains(path, needle, sizeof needle - 1) == 1) {
          set_why(why, why_len, "ok: sce_module/%s importa libSceAmpr", de->d_name);
          closedir(d);
          return 0;
        }
      }
      closedir(d);
    }
  }
  set_why(why, why_len, "o jogo não usa libSceAmpr (eboot.bin e sce_module/*.prx|*.sprx sem a string)");
  return 5;
}

typedef struct {
  const conv_opts_t *o;
  conv_progress_t *p;
  int dst_created;
} run_t;

static int run_fail(run_t *r, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static int run_fail(run_t *r, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(r->p->error, sizeof r->p->error, fmt, ap);
  va_end(ap);
  return -1;
}

static int is_cancel(run_t *r) { return atomic_load(&r->p->cancel) != 0; }

static void set_times(const char *path, int64_t mtime) {
  struct timeval tv[2];
  tv[0].tv_sec = (time_t)mtime; tv[0].tv_usec = 0;
  tv[1].tv_sec = (time_t)mtime; tv[1].tv_usec = 0;
  (void)utimes(path, tv);
}

static int do_run(run_t *R, scan_result_t *sr, ampr_pack_result_t *pr) {
  const conv_opts_t *o = R->o;
  conv_progress_t *p = R->p;
  char path[2048], tmp[2048], err[256];
  char why[256];

  if (!o->src_dir || !o->dst_dir || !*o->dst_dir) return run_fail(R, "opções inválidas (src/dst)");
  if (o->level < 1 || o->level > 12) return run_fail(R, "nível LZ4 inválido: %d (1..12)", o->level);
  if (o->workers < 1 || o->workers > 16) return run_fail(R, "workers inválido: %d (1..16)", o->workers);
  int skip_check = 0;
#ifndef __PROSPERO__
  /* Só no host: permite testar o formato com dados reais de títulos sem libSceAmpr. */
  skip_check = getenv("AMPR_HOST_SKIP_SOURCE_CHECK") != NULL;
#endif
  if (conv_check_source(o->src_dir, why, sizeof why) != 0) {
    char apath[2048];
    struct stat ast;
    int already = path_join(apath, sizeof apath, o->src_dir, AMPR_PAK4_NAME) == 0 && lstat(apath, &ast) == 0;
    if (!skip_check || already) return run_fail(R, "%s", why);
  }

  struct stat st;
  if (lstat(o->dst_dir, &st) == 0) return run_fail(R, "destino já existe: %s", o->dst_dir);

  /* SCAN */
  atomic_store(&p->stage, CONV_STAGE_SCAN);
  if (scan_tree_ex(o->src_dir, o->fakelib_sprx, o->keep_loose, sr, err, sizeof err) != 0) return run_fail(R, "varredura: %s", err);
  atomic_store(&p->src_bytes, sr->total_bytes);
  scan_publish_estimate(sr, p);

  /* espaço livre no pai do destino */
  char parent[2048];
  snprintf(parent, sizeof parent, "%s", o->dst_dir);
  char *slash = strrchr(parent, '/');
  if (slash && slash != parent) *slash = 0;
  else if (slash) parent[1] = 0;
  else snprintf(parent, sizeof parent, ".");
  if (mkdir_p(parent, 0755) != 0) return run_fail(R, "não criou %s: %s", parent, strerror(errno));
  struct statvfs vf;
  if (statvfs(parent, &vf) != 0) return run_fail(R, "statvfs(%s): %s", parent, strerror(errno));
  uint64_t avail = (uint64_t)vf.f_bavail * (uint64_t)(vf.f_frsize ? vf.f_frsize : vf.f_bsize);
  uint64_t need = sr->total_bytes + SPACE_MARGIN;
  if (avail < need)
    return run_fail(R, "espaço insuficiente em %s: livre %llu MiB, necessário %llu MiB", parent,
                    (unsigned long long)(avail >> 20), (unsigned long long)(need >> 20));

  if (mkdir(o->dst_dir, 0755) != 0) return run_fail(R, "não criou %s: %s", o->dst_dir, strerror(errno));
  R->dst_created = 1;
  for (size_t i = 0; i < sr->n_dirs; i++) {
    if (path_join(path, sizeof path, o->dst_dir, sr->dirs[i]) != 0 || mkdir_p(path, 0755) != 0)
      return run_fail(R, "não criou diretório %s", sr->dirs[i]);
  }
  if (is_cancel(R)) return -2;

  /* PACK */
  atomic_store(&p->stage, CONV_STAGE_PACK);
  conv_notify("AMPR: compactando %s", o->src_dir);
  ampr_pack_opts_t po = {o->dst_dir, o->level, o->workers, 1, 0, p};
#ifndef __PROSPERO__
  /* Só no host: volume máximo reduzido para testar o rollover sem 16 GB de dados. */
  if (getenv("AMPR_HOST_MAX_VOLUME")) po.max_volume = strtoull(getenv("AMPR_HOST_MAX_VOLUME"), NULL, 10);
#endif
  int prc = ampr_pack_build(sr, &po, pr, err, sizeof err);
  if (prc == -2) return -2;
  if (prc != 0) return run_fail(R, "compactação: %s", err);

  /* COPY (loose) */
  atomic_store(&p->stage, CONV_STAGE_COPY);
  uint64_t loose_total = 0;
  for (size_t i = 0; i < sr->n_files; i++)
    if (sr->files[i].action == SCAN_ACT_LOOSE) loose_total += sr->files[i].size;
  atomic_store(&p->bytes_total, loose_total);
  atomic_store(&p->bytes_done, 0);
  for (size_t i = 0; i < sr->n_files; i++) {
    scan_file_t *f = &sr->files[i];
    if (f->action != SCAN_ACT_LOOSE) continue;
    if (path_join(path, sizeof path, o->dst_dir, f->rel) != 0) return run_fail(R, "caminho longo: %s", f->rel);
    int c = copy_file_atomic(f->src, path, f->exec ? 0755 : 0644, &p->bytes_done, &p->cancel);
    if (c == -2) return -2;
    if (c != 0) return run_fail(R, "cópia de %s: %s", f->rel, strerror(errno));
    set_times(path, f->mtime);
  }

  /* AMPRIDX3 */
  uint8_t *idx = NULL;
  size_t idx_len = 0;
  if (ampr_idx3_build(sr, &idx, &idx_len, err, sizeof err) != 0) return run_fail(R, "%s", err);
  if (path_join(path, sizeof path, o->dst_dir, AMPR_IDX3_NAME) != 0 || write_file_atomic(path, idx, idx_len, 0644) != 0) {
    free(idx);
    return run_fail(R, "gravando %s: %s", AMPR_IDX3_NAME, strerror(errno));
  }
  free(idx);
  if (is_cancel(R)) return -2;

  /* Publicação: volumes -> .crc -> .runtime -> (verificação) -> manifesto por último */
  for (uint32_t i = 0; i < pr->n_packs; i++) {
    char name[64];
    snprintf(name, sizeof name, AMPR_PACK_NAME_FMT, i);
    if (path_join(path, sizeof path, o->dst_dir, name) != 0) return run_fail(R, "caminho longo");
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    if (rename(tmp, path) != 0) return run_fail(R, "publicando %s: %s", name, strerror(errno));
  }
  fsync_dir(o->dst_dir);
  char crc_path[2048], man_path[2048];
  if (path_join(crc_path, sizeof crc_path, o->dst_dir, AMPR_CRC1_NAME) != 0 ||
      write_file_atomic(crc_path, pr->crc, pr->crc_len, 0644) != 0)
    return run_fail(R, "gravando %s: %s", AMPR_CRC1_NAME, strerror(errno));
  if (path_join(path, sizeof path, o->dst_dir, AMPR_CFG1_NAME) != 0 ||
      write_file_atomic(path, pr->runtime, AMPR_CFG1_SIZE, 0644) != 0)
    return run_fail(R, "gravando %s: %s", AMPR_CFG1_NAME, strerror(errno));

  /* VERIFY: manifesto gravado como .tmp, verificado a partir do disco e só então publicado. */
  atomic_store(&p->stage, CONV_STAGE_VERIFY);
  if (path_join(man_path, sizeof man_path, o->dst_dir, AMPR_PAK4_NAME) != 0) return run_fail(R, "caminho longo");
  snprintf(tmp, sizeof tmp, "%s.tmp", man_path);
  {
    FILE *fp = fopen(tmp, "wb");
    if (!fp || fwrite(pr->manifest, 1, pr->manifest_len, fp) != pr->manifest_len || fflush(fp) != 0 ||
        fsync(fileno(fp)) != 0) {
      int e = errno;
      if (fp) fclose(fp);
      return run_fail(R, "gravando %s.tmp: %s", AMPR_PAK4_NAME, strerror(e));
    }
    if (fclose(fp) != 0) return run_fail(R, "gravando %s.tmp: %s", AMPR_PAK4_NAME, strerror(errno));
  }
  int vrc = ampr_verify(o->dst_dir, tmp, crc_path, p, err, sizeof err);
#ifndef __PROSPERO__
  /* Só no host: simula falha da verificação (testa que a plataforma não apaga a origem). */
  if (vrc == 0 && getenv("AMPR_HOST_FAIL_VERIFY")) { vrc = -1; snprintf(err, sizeof err, "verificação: falha simulada"); }
#endif
  if (vrc == -2) return -2;
  if (vrc != 0) return run_fail(R, "%s", err);
  if (rename(tmp, man_path) != 0) return run_fail(R, "publicando %s: %s", AMPR_PAK4_NAME, strerror(errno));
  fsync_dir(o->dst_dir);

  uint64_t out = pr->volume_bytes + pr->crc_len + AMPR_CFG1_SIZE + pr->manifest_len + idx_len;
  for (size_t i = 0; i < sr->n_files; i++)
    if (sr->files[i].action == SCAN_ACT_LOOSE) out += sr->files[i].size;
  atomic_store(&p->out_bytes, out);
  return 0;
}

int conv_run(const conv_opts_t *o, conv_progress_t *p) {
  atomic_store(&p->stage, CONV_STAGE_SCAN);
  atomic_store(&p->bytes_total, 0);
  atomic_store(&p->bytes_done, 0);
  atomic_store(&p->src_bytes, 0);
  atomic_store(&p->out_bytes, 0);
  p->error[0] = 0;
  run_t R = {o, p, 0};
  scan_result_t sr;
  memset(&sr, 0, sizeof sr);
  ampr_pack_result_t pr;
  memset(&pr, 0, sizeof pr);
  uint64_t t0 = now_ms();

  int rc = do_run(&R, &sr, &pr);
  if (rc == 0 && is_cancel(&R)) rc = -2;
  if (rc != 0 && R.dst_created) rm_rf(o->dst_dir);
  if (rc == -2) {
    snprintf(p->error, sizeof p->error, "cancelado");
    atomic_store(&p->stage, CONV_STAGE_CANCELLED);
    conv_notify("AMPR: conversão cancelada");
  } else if (rc != 0) {
    atomic_store(&p->stage, CONV_STAGE_ERROR);
    conv_notify("AMPR: erro: %s", p->error);
  } else {
    uint64_t src = atomic_load(&p->src_bytes), out = atomic_load(&p->out_bytes);
    atomic_store(&p->stage, CONV_STAGE_DONE);
    conv_notify("AMPR: concluído %llu MiB -> %llu MiB (%.1f%%) em %llus", (unsigned long long)(src >> 20),
                (unsigned long long)(out >> 20), src ? 100.0 * (double)out / (double)src : 0.0,
                (unsigned long long)((now_ms() - t0) / 1000));
  }
  ampr_pack_result_free(&pr);
  scan_free(&sr);
  return rc == 0 ? 0 : -1;
}
