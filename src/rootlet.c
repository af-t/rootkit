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

#define DEFAULT_SU "/usr/bin/su"
#define TERM_GRACE_SECONDS 3
#define SESSION_DRAIN_MS 200
#define MAX_BINDS 64

/* A -b bind; "guest" is the path inside the tree, defaulting to "host".
   A regular-file host is a filesystem image to loop-mount rather than a
   path to bind. Binds are extra mounts only: the root itself always comes
   from -i, so "/" is rejected as a guest. */
struct bind {
  const char *host;
  const char *guest;
  int is_image;
};

static char distro_path[PATH_MAX];
/* -i source:subpath, without the slashes: empty when no subpath given. */
static char image_sub[256];
/* -i names a directory bound at the root instead of an image file. */
static int image_is_dir;
/* Short enough that every subpath cp() appends still fits in PATH_MAX. */
static char chroot_path[PATH_MAX - 256];
static size_t chroot_len;
/* The session root: chroot_path itself, or one level down with
   -i source:subpath. cp() targets live here; the teardown still sweeps
   the whole tree at chroot_path. */
static char setup_path[PATH_MAX];
/* Shallowest directory the teardown may remove, and SIZE_MAX while this run
   has created none: a mount point that was already there is left alone. */
static size_t chroot_created = SIZE_MAX;
static const char *su_path = DEFAULT_SU;
static struct bind binds[MAX_BINDS];
static size_t bind_count;
/* One loop device per image: -i plus at most one per -b. Detached in
   reverse attach order by loop_detach_all(). */
#define MAX_LOOPS (MAX_BINDS + 1)
static char loop_nodes[MAX_LOOPS][32];
static int loop_fds[MAX_LOOPS];
static size_t loop_count;
/* Flocked image files, held open until exit so the exclusion outlives a
   crash. Anything else needs no lock file: once its mounts are in the
   tree, a second run already aborts on them. */
static const char *locked_paths[MAX_LOOPS + 1];
static int lock_fds[MAX_LOOPS + 1];
static size_t lock_count;
static int ns_enabled;
static int debug_enabled;
static int rootfs_mounted;
static int cleanup_done;
static volatile sig_atomic_t session_gone;
static volatile sig_atomic_t stop_signal;

/* Build "<setup_path><sub>" into one of a few rotating buffers, so one
   expression can hold several results. */
