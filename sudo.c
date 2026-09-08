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
#include <getopt.h>
#include <termios.h>
#include <pwd.h>
#include <grp.h>
#include <pty.h>
#include <poll.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/time.h>

#include "io.h"
#include "tty.h"

#define SOCKET_PATH   "/data/data/com.termux/files/usr/tmp/.sudo.sock"
#define LOCK_PATH     "/data/local/tmp/.sudo.lock"
#define SERVER_FLAG   "--server-daemon"
#define ROOT_HOME     "/data/data/com.termux/files/home/.suroot"
#define ROOT_TMP      ROOT_HOME "/.tmp"
#define DEFAULT_SHELL "/data/data/com.termux/files/usr/bin/bash"

#define MAX_ENV_SIZE  (1u << 20)   /* cap on the client-supplied env blob */
#define MAX_ARGS      128
#define MAX_SESSIONS  64
#define DRAIN_MS      200          /* flush pty output around session end */

extern char **environ;

struct sudo_req {
  int flag_i;
  int flag_s;
  uid_t uid;
  gid_t gid;
  unsigned short rows;
  unsigned short cols;
  int argc;
  char cwd[PATH_MAX];
  char args[2048];
};

/* server session tracking (main-loop only, never touched from a handler) */
struct session {
  pid_t pid;
  int client_fd;
};
static struct session sessions[MAX_SESSIONS];
static int n_sessions;
static unsigned long total_served;
static int sigchld_pipe[2] = { -1, -1 };

static int send_fd(int socket, int fd)
{
  struct msghdr msg = {0};
  char buf[CMSG_SPACE(sizeof(int))];
  struct iovec io = { .iov_base = "FD", .iov_len = 2 };

  memset(buf, 0, sizeof(buf));
  msg.msg_iov = &io;
  msg.msg_iovlen = 1;
  msg.msg_control = buf;
  msg.msg_controllen = sizeof(buf);

  struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
  cmsg->cmsg_level = SOL_SOCKET;
  cmsg->cmsg_type = SCM_RIGHTS;
  cmsg->cmsg_len = CMSG_LEN(sizeof(int));
  memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));

  return sendmsg(socket, &msg, 0);
}

static int recv_fd(int socket)
{
  struct msghdr msg = {0};
  char buf[CMSG_SPACE(sizeof(int))];
  char dummy[2];
  struct iovec io = { .iov_base = dummy, .iov_len = 2 };

  msg.msg_iov = &io;
  msg.msg_iovlen = 1;
  msg.msg_control = buf;
  msg.msg_controllen = sizeof(buf);

  if (recvmsg(socket, &msg, 0) <= 0)
    return -1;

  struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
  if (cmsg && cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
    int fd;
    memcpy(&fd, CMSG_DATA(cmsg), sizeof(int));
    return fd;
  }

  return -1;
}

/* Wrap s in single quotes so it survives the shell that su -c hands it to. */
static int shell_squote(const char *s, char *out, size_t outlen)
{
  size_t o = 0;

  if (o + 1 >= outlen)
    return -1;
  out[o++] = '\'';

  for (; *s; s++) {
    if (*s == '\'') {
      if (o + 4 >= outlen)
        return -1;
      out[o++] = '\'';
      out[o++] = '\\';
      out[o++] = '\'';
      out[o++] = '\'';
    } else {
      if (o + 1 >= outlen)
        return -1;
      out[o++] = *s;
    }
  }

  if (o + 2 > outlen)
    return -1;
  out[o++] = '\'';
  out[o] = '\0';

  return 0;
}

/* Parse a decimal id, rejecting anything not fully numeric. */
static int parse_id(const char *s, unsigned *out)
{
  char *end;
  unsigned long v;

  if (!*s)
    return -1;
  errno = 0;
  v = strtoul(s, &end, 10);
  if (errno != 0 || *end != '\0' || v > 0xffffffffUL)
    return -1;

  *out = (unsigned)v;
  return 0;
}

