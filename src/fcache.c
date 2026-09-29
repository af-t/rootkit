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
#include <stdint.h>
#include <signal.h>
#include <ctype.h>
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
    // 0 = no quota; bounded only by filesystem free space (set via -o cache_size=256M).
    uint64_t max_cache_bytes;
    struct fcache_mount_opts mount_opts;
};

struct fcache_file_handle {
    int cache_fd;
    int remote_fd;
    int open_flags;
    off_t file_size;
    struct fcache_config *cfg;
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

// Serializes eviction vs tmp creation so two fillers can't both see space, then both hit ENOSPC.
static pthread_mutex_t g_evict_mutex = PTHREAD_MUTEX_INITIALIZER;

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
        "                  cache_size=SIZE     cap cache usage (e.g. 256M, 1G).\n"
        "                                    default 0 = bounded only by\n"
        "                                    filesystem free space\n"
        "  -h              show this help\n",
        prog);
}

[[nodiscard]] static int parse_size(const char *s, uint64_t *out) {
    if (!s || !*s) return -EINVAL;
    char *end = NULL;
    unsigned long long v = strtoull(s, &end, 10);
    if (end == s) return -EINVAL;
    uint64_t mul = 1;
    if (*end) {
        switch (tolower((unsigned char)*end)) {
        case 'k': mul = (uint64_t)1024; end++; break;
        case 'm': mul = (uint64_t)1024 * 1024; end++; break;
        case 'g': mul = (uint64_t)1024 * 1024 * 1024; end++; break;
        default: return -EINVAL;
        }
        if (tolower((unsigned char)*end) == 'i') {
            end++;
            if (tolower((unsigned char)*end) == 'b') end++;
        } else if (tolower((unsigned char)*end) == 'b') {
            end++;
        }
    }
    if (*end) return -EINVAL;
    if (v > UINT64_MAX / mul) return -EINVAL;
    *out = (uint64_t)v * mul;
    return 0;
}

