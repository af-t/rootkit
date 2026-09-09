#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <linux/loop.h>

#include "io.h"
#include "tty.h"

#define DEFAULT_MOUNT "/mnt/ubuntu"
#define DEFAULT_SU "/usr/bin/su"
#define TERM_GRACE_SECONDS 3
#define SESSION_DRAIN_MS 200
#define MAX_BINDS 64

/* A -b bind; "guest" is the path inside the tree, defaulting to "host". */
struct bind {
  const char *host;
  const char *guest;
};

static char distro_path[PATH_MAX];
/* Short enough that every subpath cp() appends still fits in PATH_MAX. */
static char chroot_path[PATH_MAX - 256];
static size_t chroot_len;
/* Shallowest directory the teardown may remove, and SIZE_MAX while this run
   has created none: a mount point that was already there is left alone. */
static size_t chroot_created = SIZE_MAX;
static const char *su_path = DEFAULT_SU;
static struct bind binds[MAX_BINDS];
static size_t bind_count;
static char loop_node[32];
static int loop_fd = -1;
static int lock_fd = -1;
static int ns_enabled;
static int debug_enabled;
static int rootfs_mounted;
static int cleanup_done;
static volatile sig_atomic_t session_gone;
static volatile sig_atomic_t stop_signal;

/* Build "<chroot_path><sub>" into one of a few rotating buffers, so one
   expression can hold several results. */
static const char *cp(const char *sub)
{
  static char bufs[4][PATH_MAX];
  static unsigned turn;
  char *buf = bufs[turn++ % (sizeof(bufs) / sizeof(bufs[0]))];

  snprintf(buf, PATH_MAX, "%s%s", chroot_path, sub);
  return buf;
}

static void dbg(const char *fmt, ...)
{
  va_list ap;

  if (!debug_enabled)
    return;

  fputs("[rootlet] ", stderr);
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
}

static void die(const char *what)
{
  fprintf(stderr, "%s: %s\n", what, strerror(errno));
  exit(1);
}

/* Creates every missing component of "path". "created", when not NULL, is
   lowered to the length of the shallowest component this call created. */
static int mkdir_p(const char *path, size_t *created)
{
  char buf[PATH_MAX];
  char *p;

  if ((size_t)snprintf(buf, sizeof(buf), "%s", path) >= sizeof(buf)) {
    errno = ENAMETOOLONG;
    return -1;
  }

  for (p = buf + 1; *p; p++) {
    if (*p != '/')
      continue;
    *p = '\0';
    if (mkdir(buf, 0755) != 0) {
      if (errno != EEXIST)
        return -1;
    } else if (created && strlen(buf) < *created) {
      *created = strlen(buf);
    }
    *p = '/';
  }

  if (mkdir(buf, 0755) != 0) {
    if (errno != EEXIST)
      return -1;
  } else if (created && strlen(buf) < *created) {
    *created = strlen(buf);
  }

  return 0;
}

/* Android keeps block nodes under /dev/block, and ueventd can create a
   freshly allocated one slightly after LOOP_CTL_GET_FREE returns. */
static int open_loop_node(int number)
{
  static const char *const dirs[] = { "/dev/block", "/dev" };
  struct timespec delay = { 0, 100 * 1000 * 1000 };
  int attempt;
  size_t i;

  for (attempt = 0; attempt < 5; attempt++) {
    for (i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
      int fd;

      snprintf(loop_node, sizeof(loop_node), "%s/loop%d",
         dirs[i], number);

      fd = open(loop_node, O_RDWR | O_CLOEXEC);
      if (fd >= 0)
        return fd;

      dbg("open %s: %s", loop_node, strerror(errno));
      if (errno != ENOENT)
        return -1;
    }

    nanosleep(&delay, NULL);
  }

  snprintf(loop_node, sizeof(loop_node), "loop%d", number);
  errno = ENOENT;
  return -1;
}

