#include "platform.h"
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

plat_cfg_t g_cfg = {8095, 4, 6, 1};

const char *plat_prefix(void) {
#ifdef AMPR_PS5
  return "";
#else
  const char *p = getenv("AMPR_ROOT_PREFIX");
  return p ? p : "";
#endif
}

void plat_path(char *out, size_t n, const char *abs_path) {
  snprintf(out, n, "%s%s", plat_prefix(), abs_path);
}

const char *plat_drive_root(const char *d) {
  if (!d) return NULL;
  if (!strcmp(d, "data")) return "/data";
  if (!strcmp(d, "ext0")) return "/mnt/ext0";
  if (!strcmp(d, "ext1")) return "/mnt/ext1";
  return NULL;
}

static int under(const char *path, const char *root) {
  size_t l = strlen(root);
  return !strncmp(path, root, l) && path[l] == '/';
}

const char *plat_drive_of(const char *path) {
  if (under(path, "/data")) return "data";
  if (under(path, "/mnt/ext0")) return "ext0";
  if (under(path, "/mnt/ext1")) return "ext1";
  return NULL;
}

void plat_local_ip(char *out, size_t n) {
  snprintf(out, n, "127.0.0.1");
  /* truque UDP: connect não envia pacotes, só escolhe a interface de saída */
  int s = socket(AF_INET, SOCK_DGRAM, 0);
  if (s >= 0) {
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(53);
    inet_pton(AF_INET, "192.0.2.1", &a.sin_addr);
    if (connect(s, (struct sockaddr *)&a, sizeof a) == 0) {
      struct sockaddr_in l;
      socklen_t ll = sizeof l;
      if (getsockname(s, (struct sockaddr *)&l, &ll) == 0 && l.sin_addr.s_addr != 0) {
        inet_ntop(AF_INET, &l.sin_addr, out, (socklen_t)n);
        close(s);
        return;
      }
    }
    close(s);
  }
  struct ifaddrs *ifa, *it;
  if (getifaddrs(&ifa) == 0) {
    for (it = ifa; it; it = it->ifa_next) {
      if (!it->ifa_addr || it->ifa_addr->sa_family != AF_INET) continue;
      struct sockaddr_in *sin = (struct sockaddr_in *)it->ifa_addr;
      if ((ntohl(sin->sin_addr.s_addr) >> 24) == 127) continue;
      inet_ntop(AF_INET, &sin->sin_addr, out, (socklen_t)n);
      break;
    }
    freeifaddrs(ifa);
  }
}

static char *trim(char *s) {
  while (*s == ' ' || *s == '\t') s++;
  char *e = s + strlen(s);
  while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
  return s;
}

void plat_load_config(void) {
  char path[512], line[256];
  plat_path(path, sizeof path, "/data/ampr-lz4/config.ini");
  FILE *f = fopen(path, "r");
  if (!f) return;
  while (fgets(line, sizeof line, f)) {
    char *s = trim(line);
    if (*s == '#' || *s == ';' || !*s) continue;
    char *eq = strchr(s, '=');
    if (!eq) continue;
    *eq = 0;
    char *k = trim(s), *v = trim(eq + 1);
    int x = atoi(v);
    if (!strcmp(k, "port") && x > 0 && x < 65536) g_cfg.port = x;
    else if (!strcmp(k, "workers") && x >= 1 && x <= 16) g_cfg.workers = x;
    else if (!strcmp(k, "shortcut")) (void)x; /* obsoleta: no-op (atalho = build/ampr-shortcut.elf) */
    else if (!strcmp(k, "level") && x >= 1 && x <= 12) g_cfg.level = x;
  }
  fclose(f);
}