static void sigchld_handler(int sig)
{
  (void)sig;
  int saved_errno = errno;
  char b = 1;

  if (sigchld_pipe[1] >= 0) {
    ssize_t w = write(sigchld_pipe[1], &b, 1);
    (void)w;
  }
  errno = saved_errno;
}

static void daemonize(void)
{
  pid_t pid = fork();
  if (pid < 0)
    exit(EXIT_FAILURE);
  if (pid > 0)
    exit(EXIT_SUCCESS);

  if (setsid() < 0)
    exit(EXIT_FAILURE);

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

/* Apply the client's environment into a root child, dropping LD_* so a caller
   cannot inject a preload into the privileged exec, and TERMUX_EXEC__* since
   the privileged exec runs without libtermux-exec (LD_PRELOAD is cleared) to
   refresh it: a stale TERMUX_EXEC__PROC_SELF_EXE from the client would still
   name the client's own binary, and a program that trusts it over a linker-
   shadowed /proc/self/exe would misidentify itself. */
static void apply_environment(const char *env_buf, uint32_t env_len)
{
  const char *p = env_buf;
  const char *end = env_buf + env_len;

  while (p < end) {
    size_t remaining = (size_t)(end - p);
    size_t klen = strnlen(p, remaining);

    if (klen == remaining)   /* not NUL-terminated: stop before overrun */
      break;

    const char *eq = memchr(p, '=', klen);
    if (eq && strncmp(p, "LD_", 3) != 0 &&
        strncmp(p, "TERMUX_EXEC__", 13) != 0) {
      size_t nlen = (size_t)(eq - p);
      char name[256];

      if (nlen < sizeof(name)) {
        memcpy(name, p, nlen);
        name[nlen] = '\0';
        setenv(name, eq + 1, 1);
      }
    }
    p += klen + 1;
  }
}

/* Switch to the requested user, resetting supplementary groups and aborting
   the child if any step fails. Default (uid 0, gid 0) keeps root. */
static void drop_privileges(const struct sudo_req *req)
{
  gid_t gid = req->gid;
  struct passwd *pw = NULL;

  if (req->uid == 0 && req->gid == 0)
    return;

  if (req->uid != 0) {
    pw = getpwuid(req->uid);
    if (req->gid == 0 && pw)
      gid = pw->pw_gid;
  }

  if (setgid(gid) != 0) {
    perror("sudo: setgid");
    _exit(1);
  }

  /* Replace the inherited (root) supplementary set. */
  if (pw) {
    if (initgroups(pw->pw_name, gid) != 0) {
      perror("sudo: initgroups");
      _exit(1);
    }
  } else if (setgroups(1, &gid) != 0) {
    perror("sudo: setgroups");
    _exit(1);
  }

  if (req->uid != 0 && setuid(req->uid) != 0) {
    perror("sudo: setuid");
    _exit(1);
  }
}

/* Overrides client HOME/TMPDIR; call after apply_environment. */
static void setup_root_env(void)
{
  mkdir(ROOT_HOME, 0700);
  mkdir(ROOT_TMP, 0700);
  setenv("HOME", ROOT_HOME, 1);
  setenv("TMPDIR", ROOT_TMP, 1);
}

static void run_child(struct sudo_req *req, char *env_buf, uint32_t env_size,
                      int slave_fd)
{
  char *exec_args[MAX_ARGS];
  int argi = 0;
  char *exec_path;
  char *shell;

  setsid();
  ioctl(slave_fd, TIOCSCTTY, 0);
  dup2(slave_fd, STDIN_FILENO);
  dup2(slave_fd, STDOUT_FILENO);
  dup2(slave_fd, STDERR_FILENO);
  if (slave_fd > STDERR_FILENO)
    close(slave_fd);

  if (env_buf)
    apply_environment(env_buf, env_size);
  drop_privileges(req);

  if (req->uid == 0)
    setup_root_env();

  if (!req->flag_i) {
    size_t cwd_len = strnlen(req->cwd, sizeof(req->cwd));

    if (cwd_len > 0 && cwd_len < sizeof(req->cwd) && chdir(req->cwd) != 0)
      fprintf(stderr, "sudo: warning: could not change directory to %s: %s\n",
              req->cwd, strerror(errno));
  }

  /* Read SHELL only after the client's environment is in place. */
  shell = getenv("SHELL");
  if (!shell)
    shell = DEFAULT_SHELL;
  exec_path = shell;

  if (req->flag_i) {
    const char *base = strrchr(shell, '/');
    static char login_argv0[64];

    setenv("USER", "root", 1);
    setenv("LOGNAME", "root", 1);
    if (chdir(ROOT_HOME) != 0 && chdir("/") != 0) {
      /* stay in the current directory */
    }

    base = base ? base + 1 : shell;
    snprintf(login_argv0, sizeof(login_argv0), "-%s", base);
    exec_args[argi++] = login_argv0;
  } else if (req->argc == 0 || req->flag_s) {
    exec_args[argi++] = shell;
  } else {
    char *ptr = req->args;
    char *aend = req->args + sizeof(req->args);
    int i;

    for (i = 0; i < req->argc && argi < MAX_ARGS - 1; i++) {
      size_t avail = (size_t)(aend - ptr);
      size_t len = strnlen(ptr, avail);

      if (len == avail)   /* not NUL-terminated within the buffer */
        break;
      exec_args[argi++] = ptr;
      ptr += len + 1;
    }
    if (argi == 0)
      _exit(127);
    exec_path = exec_args[0];
  }
  exec_args[argi] = NULL;

  execvp(exec_path, exec_args);

  /* Match sudo: a missing path or anything that is not an executable regular
     file is reported as "command not found" rather than the raw exec error. */
  if (errno == ENOENT) {
    fprintf(stderr, "sudo: %s: command not found\n", exec_path);
    _exit(127);
  }
  if (errno == EACCES) {
    struct stat st;

    if (stat(exec_path, &st) == 0 && !S_ISREG(st.st_mode)) {
      fprintf(stderr, "sudo: %s: command not found\n", exec_path);
      _exit(127);
    }
  }
  fprintf(stderr, "sudo: %s: %s\n", exec_path, strerror(errno));
  _exit(126);
}

static void track_session(pid_t pid, int client_fd)
{
  if (n_sessions < MAX_SESSIONS) {
    sessions[n_sessions].pid = pid;
    sessions[n_sessions].client_fd = client_fd;
    n_sessions++;
  } else {
    close(client_fd);   /* cannot report status; reap without tracking */
  }
}

/* Reap every exited child and forward its raw wait status to the waiting
   client, then close that connection. */
static void reap_sessions(void)
{
  pid_t pid;
  int status;

  while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
    int i;

    for (i = 0; i < n_sessions; i++) {
      if (sessions[i].pid == pid) {
        write_all(sessions[i].client_fd, &status, sizeof(status));
        close(sessions[i].client_fd);
        sessions[i] = sessions[--n_sessions];
        break;
      }
    }
  }
}