static void loop_attach(void)
{
  struct loop_info64 info;
  size_t name_len;
  int ctl, img, attempt;

  ctl = open("/dev/loop-control", O_RDWR | O_CLOEXEC);
  if (ctl < 0)
    die("open /dev/loop-control");

  img = open(distro_path, O_RDWR | O_CLOEXEC);
  if (img < 0)
    die(distro_path);

  /* GET_FREE only reserves a number; a racing attach can take it first. */
  for (attempt = 0; attempt < 16; attempt++) {
    int node;

    node = ioctl(ctl, LOOP_CTL_GET_FREE);
    if (node < 0)
      die("LOOP_CTL_GET_FREE");

    dbg("LOOP_CTL_GET_FREE -> %d", node);

    loop_fd = open_loop_node(node);
    if (loop_fd < 0) {
      fprintf(stderr, "%s: %s (tried /dev/block and /dev)\n",
        loop_node, strerror(errno));
      exit(1);
    }

    if (ioctl(loop_fd, LOOP_SET_FD, img) == 0)
      break;

    dbg("LOOP_SET_FD %s: %s", loop_node, strerror(errno));
    if (errno != EBUSY)
      die("LOOP_SET_FD");

    close(loop_fd);
    loop_fd = -1;
  }

  close(img);
  close(ctl);

  if (loop_fd < 0) {
    fprintf(stderr, "no free loop device\n");
    exit(1);
  }

  dbg("attached %s -> %s", loop_node, distro_path);

  /* Cosmetic: makes `losetup -l` show the backing file, truncated to fit. */
  memset(&info, 0, sizeof(info));
  name_len = strlen(distro_path);
  if (name_len >= sizeof(info.lo_file_name))
    name_len = sizeof(info.lo_file_name) - 1;
  memcpy(info.lo_file_name, distro_path, name_len);
  ioctl(loop_fd, LOOP_SET_STATUS64, &info);
}

static void loop_detach(void)
{
  if (loop_fd < 0)
    return;

  if (ioctl(loop_fd, LOOP_CLR_FD, 0) != 0)
    fprintf(stderr, "detach %s: %s\n", loop_node, strerror(errno));
  else
    dbg("detached %s", loop_node);

  close(loop_fd);
  loop_fd = -1;
}

static int try_mount_rootfs(const char *type)
{
  if (mount(loop_node, chroot_path, type, 0, NULL) == 0) {
    dbg("mounted %s on %s as %s", loop_node, chroot_path, type);
    rootfs_mounted = 1;
    return 1;
  }

  dbg("try %s: %s", type, strerror(errno));

  /* Anything else means the type matched but the mount cannot work. */
  if (errno != EINVAL && errno != ENODEV && errno != ENXIO)
    die("mount rootfs");

  return 0;
}

/* mount(2) has no type autodetection, so replay what mount(8) would probe. */
static void mount_rootfs(void)
{
  static const char *const preferred[] = { "ext4", "ext3", "ext2", "f2fs" };
  char line[128];
  size_t i;
  FILE *fp;

  for (i = 0; i < sizeof(preferred) / sizeof(preferred[0]); i++)
    if (try_mount_rootfs(preferred[i]))
      return;

  fp = fopen("/proc/filesystems", "r");
  if (fp) {
    while (fgets(line, sizeof(line), fp)) {
      char *type = line;
      char *end;

      if (strncmp(type, "nodev", 5) == 0)
        continue;

      type += strspn(type, " \t");
      end = type + strcspn(type, " \t\r\n");
      *end = '\0';
      if (*type == '\0')
        continue;

      for (i = 0; i < sizeof(preferred) / sizeof(preferred[0]); i++)
        if (strcmp(type, preferred[i]) == 0)
          break;
      if (i < sizeof(preferred) / sizeof(preferred[0]))
        continue;

      if (try_mount_rootfs(type)) {
        fclose(fp);
        return;
      }
    }
    fclose(fp);
  }

  fprintf(stderr, "mount rootfs failed: unrecognized filesystem\n");
  exit(1);
}

static void mount_soft(const char *source, const char *target,
           const char *type, unsigned long flags, const char *data)
{
  if (mount(source, target, type, flags, data) != 0)
    fprintf(stderr, "mount %s: %s\n", target, strerror(errno));
  else
    dbg("mount ok: %s", target);
}

/* Symlinks are shown, not followed: from out here an absolute link resolves
   against the host, reporting a binary that execs fine after the chroot as
   missing. */
static void probe_path(const char *path)
{
  char target[PATH_MAX];
  struct stat st;
  ssize_t len;

  if (lstat(path, &st) != 0) {
    dbg("%s: %s", path, strerror(errno));
    return;
  }

  if (S_ISLNK(st.st_mode)) {
    len = readlink(path, target, sizeof(target) - 1);
    if (len < 0) {
      dbg("%s: readlink: %s", path, strerror(errno));
      return;
    }
    target[len] = '\0';
    dbg("%s -> %s", path, target);
    return;
  }

  dbg("%s: mode=%04o size=%lld", path, (unsigned)(st.st_mode & 07777),
      (long long)st.st_size);
}