static const char *cp(const char *sub)
{
  static char bufs[4][PATH_MAX];
  static unsigned turn;
  char *buf = bufs[turn++ % (sizeof(bufs) / sizeof(bufs[0]))];

  snprintf(buf, PATH_MAX, "%s%s", setup_path, sub);
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
static int open_loop_node(int number, char *node, size_t node_size)
{
  static const char *const dirs[] = { "/dev/block", "/dev" };
  struct timespec delay = { 0, 100 * 1000 * 1000 };
  int attempt;
  size_t i;

  for (attempt = 0; attempt < 5; attempt++) {
    for (i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
      int fd;

      snprintf(node, node_size, "%s/loop%d",
         dirs[i], number);

      fd = open(node, O_RDWR | O_CLOEXEC);
      if (fd >= 0)
        return fd;

      dbg("open %s: %s", node, strerror(errno));
      if (errno != ENOENT)
        return -1;
    }

    nanosleep(&delay, NULL);
  }

  snprintf(node, node_size, "loop%d", number);
  errno = ENOENT;
  return -1;
}

/* Attaches "img_path" to a free loop device: the node path goes into
   "node" and the loop fd is returned, or -1 on failure after reporting
   it. The caller decides whether that is fatal (the root) or a warning
   (an extra -b). */
static int attach_image(const char *img_path, char *node, size_t node_size)
{
  struct loop_info64 info;
  size_t name_len;
  int ctl, img, fd = -1, attempt;

  ctl = open("/dev/loop-control", O_RDWR | O_CLOEXEC);
  if (ctl < 0) {
    fprintf(stderr, "open /dev/loop-control: %s\n", strerror(errno));
    return -1;
  }

  img = open(img_path, O_RDWR | O_CLOEXEC);
  if (img < 0) {
    fprintf(stderr, "%s: %s\n", img_path, strerror(errno));
    close(ctl);
    return -1;
  }

  /* GET_FREE only reserves a number; a racing attach can take it first. */
  for (attempt = 0; attempt < 16 && fd < 0; attempt++) {
    int number;

    number = ioctl(ctl, LOOP_CTL_GET_FREE);
    if (number < 0) {
      fprintf(stderr, "LOOP_CTL_GET_FREE: %s\n", strerror(errno));
      break;
    }

    dbg("LOOP_CTL_GET_FREE -> %d", number);

    fd = open_loop_node(number, node, node_size);
    if (fd < 0) {
      fprintf(stderr, "%s: %s (tried /dev/block and /dev)\n",
        node, strerror(errno));
      break;
    }

    if (ioctl(fd, LOOP_SET_FD, img) == 0)
      break;

    dbg("LOOP_SET_FD %s: %s", node, strerror(errno));
    if (errno != EBUSY) {
      fprintf(stderr, "LOOP_SET_FD: %s\n", strerror(errno));
      close(fd);
      fd = -1;
      break;
    }

    close(fd);
    fd = -1;
  }

  close(img);
  close(ctl);

  if (fd < 0) {
    if (attempt >= 16)
      fprintf(stderr, "no free loop device\n");
    return -1;
  }

  dbg("attached %s -> %s", node, img_path);

  /* Cosmetic: makes `losetup -l` show the backing file, truncated to fit. */
  memset(&info, 0, sizeof(info));
  name_len = strlen(img_path);
  if (name_len >= sizeof(info.lo_file_name))
    name_len = sizeof(info.lo_file_name) - 1;
  memcpy(info.lo_file_name, img_path, name_len);
  ioctl(fd, LOOP_SET_STATUS64, &info);

  return fd;
}

static void track_loop(int fd, const char *node)
{
  if (loop_count >= MAX_LOOPS) {
    fprintf(stderr, "too many attached images (max %d)\n", MAX_LOOPS);
    exit(1);
  }

  snprintf(loop_nodes[loop_count], sizeof(loop_nodes[loop_count]), "%s",
    node);
  loop_fds[loop_count] = fd;
  loop_count++;
}

static void loop_detach_all(void)
{
  while (loop_count > 0) {
    int fd = loop_fds[--loop_count];
    const char *node = loop_nodes[loop_count];

    if (ioctl(fd, LOOP_CLR_FD, 0) != 0)
      fprintf(stderr, "detach %s: %s\n", node, strerror(errno));
    else
      dbg("detached %s", node);

    close(fd);
  }
}

/* 1 when mounted, 0 when the type simply does not match, -1 on a hard
   failure (already reported). */
static int try_mount_type(const char *node, const char *target,
             const char *type)
{
  if (mount(node, target, type, 0, NULL) == 0) {
    dbg("mounted %s on %s as %s", node, target, type);
    return 1;
  }

  dbg("try %s on %s: %s", type, target, strerror(errno));

  /* Anything else means the type matched but the mount cannot work. */
  if (errno != EINVAL && errno != ENODEV && errno != ENXIO) {
    fprintf(stderr, "mount %s: %s\n", target, strerror(errno));
    return -1;
  }

  return 0;
}

/* mount(2) has no type autodetection, so replay what mount(8) would probe.
   Returns 0 on success, -1 on failure (already reported). */
static int mount_image_at(const char *node, const char *target)
{
  static const char *const preferred[] = {
    "ext4", "ext3", "ext2", "f2fs", "erofs", "squashfs"
  };
  char line[128];
  size_t i;
  int r;
  FILE *fp;

  for (i = 0; i < sizeof(preferred) / sizeof(preferred[0]); i++) {
    r = try_mount_type(node, target, preferred[i]);
    if (r != 0)
      return r > 0 ? 0 : -1;
  }

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

      r = try_mount_type(node, target, type);
      if (r != 0) {
        fclose(fp);
        return r > 0 ? 0 : -1;
      }
    }
    fclose(fp);
  }

  fprintf(stderr, "mount %s: unrecognized filesystem\n", target);
  return -1;
}