static void handle_connection(int server_fd)
{
  struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
  struct sudo_req req;
  uint32_t env_size = 0;
  char *env_buf = NULL;
  int master_fd, slave_fd;
  struct winsize ws;
  pid_t pid;

  int client_fd = accept(server_fd, NULL, NULL);
  if (client_fd < 0)
    return;

  /* A stalled client must not wedge the single-threaded daemon. */
  setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  memset(&req, 0, sizeof(req));
  if (read_all(client_fd, &req, sizeof(req)) != 0 ||
      read_all(client_fd, &env_size, sizeof(env_size)) != 0 ||
      env_size > MAX_ENV_SIZE || req.argc < 0 || req.argc > MAX_ARGS - 1) {
    close(client_fd);
    return;
  }

  if (env_size > 0) {
    env_buf = malloc(env_size);
    if (!env_buf || read_all(client_fd, env_buf, env_size) != 0) {
      free(env_buf);
      close(client_fd);
      return;
    }
  }

  ws.ws_row = req.rows;
  ws.ws_col = req.cols;
  ws.ws_xpixel = 0;
  ws.ws_ypixel = 0;

  if (openpty(&master_fd, &slave_fd, NULL, NULL, &ws) != 0) {
    free(env_buf);
    close(client_fd);
    return;
  }

  pid = fork();
  if (pid == 0) {
    close(server_fd);
    close(client_fd);
    close(master_fd);
    if (sigchld_pipe[0] >= 0)
      close(sigchld_pipe[0]);
    if (sigchld_pipe[1] >= 0)
      close(sigchld_pipe[1]);
    run_child(&req, env_buf, env_size, slave_fd);
  } else if (pid > 0) {
    close(slave_fd);
    if (send_fd(client_fd, master_fd) < 0) {
      kill(pid, SIGKILL);   /* client gone: don't leave a root shell adrift */
      close(client_fd);
    } else {
      track_session(pid, client_fd);
    }
    close(master_fd);
    total_served++;
  } else {
    close(slave_fd);
    close(master_fd);
    close(client_fd);
  }

  free(env_buf);
}