/* What the image contains, for when the session binary fails to exec. */
static void probe_rootfs(void)
{
  struct dirent *ent;
  int shown = 0;
  DIR *dir;

  if (!debug_enabled)
    return;

  dir = opendir(chroot_path);
  if (!dir) {
    dbg("opendir %s: %s", chroot_path, strerror(errno));
    return;
  }

  while ((ent = readdir(dir)) != NULL && shown < 64) {
    if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
      continue;
    dbg("  rootfs: %s", ent->d_name);
    shown++;
  }
  closedir(dir);

  probe_path(cp(su_path));
  probe_path(cp("/bin/su"));
}

/* The target is created first, so a bind onto a path the image lacks works. */
static void mount_binds(void)
{
  size_t i;

  for (i = 0; i < bind_count; i++) {
    const char *target = cp(binds[i].guest);

    if (mkdir_p(target, NULL) != 0) {
      fprintf(stderr, "mkdir %s: %s\n", target, strerror(errno));
      continue;
    }
    mount_soft(binds[i].host, target, NULL, MS_BIND, NULL);
  }
}

/* Serialize runs sharing an image: mounting the same read-write image twice
   corrupts it. The lock rides on the open file description, so it releases
   even if this process crashes, and O_CLOEXEC keeps it out of the session. */
static void acquire_lock(void)
{
  int fd = open(distro_path, O_RDONLY | O_CLOEXEC);

  if (fd < 0)
    die(distro_path);

  if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
    if (errno == EWOULDBLOCK)
      fprintf(stderr, "%s: already in use by another rootlet\n",
        distro_path);
    else
      fprintf(stderr, "flock %s: %s\n", distro_path, strerror(errno));
    exit(1);
  }

  lock_fd = fd;
  dbg("locked %s", distro_path);
}

/* mountinfo escapes space, tab, newline and backslash as octal. */
static void unescape_octal(char *s)
{
  char *out = s;

  while (*s) {
    if (s[0] == '\\' && s[1] >= '0' && s[1] <= '3' &&
        s[2] >= '0' && s[2] <= '7' && s[3] >= '0' && s[3] <= '7') {
      *out++ = (char)(((s[1] - '0') << 6) | ((s[2] - '0') << 3) |
           (s[3] - '0'));
      s += 4;
      continue;
    }
    *out++ = *s++;
  }

  *out = '\0';
}

/* The image lock does not cover the mount point, so two runs with different
   images could stack on one. The teardown of whichever left first would then
   kill the other session's processes, which it matches by path. */
static int mount_point_busy(void)
{
  char line[PATH_MAX * 2];
  int busy = 0;
  FILE *fp;

  fp = fopen("/proc/self/mountinfo", "r");
  if (!fp)
    return 0;

  while (!busy && fgets(line, sizeof(line), fp)) {
    char *field = line;
    int i;

    /* The mount point is the fifth space separated field. */
    for (i = 0; i < 4; i++) {
      field += strcspn(field, " ");
      field += strspn(field, " ");
    }
    field[strcspn(field, " \r\n")] = '\0';

    unescape_octal(field);
    busy = strcmp(field, chroot_path) == 0;
  }

  fclose(fp);
  return busy;
}

static void do_mount(void)
{
  if (mount_point_busy()) {
    fprintf(stderr, "%s: already a mount point, unmount it first\n",
      chroot_path);
    exit(1);
  }

  loop_attach();

  if (mkdir_p(chroot_path, &chroot_created) != 0)
    die(chroot_path);

  mount_rootfs();

  /* Detach the tree from its peer group, so the mounts below only show up
     here and peers just see chroot_path itself. */
  mount_soft(NULL, chroot_path, NULL, MS_REC | MS_PRIVATE, NULL);

  mount_soft("/sys", cp("/sys"), NULL, MS_BIND, NULL);
  mount_soft("/dev", cp("/dev"), NULL, MS_BIND, NULL);
  mount_soft("/dev/pts", cp("/dev/pts"), NULL, MS_BIND, NULL);
  mount_soft("proc", cp("/proc"), "proc", 0, NULL);
  mount_soft("tmpfs", cp("/mnt"), "tmpfs", 0, "size=20%,mode=0755");
  mount_soft("tmpfs", cp("/tmp"), "tmpfs", 0, "size=50%,mode=1777");

  mount_binds();

  probe_rootfs();
}

