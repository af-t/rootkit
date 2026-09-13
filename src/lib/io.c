#include <errno.h>
#include <time.h>
#include <unistd.h>

#include "io.h"

int write_all(int fd, const void *buf, size_t len)
{
  const char *p = buf;

  while (len > 0) {
    ssize_t n = write(fd, p, len);

    if (n < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    p += n;
    len -= (size_t)n;
  }

  return 0;
}

int read_all(int fd, void *buf, size_t len)
{
  char *p = buf;

  while (len > 0) {
    ssize_t n = read(fd, p, len);

    if (n < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    if (n == 0)
      return -1;
    p += n;
    len -= (size_t)n;
  }

  return 0;
}

int ms_until(const struct timespec *deadline)
{
  struct timespec now;
  long ms;

  clock_gettime(CLOCK_MONOTONIC, &now);
  ms = (deadline->tv_sec - now.tv_sec) * 1000 +
       (deadline->tv_nsec - now.tv_nsec) / 1000000;

  return ms > 0 ? (int)ms : 0;
}
