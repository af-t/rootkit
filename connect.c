#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <time.h>
#include <termios.h>
#include <pwd.h>
#include <pty.h>
#include <poll.h>
#include <signal.h>
#include <getopt.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include <pthread.h>

#include "fwd.h"
#include "io.h"
#include "tty.h"

#define DEFAULT_HOST    "127.0.0.1"
#define DEFAULT_PORT    7722
#define DEFAULT_SHELL   "/bin/bash"
#define DRAIN_MS        200

struct connect_hello {
  unsigned short rows;
  unsigned short cols;
  int pipe_mode;   /* 0 = pty (0,1,2 are ttys), 1 = relay pipes, no pty */
};

extern char **environ;

static void daemonize(void)
{
  pid_t pid = fork();
  if (pid < 0)  exit(EXIT_FAILURE);
  if (pid > 0)  exit(EXIT_SUCCESS);
  if (setsid() < 0) exit(EXIT_FAILURE);
  signal(SIGHUP, SIG_IGN);

  int devnull = open("/dev/null", O_RDWR);
  if (devnull >= 0) {
    dup2(devnull, STDIN_FILENO);
    dup2(devnull, STDOUT_FILENO);
    dup2(devnull, STDERR_FILENO);
    if (devnull > STDERR_FILENO)
      close(devnull);
  }
}

/* Returns 1 if "name=..." should be forwarded from the client (LANG, LC_*). */
static int is_locale_var(const char *entry)
{
  if (strncmp(entry, "LANG=", 5) == 0)
    return 1;
  if (strncmp(entry, "LC_", 3) == 0)
    return 1;
  return 0;
}

/* Collect LANG + LC_* entries from environ into a freshly malloc'd blob. */
static char *build_lc_blob(uint32_t *out_size)
{
  size_t total = 0;
  char **e;

  for (e = environ; *e; e++)
    if (is_locale_var(*e))
      total += strlen(*e) + 1;

  *out_size = (uint32_t)total;
  if (total == 0)
    return NULL;

  char *buf = malloc(total);
  if (!buf) return NULL;

  char *p = buf;
  for (e = environ; *e; e++) {
    if (is_locale_var(*e)) {
      size_t len = strlen(*e) + 1;
      memcpy(p, *e, len);
      p += len;
    }
  }
  return buf;
}

/* Apply the client's locale blob into the current process environment. */
static void apply_lc_blob(const char *blob, uint32_t size)
{
  const char *p   = blob;
  const char *end = blob + size;

  while (p < end) {
    size_t remaining = (size_t)(end - p);
    size_t klen = strnlen(p, remaining);

    if (klen == remaining)   /* no NUL terminator: stop */
      break;

    if (is_locale_var(p)) {
      const char *eq = memchr(p, '=', klen);
      if (eq) {
        size_t nlen = (size_t)(eq - p);
        char name[64];
        if (nlen < sizeof(name)) {
          memcpy(name, p, nlen);
          name[nlen] = '\0';
          setenv(name, eq + 1, 1);
        }
      }
    }
    p += klen + 1;
  }
}

/* Shared exec tail: locale overrides, then a login shell. Never returns. */
static void exec_login_shell(const char *lc_blob, uint32_t lc_size)
{
  struct passwd *pw;
  const char *shell;
  const char *base;
  static char login_argv0[64];
  char *exec_args[2];

  /* Apply the client's locale overrides (LANG, LC_*). */
  if (lc_blob && lc_size > 0)
    apply_lc_blob(lc_blob, lc_size);

  pw = getpwuid(getuid());
  if (pw) {
    setenv("USER",    pw->pw_name, 1);
    setenv("LOGNAME", pw->pw_name, 1);
    setenv("HOME",    pw->pw_dir,  1);
    if (chdir(pw->pw_dir) != 0 && chdir("/") != 0) {
      /* stay wherever we are */
    }
    shell = pw->pw_shell;
    if (!shell || shell[0] == '\0')
      shell = DEFAULT_SHELL;
  } else {
    shell = DEFAULT_SHELL;
  }

  /* login shell: prefix argv[0] with '-' */
  base = strrchr(shell, '/');
  base = base ? base + 1 : shell;
  snprintf(login_argv0, sizeof(login_argv0), "-%s", base);

  exec_args[0] = login_argv0;
  exec_args[1] = NULL;

  execvp(shell, exec_args);

  fprintf(stderr, "connect: %s: %s\n", shell, strerror(errno));
  _exit(126);
}