static int umount_soft(const char *target)
{
  if (umount2(target, 0) != 0) {
    fprintf(stderr, "umount %s: %s\n", target, strerror(errno));
    return -1;
  }
  dbg("umount ok: %s", target);
  return 0;
}

/* mountinfo parsing shared with mount_point_busy(): the mount point is the
   fifth space separated field, octal-escaped. Written into "out" and returns
   1 when it falls strictly under chroot_path (chroot_path itself excluded,
   that one is unmounted separately once everything below it is clear). */
static int mount_entry_under_chroot(char *line, char *out, size_t out_size)
{
  char *field = line;
  int i;

  for (i = 0; i < 4; i++) {
    field += strcspn(field, " ");
    field += strspn(field, " ");
  }
  field[strcspn(field, " \r\n")] = '\0';
  unescape_octal(field);

  if (strncmp(field, chroot_path, chroot_len) != 0 || field[chroot_len] != '/')
    return 0;

  snprintf(out, out_size, "%s", field);
  return 1;
}

static int cmp_mount_len_desc(const void *a, const void *b)
{
  size_t la = strlen((const char *)a);
  size_t lb = strlen((const char *)b);

  return (la < lb) - (la > lb);
}

/* A session is free to mount something of its own inside the tree (no -p,
   or -p without CONFIG_PID_NS), so unmounting cannot rely on a fixed list of
   what rootlet itself made. Sweeps /proc/self/mountinfo for whatever is
   still under chroot_path and clears it, deepest first, repeating until a
   pass turns up nothing left. */
static void unmount_stragglers(void)
{
  char (*paths)[PATH_MAX] = NULL;
  size_t cap = 0;
  int pass;

  for (pass = 0; pass < 8; pass++) {
    char line[PATH_MAX * 2];
    size_t count = 0;
    FILE *fp;

    fp = fopen("/proc/self/mountinfo", "r");
    if (!fp)
      break;

    while (fgets(line, sizeof(line), fp)) {
      if (count == cap) {
        size_t next = cap ? cap * 2 : 16;
        char (*grown)[PATH_MAX] = realloc(paths, next * sizeof(*paths));

        if (!grown)
          break;
        paths = grown;
        cap = next;
      }
      if (mount_entry_under_chroot(line, paths[count], PATH_MAX))
        count++;
    }
    fclose(fp);

    if (count == 0)
      break;

    qsort(paths, count, PATH_MAX, cmp_mount_len_desc);

    int progressed = 0;

    for (size_t i = 0; i < count; i++) {
      dbg("straggler mount: %s", paths[i]);
      if (umount_soft(paths[i]) == 0)
        progressed = 1;
    }

    /* Nothing came loose this pass: retrying would just repeat the same
       errors, e.g. a directory that was already a mount point before rootlet
       ran, now shadowed under the rootfs and unreachable to unmount. */
    if (!progressed)
      break;
  }

  free(paths);
}

static void do_umount(void)
{
  unmount_stragglers();

  umount_soft(chroot_path);
}

/* Removes the mount point and every parent this run created for it. A
   directory that was already there, or that is not empty, simply stays. */
static void remove_mountpoint(void)
{
  char buf[PATH_MAX];
  size_t len = chroot_len;

  if (chroot_created > chroot_len)     /* nothing here was ours to remove */
    return;

  memcpy(buf, chroot_path, chroot_len + 1);

  for (;;) {
    buf[len] = '\0';
    if (rmdir(buf) != 0) {
      dbg("rmdir %s: %s", buf, strerror(errno));
      return;
    }

    while (len > 0 && buf[len - 1] != '/')
      len--;
    if (len < 2)
      return;
    len--;                      /* drop the separator */
    if (len < chroot_created)   /* this parent was not ours to remove */
      return;
  }
}

/* Registered with atexit(), so a die() once the image is attached still
   releases the loop device and the mounts. */
static void cleanup(void)
{
  if (cleanup_done)
    return;
  cleanup_done = 1;

  if (rootfs_mounted)
    do_umount();

  loop_detach();
  remove_mountpoint();
}

static int path_in_chroot(const char *path)
{
  /* Require a boundary after the prefix so the mount point does not also
     match a sibling like "/mnt/ubuntu-backup". */
  return strncmp(path, chroot_path, chroot_len) == 0 &&
         (path[chroot_len] == '/' || path[chroot_len] == '\0');
}

