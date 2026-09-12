#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#include "fwd.h"
#include "io.h"

void fwd_close_all(int *fds, int n);

int fwd_count(uint64_t mask)
{
  return __builtin_popcountll(mask);
}

int fwd_send(int sock, int *fds, int n)
{
  struct msghdr msg = {0};
  struct iovec io = { .iov_base = "FDS", .iov_len = 3 };
  char *buf;
  struct cmsghdr *cmsg;

  if (n <= 0 || n > FWD_MAX_FDS)
    return -1;

  buf = malloc(CMSG_SPACE(sizeof(int) * (size_t)n));
  if (!buf)
    return -1;
  memset(buf, 0, CMSG_SPACE(sizeof(int) * (size_t)n));

  msg.msg_iov = &io;
  msg.msg_iovlen = 1;
  msg.msg_control = buf;
  msg.msg_controllen = CMSG_SPACE(sizeof(int) * (size_t)n);

  cmsg = CMSG_FIRSTHDR(&msg);
  cmsg->cmsg_level = SOL_SOCKET;
  cmsg->cmsg_type = SCM_RIGHTS;
  cmsg->cmsg_len = CMSG_LEN(sizeof(int) * (size_t)n);
  memcpy(CMSG_DATA(cmsg), fds, sizeof(int) * (size_t)n);

  int r = sendmsg(sock, &msg, 0);
  free(buf);
  return r < 0 ? -1 : 0;
}

int fwd_recv(int sock, int *out, int n)
{
  struct msghdr msg = {0};
  char dummy[3];
  struct iovec io = { .iov_base = dummy, .iov_len = sizeof(dummy) };
  char *buf;
  struct cmsghdr *cmsg;
  size_t clen;

  if (n <= 0 || n > FWD_MAX_FDS)
    return -1;

  buf = malloc(CMSG_SPACE(sizeof(int) * (size_t)n));
  if (!buf)
    return -1;
  memset(buf, 0, CMSG_SPACE(sizeof(int) * (size_t)n));

  msg.msg_iov = &io;
  msg.msg_iovlen = 1;
  msg.msg_control = buf;
  msg.msg_controllen = CMSG_SPACE(sizeof(int) * (size_t)n);

  ssize_t r = recvmsg(sock, &msg, 0);
  if (r <= 0) {
    free(buf);
    return -1;
  }

  cmsg = CMSG_FIRSTHDR(&msg);
  clen = 0;
  if (cmsg && cmsg->cmsg_level == SOL_SOCKET &&
      cmsg->cmsg_type == SCM_RIGHTS)
    clen = cmsg->cmsg_len >= CMSG_LEN(0) ?
      cmsg->cmsg_len - CMSG_LEN(0) : 0;

  if (clen < sizeof(int) * (size_t)n) {
    /* Partial receipt: close what did arrive to avoid leaks. */
    if (cmsg) {
      size_t got = clen / sizeof(int);
      int *got_fds = (int *)CMSG_DATA(cmsg);
      for (size_t i = 0; i < got; i++)
        if (got_fds[i] >= 0)
          close(got_fds[i]);
    }
    free(buf);
    return -1;
  }

  memcpy(out, CMSG_DATA(cmsg), sizeof(int) * (size_t)n);
  free(buf);

  for (int i = 0; i < n; i++) {
    if (out[i] < 0) {
      fwd_close_all(out, n);
      return -1;
    }
  }
  return 0;
}

void fwd_close_all(int *fds, int n)
{
  for (int i = 0; i < n; i++)
    if (fds[i] >= 0)
      close(fds[i]);
}

static void clear_cloexec(int fd)
{
  int fl = fcntl(fd, F_GETFD);
  if (fl >= 0)
    fcntl(fd, F_SETFD, fl & ~FD_CLOEXEC);
}

static int keep_contains(const int *keep, int nkeep, int fd)
{
  for (int i = 0; i < nkeep; i++)
    if (keep[i] == fd)
      return 1;
  return 0;
}

/* Close every fd >= lo except those in keep. Enumerates /proc/self/fd so
   only open fds are touched (a bounded sweep is the fallback). */