static void run_server(void)
{
  int lock_fd, server_fd;
  struct sockaddr_un addr;
  struct sigaction sa;
  struct pollfd pfd[2];
  struct stat ds;
  const char *slash;
  char dir[sizeof(addr.sun_path)];
  size_t dlen;

  lock_fd = open(LOCK_PATH, O_RDWR | O_CREAT | O_CLOEXEC, 0666);
  if (lock_fd < 0)
    exit(EXIT_FAILURE);
  if (flock(lock_fd, LOCK_EX | LOCK_NB) < 0) {
    close(lock_fd);
    exit(0);
  }

  daemonize();

  if (ftruncate(lock_fd, 0) == 0)
    dprintf(lock_fd, "%d\n", getpid());

  signal(SIGPIPE, SIG_IGN);

  if (pipe2(sigchld_pipe, O_NONBLOCK | O_CLOEXEC) != 0)
    exit(EXIT_FAILURE);

  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = sigchld_handler;
  sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
  sigaction(SIGCHLD, &sa, NULL);

  unlink(SOCKET_PATH);
  server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (server_fd < 0)
    exit(EXIT_FAILURE);

  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

  if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    exit(EXIT_FAILURE);

  /* Hand the socket to the (non-root) UID that owns the tmp dir so a tight
     0600 still lets the client connect; fall back to a working mode. */
  slash = strrchr(SOCKET_PATH, '/');
  dlen = (size_t)(slash - SOCKET_PATH);
  memcpy(dir, SOCKET_PATH, dlen);
  dir[dlen] = '\0';
  if (stat(dir, &ds) == 0 && chown(SOCKET_PATH, ds.st_uid, ds.st_gid) == 0)
    chmod(SOCKET_PATH, 0600);
  else
    chmod(SOCKET_PATH, 0666);

  if (listen(server_fd, 10) < 0)
    exit(EXIT_FAILURE);

  pfd[0].fd = server_fd;
  pfd[0].events = POLLIN;
  pfd[1].fd = sigchld_pipe[0];
  pfd[1].events = POLLIN;

  for (;;) {
    int pr = poll(pfd, 2, 1000);

    if (pr < 0) {
      if (errno == EINTR)
        continue;
      break;
    }

    if (pr > 0) {
      if (pfd[1].revents & POLLIN) {
        char drain[64];
        while (read(sigchld_pipe[0], drain, sizeof(drain)) > 0)
          ;
        reap_sessions();
      }
      if (pfd[0].revents & POLLIN)
        handle_connection(server_fd);
    }

    /* Nothing pending and no live session: safe to self-exit. */
    if (pr == 0 && total_served > 0 && n_sessions == 0)
      break;
  }

  close(server_fd);
  unlink(SOCKET_PATH);
  unlink(LOCK_PATH);
  close(lock_fd);
  exit(0);
}