/* Matches on exe, root, cwd or any open fd: a helper exec'd from outside the
   tree, or one whose binary was unlinked, still pins the mount. "match" comes
   back holding the path that matched. */
static int belongs_to_chroot(pid_t pid, char *match, size_t match_size)
{
  static const char *const links[] = { "exe", "root", "cwd" };
  char path[64];
  struct dirent *ent;
  ssize_t len;
  DIR *dir;
  size_t i;

  for (i = 0; i < sizeof(links) / sizeof(links[0]); i++) {
    snprintf(path, sizeof(path), "/proc/%d/%s", (int)pid, links[i]);
    len = readlink(path, match, match_size - 1);
    if (len < 0)
      continue;
    match[len] = '\0';
    if (path_in_chroot(match))
      return 1;
  }

  snprintf(path, sizeof(path), "/proc/%d/fd", (int)pid);
  dir = opendir(path);
  if (!dir)
    return 0;

  while ((ent = readdir(dir)) != NULL) {
    if (ent->d_name[0] == '.')
      continue;

    len = readlinkat(dirfd(dir), ent->d_name, match, match_size - 1);
    if (len < 0)
      continue;
    match[len] = '\0';

    if (path_in_chroot(match)) {
      closedir(dir);
      return 1;
    }
  }

  closedir(dir);
  return 0;
}

/* Signal every match first, so they share one grace period. */
static void kill_chroot_processes(void)
{
  struct timespec grace = { TERM_GRACE_SECONDS, 0 };
  char match[PATH_MAX];
  pid_t self = getpid();
  pid_t *killed = NULL;
  size_t count = 0, cap = 0;
  struct dirent *ent;
  DIR *dir;

  dir = opendir("/proc");
  if (!dir)
    return;

  while ((ent = readdir(dir)) != NULL) {
    char *end;
    long pid;

    pid = strtol(ent->d_name, &end, 10);
    if (*end != '\0' || pid <= 0 || (pid_t)pid == self)
      continue;

    if (!belongs_to_chroot((pid_t)pid, match, sizeof(match)))
      continue;

    if (kill((pid_t)pid, SIGTERM) != 0)
      continue;

    dbg("SIGTERM %ld (%s)", pid, match);

    if (count == cap) {
      size_t next = cap ? cap * 2 : 64;
      pid_t *grown = realloc(killed, next * sizeof(*killed));

      if (!grown)
        continue;

      killed = grown;
      cap = next;
    }

    killed[count++] = (pid_t)pid;
  }

  closedir(dir);
  dbg("chroot processes signalled: %zu", count);

  if (count == 0) {
    free(killed);
    return;
  }

  while (nanosleep(&grace, &grace) != 0 && errno == EINTR)
    ;

  /* Re-check membership so a recycled PID is not hit with SIGKILL. */
  for (size_t i = 0; i < count; i++) {
    if (kill(killed[i], 0) != 0)
      continue;
    if (!belongs_to_chroot(killed[i], match, sizeof(match)))
      continue;

    kill(killed[i], SIGKILL);
    dbg("SIGKILL %d (%s)", (int)killed[i], match);
  }

  free(killed);
}

/* Only raise a flag: the teardown belongs on the main path, which the default
   disposition would skip. */
static void on_stop(int sig)
{
  stop_signal = sig;
}

static void on_session_exit(int sig)
{
  (void)sig;
  session_gone = 1;
}

/* So job control, ^C and ^Z belong to the inner shell, not the caller. */
static void open_pty(int *master, int *slave)
{
  char name[PATH_MAX];
  int fd;

  fd = posix_openpt(O_RDWR | O_NOCTTY);
  if (fd < 0)
    die("posix_openpt");
  if (grantpt(fd) != 0 || unlockpt(fd) != 0)
    die("unlockpt");
  if (ptsname_r(fd, name, sizeof(name)) != 0)
    die("ptsname_r");

  /* Opened before the chroot: the descriptor survives the root change,
     while the /dev/pts path need not resolve the same way afterwards. */
  *slave = open(name, O_RDWR | O_NOCTTY);
  if (*slave < 0)
    die(name);

  push_window_size(fd);
  dbg("session tty: %s", name);

  *master = fd;
}

/* So job control, ^C and ^Z belong to the inner shell, not the caller. */
static void attach_pty(int slave)
{
  if (setsid() < 0) {
    fprintf(stderr, "setsid: %s\n", strerror(errno));
    _exit(125);
  }

  if (ioctl(slave, TIOCSCTTY, 0) != 0) {
    fprintf(stderr, "TIOCSCTTY: %s\n", strerror(errno));
    _exit(125);
  }

  dup2(slave, STDIN_FILENO);
  dup2(slave, STDOUT_FILENO);
  dup2(slave, STDERR_FILENO);
  if (slave > STDERR_FILENO)
    close(slave);
}

