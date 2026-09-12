#ifndef FWD_H
#define FWD_H

#include <stdint.h>

/* Shared fd-shape forwarding for sudo (and later connect).
   A client snapshots which of its fds 0..63 are open into a bit mask,
   sends the open fds in increasing order over a Unix socket with
   SCM_RIGHTS, and the server dups them onto the same numbers in the
   child. Pipes stay pipes, files stay files, /dev/null stays /dev/null,
   closed stays closed. */

#define FWD_MAX_FDS 64
#define FWD_STASH_BASE 256

/* Bits set in mask. */
int fwd_count(uint64_t mask);

/* Send/receive n open fds in one SCM_RIGHTS message, increasing order. */
int fwd_send(int sock, int *fds, int n);
int fwd_recv(int sock, int *out, int n);

/* Close every fd >= 0 in the array. */
void fwd_close_all(int *fds, int n);

/* Child-side: dup received fds onto their target numbers in [lo, 63].
   recv holds n fds in increasing target order for bits set in mask.
   Every other fd at/above lo is closed (except the stash), so nothing
   from the daemon leaks into the exec'd program.
   Never returns on error (calls _exit), as it runs after fork. */
void fwd_install(uint64_t mask, int *recv, int n, int lo);

/* Bit i set when fd i is open and inheritable (no CLOEXEC: such a fd
   would be gone after exec anyway, so it is skipped). */
uint64_t fwd_snapshot(void);

/* 1 when fds 0, 1 and 2 are all open ttys, else 0. */
int fwd_std_all_tty(void);

/* Build the ordered send list for mask, skipping exclude_fd and anything
   that has since closed; clears those bits in *mask. Returns the count. */
int fwd_collect(uint64_t *mask, int exclude_fd, int *out);

/* Pipe mode client: fds are shared directly, so just wait for the raw
   wait status on sock and map it to an exit code. */
int fwd_wait_status(int sock);

#endif
