#ifndef ROOTLET_IO_H
#define ROOTLET_IO_H

#include <stddef.h>
#include <time.h>

int write_all(int fd, const void *buf, size_t len);
int read_all(int fd, void *buf, size_t len);

/* Milliseconds left until "deadline", never negative. */
int ms_until(const struct timespec *deadline);

#endif