static void relay(int master)
{
  struct timespec drain_until;
  struct pollfd fds[2];
  char buf[4096];
  int stdin_open = 1;
  int draining = 0;

  winch_install();

  for (;;) {
    int timeout = -1;
    ssize_t r;

    if (stop_signal)
      break;

    if (winch_pending) {
      winch_pending = 0;
      push_window_size(master);
    }

    /* The master reports end of file only once every slave is closed, and a
       leftover process can hold one open indefinitely. So once the session is
       gone, take what the pty still has and leave. */
    if (session_gone && !draining) {
      clock_gettime(CLOCK_MONOTONIC, &drain_until);
      drain_until.tv_nsec += SESSION_DRAIN_MS * 1000000L;
      if (drain_until.tv_nsec >= 1000000000L) {
        drain_until.tv_nsec -= 1000000000L;
        drain_until.tv_sec++;
      }
      draining = 1;
    }
    if (draining)
      timeout = ms_until(&drain_until);

    fds[0].fd = stdin_open ? STDIN_FILENO : -1;
    fds[0].events = POLLIN;
    fds[0].revents = 0;
    fds[1].fd = master;
    fds[1].events = POLLIN;
    fds[1].revents = 0;

    r = poll(fds, 2, timeout);
    if (r < 0) {
      if (errno == EINTR)
        continue;
      break;
    }
    if (r == 0)   /* only reachable while draining: nothing left to read */
      break;

    if (fds[1].revents) {
      r = read(master, buf, sizeof(buf));
      if (r < 0 && errno == EINTR)
        continue;
      /* The master reports EIO once the last slave closes. */
      if (r <= 0)
        break;
      if (write_all(STDOUT_FILENO, buf, (size_t)r) != 0)
        break;
    }

    if (fds[0].revents) {
      r = read(STDIN_FILENO, buf, sizeof(buf));
      if (r < 0 && errno == EINTR)
        continue;
      if (r <= 0)
        stdin_open = 0;
      else if (write_all(master, buf, (size_t)r) != 0)
        break;
    }
  }

  signal(SIGWINCH, SIG_DFL);
}

/* Never returns. */
static void exec_session(int slave)
{
  const char *base = strrchr(su_path, '/');
  char *const session_argv[] = { (char *)(base ? base + 1 : su_path),
                                 "-", NULL };

  signal(SIGINT, SIG_DFL);
  signal(SIGQUIT, SIG_DFL);

  attach_pty(slave);

  if (chdir(chroot_path) != 0 || chroot(chroot_path) != 0 ||
      chdir("/") != 0) {
    fprintf(stderr, "chroot %s: %s\n", chroot_path, strerror(errno));
    _exit(125);
  }

  unsetenv("LD_PRELOAD");
  unsetenv("LD_LIBRARY_PATH");

  dbg("chroot ok, exec %s", su_path);

  /* Straight to the syscall: Termux's libtermux-exec interposes execve() and
     rewrites the path to a prefix that does not exist inside the chroot. It is
     already mapped here, so clearing the environment cannot disarm it. */
  syscall(SYS_execve, su_path, session_argv, environ);

  /* execve reports ENOENT both for a missing binary and for a binary whose
     ELF interpreter is missing. */
  if (errno == ENOENT && access(su_path, F_OK) == 0)
    fprintf(stderr, "exec %s: file exists, its ELF interpreter is "
      "missing\n", su_path);
  else
    fprintf(stderr, "exec %s: %s\n", su_path, strerror(errno));
  _exit(126);
}

/* unshare(CLONE_NEWPID) places the *next* child in the new namespace as PID 1,
   hence the intermediate fork: it stays behind to reap PID 1 and forward its
   exit status. On failure this warns and returns, and the caller runs a plain
   session on the mount and pty already set up.

   The two unshares are separate, PID first, so giving up leaves this process
   untouched. Combined, they failed without CONFIG_PID_NS but still cost the
   caller the ability to resolve the mount point, and the plain session then
   died in chroot(). Returning after CLONE_NEWNS fails is fine as well: the PID
   namespace only takes effect for a child, and exec_session does not fork. */