static int mount_soft(const char *source, const char *target,
            const char *type, unsigned long flags, const char *data)
{
  if (mount(source, target, type, flags, data) != 0) {
    fprintf(stderr, "mount %s: %s\n", target, strerror(errno));
    return -1;
  }

  dbg("mount ok: %s", target);
  return 0;
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

  dir = opendir(setup_path);
  if (!dir) {
    dbg("opendir %s: %s", setup_path, strerror(errno));
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

/* Mounts one -b entry: a regular-file host is a filesystem image and gets
   loop-mounted, anything else is bound. The target is created first, so a
   bind onto a path the image lacks works. Returns 0 on success; a failed
   extra only warns through the calls below. */
static int mount_one_bind(const struct bind *b)
{
  const char *target = cp(b->guest);

  if (mkdir_p(target, NULL) != 0) {
    fprintf(stderr, "mkdir %s: %s\n", target, strerror(errno));
    return -1;
  }

  if (!b->is_image)
    return mount_soft(b->host, target, NULL, MS_BIND, NULL);

  {
    char node[32];
    int fd = attach_image(b->host, node, sizeof(node));

    if (fd < 0)
      return -1;
    track_loop(fd, node);
    return mount_image_at(node, target);
  }
}

/* Serialize runs sharing an image: mounting the same read-write image twice
   corrupts it. The lock rides on the open file description, so it releases
   even if this process crashes, and O_CLOEXEC keeps it out of the session.
   The same path locked twice by one run (e.g. -i plus an identical -b) is
   locked once. */
static void lock_image_file(const char *path)
{
  size_t i;
  int fd;

  for (i = 0; i < lock_count; i++)
    if (strcmp(locked_paths[i], path) == 0)
      return;

  if (lock_count >= sizeof(lock_fds) / sizeof(lock_fds[0])) {
    fprintf(stderr, "too many locked files\n");
    exit(1);
  }

  fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    die(path);

  if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
    if (errno == EWOULDBLOCK)
      fprintf(stderr, "%s: already in use by another rootlet\n",
        path);
    else
      fprintf(stderr, "flock %s: %s\n", path, strerror(errno));
    exit(1);
  }

  locked_paths[lock_count] = path;
  lock_fds[lock_count] = fd;
  lock_count++;
  dbg("locked %s", path);
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

/* A run tears down every mount at or under chroot_path on exit, so the
   tree must hold none when it starts: mounts owned by someone else would
   be torn down too and conflict. Returns 1 when chroot_path itself is a
   mount point, 2 when something is mounted strictly beneath it, 0 when
   the tree is clear. Reads /proc/self/mountinfo directly. */
static int tree_has_mounts(void)
{
  char line[PATH_MAX * 2];
  int exact = 0;
  int beneath = 0;
  FILE *fp;

  fp = fopen("/proc/self/mountinfo", "r");
  if (!fp)
    return 0;

  while (fgets(line, sizeof(line), fp)) {
    char *field = line;
    int i;

    /* The mount point is the fifth space separated field. */
    for (i = 0; i < 4; i++) {
      field += strcspn(field, " ");
      field += strspn(field, " ");
    }
    field[strcspn(field, " \r\n")] = '\0';

    unescape_octal(field);
    if (strcmp(field, chroot_path) == 0) {
      exact = 1;
      break;
    }
    if (strncmp(field, chroot_path, chroot_len) == 0 &&
        field[chroot_len] == '/')
      beneath = 1;
  }

  fclose(fp);
  return exact ? 1 : beneath ? 2 : 0;
}

static void do_mount(void)
{
  size_t i;
  char node[32];
  int fd;

  if (mkdir_p(chroot_path, &chroot_created) != 0)
    die(chroot_path);

  /* The tree arrives clean (checked in main) and holds no root yet: -i
     provides one (an image file loop-mounted, or a directory bound),
     otherwise -b binds assemble the tree in place. Failing the root
     setup is fatal, with the reason already reported above. Loop
     tracking comes first so the teardown still detaches the device. */
  if (distro_path[0] != '\0' && !image_is_dir) {
    fd = attach_image(distro_path, node, sizeof(node));
    if (fd < 0)
      exit(1);
    track_loop(fd, node);
    if (mount_image_at(node, chroot_path) != 0)
      exit(1);
    rootfs_mounted = 1;
  }

  /* No image backing the root means no image file to flock, and none is
     needed: the mounts below already mark the tree, so a second run
     aborts on them. */
  if (image_is_dir) {
    if (mount(distro_path, chroot_path, NULL, MS_BIND, NULL) != 0) {
      fprintf(stderr, "bind %s -> %s: %s\n", distro_path, chroot_path,
        strerror(errno));
      exit(1);
    }
    /* Like an image mount, the bind at the root itself is not covered by
       the straggler sweep under it, so the teardown must drop it too. */
    rootfs_mounted = 1;
  }

  /* With -i source:subpath the session lives one level down: the standard
     mounts, the -b binds and the chroot all target it, while the teardown
     still sweeps the whole tree. */
  if (image_sub[0] != '\0') {
    if ((size_t)snprintf(setup_path, sizeof(setup_path), "%s/%s",
          chroot_path, image_sub) >= sizeof(setup_path)) {
      fprintf(stderr, "setup path is too long\n");
      exit(1);
    }
    if (mkdir_p(setup_path, NULL) != 0)
      die(setup_path);
  } else if ((size_t)snprintf(setup_path, sizeof(setup_path), "%s",
        chroot_path) >= sizeof(setup_path)) {
    fprintf(stderr, "setup path is too long\n");
    exit(1);
  }

  /* Detach the tree from its peer group, so the mounts below only show up
     here and peers just see chroot_path itself. */
  mount_soft(NULL, chroot_path, NULL, MS_REC | MS_PRIVATE, NULL);

  /* The image may lack them and a bare -m always does: the standard
     targets are created first, so their mounts do not fail with ENOENT. */
  {
    static const char *const std_dirs[] = {
      "/sys", "/dev", "/dev/pts", "/proc", "/mnt", "/tmp",
    };

    for (i = 0; i < sizeof(std_dirs) / sizeof(std_dirs[0]); i++)
      mkdir_p(cp(std_dirs[i]), NULL);
  }

  mount_soft("/sys", cp("/sys"), NULL, MS_BIND, NULL);
  mount_soft("/dev", cp("/dev"), NULL, MS_BIND, NULL);
  mount_soft("/dev/pts", cp("/dev/pts"), NULL, MS_BIND, NULL);
  mount_soft("proc", cp("/proc"), "proc", 0, NULL);
  mount_soft("tmpfs", cp("/mnt"), "tmpfs", 0, "size=20%,mode=0755");
  mount_soft("tmpfs", cp("/tmp"), "tmpfs", 0, "size=50%,mode=1777");

  for (i = 0; i < bind_count; i++)
    mount_one_bind(&binds[i]);

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

/* mountinfo parsing shared with tree_has_mounts(): the mount point is the
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

/* Registered with atexit(), so a die() once the setup is underway still
   releases the loop devices and the mounts. The straggler sweep runs
   unconditionally; the root mount itself (-i image or directory) is
   dropped right after, since the sweep only covers what is under it. */
static void cleanup(void)
{
  if (cleanup_done)
    return;
  cleanup_done = 1;

  unmount_stragglers();

  if (rootfs_mounted)
    umount_soft(chroot_path);

  loop_detach_all();
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

  if (chdir(setup_path) != 0 || chroot(setup_path) != 0 ||
      chdir("/") != 0) {
    fprintf(stderr, "chroot %s: %s\n", setup_path, strerror(errno));
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
    "usage: %s -m mountpoint [-i source[:subpath]] [-s login] "
    "[-b host[:guest]]... [-p] [-d]\n"
    "  -m mountpoint   absolute path used as the container root (required;\n"
    "                  must hold no mounts when the run starts)\n"
    "  -i source       root source (optional): an image file loop-mounted\n"
    "                  at the root, or a directory bind-mounted there;\n"
    "                  with :subpath the session is set up and chrooted\n"
    "                  at <root>/subpath instead\n"
    "  -s login        login program run inside the chroot (default %s)\n"
    "  -b host[:guest] bind host path into the chroot at guest\n"
    "                  (guest defaults to host); the guest must not be /\n"
    "                  (the root comes from -i, never from a bind); a\n"
    "                  regular-file host is loop-mounted as an image\n"
    "                  instead of bound; repeatable\n"
    "  -p              run the session in new PID and mount namespaces\n"
    "  -d              enable debug output (same as DEBUG=1)\n",
    prog, DEFAULT_SU);
}

int main(int argc, char **argv)
{
  const char *image = NULL;
  const char *mount_at = NULL;
  size_t len, i;
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
      struct stat st;

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
      /* Normalize like the mount point itself. The guest is part of
         optarg, so it is mutable. */
      {
        char *guest = (char *)binds[bind_count].guest;
        size_t glen = strlen(guest);

        while (glen > 1 && guest[glen - 1] == '/')
          guest[--glen] = '\0';
      }
      /* The root comes from -i, never from a bind: "/" would stack a
         mount exactly where the image (or nothing) belongs. */
      if (strcmp(binds[bind_count].guest, "/") == 0) {
        fprintf(stderr, "bind target must not be '/': the root comes "
          "from -i\n");
        return 1;
      }
      /* A regular file is not something to bind: it is a filesystem
         image to loop-mount (e.g. an erofs/squashfs file). Anything
         stat cannot see stays a bind and fails, softly, at mount. */
      binds[bind_count].is_image =
        stat(binds[bind_count].host, &st) == 0 && S_ISREG(st.st_mode);
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

  /* The root is mandatory; without it there is nothing to enter. */
  if (!mount_at) {
    usage(argv[0]);
    return 1;
  }

  /* -i is optional and has no default: an image file loop-mounted at the
     root, or a directory bound there instead; an appended :subpath moves
     the session setup and the chroot one level down. Plain -m with
     neither is fine too: the tree then holds just the standard mounts. */
  if (image) {
    const char *sep = strchr(image, ':');
    size_t srclen = sep ? (size_t)(sep - image) : strlen(image);
    struct stat st;

    if (srclen == 0 || srclen >= sizeof(distro_path)) {
      fprintf(stderr, "bad -i source: %s\n", image);
      return 1;
    }
    memcpy(distro_path, image, srclen);
    distro_path[srclen] = '\0';

    if (sep) {
      const char *sub = sep + 1;
      size_t sublen;

      while (*sub == '/')
        sub++;
      sublen = strlen(sub);
      while (sublen > 0 && sub[sublen - 1] == '/')
        sublen--;
      if (sublen >= sizeof(image_sub)) {
        fprintf(stderr, "-i subpath is too long\n");
        return 1;
      }
      memcpy(image_sub, sub, sublen);
      image_sub[sublen] = '\0';
      if (image_sub[0] != '\0' && has_dotdot_component(image_sub)) {
        fprintf(stderr, "-i subpath must not contain '..': %s\n", image);
        return 1;
      }
    }

    image_is_dir = stat(distro_path, &st) == 0 && S_ISDIR(st.st_mode);
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

  /* The teardown sweeps every mount at or under the tree, so a tree that
     already holds mounts is refused before anything is locked or made. */
  switch (tree_has_mounts()) {
  case 1:
    fprintf(stderr, "%s: already a mount point, unmount it first\n",
      chroot_path);
    return 1;
  case 2:
    fprintf(stderr, "%s: contains mounts, unmount them first\n",
      chroot_path);
    return 1;
  }

  dbg("image=%s", distro_path);
  dbg("mount=%s", chroot_path);
  dbg("login=%s", su_path);

  /* Every image this run mounts is locked before anything else, so a
     second run fails instead of mounting the same image twice. */
  if (image && !image_is_dir)
    lock_image_file(distro_path);
  for (i = 0; i < bind_count; i++)
    if (binds[i].is_image)
      lock_image_file(binds[i].host);

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

  for (i = 0; i < lock_count; i++)
    close(lock_fds[i]);

  /* Die from the stop signal the way a program without a handler would, now
     that the teardown it would have skipped is done. */
  if (stop_signal) {
    signal(stop_signal, SIG_DFL);
    raise(stop_signal);
  }

  return status;
}