void fwd_close_from(int lo, const int *keep, int nkeep)
{
  DIR *d = opendir("/proc/self/fd");

  if (d) {
    int dfd = dirfd(d);
    struct dirent *e;

    while ((e = readdir(d)) != NULL) {
      char *end;
      long fd = strtol(e->d_name, &end, 10);

      if (end == e->d_name || *end != '\0')
        continue;
      if (fd < lo || fd > INT_MAX)
        continue;
      if ((int)fd == dfd)
        continue;
      if (keep_contains(keep, nkeep, (int)fd))
        continue;
      close((int)fd);
    }
    closedir(d);
    return;
  }

  long maxfd = sysconf(_SC_OPEN_MAX);
  if (maxfd < 0)
    maxfd = 1024;
  if (maxfd > 65536)
    maxfd = 65536;
  for (long fd = lo; fd < maxfd; fd++) {
    if (keep_contains(keep, nkeep, (int)fd))
      continue;
    close((int)fd);
  }
}

void fwd_install(uint64_t mask, int *recv, int n, int lo)
{
  int stash[FWD_MAX_FDS];
  int targets[FWD_MAX_FDS];
  int ns = 0;
  int idx = 0;

  for (int t = lo; t < FWD_MAX_FDS; t++) {
    if (!(mask & (1ULL << t)))
      continue;
    if (idx >= n)
      _exit(1);
    targets[ns] = t;
    stash[ns] = -1;
    ns++;
    idx++;
  }
  if (ns != n)
    _exit(1);

  /* Stash above FWD_STASH_BASE so closing [lo, 63] below is safe. */
  idx = 0;
  for (int t = lo; t < FWD_MAX_FDS && idx < n; t++) {
    if (!(mask & (1ULL << t)))
      continue;
    int s = fcntl(recv[idx], F_DUPFD, FWD_STASH_BASE);
    if (s < 0)
      _exit(1);
    close(recv[idx]);
    recv[idx] = -1;
    stash[idx] = s;
    idx++;
  }

  /* Everything at/above lo goes except the stash; stash sits above
     FWD_STASH_BASE so it always survives this. */
  fwd_close_from(lo, stash, n);

  for (int i = 0; i < n; i++) {
    if (dup2(stash[i], targets[i]) < 0)
      _exit(1);
    clear_cloexec(targets[i]);
    close(stash[i]);
  }
}

/* Open and inheritable across exec (no CLOEXEC): only these are worth
   forwarding, matching what an exec without forwarding would keep. */
static int inheritable_open(int fd)
{
  int fl = fcntl(fd, F_GETFD);
  return fl >= 0 && !(fl & FD_CLOEXEC);
}

uint64_t fwd_snapshot(void)
{
  uint64_t mask = 0;

  for (int fd = 0; fd < FWD_MAX_FDS; fd++) {
    if (inheritable_open(fd))
      mask |= (1ULL << fd);
  }

  return mask;
}

int fwd_std_all_tty(void)
{
  return inheritable_open(0) && inheritable_open(1) &&
         inheritable_open(2) && isatty(0) && isatty(1) && isatty(2);
}

int fwd_collect(uint64_t *mask, int exclude_fd, int *out)
{
  int n = 0;

  for (int fd = 0; fd < FWD_MAX_FDS; fd++) {
    if (!(*mask & (1ULL << fd)))
      continue;
    if (fd == exclude_fd || !inheritable_open(fd)) {
      *mask &= ~(1ULL << fd);
      continue;
    }
    out[n++] = fd;
  }

  return n;
}

int fwd_wait_status(int sock)
{
  int raw_status = -1;

  /* No receive timeout: piped commands may run long. The daemon's
     hangup detection kills the child if we go away. */
  {
    struct timeval tv = { 0, 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  }

  if (read_all(sock, &raw_status, sizeof(raw_status)) != 0)
    raw_status = -1;
  close(sock);

  if (raw_status != -1) {
    if (WIFSIGNALED(raw_status))
      return 128 + WTERMSIG(raw_status);
    if (WIFEXITED(raw_status))
      return WEXITSTATUS(raw_status);
  }
  return 1;
}