static void run_child(const char *lc_blob, uint32_t lc_size, int slave_fd)
{
  setsid();
  ioctl(slave_fd, TIOCSCTTY, 0);
  dup2(slave_fd, STDIN_FILENO);
  dup2(slave_fd, STDOUT_FILENO);
  dup2(slave_fd, STDERR_FILENO);
  if (slave_fd > STDERR_FILENO)
    close(slave_fd);

  fwd_close_from(3, NULL, 0);
  exec_login_shell(lc_blob, lc_size);
}

/* Pipe mode: same shell, but stdin/stdout/stderr are relay pipes with no
   controlling terminal (TCP cannot pass fds, so streams are relayed). */
static void run_child_pipes(const char *lc_blob, uint32_t lc_size,
                            int stdin_r, int stdout_w, int stderr_w)
{
  setsid();
  dup2(stdin_r, STDIN_FILENO);
  dup2(stdout_w, STDOUT_FILENO);
  dup2(stderr_w, STDERR_FILENO);
  if (stdin_r > STDERR_FILENO)
    close(stdin_r);
  if (stdout_w > STDERR_FILENO)
    close(stdout_w);
  if (stderr_w > STDERR_FILENO)
    close(stderr_w);

  fwd_close_from(3, NULL, 0);
  exec_login_shell(lc_blob, lc_size);
}

#define PKT_DATA  1
#define PKT_WINCH 2
#define PKT_EXIT  3
#define PKT_STDERR 4   /* pipe mode: server->client stderr (DATA = stdout) */

struct pkt_hdr {
  uint8_t  type;
  uint32_t len;
} __attribute__((packed));

struct pkt_winch {
  uint16_t rows;
  uint16_t cols;
} __attribute__((packed));

static int send_pkt(int fd, uint8_t type, const void *payload, uint32_t len)
{
  struct pkt_hdr hdr;
  hdr.type = type;
  hdr.len  = htonl(len);
  if (write_all(fd, &hdr, sizeof(hdr)) != 0)
    return -1;
  if (len > 0 && payload) {
    if (write_all(fd, payload, len) != 0)
      return -1;
  }
  return 0;
}

static int recv_pkt(int fd, uint8_t *type, void *buf, uint32_t max_len, uint32_t *out_len)
{
  struct pkt_hdr hdr;
  if (read_all(fd, &hdr, sizeof(hdr)) != 0)
    return -1;
  *type = hdr.type;
  uint32_t len = ntohl(hdr.len);
  if (len > max_len)
    return -1;
  if (len > 0 && buf) {
    if (read_all(fd, buf, len) != 0)
      return -1;
  }
  *out_len = len;
  return 0;
}

struct session_args {
  int      client_fd;
  int      master_fd;
  pid_t    shell_pid;
};

