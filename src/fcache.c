#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE
#define FUSE_USE_VERSION 35

#include <fuse3/fuse.h>
#include <fuse3/fuse_lowlevel.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <dirent.h>
#include <libgen.h>
#include <limits.h>
#include <pthread.h>
#include <stddef.h>
#include <signal.h>
#include <sys/mount.h>

struct fcache_mount_opts {
    int allow_other;
    int default_permissions;
    uid_t uid;
    gid_t gid;
    mode_t umask;
    int uid_set;
    int gid_set;
    int umask_set;
};

struct fcache_config {
    char cache_dir[PATH_MAX];
    char remote_dir[PATH_MAX];
    struct fcache_mount_opts mount_opts;
};

struct fcache_file_handle {
    int cache_fd;
    int remote_fd;
    off_t file_size;
    char *remote_path;
    char *cache_path;
    char tmp_path[PATH_MAX];
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int caching_done;
    int complete;
    int release_called;
    int is_streaming;
    int is_cache_backed;
    pthread_t cache_thread;
};

static void usage(const char *prog) {
    fprintf(stderr,
        "usage: %s <cache_dir> <remote_dir> <mountpoint> [options]\n"
        "\n"
        "Options:\n"
        "  -f              run in foreground (repeat Ctrl+C to force quit if stuck)\n"
        "  -d              enable debug output (same as -odebug)\n"
        "  -o opt[,opt...] mount options (comma-separated):\n"
        "                  allow_other         allow access to other users\n"
        "                  default_permissions enable permission checking by kernel\n"
        "                  uid=N               set mount point owner uid\n"
        "                  gid=N               set mount point owner gid\n"
        "                  umask=NNN           file creation mask, overrides caller umask\n"
        "  -h              show this help\n",
        prog);
}

static int parse_mount_opts(const char *optstr, struct fcache_mount_opts *opts) {
    char *copy = strdup(optstr);
    if (!copy) return -ENOMEM;

    char *token = strtok(copy, ",");
    while (token) {
        if (strcmp(token, "allow_other") == 0) {
            opts->allow_other = 1;
        } else if (strcmp(token, "default_permissions") == 0) {
            opts->default_permissions = 1;
        } else if (strncmp(token, "uid=", 4) == 0) {
            opts->uid = (uid_t)strtoul(token + 4, NULL, 10);
            opts->uid_set = 1;
        } else if (strncmp(token, "gid=", 4) == 0) {
            opts->gid = (gid_t)strtoul(token + 4, NULL, 10);
            opts->gid_set = 1;
        } else if (strncmp(token, "umask=", 6) == 0) {
            opts->umask = (mode_t)strtoul(token + 6, NULL, 8);
            opts->umask_set = 1;
        } else {
            fprintf(stderr, "fcache: unknown mount option: %s\n", token);
            free(copy);
            return -EINVAL;
        }
        token = strtok(NULL, ",");
    }

    free(copy);
    return 0;
}

[[nodiscard]] static struct fcache_config *get_config(void) {
    return (struct fcache_config *) fuse_get_context()->private_data;
}

[[nodiscard]] static int build_path(char *dest, size_t size, const char *base, const char *path) {
    if (strstr(path, "..")) {
        return -EPERM;
    }
    snprintf(dest, size, "%s%s", base, path);
    return 0;
}

[[nodiscard]] static int mkdir_p(const char *path, mode_t mode) {
    char tmp[PATH_MAX];
    char *p = NULL;
    size_t len;

    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    if (tmp[len - 1] == '/')
        tmp[len - 1] = 0;

    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            if (mkdir(tmp, mode) != 0 && errno != EEXIST) {
                return -errno;
            }
            *p = '/';
        }
    }
    if (mkdir(tmp, mode) != 0 && errno != EEXIST) {
        return -errno;
    }
    return 0;
}

static void create_parent_dirs(const char *file_path) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", file_path);
    char *dir = dirname(tmp);
    (void)mkdir_p(dir, 0755);
}

[[nodiscard]] static mode_t effective_umask(struct fcache_config *cfg) {
    if (cfg->mount_opts.umask_set) {
        return cfg->mount_opts.umask;
    }
    return fuse_get_context()->umask;
}