static void enter_namespaces(int slave)
{
  pid_t leaf;
  int status;

  signal(SIGINT, SIG_IGN);
  signal(SIGQUIT, SIG_IGN);

  if (unshare(CLONE_NEWPID) != 0) {
    if (errno == EINVAL)
      fprintf(stderr, "unshare pid: %s (kernel built without "
        "CONFIG_PID_NS); continuing without namespace isolation\n",
        strerror(errno));
    else
      fprintf(stderr, "unshare pid: %s; continuing without namespace "
        "isolation\n", strerror(errno));
    return;
  }

  if (unshare(CLONE_NEWNS) != 0) {
    fprintf(stderr, "unshare mount: %s; continuing without namespace "
      "isolation\n", strerror(errno));
    return;
  }

  /* Keeps the proc remount below from propagating to the host. */
  if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0)
    fprintf(stderr, "make private: %s\n", strerror(errno));

  leaf = fork();
  if (leaf < 0) {
    fprintf(stderr, "fork: %s; continuing without namespace "
      "isolation\n", strerror(errno));
    return;
  }

  if (leaf > 0) {
    close(slave);
    while (waitpid(leaf, &status, 0) < 0 && errno == EINTR)
      ;
    if (WIFSIGNALED(status))
      _exit(128 + WTERMSIG(status));
    _exit(WIFEXITED(status) ? WEXITSTATUS(status) : 125);
  }

  /* leaf: PID 1 here. A proc mount reflects the mounter's PID namespace, so
     this one has to replace the host's. */
  if (mount("proc", cp("/proc"), "proc", 0, NULL) != 0)
    fprintf(stderr, "mount proc: %s\n", strerror(errno));
}

/* Returns the status to exit with: the session's own, or 128 + signal. */
static int run_session(void)
{
  struct sigaction chld;
  int master, slave;
  pid_t pid, done;
  int status;

  open_pty(&master, &slave);

  /* Before the fork, so a session that exits at once cannot leave relay()
     waiting on the pty. SA_NOCLDSTOP: merely stopped does not count. */
  memset(&chld, 0, sizeof(chld));
  chld.sa_handler = on_session_exit;
  chld.sa_flags = SA_NOCLDSTOP;
  sigemptyset(&chld.sa_mask);
  sigaction(SIGCHLD, &chld, NULL);

  pid = fork();
  if (pid < 0)
    die("fork");

  if (pid == 0) {
    close(master);
    /* Report through the pty from here on: the inherited stderr races with the
       raw mode the parent is about to enter, where a bare newline no longer
       returns the carriage. */
    dup2(slave, STDERR_FILENO);
    if (ns_enabled)
      enter_namespaces(slave);
    exec_session(slave);
  }

  /* Keep the cleanup path reachable when the session is interrupted. */
  signal(SIGINT, SIG_IGN);
  signal(SIGQUIT, SIG_IGN);

  /* The slave must not stay open on this side, or reading the master
     would never report end of file when the session exits. */
  close(slave);

  raw_mode_enter();
  relay(master);
  raw_mode_leave();
  close(master);

  /* After a stop signal the session is still running, and ending it is left
     to kill_chroot_processes(), so this must not block on it. */
  do {
    done = waitpid(pid, &status, stop_signal ? WNOHANG : 0);
  } while (done < 0 && errno == EINTR);

  signal(SIGCHLD, SIG_DFL);

  if (done < 0) {
    dbg("waitpid: %s", strerror(errno));
    return 125;
  }
  if (done == 0) {
    dbg("stopping on signal %d, session still running", (int)stop_signal);
    return 128 + (int)stop_signal;
  }
  if (WIFSIGNALED(status)) {
    dbg("session killed by signal %d", WTERMSIG(status));
    return 128 + WTERMSIG(status);
  }

  dbg("session exited with %d", WEXITSTATUS(status));
  return WEXITSTATUS(status);
}

/* do_mount() mounts a -b bind at the literal joined path, unchrooted in the
   host namespace, so a ".." component would resolve outside chroot_path
   entirely -- and, since unmount_stragglers() only clears mounts it finds
   under chroot_path, such a bind would then never get unmounted either. */
static int has_dotdot_component(const char *path)
{
  const char *p = path;

  while (*p) {
    size_t len = strcspn(p, "/");

    if (len == 2 && p[0] == '.' && p[1] == '.')
      return 1;
    p += len;
    if (*p == '/')
      p++;
  }

  return 0;
}

