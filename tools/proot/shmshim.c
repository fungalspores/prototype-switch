// /dev/shm is absent in this PRoot sandbox; redirect POSIX shm to $SHM_DIR (/tmp/shm).
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static void shm_path(const char *name, char *out, size_t n) {
  const char *dir = getenv("SHM_DIR");
  if (!dir) dir = "/tmp/shm";
  mkdir(dir, 0777);
  while (*name == '/') name++;
  snprintf(out, n, "%s/%s", dir, name);
}

int shm_open(const char *name, int flag, mode_t mode) {
  char p[512];
  shm_path(name, p, sizeof p);
  return open(p, flag | O_CLOEXEC | O_NOFOLLOW, mode);
}

int shm_unlink(const char *name) {
  char p[512];
  shm_path(name, p, sizeof p);
  return unlink(p);
}