[[nodiscard]] static void *cache_worker(void *arg) {
    struct fcache_file_handle *fh = (struct fcache_file_handle *)arg;
    char buf[128 * 1024];
    ssize_t bytes_read;
    off_t offset = 0;
    int complete = 0;

    while ((bytes_read = pread(fh->remote_fd, buf, sizeof(buf), offset)) > 0) {
        pthread_mutex_lock(&fh->lock);
        if (fh->cache_fd >= 0) {
            if (pwrite(fh->cache_fd, buf, (size_t)bytes_read, offset) < 0) {
                pthread_mutex_unlock(&fh->lock);
                break;
            }
        }
        pthread_mutex_unlock(&fh->lock);
        offset += bytes_read;
    }
    if (bytes_read == 0) {
        complete = 1;
    }

    // Sole finalizer: release() never joins, so teardown can't stall on
    // slow remotes. Wait for release (cond wait, not I/O), then publish.
    pthread_mutex_lock(&fh->lock);
    fh->caching_done = 1;
    fh->complete = complete;
    while (!fh->release_called) {
        pthread_cond_wait(&fh->cond, &fh->lock);
    }
    int cfd = fh->cache_fd;
    fh->cache_fd = -1;
    int rfd = fh->remote_fd;
    fh->remote_fd = -1;
    int publish = fh->complete;
    char tmp[PATH_MAX], dst[PATH_MAX];
    memcpy(tmp, fh->tmp_path, sizeof(tmp));
    memcpy(dst, fh->cache_path, sizeof(dst));
    char *rpath = fh->remote_path;
    char *cpath = fh->cache_path;
    fh->remote_path = NULL;
    fh->cache_path = NULL;
    pthread_mutex_unlock(&fh->lock);

    if (cfd >= 0) {
        if (publish) {
            if (rename(tmp, dst) != 0) {
                unlink(tmp);
            }
        } else {
            unlink(tmp);
        }
        close(cfd);
    }
    if (rfd >= 0) {
        close(rfd);
    }
    pthread_cond_destroy(&fh->cond);
    pthread_mutex_destroy(&fh->lock);
    free(rpath);
    free(cpath);
    free(fh);
    return NULL;
}

static void sweep_stale_tmps(const char *cache_path) {
    const char *base = strrchr(cache_path, '/');
    base = base ? base + 1 : cache_path;

    char dir[PATH_MAX], prefix[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", cache_path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = 0;
    } else {
        return;
    }
    int n = snprintf(prefix, sizeof(prefix), "%s.tmp.%d.", base, getpid());
    if (n < 0 || (size_t)n >= sizeof(prefix)) {
        return;
    }
    size_t plen = (size_t)n;

    DIR *dp = opendir(dir);
    if (!dp) {
        return;
    }
    struct dirent *de;
    while ((de = readdir(dp)) != NULL) {
        if (strncmp(de->d_name, prefix, plen) != 0) {
            continue;
        }
        char victim[PATH_MAX];
        int m = snprintf(victim, sizeof(victim), "%s/%s", dir, de->d_name);
        if (m < 0 || (size_t)m >= sizeof(victim)) {
            continue;
        }
        unlink(victim);
    }
    closedir(dp);
}

[[nodiscard]] static int start_background_cache(struct fcache_file_handle *fh, mode_t src_mode) {
    int n = snprintf(fh->tmp_path, sizeof(fh->tmp_path), "%s.tmp.%d.%p",
                     fh->cache_path, getpid(), (void *)fh);
    if (n < 0 || (size_t)n >= sizeof(fh->tmp_path)) {
        return -ENAMETOOLONG;
    }

    create_parent_dirs(fh->cache_path);
    sweep_stale_tmps(fh->cache_path);

    mode_t tmp_mode = (src_mode & 0777) != 0 ? (src_mode & 0777) : 0644;
    int fd = open(fh->tmp_path, O_WRONLY | O_CREAT | O_TRUNC, tmp_mode);
    if (fd < 0) return -errno;

    pthread_mutex_lock(&fh->lock);
    fh->cache_fd = fd;
    fh->caching_done = 0;
    fh->is_streaming = 1;
    fh->release_called = 0;
    pthread_mutex_unlock(&fh->lock);

    if (pthread_create(&fh->cache_thread, NULL, cache_worker, fh) != 0) {
        int err = errno;
        close(fd);
        pthread_mutex_lock(&fh->lock);
        fh->cache_fd = -1;
        pthread_mutex_unlock(&fh->lock);
        unlink(fh->tmp_path);
        return -err;
    }
    return 0;
}