static int connect_server(void)
{
  struct sockaddr_un addr;
  char self_path[PATH_MAX];
  char quoted[PATH_MAX * 2];
  char su_cmd[PATH_MAX * 2 + 64];
  ssize_t len;
  pid_t mid;
  int fd, delay_us, elapsed;

  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

  fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0)
    return -1;
  if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0)
    return fd;
  close(fd);

  len = readlink("/proc/self/exe", self_path, sizeof(self_path) - 1);
  if (len <= 0 || (size_t)len >= sizeof(self_path) - 1)
    return -1;
  self_path[len] = '\0';

  if (shell_squote(self_path, quoted, sizeof(quoted)) != 0)
    return -1;
  snprintf(su_cmd, sizeof(su_cmd), "env -u LD_PRELOAD %s %s", quoted, SERVER_FLAG);

  /* Double-fork so the daemon reparents to init instead of blocking us. */
  mid = fork();
  if (mid == 0) {
    pid_t grand = fork();
    if (grand == 0) {
      execl("/system/bin/su", "su", "-c", su_cmd, (char *)NULL);
      _exit(127);
    }
    _exit(0);
  } else if (mid > 0) {
    while (waitpid(mid, NULL, 0) < 0 && errno == EINTR)
      ;
  }

  /* Adaptive backoff, ~3s total budget. */
  delay_us = 5000;
  elapsed = 0;
  while (elapsed < 3000000) {
    usleep(delay_us);
    elapsed += delay_us;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd >= 0) {
      if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0)
        return fd;
      close(fd);
    }
    if (delay_us < 100000)
      delay_us *= 2;
  }

  return -1;
}

/* Relay stdin/pty, wait for the child's exit status on the socket, and drain
   remaining output. Returns the process exit code. */
static int run_client(int socket_fd, int master_fd)
{
  struct pollfd fds[3];
  struct timespec deadline;
  char buf[4096];
  int raw_status = -1;
  int stdin_open = 1, master_eof = 0, status_recv = 0, ending = 0;
  int stdin_tty = isatty(STDIN_FILENO);

  raw_mode_enter();

  winch_install();

  for (;;) {
    int timeout = -1;
    int r;

    if (winch_pending) {
      winch_pending = 0;
      push_window_size(master_fd);
    }

    if (status_recv && master_eof)
      break;

    if ((status_recv || master_eof) && !ending) {
      clock_gettime(CLOCK_MONOTONIC, &deadline);
      deadline.tv_nsec += DRAIN_MS * 1000000L;
      if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_nsec -= 1000000000L;
        deadline.tv_sec++;
      }
      ending = 1;
    }
    if (ending)
      timeout = ms_until(&deadline);

    fds[0].fd = stdin_open ? STDIN_FILENO : -1;
    fds[0].events = POLLIN;
    fds[0].revents = 0;
    fds[1].fd = master_eof ? -1 : master_fd;
    fds[1].events = POLLIN;
    fds[1].revents = 0;
    fds[2].fd = status_recv ? -1 : socket_fd;
    fds[2].events = POLLIN;
    fds[2].revents = 0;

    r = poll(fds, 3, timeout);
    if (r < 0) {
      if (errno == EINTR)
        continue;
      break;
    }
    if (r == 0)   /* drain deadline elapsed */
      break;

    if (fds[1].revents) {
      ssize_t n = read(master_fd, buf, sizeof(buf));
      if (n < 0 && errno == EINTR)
        continue;
      if (n <= 0)
        master_eof = 1;
      else if (write_all(STDOUT_FILENO, buf, (size_t)n) != 0)
        master_eof = 1;
    }

    if (fds[2].revents) {
      if (read_all(socket_fd, &raw_status, sizeof(raw_status)) != 0)
        raw_status = -1;
      status_recv = 1;
    }

    if (fds[0].revents) {
      ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
      if (n < 0 && errno == EINTR)
        continue;
      if (n <= 0) {
        stdin_open = 0;
        if (!stdin_tty) {
          char eof = 004;   /* VEOF: end a canonical-mode read */
          write_all(master_fd, &eof, 1);
        }
      } else if (write_all(master_fd, buf, (size_t)n) != 0) {
        stdin_open = 0;
      }
    }
  }

  close(master_fd);
  close(socket_fd);
  raw_mode_leave();

  if (status_recv && raw_status != -1) {
    if (WIFSIGNALED(raw_status))
      return 128 + WTERMSIG(raw_status);
    if (WIFEXITED(raw_status))
      return WEXITSTATUS(raw_status);
  }
  return 1;
}