static void *session_thread(void *arg)
{
  struct session_args *sa = arg;
  int      client_fd = sa->client_fd;
  int      master_fd = sa->master_fd;
  pid_t    shell_pid = sa->shell_pid;
  char     buf[4096];
  struct   pollfd pf[2];
  int      status = -1;
  int      reaped = 0;

  free(sa);

  /* Remove the handshake timeout set before the thread was spawned. */
  {
    struct timeval zero = { 0, 0 };
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &zero, sizeof(zero));
  }

  /* Relay loop: bridge master_fd <-> client_fd using packets. */
  for (;;) {
    pid_t wp = waitpid(shell_pid, &status, WNOHANG);
    if (wp > 0) {
      reaped = 1;
      break;
    }

    pf[0].fd      = master_fd;
    pf[0].events  = POLLIN;
    pf[0].revents = 0;
    pf[1].fd      = client_fd;
    pf[1].events  = POLLIN;
    pf[1].revents = 0;

    if (poll(pf, 2, -1) < 0) {
      if (errno == EINTR) continue;
      break;
    }

    /* Master pty output -> send as PKT_DATA to client */
    if (pf[0].revents & POLLIN) {
      ssize_t n = read(master_fd, buf, sizeof(buf));
      if (n <= 0) break;
      if (send_pkt(client_fd, PKT_DATA, buf, (uint32_t)n) != 0) break;
    }

    /* Packet from client -> process data or window resize */
    if (pf[1].revents & POLLIN) {
      uint8_t type;
      uint32_t len;
      if (recv_pkt(client_fd, &type, buf, sizeof(buf), &len) != 0) break;

      if (type == PKT_DATA && len > 0) {
        if (write_all(master_fd, buf, len) != 0) break;
      } else if (type == PKT_WINCH && len == sizeof(struct pkt_winch)) {
        struct pkt_winch *w = (struct pkt_winch *)buf;
        struct winsize ws;
        memset(&ws, 0, sizeof(ws));
        ws.ws_row = ntohs(w->rows);
        ws.ws_col = ntohs(w->cols);
        ioctl(master_fd, TIOCSWINSZ, &ws);
      }
    }

    if (pf[0].revents & (POLLHUP | POLLERR)) break;
    if (pf[1].revents & (POLLHUP | POLLERR)) break;
  }

  if (!reaped) {
    kill(shell_pid, SIGHUP);
    if (waitpid(shell_pid, &status, 0) < 0)
      status = -1;
  }

  int32_t net_status = htonl((int32_t)status);
  send_pkt(client_fd, PKT_EXIT, &net_status, sizeof(net_status));

  close(master_fd);
  close(client_fd);
  return NULL;
}

struct pipe_args {
  int      client_fd;
  int      stdin_w;
  int      stdout_r;
  int      stderr_r;
  pid_t    child_pid;
};

/* Upper bound for one poll() sleep with no fd events. A child that exits
   after its pipes already reached EOF produces no further events (both
   pipes poll as -1 while the socket may stay idle), so without a bound the
   waitpid() at the top of the loop would never run again and the exit
   status would never be sent. */
#define REAP_POLL_MS 200

/* Pipe mode relay: child stdout/stderr arrive on separate pipes and go
   out as PKT_DATA/PKT_STDERR; client PKT_DATA feeds child stdin, and an
   empty PKT_DATA means stdin EOF (the socket itself stays open, so a
   later disconnect is still told apart from EOF and kills the child). */