[[nodiscard]] static struct fcache_file_handle *new_direct_handle(int fd,
        const char *rpath, const char *cpath, int is_cache_backed) {
    struct fcache_file_handle *fh = calloc(1, sizeof(*fh));
    if (!fh) return NULL;
    fh->cache_fd = fd;
    fh->remote_fd = -1;
    fh->caching_done = 1;
    fh->is_streaming = 0;
    fh->is_cache_backed = is_cache_backed;
    fh->remote_path = strdup(rpath);
    fh->cache_path = strdup(cpath);
    if (!fh->remote_path || !fh->cache_path) {
        free(fh->remote_path);
        free(fh->cache_path);
        free(fh);
        return NULL;
    }
    pthread_mutex_init(&fh->lock, NULL);
    return fh;
}

static void free_handle(struct fcache_file_handle *fh) {
    if (!fh) return;
    free(fh->remote_path);
    free(fh->cache_path);
    pthread_mutex_destroy(&fh->lock);
    free(fh);
}

[[nodiscard]] static int fcache_statfs(const char *path, struct statvfs *stbuf) {
    (void) path;
    struct fcache_config *cfg = get_config();
    if (statvfs(cfg->remote_dir, stbuf) != 0) {
        return -errno;
    }
    return 0;
}

[[nodiscard]] static int fcache_getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi) {
    (void) fi;
    struct fcache_config *cfg = get_config();
    char cpath[PATH_MAX], rpath[PATH_MAX];

    if (build_path(cpath, sizeof(cpath), cfg->cache_dir, path) != 0 ||
        build_path(rpath, sizeof(rpath), cfg->remote_dir, path) != 0) {
        return -EPERM;
    }

    if (stat(cpath, stbuf) == 0) {
        if (cfg->mount_opts.uid_set) stbuf->st_uid = cfg->mount_opts.uid;
        if (cfg->mount_opts.gid_set) stbuf->st_gid = cfg->mount_opts.gid;
        return 0;
    }

    if (stat(rpath, stbuf) == 0) {
        if (cfg->mount_opts.uid_set) stbuf->st_uid = cfg->mount_opts.uid;
        if (cfg->mount_opts.gid_set) stbuf->st_gid = cfg->mount_opts.gid;
        return 0;
    }

    return -errno;
}

[[nodiscard]] static int fcache_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                           off_t offset, struct fuse_file_info *fi,
                           enum fuse_readdir_flags flags) {
    (void) offset;
    (void) fi;
    (void) flags;

    struct fcache_config *cfg = get_config();
    char rpath[PATH_MAX], cpath[PATH_MAX];

    if (build_path(rpath, sizeof(rpath), cfg->remote_dir, path) != 0 ||
        build_path(cpath, sizeof(cpath), cfg->cache_dir, path) != 0) {
        return -EPERM;
    }

    DIR *dp = opendir(rpath);
    if (!dp) {
        dp = opendir(cpath);
        if (!dp) return -errno;
    }

    struct dirent *de;
    while ((de = readdir(dp)) != NULL) {
        struct stat st;
        memset(&st, 0, sizeof(st));
        st.st_ino = de->d_ino;
        st.st_mode = de->d_type << 12;
        
        if (cfg->mount_opts.uid_set) st.st_uid = cfg->mount_opts.uid;
        if (cfg->mount_opts.gid_set) st.st_gid = cfg->mount_opts.gid;

        if (filler(buf, de->d_name, &st, 0, 0)) break;
    }

    closedir(dp);
    return 0;
}