int main(int argc, char *argv[])
{
  struct sudo_req req;
  struct winsize ws;
  unsigned target_uid = 0, target_gid = 0;
  char *ptr;
  size_t remaining;
  int opt, n, socket_fd, master_fd, i;

  if (argc > 1 && strcmp(argv[1], SERVER_FLAG) == 0) {
    run_server();
    return 0;
  }

  memset(&req, 0, sizeof(req));

  optind = 1;
  while ((opt = getopt(argc, argv, "+isu:g:")) != -1) {
    switch (opt) {
    case 'i':
      req.flag_i = 1;
      break;
    case 's':
      req.flag_s = 1;
      break;
    case 'u': {
      struct passwd *pw = getpwnam(optarg);
      if (pw) {
        target_uid = pw->pw_uid;
      } else if (parse_id(optarg, &target_uid) != 0) {
        fprintf(stderr, "sudo: unknown user: %s\n", optarg);
        return 1;
      }
      break;
    }
    case 'g': {
      struct group *gr = getgrnam(optarg);
      if (gr) {
        target_gid = gr->gr_gid;
      } else if (parse_id(optarg, &target_gid) != 0) {
        fprintf(stderr, "sudo: unknown group: %s\n", optarg);
        return 1;
      }
      break;
    }
    default:
      fprintf(stderr, "Usage: sudo [-i] [-s] [-u user] [-g group] [command...]\n");
      return 1;
    }
  }

  req.uid = target_uid;
  req.gid = target_gid;

  if (!getcwd(req.cwd, sizeof(req.cwd)))
    req.cwd[0] = '\0';

  ptr = req.args;
  remaining = sizeof(req.args);
  n = 0;
  for (i = optind; i < argc; i++) {
    size_t arg_len = strlen(argv[i]) + 1;

    if (arg_len > remaining || n >= MAX_ARGS - 1) {
      fprintf(stderr, "sudo: command line too long\n");
      return 1;
    }
    memcpy(ptr, argv[i], arg_len);
    ptr += arg_len;
    remaining -= arg_len;
    n++;
  }
  req.argc = n;

  if (req.argc == 0 && !req.flag_i && !req.flag_s) {
    fprintf(stderr, "Usage: sudo [-i] [-s] [-u user] [-g group] [command...]\n");
    return 1;
  }

  if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == 0) {
    req.rows = ws.ws_row;
    req.cols = ws.ws_col;
  } else {
    req.rows = 24;
    req.cols = 80;
  }

  socket_fd = connect_server();
  if (socket_fd < 0) {
    fprintf(stderr, "sudo: cannot reach the daemon\n");
    return 1;
  }

  /* Bound recv_fd so a daemon that self-exits mid-connect cannot hang us. */
  {
    struct timeval tv = { .tv_sec = 10, .tv_usec = 0 };
    setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  }

  if (write_all(socket_fd, &req, sizeof(req)) != 0) {
    fprintf(stderr, "sudo: failed to send request\n");
    close(socket_fd);
    return 1;
  }

  uint32_t env_size = 0;
  for (char **e = environ; *e != NULL; e++)
    env_size += strlen(*e) + 1;

  if (write_all(socket_fd, &env_size, sizeof(env_size)) != 0) {
    fprintf(stderr, "sudo: failed to send environment\n");
    close(socket_fd);
    return 1;
  }
  for (char **e = environ; *e != NULL; e++) {
    if (write_all(socket_fd, *e, strlen(*e) + 1) != 0) {
      fprintf(stderr, "sudo: failed to send environment\n");
      close(socket_fd);
      return 1;
    }
  }

  master_fd = recv_fd(socket_fd);
  if (master_fd < 0) {
    fprintf(stderr, "sudo: failed to receive pty from daemon\n");
    close(socket_fd);
    return 1;
  }

  return run_client(socket_fd, master_fd);
}