static void *pipe_session_thread(void *arg)
{
  struct pipe_args *pa = arg;
  int      client_fd = pa->client_fd;
  int      stdin_w   = pa->stdin_w;
  int      stdout_r  = pa->stdout_r;
  int      stderr_r  = pa->stderr_r;
  pid_t    child_pid = pa->child_pid;
  char     buf[4096];
  struct   pollfd pf[3];
  struct   timespec deadline;
  int      status = -1;
  int      reaped = 0;
  int      in_open = 1, out_eof = 0, err_eof = 0, ending = 0;

  free(pa);

  /* Remove the handshake timeout set before the thread was spawned. */
  {
    struct timeval zero = { 0, 0 };
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &zero, sizeof(zero));
  }

  for (;;) {
    pid_t wp = waitpid(child_pid, &status, WNOHANG);
    int timeout = REAP_POLL_MS;
    int r;

    if (wp > 0)
      reaped = 1;
    if (reaped && out_eof && err_eof)
      break;

    /* Child gone: take what the pipes still hold, then leave. */
    if (reaped && !ending) {
      clock_gettime(CLOCK_MONOTONIC, &deadline);
      deadline.tv_nsec += DRAIN_MS * 1000000L;
      if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_nsec -= 1000000000L;
        deadline.tv_sec++;
      }
      ending = 1;
    }
    if (ending) {
      int left = ms_until(&deadline);
      if (left < timeout)
        timeout = left;
    }

    pf[0].fd      = out_eof ? -1 : stdout_r;
    pf[0].events  = POLLIN;
    pf[0].revents = 0;
    pf[1].fd      = err_eof ? -1 : stderr_r;
    pf[1].events  = POLLIN;
    pf[1].revents = 0;
    pf[2].fd      = client_fd;
    pf[2].events  = POLLIN;
    pf[2].revents = 0;

    r = poll(pf, 3, timeout);
    if (r < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (r == 0) {
      /* No events in time: leave only when the drain deadline elapsed,
         otherwise loop back and re-check the child. */
      if (ending && ms_until(&deadline) == 0)
        break;
      continue;
    }

    if (pf[0].revents & POLLIN) {
      ssize_t n = read(stdout_r, buf, sizeof(buf));
      if (n < 0 && errno == EINTR)
        continue;
      if (n <= 0)
        out_eof = 1;
      else if (send_pkt(client_fd, PKT_DATA, buf, (uint32_t)n) != 0)
        break;
    } else if (pf[0].revents & (POLLHUP | POLLERR)) {
      out_eof = 1;
    }

    if (pf[1].revents & POLLIN) {
      ssize_t n = read(stderr_r, buf, sizeof(buf));
      if (n < 0 && errno == EINTR)
        continue;
      if (n <= 0)
        err_eof = 1;
      else if (send_pkt(client_fd, PKT_STDERR, buf, (uint32_t)n) != 0)
        break;
    } else if (pf[1].revents & (POLLHUP | POLLERR)) {
      err_eof = 1;
    }

    /* Any readability here is either an input packet or a dead client:
       packets keep flowing, anything else ends the session. */
    if (pf[2].revents & (POLLIN | POLLHUP | POLLERR)) {
      uint8_t type;
      uint32_t len;
      if (recv_pkt(client_fd, &type, buf, sizeof(buf), &len) != 0)
        break;
      if (type == PKT_DATA && in_open) {
        if (len == 0) {
          in_open = 0;
          close(stdin_w);
        } else if (write_all(stdin_w, buf, len) != 0) {
          in_open = 0;
          close(stdin_w);
        }
      }
      /* PKT_WINCH and anything else: no pty here, ignore. */
    }
  }

  if (!reaped) {
    kill(child_pid, SIGHUP);
    if (waitpid(child_pid, &status, 0) < 0)
      status = -1;
  }

  {
    int32_t net_status = htonl((int32_t)status);
    send_pkt(client_fd, PKT_EXIT, &net_status, sizeof(net_status));
  }

  if (in_open)
    close(stdin_w);
  close(stdout_r);
  close(stderr_r);
  close(client_fd);
  return NULL;
}

static int make_pipe(int p[2])
{
  if (pipe2(p, O_CLOEXEC) == 0)
    return 0;
  if (pipe(p) != 0)
    return -1;
  fcntl(p[0], F_SETFD, FD_CLOEXEC);
  fcntl(p[1], F_SETFD, FD_CLOEXEC);
  return 0;
}

/* Pipe mode accept path: no pty, child stdio are relay pipes owned by
   a detached thread. Takes over lc_blob (frees it) and client_fd. */
static void handle_pipe_connection(int server_fd, int client_fd,
                                   char *lc_blob, uint32_t lc_size)
{
  int in_p[2] = { -1, -1 }, out_p[2] = { -1, -1 }, err_p[2] = { -1, -1 };
  struct pipe_args *pa;
  pthread_t tid;
  pthread_attr_t attr;
  uint8_t ok;
  pid_t pid;

  if (make_pipe(in_p) != 0 || make_pipe(out_p) != 0 ||
      make_pipe(err_p) != 0) {
    ok = 0;
    write_all(client_fd, &ok, sizeof(ok));
    goto fail;
  }

  pid = fork();
  if (pid == 0) {
    close(server_fd);
    close(client_fd);
    close(in_p[1]);
    close(out_p[0]);
    close(err_p[0]);
    run_child_pipes(lc_blob, lc_size, in_p[0], out_p[1], err_p[1]);
  }

  close(in_p[0]);
  in_p[0] = -1;
  close(out_p[1]);
  out_p[1] = -1;
  close(err_p[1]);
  err_p[1] = -1;
  free(lc_blob);
  lc_blob = NULL;

  if (pid < 0) {
    ok = 0;
    write_all(client_fd, &ok, sizeof(ok));
    goto fail;
  }

  /* Tell the client the shell is ready. */
  ok = 1;
  if (write_all(client_fd, &ok, sizeof(ok)) != 0) {
    kill(pid, SIGKILL);
    goto fail;
  }

  pa = malloc(sizeof(*pa));
  if (!pa) {
    kill(pid, SIGKILL);
    close(client_fd);
    close(in_p[1]);
    close(out_p[0]);
    close(err_p[0]);
    free(lc_blob);
    return;
  }
  pa->client_fd = client_fd;
  pa->stdin_w   = in_p[1];
  pa->stdout_r  = out_p[0];
  pa->stderr_r  = err_p[0];
  pa->child_pid = pid;

  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  if (pthread_create(&tid, &attr, pipe_session_thread, pa) != 0) {
    free(pa);
    kill(pid, SIGKILL);
    waitpid(pid, NULL, WNOHANG);
    close(client_fd);
    close(in_p[1]);
    close(out_p[0]);
    close(err_p[0]);
    free(lc_blob);
    return;
  }
  pthread_attr_destroy(&attr);
  return;

fail:
  close(client_fd);
  free(lc_blob);
  if (in_p[0] >= 0)
    close(in_p[0]);
  if (in_p[1] >= 0)
    close(in_p[1]);
  if (out_p[0] >= 0)
    close(out_p[0]);
  if (out_p[1] >= 0)
    close(out_p[1]);
  if (err_p[0] >= 0)
    close(err_p[0]);
  if (err_p[1] >= 0)
    close(err_p[1]);
}