[[nodiscard]] static int fcache_open(const char *path, struct fuse_file_info *fi) {
    struct fcache_config *cfg = get_config();
    char cpath[PATH_MAX], rpath[PATH_MAX];

    if (build_path(cpath, sizeof(cpath), cfg->cache_dir, path) != 0 ||
        build_path(rpath, sizeof(rpath), cfg->remote_dir, path) != 0) {
        return -EPERM;
    }

    int is_cached = (access(cpath, F_OK) == 0);
    int accmode = fi->flags & O_ACCMODE;

    if (!is_cached && accmode == O_RDONLY && access(rpath, F_OK) == 0) {
        int remote_fd = open(rpath, O_RDONLY);
        if (remote_fd >= 0) {
            struct stat st;
            if (fstat(remote_fd, &st) == 0) {
                struct fcache_file_handle *fh = calloc(1, sizeof(*fh));
                if (fh) {
                    fh->remote_fd = remote_fd;
                    fh->cache_fd = -1;
                    fh->file_size = st.st_size;
                    fh->remote_path = strdup(rpath);
                    fh->cache_path = strdup(cpath);
                    if (fh->remote_path && fh->cache_path) {
                        pthread_mutex_init(&fh->lock, NULL);
                        pthread_cond_init(&fh->cond, NULL);
                        if (start_background_cache(fh, st.st_mode) == 0) {
                            fi->fh = (uintptr_t)fh;
                            return 0;
                        }
                        pthread_cond_destroy(&fh->cond);
                        pthread_mutex_destroy(&fh->lock);
                    }
                    free(fh->remote_path);
                    free(fh->cache_path);
                    free(fh);
                }
            }
            close(remote_fd);
        }
    }

    const char *target_path = is_cached ? cpath : rpath;
    if ((fi->flags & O_TRUNC) != 0 && accmode != O_RDONLY) {
        // open() below would truncate target_path only, leaving the
        // other copy with a stale tail (getattr prefers cache). Cut
        // the other side first so failure leaves target untouched.
        const char *other = is_cached ? rpath : cpath;
        if (truncate(other, 0) != 0 && errno != ENOENT) {
            return -errno;
        }
    }
    int fd = open(target_path, fi->flags);
    if (fd < 0) return -errno;

    struct fcache_file_handle *fh = new_direct_handle(fd, rpath, cpath, is_cached);
    if (!fh) {
        close(fd);
        return -ENOMEM;
    }
    fi->fh = (uintptr_t)fh;
    return 0;
}

[[nodiscard]] static int fcache_read(const char *path, char *buf, size_t size, off_t offset,
                        struct fuse_file_info *fi) {
    (void) path;
    struct fcache_file_handle *fh = (struct fcache_file_handle *)(uintptr_t)fi->fh;
    if (!fh) return -EBADF;

    int fd = fh->is_streaming ? fh->remote_fd : fh->cache_fd;
    if (fd < 0) return -EBADF;
    ssize_t res = pread(fd, buf, size, offset);
    if (res < 0) return -errno;
    return (int)res;
}

[[nodiscard]] static int fcache_write(const char *path, const char *buf, size_t size, off_t offset,
                         struct fuse_file_info *fi) {
    struct fcache_file_handle *fh = (struct fcache_file_handle *)(uintptr_t)fi->fh;
    if (!fh || fh->cache_fd < 0) return -EBADF;

    ssize_t res = pwrite(fh->cache_fd, buf, size, offset);
    if (res < 0) return -errno;

    if (!fh->is_streaming && fh->is_cache_backed) {
        struct fcache_config *cfg = get_config();
        char rpath[PATH_MAX];
        if (build_path(rpath, sizeof(rpath), cfg->remote_dir, path) == 0) {
            int rfd = open(rpath, O_WRONLY);
            if (rfd >= 0) {
                if (pwrite(rfd, buf, (size_t)res, offset) < 0) {
                    /* best effort write-through */
                }
                close(rfd);
            }
        }
    }

    return (int)res;
}

[[nodiscard]] static int fcache_create(const char *path, mode_t mode, struct fuse_file_info *fi) {
    struct fcache_config *cfg = get_config();
    char cpath[PATH_MAX], rpath[PATH_MAX];

    if (build_path(cpath, sizeof(cpath), cfg->cache_dir, path) != 0 ||
        build_path(rpath, sizeof(rpath), cfg->remote_dir, path) != 0) {
        return -EPERM;
    }

    create_parent_dirs(cpath);
    create_parent_dirs(rpath);

    mode &= ~effective_umask(cfg);

    int rfd = open(rpath, fi->flags | O_CREAT, mode);
    if (rfd < 0) return -errno;

    int cfd = open(cpath, fi->flags | O_CREAT, mode);
    struct fcache_file_handle *fh;
    if (cfd >= 0) {
        fh = new_direct_handle(cfd, rpath, cpath, 1);
        if (!fh) {
            close(cfd);
            close(rfd);
            return -ENOMEM;
        }
        close(rfd);
    } else {
        fh = new_direct_handle(rfd, rpath, cpath, 0);
        if (!fh) {
            close(rfd);
            return -ENOMEM;
        }
    }
    fi->fh = (uintptr_t)fh;

    return 0;
}