static void usage(const char *prog)
{
  fprintf(stderr,
    "usage: %s [-i image] [-m mountpoint] [-s login] "
    "[-b host[:guest]]... [-p] [-d]\n"
    "  -i image        distro image file (default $HOME/distro.img)\n"
    "  -m mountpoint   absolute path to mount and chroot into "
    "(default %s)\n"
    "  -s login        login program run inside the chroot (default %s)\n"
    "  -b host[:guest] bind host path into the chroot at guest\n"
    "                  (guest defaults to host); repeatable\n"
    "  -p              run the session in new PID and mount namespaces\n"
    "  -d              enable debug output (same as DEBUG=1)\n",
    prog, DEFAULT_MOUNT, DEFAULT_SU);
}

int main(int argc, char **argv)
{
  const char *home = getenv("HOME");
  const char *image = NULL;
  const char *mount_at = DEFAULT_MOUNT;
  size_t len;
  int status;
  int opt;

  debug_enabled = getenv("DEBUG") != NULL;

  while ((opt = getopt(argc, argv, "i:m:s:b:pdh")) != -1) {
    switch (opt) {
    case 'i':
      image = optarg;
      break;
    case 'm':
      mount_at = optarg;
      break;
    case 's':
      su_path = optarg;
      break;
    case 'b': {
      char *sep = strchr(optarg, ':');

      if (bind_count >= MAX_BINDS) {
        fprintf(stderr, "too many -b binds (max %d)\n", MAX_BINDS);
        return 1;
      }
      if (sep)
        *sep = '\0';
      binds[bind_count].host = optarg;
      binds[bind_count].guest = sep ? sep + 1 : optarg;
      if (binds[bind_count].host[0] == '\0') {
        fprintf(stderr, "bind source is empty\n");
        return 1;
      }
      if (binds[bind_count].guest[0] != '/') {
        fprintf(stderr, "bind target must be absolute: %s\n",
          binds[bind_count].guest);
        return 1;
      }
      if (has_dotdot_component(binds[bind_count].guest)) {
        fprintf(stderr, "bind target must not contain '..': %s\n",
          binds[bind_count].guest);
        return 1;
      }
      bind_count++;
      break;
    }
    case 'p':
      ns_enabled = 1;
      break;
    case 'd':
      debug_enabled = 1;
      break;
    case 'h':
      usage(argv[0]);
      return 0;
    default:
      usage(argv[0]);
      return 2;
    }
  }

  if (image) {
    if ((size_t)snprintf(distro_path, sizeof(distro_path), "%s", image) >=
        sizeof(distro_path)) {
      fprintf(stderr, "distro image path is too long\n");
      return 1;
    }
  } else {
    if (!home || !*home) {
      fprintf(stderr, "HOME is not set and no image given (-i)\n");
      return 1;
    }
    if ((size_t)snprintf(distro_path, sizeof(distro_path), "%s/distro.img",
             home) >= sizeof(distro_path)) {
      fprintf(stderr, "distro image path is too long\n");
      return 1;
    }
  }

  /* Absolute because the cleanup scan matches it against /proc/<pid> links;
     trailing slashes dropped so joins and the boundary check stay exact. */
  if (mount_at[0] != '/') {
    fprintf(stderr, "mount point must be absolute: %s\n", mount_at);
    return 1;
  }
  len = strlen(mount_at);
  while (len > 1 && mount_at[len - 1] == '/')
    len--;
  if (len < 2) {
    fprintf(stderr, "invalid mount point: %s\n", mount_at);
    return 1;
  }
  if (len >= sizeof(chroot_path)) {
    fprintf(stderr, "mount point is too long\n");
    return 1;
  }
  memcpy(chroot_path, mount_at, len);
  chroot_path[len] = '\0';
  chroot_len = len;

  dbg("image=%s", distro_path);
  dbg("mount=%s", chroot_path);
  dbg("login=%s", su_path);

  acquire_lock();

  if (atexit(cleanup) != 0) {
    fprintf(stderr, "cannot register the teardown\n");
    return 1;
  }
  signal(SIGTERM, on_stop);
  signal(SIGHUP, on_stop);

  do_mount();
  status = run_session();
  kill_chroot_processes();
  cleanup();

  if (lock_fd >= 0)
    close(lock_fd);

  /* Die from the stop signal the way a program without a handler would, now
     that the teardown it would have skipped is done. */
  if (stop_signal) {
    signal(stop_signal, SIG_DFL);
    raise(stop_signal);
  }

  return status;
}