static void handle_connection(int server_fd)
{
  struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
  struct connect_hello hello;
  uint32_t lc_size = 0;
  char *lc_blob = NULL;
  int master_fd, slave_fd;
  struct winsize ws;
  uint8_t ok;
  pid_t shell_pid;
  struct session_args *sa;
  pthread_t tid;
  pthread_attr_t attr;

  int client_fd = accept(server_fd, NULL, NULL);
  if (client_fd < 0)
    return;

  /* Enable TCP keepalive so we notice dead clients. */
  {
    int val = 1;
    setsockopt(client_fd, SOL_SOCKET, SO_KEEPALIVE, &val, sizeof(val));
#ifdef TCP_KEEPIDLE
    val = 60;
    setsockopt(client_fd, IPPROTO_TCP, TCP_KEEPIDLE, &val, sizeof(val));
#endif
  }

  /* A stalled client must not block the accept loop. */
  setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  if (read_all(client_fd, &hello, sizeof(hello)) != 0 ||
      read_all(client_fd, &lc_size, sizeof(lc_size)) != 0 ||
      lc_size > (1u << 16) ||
      (hello.pipe_mode != 0 && hello.pipe_mode != 1)) {
    close(client_fd);
    return;
  }

  if (lc_size > 0) {
    lc_blob = malloc(lc_size);
    if (!lc_blob || read_all(client_fd, lc_blob, lc_size) != 0) {
      free(lc_blob);
      close(client_fd);
      return;
    }
  }

  if (hello.pipe_mode)
    return handle_pipe_connection(server_fd, client_fd, lc_blob, lc_size);

  ws.ws_row    = hello.rows ? hello.rows : 24;
  ws.ws_col    = hello.cols ? hello.cols : 80;
  ws.ws_xpixel = 0;
  ws.ws_ypixel = 0;

  if (openpty(&master_fd, &slave_fd, NULL, NULL, &ws) != 0) {
    free(lc_blob);
    close(client_fd);
    return;
  }

  shell_pid = fork();
  if (shell_pid == 0) {
    close(client_fd);
    close(master_fd);
    run_child(lc_blob, lc_size, slave_fd);
    /* run_child never returns */
  }

  close(slave_fd);
  free(lc_blob);

  if (shell_pid < 0) {
    ok = 0;
    write_all(client_fd, &ok, sizeof(ok));
    close(master_fd);
    close(client_fd);
    return;
  }

  /* Tell the client the shell is ready. */
  ok = 1;
  if (write_all(client_fd, &ok, sizeof(ok)) != 0) {
    kill(shell_pid, SIGKILL);
    close(master_fd);
    close(client_fd);
    return;
  }

  /* Hand off to a detached relay thread. */
  sa = malloc(sizeof(*sa));
  if (!sa) {
    kill(shell_pid, SIGKILL);
    close(master_fd);
    close(client_fd);
    return;
  }
  sa->client_fd = client_fd;
  sa->master_fd = master_fd;
  sa->shell_pid = shell_pid;

  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  if (pthread_create(&tid, &attr, session_thread, sa) != 0) {
    free(sa);
    kill(shell_pid, SIGKILL);
    waitpid(shell_pid, NULL, WNOHANG);
    close(master_fd);
    close(client_fd);
    pthread_attr_destroy(&attr);
    return;
  }
  pthread_attr_destroy(&attr);
}