[[nodiscard]] static int fcache_mkdir(const char *path, mode_t mode) {
    struct fcache_config *cfg = get_config();
    char cpath[PATH_MAX], rpath[PATH_MAX];

    (void)build_path(cpath, sizeof(cpath), cfg->cache_dir, path);
    (void)build_path(rpath, sizeof(rpath), cfg->remote_dir, path);

    mode &= ~effective_umask(cfg);

    mkdir(cpath, mode);
    if (mkdir(rpath, mode) != 0 && errno != EEXIST) {
        return -errno;
    }

    return 0;
}

[[nodiscard]] static int fcache_mknod(const char *path, mode_t mode, dev_t rdev) {
    struct fcache_config *cfg = get_config();
    char cpath[PATH_MAX], rpath[PATH_MAX];

    if (build_path(cpath, sizeof(cpath), cfg->cache_dir, path) != 0 ||
        build_path(rpath, sizeof(rpath), cfg->remote_dir, path) != 0) {
        return -EPERM;
    }

    create_parent_dirs(cpath);
    create_parent_dirs(rpath);

    mode &= ~effective_umask(cfg);

    (void)mknod(cpath, mode, rdev);
    if (mknod(rpath, mode, rdev) != 0) {
        return -errno;
    }

    return 0;
}

[[nodiscard]] static int fcache_rename(const char *from, const char *to, unsigned int flags) {
    if (flags != 0) {
        return -EINVAL;
    }
    struct fcache_config *cfg = get_config();
    char cfrom[PATH_MAX], rfrom[PATH_MAX], cto[PATH_MAX], rto[PATH_MAX];

    if (build_path(cfrom, sizeof(cfrom), cfg->cache_dir, from) != 0 ||
        build_path(rfrom, sizeof(rfrom), cfg->remote_dir, from) != 0 ||
        build_path(cto, sizeof(cto), cfg->cache_dir, to) != 0 ||
        build_path(rto, sizeof(rto), cfg->remote_dir, to) != 0) {
        return -EPERM;
    }

    create_parent_dirs(cto);
    create_parent_dirs(rto);

    rename(cfrom, cto);
    if (rename(rfrom, rto) != 0) {
        int err = errno;
        rename(cto, cfrom);
        return -err;
    }
    return 0;
}

[[nodiscard]] static int fcache_unlink(const char *path) {
    struct fcache_config *cfg = get_config();
    char cpath[PATH_MAX], rpath[PATH_MAX];

    (void)build_path(cpath, sizeof(cpath), cfg->cache_dir, path);
    (void)build_path(rpath, sizeof(rpath), cfg->remote_dir, path);

    unlink(cpath);
    if (unlink(rpath) != 0) {
        return -errno;
    }

    return 0;
}

[[nodiscard]] static int fcache_rmdir(const char *path) {
    struct fcache_config *cfg = get_config();
    char cpath[PATH_MAX], rpath[PATH_MAX];

    (void)build_path(cpath, sizeof(cpath), cfg->cache_dir, path);
    (void)build_path(rpath, sizeof(rpath), cfg->remote_dir, path);

    rmdir(cpath);
    if (rmdir(rpath) != 0) {
        return -errno;
    }

    return 0;
}

[[nodiscard]] static int fcache_release(const char *path, struct fuse_file_info *fi) {
    (void) path;
    struct fcache_file_handle *fh = (struct fcache_file_handle *)(uintptr_t)fi->fh;
    if (!fh) return 0;

    if (fh->is_streaming) {
        // Hand off to the worker: it finalizes and frees itself. Never
        // join here, or teardown stalls on slow remotes (Ctrl+C looks hung).
        pthread_mutex_lock(&fh->lock);
        fh->release_called = 1;
        pthread_cond_signal(&fh->cond);
        pthread_mutex_unlock(&fh->lock);
        pthread_detach(fh->cache_thread);
    } else {
        if (fh->cache_fd >= 0) close(fh->cache_fd);
        free_handle(fh);
    }
    return 0;
}