static int parse_mount_opts(const char *optstr, struct fcache_mount_opts *opts,
                            uint64_t *max_cache_bytes) {
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
        } else if (strncmp(token, "cache_size=", 11) == 0) {
            if (parse_size(token + 11, max_cache_bytes) != 0) {
                fprintf(stderr, "fcache: invalid cache_size: %s\n", token + 11);
                free(copy);
                return -EINVAL;
            }
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

// LRU over atime (not TTL): hits refresh atime, a full cache evicts least-recently-used first.

// Refresh atime only (mtime must keep mirroring remote); best effort.
static void touch_for_lru(const char *cpath) {
    struct timespec tv[2];
    tv[0].tv_sec = 0; tv[0].tv_nsec = UTIME_NOW;
    tv[1].tv_sec = 0; tv[1].tv_nsec = UTIME_OMIT;
    (void)utimensat(AT_FDCWD, cpath, tv, AT_SYMLINK_NOFOLLOW);
}

[[nodiscard]] static int is_tmp_name(const char *name) {
    return strstr(name, ".tmp.") != NULL;
}

struct cache_entry {
    char path[PATH_MAX];
    uint64_t size;
    time_t atime;
};

static void scan_dir(const char *dir, const char *except,
                     struct cache_entry **list, size_t *n, size_t *cap) {
    DIR *dp = opendir(dir);
    if (!dp) return;
    struct dirent *de;
    while ((de = readdir(dp)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        if (is_tmp_name(de->d_name)) continue;
        char full[PATH_MAX];
        int m = snprintf(full, sizeof(full), "%s/%s", dir, de->d_name);
        if (m < 0 || (size_t)m >= sizeof(full)) continue;
        if (except && strcmp(full, except) == 0) continue;
        struct stat st;
        if (lstat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            scan_dir(full, except, list, n, cap);
        } else if (S_ISREG(st.st_mode)) {
            if (*n >= *cap) {
                size_t ncap = *cap ? *cap * 2 : 64;
                struct cache_entry *nl = realloc(*list, ncap * sizeof(**list));
                if (!nl) break;
                *list = nl;
                *cap = ncap;
            }
            int k = snprintf((*list)[*n].path, sizeof((*list)[*n].path), "%s", full);
            if (k < 0 || (size_t)k >= sizeof((*list)[*n].path)) continue;
            (*list)[*n].size = (uint64_t)st.st_size;
            (*list)[*n].atime = st.st_atime;
            (*n)++;
        }
    }
    closedir(dp);
}

static int cmp_atime(const void *a, const void *b) {
    const struct cache_entry *ea = a, *eb = b;
    if (ea->atime < eb->atime) return -1;
    if (ea->atime > eb->atime) return 1;
    return 0;
}

[[nodiscard]] static uint64_t cache_fs_total(struct fcache_config *cfg) {
    struct statvfs sv;
    if (statvfs(cfg->cache_dir, &sv) != 0) return UINT64_MAX;
    return (uint64_t)sv.f_blocks * (uint64_t)sv.f_frsize;
}

[[nodiscard]] static uint64_t cache_capacity(struct fcache_config *cfg) {
    if (cfg->max_cache_bytes > 0) return cfg->max_cache_bytes;
    return cache_fs_total(cfg);
}

// Free `need` bytes via eviction (`except` spared, no fh lock held); -ENOSPC means bypass, not fail I/O.
[[nodiscard]] static int ensure_cache_space(struct fcache_config *cfg,
                                            uint64_t need, const char *except) {
    pthread_mutex_lock(&g_evict_mutex);

    uint64_t cap = cache_capacity(cfg);
    if (need > cap) {
        pthread_mutex_unlock(&g_evict_mutex);
        return -ENOSPC;
    }

    struct statvfs sv;
    uint64_t fs_free = UINT64_MAX;
    if (statvfs(cfg->cache_dir, &sv) == 0) {
        fs_free = (uint64_t)sv.f_bavail * (uint64_t)sv.f_frsize;
    }

    uint64_t use = 0;
    struct cache_entry *list = NULL;
    size_t n = 0, listcap = 0;
    if (cfg->max_cache_bytes > 0 || fs_free < need) {
        scan_dir(cfg->cache_dir, except, &list, &n, &listcap);
        for (size_t i = 0; i < n; i++) use += list[i].size;
    }

    // `except` still occupies quota although unevictable; count it.
    uint64_t excl = 0;
    if (except) {
        struct stat est;
        if (stat(except, &est) == 0 && S_ISREG(est.st_mode)) {
            excl = (uint64_t)est.st_size;
        }
    }

    int enough = (fs_free >= need) &&
                 (cfg->max_cache_bytes == 0 || use + excl + need <= cfg->max_cache_bytes);
    if (!enough && n > 0) {
        qsort(list, n, sizeof(*list), cmp_atime);
        size_t cdlen = strlen(cfg->cache_dir);
        // Pass 0 drops unreachable files (remote gone), pass 1 LRU over mirrored; blanked entries skipped.
        for (int pass = 0; pass < 2 && !enough; pass++) {
            for (size_t i = 0; i < n && !enough; i++) {
                if (list[i].path[0] == '\0') continue;
                if (strncmp(list[i].path, cfg->cache_dir, cdlen) != 0) {
                    continue;
                }
                char rmt[PATH_MAX];
                int m = snprintf(rmt, sizeof(rmt), "%s%s",
                                 cfg->remote_dir, list[i].path + cdlen);
                int reachable = (m >= 0 && (size_t)m < sizeof(rmt) &&
                                 access(rmt, F_OK) == 0);
                if ((pass == 0) == reachable) continue;
                if (unlink(list[i].path) == 0) {
                    if (use >= list[i].size) use -= list[i].size;
                    else use = 0;
                    if (fs_free != UINT64_MAX) fs_free += list[i].size;
                    list[i].path[0] = '\0';
                }
                enough = (fs_free >= need) &&
                         (cfg->max_cache_bytes == 0 ||
                          use + excl + need <= cfg->max_cache_bytes);
            }
        }
    }

    free(list);
    pthread_mutex_unlock(&g_evict_mutex);
    return enough ? 0 : -ENOSPC;
}

[[nodiscard]] static void *cache_worker(void *arg) {
    struct fcache_file_handle *fh = (struct fcache_file_handle *)arg;
    char buf[128 * 1024];
    ssize_t bytes_read;
    off_t offset = 0;
    int complete = 0;

    while ((bytes_read = pread(fh->remote_fd, buf, sizeof(buf), offset)) > 0) {
        pthread_mutex_lock(&fh->lock);
        int cfd = fh->cache_fd;
        pthread_mutex_unlock(&fh->lock);
        if (cfd >= 0) {
            ssize_t w = pwrite(cfd, buf, (size_t)bytes_read, offset);
            if (w < 0 && errno == ENOSPC) {
                // Filled mid-copy by a racer: evict, retry once, else stay uncached (reads already came from remote).
                uint64_t remain = (fh->file_size > offset)
                    ? (uint64_t)(fh->file_size - offset)
                    : (uint64_t)bytes_read;
                if (ensure_cache_space(fh->cfg, remain, NULL) == 0) {
                    w = pwrite(cfd, buf, (size_t)bytes_read, offset);
                }
            }
            if (w < 0 || (size_t)w != (size_t)bytes_read) {
                break;
            }
        }
        offset += bytes_read;
    }
    if (bytes_read == 0) {
        complete = 1;
    }

    // Sole finalizer (release never joins): wait for release, then publish.
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

[[nodiscard]] static int start_background_cache(struct fcache_config *cfg,
        struct fcache_file_handle *fh, mode_t src_mode) {
    int n = snprintf(fh->tmp_path, sizeof(fh->tmp_path), "%s.tmp.%d.%p",
                     fh->cache_path, getpid(), (void *)fh);
    if (n < 0 || (size_t)n >= sizeof(fh->tmp_path)) {
        return -ENAMETOOLONG;
    }

    // Larger-than-cache files are never cached (would evict everything); serve from remote.
    uint64_t need = fh->file_size > 0 ? (uint64_t)fh->file_size : 4096;
    if (need > cache_capacity(cfg)) {
        return -ENOSPC;
    }
    if (ensure_cache_space(cfg, need, NULL) != 0) {
        return -ENOSPC;
    }

    // Serialize check-and-create with eviction under one lock.
    pthread_mutex_lock(&g_evict_mutex);
    create_parent_dirs(fh->cache_path);
    sweep_stale_tmps(fh->cache_path);

    mode_t tmp_mode = (src_mode & 0777) != 0 ? (src_mode & 0777) : 0644;
    int fd = open(fh->tmp_path, O_WRONLY | O_CREAT | O_TRUNC, tmp_mode);
    if (fd < 0 && errno == ENOSPC) {
        pthread_mutex_unlock(&g_evict_mutex);
        if (ensure_cache_space(cfg, need, NULL) != 0) {
            return -ENOSPC;
        }
        pthread_mutex_lock(&g_evict_mutex);
        fd = open(fh->tmp_path, O_WRONLY | O_CREAT | O_TRUNC, tmp_mode);
    }
    pthread_mutex_unlock(&g_evict_mutex);
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

[[nodiscard]] static struct fcache_file_handle *new_direct_handle(int fd, int flags,
        const char *rpath, const char *cpath, int is_cache_backed) {
    struct fcache_file_handle *fh = calloc(1, sizeof(*fh));
    if (!fh) return NULL;
    fh->cache_fd = fd;
    fh->remote_fd = -1;
    fh->open_flags = flags;
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

// Drop a too-small cache copy and continue on remote; caller holds fh->lock.
[[nodiscard]] static int convert_direct_to_remote(struct fcache_file_handle *fh) {
    int flags = fh->open_flags & ~(O_CREAT | O_TRUNC | O_EXCL);
    int rfd = open(fh->remote_path, flags | O_CREAT, 0644);
    if (rfd < 0) return -errno;
    if (fh->cache_fd >= 0) close(fh->cache_fd);
    fh->cache_fd = rfd;
    fh->is_cache_backed = 0;
    if (fh->cache_path) unlink(fh->cache_path);
    return 0;
}

[[nodiscard]] static int fcache_statfs(const char *path, struct statvfs *stbuf) {
    (void) path;
    struct fcache_config *cfg = get_config();
    // Advertise remote size: bypassed writes fail only when remote itself is full.
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

    // Remote-authoritative: missing from remote means nonexistent, stale cache copy or not.
    (void) cpath;
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

    // Remote-only listing: a backend deletion hides the file immediately, stale cache copy or not.
    DIR *dp = opendir(rpath);
    if (!dp) return -errno;
    (void) cpath;

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

    // Without O_CREAT (the kernel routes that to create), missing-from-remote means ENOENT.
    if ((fi->flags & O_CREAT) == 0 && access(rpath, F_OK) != 0) {
        return -ENOENT;
    }

    if (is_cached) {
        touch_for_lru(cpath);
    }

    if (!is_cached && accmode == O_RDONLY && access(rpath, F_OK) == 0) {
        int remote_fd = open(rpath, O_RDONLY);
        if (remote_fd >= 0) {
            struct stat st;
            if (fstat(remote_fd, &st) == 0) {
                struct fcache_file_handle *fh = calloc(1, sizeof(*fh));
                if (fh) {
                    fh->remote_fd = remote_fd;
                    fh->cache_fd = -1;
                    fh->open_flags = fi->flags;
                    fh->file_size = st.st_size;
                    fh->cfg = cfg;
                    fh->remote_path = strdup(rpath);
                    fh->cache_path = strdup(cpath);
                    if (fh->remote_path && fh->cache_path) {
                        pthread_mutex_init(&fh->lock, NULL);
                        pthread_cond_init(&fh->cond, NULL);
                        if (start_background_cache(cfg, fh, st.st_mode) == 0) {
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
    int other_missing = 0;
    if ((fi->flags & O_TRUNC) != 0 && accmode != O_RDONLY) {
        // Truncate the other copy first so a failure leaves the target untouched.
        const char *other = is_cached ? rpath : cpath;
        if (truncate(other, 0) != 0) {
            if (errno != ENOENT) {
                return -errno;
            }
            other_missing = 1;
        }
    }
    int fd = open(target_path, fi->flags);
    if (fd < 0) return -errno;
    if (other_missing) {
        // O_TRUNC must leave a file on both sides; recreate the missing one (best effort).
        struct stat tst;
        mode_t m = 0644;
        if (fstat(fd, &tst) == 0 && (tst.st_mode & 0777) != 0) {
            m = tst.st_mode & 0777;
        }
        const char *other = is_cached ? rpath : cpath;
        create_parent_dirs(other);
        int ofd = open(other, O_WRONLY | O_CREAT, m);
        if (ofd >= 0) close(ofd);
    }

    struct fcache_file_handle *fh = new_direct_handle(fd, fi->flags, rpath, cpath, is_cached);
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

    if (fh->is_streaming) {
        // Reads go to remote_fd, which the worker never closes mid-stream.
        if (fh->remote_fd < 0) return -EBADF;
        ssize_t res = pread(fh->remote_fd, buf, size, offset);
        if (res < 0) return -errno;
        return (int)res;
    }

    // Lock across pread: a write-triggered conversion must not close the fd mid-read.
    pthread_mutex_lock(&fh->lock);
    int fd = fh->cache_fd;
    ssize_t res = (fd >= 0) ? pread(fd, buf, size, offset) : -1;
    int err = (res < 0) ? (fd >= 0 ? errno : EBADF) : 0;
    pthread_mutex_unlock(&fh->lock);
    if (res < 0) return -err;
    return (int)res;
}
// Mirror to remote (recreate if missing); failure fails the user write, never goes silent. Returns -errno.
[[nodiscard]] static int mirror_to_remote(mode_t cmode, const char *rpath,
                                         const char *buf, size_t len, off_t offset) {
    int rfd = open(rpath, O_WRONLY);
    if (rfd < 0 && errno == ENOENT) {
        mode_t m = (cmode & 0777) != 0 ? (cmode & 0777) : 0644;
        create_parent_dirs(rpath);
        rfd = open(rpath, O_WRONLY | O_CREAT, m);
    }
    if (rfd < 0) return -errno;
    ssize_t w = pwrite(rfd, buf, len, offset);
    int e = (w < 0) ? errno : 0;
    close(rfd);
    if (w < 0) return -e;
    if ((size_t)w != len) return -EIO;
    return 0;
}

[[nodiscard]] static int fcache_write(const char *path, const char *buf, size_t size, off_t offset,
                         struct fuse_file_info *fi) {

    struct fcache_file_handle *fh = (struct fcache_file_handle *)(uintptr_t)fi->fh;
    if (!fh || fh->is_streaming || fh->cache_fd < 0) return -EBADF;

    struct fcache_config *cfg = get_config();
    pthread_mutex_lock(&fh->lock);
    ssize_t res = pwrite(fh->cache_fd, buf, size, offset);
    // Save errno now: ensure_cache_space() clobbers it, and the checks below need the original reason.
    int e = (res < 0) ? errno : 0;
    if (res < 0 && e == ENOSPC && fh->is_cache_backed) {
        // Cache full: evict (spares the file being grown) and retry.
        if (ensure_cache_space(cfg, size, fh->cache_path) == 0) {
            res = pwrite(fh->cache_fd, buf, size, offset);
            e = (res < 0) ? errno : 0;
        }
    }
    if (res < 0 && e == ENOSPC && fh->is_cache_backed) {
        // Still full (write bigger than cache): drop the cache copy, finish on remote.
        if (convert_direct_to_remote(fh) == 0) {
            res = pwrite(fh->cache_fd, buf, size, offset);
            e = (res < 0) ? errno : 0;
        } else {
            res = -1;
            e = ENOSPC;
        }
    }
    if (res < 0) {
        pthread_mutex_unlock(&fh->lock);
        return -e;
    }
    int backed = fh->is_cache_backed;
    int cfd = fh->cache_fd;
    mode_t cmode = 0;
    if (backed && cfd >= 0) {
        // Mirror runs unlocked; capture the mode now, never touch the fd there.
        struct stat cst;
        if (fstat(cfd, &cst) == 0) cmode = cst.st_mode;
    }
    pthread_mutex_unlock(&fh->lock);

    if (backed && cfd >= 0) {
        char rpath[PATH_MAX];
        if (build_path(rpath, sizeof(rpath), cfg->remote_dir, path) == 0) {
            int mr = mirror_to_remote(cmode, rpath, buf, (size_t)res, offset);
            if (mr != 0) {
                // Report the mirror failure (never silent); drop the diverged copy, convert best-effort.
                pthread_mutex_lock(&fh->lock);
                if (fh->is_cache_backed && fh->cache_fd == cfd) {
                    if (fh->cache_path) unlink(fh->cache_path);
                    (void)convert_direct_to_remote(fh);
                }
                pthread_mutex_unlock(&fh->lock);
                return mr;
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
    if (cfd < 0 && errno == ENOSPC) {
        // Make room instead of silently going remote-only.
        if (ensure_cache_space(cfg, 4096, NULL) == 0) {
            cfd = open(cpath, fi->flags | O_CREAT, mode);
        }
    }
    struct fcache_file_handle *fh;
    if (cfd >= 0) {
        fh = new_direct_handle(cfd, fi->flags, rpath, cpath, 1);
        if (!fh) {
            close(cfd);
            close(rfd);
            return -ENOMEM;
        }
        close(rfd);
    } else {
        fh = new_direct_handle(rfd, fi->flags, rpath, cpath, 0);
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

    int cok = (unlink(cpath) == 0);
    if (unlink(rpath) != 0) {
        // Remote already gone: deleting the surviving cache copy still fulfills the unlink.
        if (errno == ENOENT && cok) return 0;
        return -errno;
    }

    return 0;
}

[[nodiscard]] static int fcache_rmdir(const char *path) {
    struct fcache_config *cfg = get_config();
    char cpath[PATH_MAX], rpath[PATH_MAX];

    (void)build_path(cpath, sizeof(cpath), cfg->cache_dir, path);
    (void)build_path(rpath, sizeof(rpath), cfg->remote_dir, path);

    int cok = (rmdir(cpath) == 0);
    if (rmdir(rpath) != 0) {
        if (errno == ENOENT && cok) return 0;
        return -errno;
    }

    return 0;
}

[[nodiscard]] static int fcache_release(const char *path, struct fuse_file_info *fi) {
    (void) path;
    struct fcache_file_handle *fh = (struct fcache_file_handle *)(uintptr_t)fi->fh;
    if (!fh) return 0;

    if (fh->is_streaming) {
        // Hand off to the worker (never join: teardown would stall on slow remotes).
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
        if (errno == ENOSPC) {
            // Full cache: evict and retry, else drop the copy so remote stays authoritative.
            struct stat st;
            uint64_t need = (uint64_t)(size > 0 ? size : 0);
            if (stat(cpath, &st) == 0 && (uint64_t)size > (uint64_t)st.st_size) {
                need = (uint64_t)size - (uint64_t)st.st_size;
            }
            if (ensure_cache_space(cfg, need, cpath) != 0 ||
                truncate(cpath, size) != 0) {
                unlink(cpath);
            }
        }
        /* missing cache copy is fine; remote is authoritative */
    }
    if (truncate(rpath, size) != 0) {
        int e = errno;
        if (e == ENOENT) {
            // Remote copy missing: recreate so the pair stays coherent.
            struct stat cst;
            mode_t m = 0644;
            if (stat(cpath, &cst) == 0 && (cst.st_mode & 0777) != 0) {
                m = cst.st_mode & 0777;
            }
            create_parent_dirs(rpath);
            int rfd = open(rpath, O_WRONLY | O_CREAT, m);
            if (rfd < 0) return -errno;
            if (ftruncate(rfd, size) != 0) {
                int fe = errno;
                close(rfd);
                return -fe;
            }
            close(rfd);
        } else {
            return -e;
        }
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
static uint64_t g_max_cache_bytes;

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

// sigwait thread: first signal exits the session AND unmounts, since exit alone stalls libfuse < 3.18 on Bionic.
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
                // Plain umount first: under proot, libfuse's MNT_FORCE can fail while plain umount succeeds.
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
    // Process-wide so every later thread inherits it; only the signal thread waits.
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
    g_max_cache_bytes = 0;

    while ((opt = getopt_long(argc, argv, "fdo:h", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'f':
            foreground = 1;
            break;
        case 'd':
            debug = 1;
            break;
        case 'o':
            if (parse_mount_opts(optarg, &g_mount_opts, &g_max_cache_bytes) != 0) {
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
    // Absolute: libfuse chdir("/")s, so a relative mountpoint would dangle at unmount time.
    static char mount_abs[PATH_MAX];
    if (!realpath(mountpoint, mount_abs)) {
        fprintf(stderr, "fcache: mountpoint '%s': %s\n",
                mountpoint, strerror(errno));
        return 1;
    }
    mountpoint = mount_abs;
    snprintf(g_mountpoint, sizeof(g_mountpoint), "%s", mount_abs);

    // Only fuse-known opts here (ours consumed above); mountpoint excluded, -f is ours, -d is -odebug.
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

    cfg.mount_opts = g_mount_opts;
    cfg.max_cache_bytes = g_max_cache_bytes;

    // Daemon must not strip bits; create/mkdir/mknod apply the umask explicitly.
    umask(0);

    // Same bootstrap as fuse_main, minus libfuse signal handling (dedicated sigwait thread instead).
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
    // Detached: -std=c23 hides pthread_cancel on Bionic, and joining a sigwait-blocked thread needs it.
    pthread_detach(g_sigthread);

    {
        char msg[PATH_MAX + 64];
        snprintf(msg, sizeof(msg), "mounted on %s, event loop starting",
                 mountpoint);
        phase_log(msg);
    }

    // Never NULL: the pre-3.12 ABI converter dereferences it; values mirror libfuse defaults.
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
    // A second unmount would only print libfuse's EINVAL noise.
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