static void run_server(const char *host, int port, int foreground)
{
  int server_fd;
  struct sockaddr_in addr;
  int opt;

  server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd < 0) {
    perror("connect: socket");
    exit(EXIT_FAILURE);
  }
  opt = 1;
  setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port   = htons((uint16_t)port);
  if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
    struct hostent *he = gethostbyname(host);
    if (!he) {
      fprintf(stderr, "connect: cannot resolve %s\n", host);
      exit(EXIT_FAILURE);
    }
    memcpy(&addr.sin_addr, he->h_addr_list[0], sizeof(addr.sin_addr));
  }

  if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    perror("connect: bind");
    exit(EXIT_FAILURE);
  }
  if (listen(server_fd, 16) < 0) {
    perror("connect: listen");
    exit(EXIT_FAILURE);
  }

  fprintf(stderr, "connect: listening on %s:%d (uid %d)\n",
          host, port, (int)getuid());

  if (!foreground)
    daemonize();

  signal(SIGPIPE, SIG_IGN);

  for (;;) {
    struct pollfd pfd;
    pfd.fd     = server_fd;
    pfd.events = POLLIN;

    int pr = poll(&pfd, 1, -1);
    if (pr < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (pfd.revents & POLLIN)
      handle_connection(server_fd);
  }

  close(server_fd);
  exit(0);
}

/* Pipe mode client: stdin feeds the remote shell, remote stdout/stderr
   come back as PKT_DATA/PKT_STDERR. No pty, no raw mode, no window
   updates. An empty PKT_DATA marks our stdin EOF; the socket stays open
   for output and the exit status. */
static int run_client_pipe_loop(int sock_fd)
{
  struct pollfd fds[2];
  char buf[4096];
  int raw_status  = -1;
  int stdin_open  = 1;
  int sock_open   = 1;

  for (;;) {
    if (!sock_open)
      break;

    fds[0].fd      = stdin_open ? STDIN_FILENO : -1;
    fds[0].events  = POLLIN;
    fds[0].revents = 0;
    fds[1].fd      = sock_open ? sock_fd : -1;
    fds[1].events  = POLLIN;
    fds[1].revents = 0;

    int r = poll(fds, 2, -1);
    if (r < 0) {
      if (errno == EINTR) continue;
      break;
    }

    if (fds[1].revents & (POLLIN | POLLHUP | POLLERR)) {
      uint8_t type;
      uint32_t len;
      if (recv_pkt(sock_fd, &type, buf, sizeof(buf), &len) != 0) {
        sock_open = 0;
      } else if (type == PKT_DATA && len > 0) {
        if (write_all(STDOUT_FILENO, buf, len) != 0)
          sock_open = 0;
      } else if (type == PKT_STDERR && len > 0) {
        if (write_all(STDERR_FILENO, buf, len) != 0)
          sock_open = 0;
      } else if (type == PKT_EXIT && len == sizeof(int32_t)) {
        int32_t net_status;
        memcpy(&net_status, buf, sizeof(net_status));
        raw_status = (int)ntohl(net_status);
        sock_open = 0;
      }
    }

    if (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
      ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
      if (n < 0 && errno == EINTR)
        continue;
      if (n <= 0) {
        stdin_open = 0;
        send_pkt(sock_fd, PKT_DATA, NULL, 0);   /* stdin EOF */
      } else {
        if (send_pkt(sock_fd, PKT_DATA, buf, (uint32_t)n) != 0)
          stdin_open = 0;
      }
    }
  }

  close(sock_fd);

  if (raw_status != -1) {
    if (WIFSIGNALED(raw_status))
      return 128 + WTERMSIG(raw_status);
    if (WIFEXITED(raw_status))
      return WEXITSTATUS(raw_status);
  }
  return 1;
}