[[nodiscard]] static int fcache_truncate(const char *path, off_t size, struct fuse_file_info *fi) {
    (void) fi;
    struct fcache_config *cfg = get_config();
    char cpath[PATH_MAX], rpath[PATH_MAX];

    (void)build_path(cpath, sizeof(cpath), cfg->cache_dir, path);
    (void)build_path(rpath, sizeof(rpath), cfg->remote_dir, path);

    if (truncate(cpath, size) != 0) {
        /* cache copy may not exist yet; remote is authoritative */
    }
    if (truncate(rpath, size) != 0) {
        return -errno;
    }
    return 0;
}

[[nodiscard]] static int fcache_utimens(const char *path, const struct timespec tv[2], struct fuse_file_info *fi) {
    (void) fi;
    struct fcache_config *cfg = get_config();
    char cpath[PATH_MAX], rpath[PATH_MAX];

    (void)build_path(cpath, sizeof(cpath), cfg->cache_dir, path);
    (void)build_path(rpath, sizeof(rpath), cfg->remote_dir, path);

    utimensat(AT_FDCWD, cpath, tv, AT_SYMLINK_NOFOLLOW);
    if (utimensat(AT_FDCWD, rpath, tv, AT_SYMLINK_NOFOLLOW) != 0) {
        return -errno;
    }
    return 0;
}

[[nodiscard]] static int fcache_chmod(const char *path, mode_t mode, struct fuse_file_info *fi) {
    (void) fi;
    struct fcache_config *cfg = get_config();
    char cpath[PATH_MAX], rpath[PATH_MAX];

    (void)build_path(cpath, sizeof(cpath), cfg->cache_dir, path);
    (void)build_path(rpath, sizeof(rpath), cfg->remote_dir, path);

    chmod(cpath, mode);
    if (chmod(rpath, mode) != 0) {
        return -errno;
    }
    return 0;
}

[[nodiscard]] static int fcache_chown(const char *path, uid_t uid, gid_t gid, struct fuse_file_info *fi) {
    (void) fi;
    struct fcache_config *cfg = get_config();
    char cpath[PATH_MAX], rpath[PATH_MAX];

    (void)build_path(cpath, sizeof(cpath), cfg->cache_dir, path);
    (void)build_path(rpath, sizeof(rpath), cfg->remote_dir, path);

    chown(cpath, uid, gid);
    if (chown(rpath, uid, gid) != 0) {
        return -errno;
    }
    return 0;
}

static struct fcache_mount_opts g_mount_opts;

static void *fcache_init(struct fuse_conn_info *conn, struct fuse_config *fuse_cfg) {
    (void) fuse_cfg;
    if (conn->capable & FUSE_CAP_DONT_MASK) {
        conn->want |= FUSE_CAP_DONT_MASK;
    }
    return fuse_get_context()->private_data;
}

static const struct fuse_operations fcache_ops = {
    .getattr  = fcache_getattr,
    .readdir  = fcache_readdir,
    .open     = fcache_open,
    .read     = fcache_read,
    .write    = fcache_write,
    .create   = fcache_create,
    .mknod    = fcache_mknod,
    .unlink   = fcache_unlink,
    .rename   = fcache_rename,
    .mkdir    = fcache_mkdir,
    .rmdir    = fcache_rmdir,
    .release  = fcache_release,
    .truncate = fcache_truncate,
    .chmod    = fcache_chmod,
    .chown    = fcache_chown,
    .utimens  = fcache_utimens,
    .statfs   = fcache_statfs,
    .init     = fcache_init,
};


static struct fuse *g_fuse;
static struct fuse_session *g_session;
static pthread_t g_sigthread;
static volatile sig_atomic_t g_unmounted;
static char g_mountpoint[PATH_MAX];
static time_t g_start_time;
static time_t g_first_sig;

#define FCACHE_SIG_GRACE_SECONDS 3

static void phase_log(const char *msg) {
    fprintf(stderr, "fcache: [+%lds] %s\n",
            (long)(time(NULL) - g_start_time), msg);
}