static int run_client_loop(int sock_fd)
{
  struct pollfd fds[2];
  char buf[4096];
  int raw_status  = -1;
  int stdin_open  = 1;
  int sock_open   = 1;
  int stdin_tty   = isatty(STDIN_FILENO);

  raw_mode_enter();
  winch_install();

  for (;;) {
    if (winch_pending) {
      winch_pending = 0;
      struct winsize ws;
      if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == 0) {
        struct pkt_winch w;
        w.rows = htons(ws.ws_row);
        w.cols = htons(ws.ws_col);
        send_pkt(sock_fd, PKT_WINCH, &w, sizeof(w));
      }
    }

    if (!sock_open)
      break;

    fds[0].fd      = stdin_open ? STDIN_FILENO : -1;
    fds[0].events  = POLLIN;
    fds[0].revents = 0;
    fds[1].fd      = sock_open ? sock_fd : -1;
    fds[1].events  = POLLIN;
    fds[1].revents = 0;

    int r = poll(fds, 2, -1);
    if (r < 0) {
      if (errno == EINTR) continue;
      break;
    }

    /* Packet from server -> check type */
    if (fds[1].revents & POLLIN) {
      uint8_t type;
      uint32_t len;
      if (recv_pkt(sock_fd, &type, buf, sizeof(buf), &len) != 0) {
        sock_open = 0;
      } else {
        if (type == PKT_DATA && len > 0) {
          if (write_all(STDOUT_FILENO, buf, len) != 0)
            sock_open = 0;
        } else if (type == PKT_EXIT && len == sizeof(int32_t)) {
          int32_t net_status;
          memcpy(&net_status, buf, sizeof(net_status));
          raw_status = (int)ntohl(net_status);
          sock_open = 0;
        }
      }
    }

    /* Local stdin -> send as PKT_DATA */
    if (fds[0].revents & POLLIN) {
      ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
      if (n < 0 && errno == EINTR)
        continue;
      if (n <= 0) {
        stdin_open = 0;
        if (!stdin_tty) {
          char eof_char = 004;
          send_pkt(sock_fd, PKT_DATA, &eof_char, 1);
        } else {
          sock_open = 0;
          shutdown(sock_fd, SHUT_WR);
        }
      } else {
        if (send_pkt(sock_fd, PKT_DATA, buf, (uint32_t)n) != 0)
          stdin_open = 0;
      }
    }
  }

  close(sock_fd);
  raw_mode_leave();

  if (raw_status != -1) {
    if (WIFSIGNALED(raw_status))
      return 128 + WTERMSIG(raw_status);
    if (WIFEXITED(raw_status))
      return WEXITSTATUS(raw_status);
  }
  return 1;
}

static int connect_to_server(const char *host, int port)
{
  struct sockaddr_in addr;
  int fd;

  fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    return -1;

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port   = htons((uint16_t)port);

  if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
    struct hostent *he = gethostbyname(host);
    if (!he) {
      close(fd);
      return -1;
    }
    memcpy(&addr.sin_addr, he->h_addr_list[0], sizeof(addr.sin_addr));
  }

  if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    close(fd);
    return -1;
  }

  return fd;
}

static void usage(const char *prog)
{
  fprintf(stderr,
    "Usage:\n"
    "  %s --server [--host ADDR] [--port PORT] [--foreground]\n"
    "  %s [--host HOST] [--port PORT]\n"
    "\n"
    "Defaults: host=%s, port=%d\n"
    "\n"
    "The server runs as the current user.  Every connecting client receives\n"
    "a login shell (sudo -i equivalent) with the server's environment.\n"
    "Only LANG and LC_* are forwarded from the client.\n"
    "When stdin/stdout/stderr are all ttys the session gets a pty;\n"
    "otherwise the streams are relayed separately with no pty.\n",
    prog, prog, DEFAULT_HOST, DEFAULT_PORT);
}