// Dedicated signal thread (sigwait, not async handlers). On first
// INT/TERM/HUP it flags session exit AND unmounts: unmounting closes
// the /dev/fuse fd, so loop workers blocked in read break out and the
// MT loop wakes on every libfuse/libc combo. Relying on session-exit
// alone stalls libfuse < 3.18 on Bionic (sem_wait with no EINTR).
static void *signal_thread(void *arg) {
    (void)arg;
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGHUP);
    for (;;) {
        int sig = 0;
        if (sigwait(&set, &sig) != 0) {
            continue;
        }
        time_t now = time(NULL);
        if (g_first_sig == 0) {
            g_first_sig = now;
            phase_log("signal received, unmounting...");
            if (g_session) {
                fuse_session_exit(g_session);
            }
            if (g_fuse) {
                // Plain unmount first: under proot MNT_FORCE (which
                // libfuse uses) can fail EINVAL-noisy while a plain
                // umount succeeds. Once the mount is gone libfuse's
                // own unmount goes quiet (POLLERR on the dead fd).
                umount2(g_mountpoint, 0);
                fuse_unmount(g_fuse);
                g_unmounted = 1;
            }
        } else if (now - g_first_sig >= FCACHE_SIG_GRACE_SECONDS) {
            phase_log("still exiting, force quitting "
                      "(stale mount may need fusermount -u)");
            _exit(128 + sig);
        } else if (g_session) {
            fuse_session_exit(g_session);
        }
    }
    return NULL;
}

static void block_exit_signals(void) {
    // Process-wide so every thread spawned later (fuse workers, cache
    // workers) inherits the mask; only the signal thread waits on them.
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGHUP);
    pthread_sigmask(SIG_BLOCK, &set, NULL);
    signal(SIGPIPE, SIG_IGN);
}

int main(int argc, char *argv[]) {
    static const struct option long_opts[] = {
        { "foreground", no_argument,       NULL, 'f' },
        { "debug",      no_argument,       NULL, 'd' },
        { "option",     required_argument, NULL, 'o' },
        { "help",       no_argument,       NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };

    int opt;
    int foreground = 0;
    int debug = 0;
    g_start_time = time(NULL);
    memset(&g_mount_opts, 0, sizeof(g_mount_opts));

    while ((opt = getopt_long(argc, argv, "fdo:h", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'f':
            foreground = 1;
            break;
        case 'd':
            debug = 1;
            break;
        case 'o':
            if (parse_mount_opts(optarg, &g_mount_opts) != 0) {
                return 1;
            }
            break;
        case 'h':
            usage(argv[0]);
            return 0;
        default:
            usage(argv[0]);
            return 1;
        }
    }

    if (optind + 3 > argc) {
        usage(argv[0]);
        return 1;
    }

    struct fcache_config cfg;
    memset(&cfg, 0, sizeof(cfg));

    if (!realpath(argv[optind], cfg.cache_dir)) {
        perror("realpath cache_dir");
        return 1;
    }
    if (!realpath(argv[optind + 1], cfg.remote_dir)) {
        perror("realpath remote_dir");
        return 1;
    }

    const char *mountpoint = argv[optind + 2];
    // Absolute: libfuse chdir("/")s on startup, so a relative mountpoint
    // would no longer resolve at unmount time (stale mount left behind).
    static char mount_abs[PATH_MAX];
    if (!realpath(mountpoint, mount_abs)) {
        fprintf(stderr, "fcache: mountpoint '%s': %s\n",
                mountpoint, strerror(errno));
        return 1;
    }
    mountpoint = mount_abs;
    snprintf(g_mountpoint, sizeof(g_mountpoint), "%s", mount_abs);

    // Build args for the fuse session. Only fuse-known options go here;
    // fcache-specific ones (umask) were consumed above. Note: the
    // mountpoint positional must NOT be included (libfuse extracts it
    // itself in fuse_main; here we pass it to fuse_mount directly),
    // -f never reaches libfuse (we daemonize ourselves), and -d is
    // passed as -odebug.
    int fuse_argc = 0;
    char *fuse_argv[16];
    char *uid_opt = NULL;
    char *gid_opt = NULL;
    char *subtype_opt = NULL;
    char *progcopy = strdup(argv[0]);
    fuse_argv[fuse_argc++] = argv[0];

    if (progcopy) {
        char *base = basename(progcopy);
        subtype_opt = malloc(strlen(base) + 16);
        if (subtype_opt) {
            snprintf(subtype_opt, strlen(base) + 16, "-osubtype=%s", base);
            fuse_argv[fuse_argc++] = subtype_opt;
        }
        free(progcopy);
    }

    if (debug) {
        fuse_argv[fuse_argc++] = "-odebug";
    }
    if (g_mount_opts.allow_other) {
        fuse_argv[fuse_argc++] = "-oallow_other";
    }
    if (g_mount_opts.default_permissions) {
        fuse_argv[fuse_argc++] = "-odefault_permissions";
    }
    if (g_mount_opts.uid_set) {
        uid_opt = malloc(32);
        if (!uid_opt) {
            free(subtype_opt);
            return 1;
        }
        snprintf(uid_opt, 32, "-ouid=%u", g_mount_opts.uid);
        fuse_argv[fuse_argc++] = uid_opt;
    }
    if (g_mount_opts.gid_set) {
        gid_opt = malloc(32);
        if (!gid_opt) {
            free(uid_opt);
            free(subtype_opt);
            return 1;
        }
        snprintf(gid_opt, 32, "-ogid=%u", g_mount_opts.gid);
        fuse_argv[fuse_argc++] = gid_opt;
    }
    fuse_argv[fuse_argc] = NULL;

    // Store mount options in config for getattr/readdir
    cfg.mount_opts = g_mount_opts;

    // Daemon must not strip bits itself; create/mkdir/mknod apply the
    // caller umask (or the mount umask override) explicitly.
    umask(0);

    // Same bootstrap fuse_main does (parse/new/mount/daemonize/loop),
    // except signal handling: no async handlers and no libfuse sigwait
    // thread. A dedicated sigwait thread owns INT/TERM/HUP and tears
    // down via session-exit + unmount (fd revocation wakes the loop on
    // every libfuse/libc combo); a repeat force-quits.
    struct fuse_args fargs = FUSE_ARGS_INIT(fuse_argc, fuse_argv);
    struct fuse *fh_fuse = fuse_new(&fargs, &fcache_ops,
                                    sizeof(fcache_ops), &cfg);
    if (!fh_fuse) {
        free(uid_opt);
        free(gid_opt);
        free(subtype_opt);
        return 1;
    }
    if (fuse_mount(fh_fuse, mountpoint) != 0) {
        fuse_destroy(fh_fuse);
        free(uid_opt);
        free(gid_opt);
        free(subtype_opt);
        return 1;
    }
    int fg = foreground || debug;
    if (fuse_daemonize(fg) != 0) {
        fuse_unmount(fh_fuse);
        fuse_destroy(fh_fuse);
        free(uid_opt);
        free(gid_opt);
        free(subtype_opt);
        return 1;
    }
    g_fuse = fh_fuse;
    g_session = fuse_get_session(fh_fuse);
    block_exit_signals();
    if (pthread_create(&g_sigthread, NULL, signal_thread, NULL) != 0) {
        perror("pthread_create");
        fuse_unmount(fh_fuse);
        fuse_destroy(fh_fuse);
        free(uid_opt);
        free(gid_opt);
        free(subtype_opt);
        return 1;
    }
    // Detached: it lives until process exit. No cancel/join — Bionic
    // with strict -std=c23 hides pthread_cancel, and joining a thread
    // blocked in sigwait would need it.
    pthread_detach(g_sigthread);

    {
        char msg[PATH_MAX + 64];
        snprintf(msg, sizeof(msg), "mounted on %s, event loop starting",
                 mountpoint);
        phase_log(msg);
    }

    // NOTE: never pass NULL here. With FUSE_USE_VERSION < 312 this
    // dispatches to the 3.2 ABI whose converter dereferences the config.
    // Values mirror libfuse defaults (clone_fd=0, idle disabled).
    struct fuse_loop_config loop_cfg;
    loop_cfg.clone_fd = 0;
    loop_cfg.max_idle_threads = (unsigned int)-1;
    int res = fuse_loop_mt(fh_fuse, &loop_cfg);

    {
        char msg[64];
        snprintf(msg, sizeof(msg), "event loop exited (res=%d), unmounting",
                 res);
        phase_log(msg);
    }
    // Already unmounted by the signal thread in the common case;
    // a second unmount would just print libfuse's EINVAL noise.
    if (!g_unmounted) {
        fuse_unmount(fh_fuse);
    }
    phase_log("unmounted, cleaning up");
    fuse_destroy(fh_fuse);
    phase_log("done");

    free(uid_opt);
    free(gid_opt);
    free(subtype_opt);
    fuse_opt_free_args(&fargs);

    return res;
}