int main(int argc, char *argv[])
{
  static const struct option long_opts[] = {
    { "server",     no_argument,       NULL, 'S' },
    { "host",       required_argument, NULL, 'H' },
    { "port",       required_argument, NULL, 'p' },
    { "foreground", no_argument,       NULL, 'f' },
    { "help",       no_argument,       NULL, 'h' },
    { NULL, 0, NULL, 0 }
  };

  const char *host   = DEFAULT_HOST;
  int         port   = DEFAULT_PORT;
  int         server = 0;
  int         fg     = 0;
  int         opt;


  while ((opt = getopt_long(argc, argv, "+SH:p:fh", long_opts, NULL)) != -1) {
    switch (opt) {
    case 'S': server = 1;              break;
    case 'H': host   = optarg;         break;
    case 'p':
      port = atoi(optarg);
      if (port <= 0 || port > 65535) {
        fprintf(stderr, "connect: invalid port: %s\n", optarg);
        return 1;
      }
      break;
    case 'f': fg = 1;                  break;
    case 'h': usage(argv[0]);          return 0;
    default:  usage(argv[0]);          return 1;
    }
  }

  if (optind < argc) {
    fprintf(stderr, "connect: unexpected argument: %s\n", argv[optind]);
    usage(argv[0]);
    return 1;
  }

  if (server) {
    run_server(host, port, fg);
    return 0;
  }

  /* Snapshot the fd shape before connect_to_server() opens the socket
     (which reuses the lowest free number, e.g. 0 after "<&-"). Extra
     fds past stderr cannot cross TCP, so only the pty/no-pty decision
     travels: all std fds ttys -> pty session, else relayed pipes. */
  int use_pipe = !fwd_std_all_tty();

  int sock_fd = connect_to_server(host, port);
  if (sock_fd < 0) {
    fprintf(stderr, "connect: cannot connect to %s:%d: %s\n",
            host, port, strerror(errno));
    return 1;
  }

  /* Set a generous receive timeout so a dead server does not hang us. */
  {
    struct timeval tv = { .tv_sec = 10, .tv_usec = 0 };
    setsockopt(sock_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  }

  /* Send the greeting: terminal size, session mode, locale blob. */
  struct connect_hello hello;
  memset(&hello, 0, sizeof(hello));
  hello.pipe_mode = use_pipe;
  {
    struct winsize ws;
    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == 0) {
      hello.rows = ws.ws_row;
      hello.cols = ws.ws_col;
    } else {
      hello.rows = 24;
      hello.cols = 80;
    }
  }

  if (write_all(sock_fd, &hello, sizeof(hello)) != 0) {
    fprintf(stderr, "connect: failed to send greeting\n");
    close(sock_fd);
    return 1;
  }

  uint32_t lc_size = 0;
  char *lc_blob = build_lc_blob(&lc_size);

  if (write_all(sock_fd, &lc_size, sizeof(lc_size)) != 0) {
    fprintf(stderr, "connect: failed to send locale size\n");
    free(lc_blob);
    close(sock_fd);
    return 1;
  }
  if (lc_size > 0 && write_all(sock_fd, lc_blob, lc_size) != 0) {
    fprintf(stderr, "connect: failed to send locale blob\n");
    free(lc_blob);
    close(sock_fd);
    return 1;
  }
  free(lc_blob);

  /* Wait for the server's "ready" byte. */
  {
    uint8_t ok = 0;
    if (read_all(sock_fd, &ok, sizeof(ok)) != 0 || ok != 1) {
      fprintf(stderr, "connect: server rejected the connection\n");
      close(sock_fd);
      return 1;
    }
  }

  /* Remove the receive timeout: interactive sessions may be idle a while.
     Keepalive handles dead connections instead. */
  {
    struct timeval tv = { .tv_sec = 0, .tv_usec = 0 };
    setsockopt(sock_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    int val = 1;
    setsockopt(sock_fd, SOL_SOCKET, SO_KEEPALIVE, &val, sizeof(val));
#ifdef TCP_KEEPIDLE
    val = 60;
    setsockopt(sock_fd, IPPROTO_TCP, TCP_KEEPIDLE, &val, sizeof(val));
#endif
  }

  if (use_pipe)
    return run_client_pipe_loop(sock_fd);

  return run_client_loop(sock_fd);
}
