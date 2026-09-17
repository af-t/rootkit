# rootlet

A tiny chroot launcher for running a Linux distribution assembled from a
disk image and/or bind mounts, built for Android. It sets the container
root up at `-m` (an image loop-mounted there with `-i`, or a directory
assembled in place with `-b`), binds the host `/dev`, `/proc` and `/sys`
into it, then `chroot`s in and starts a login shell (`su -`) on its own
pseudo-terminal.
On exit it tears everything back down: it kills the session's processes,
unmounts, and detaches the loop device. The same teardown runs when rootlet is
terminated with `SIGTERM` or `SIGHUP`, or when a run fails partway. It exits
with the session's own status.

## Build

```sh
make
```

Produces `dist/rootlet`; `make sudo` and `make connect` produce
`dist/sudo` and `dist/connect`. Object files are stored under `dist/deps/`.
Compiled with `-Wall -Wextra -Werror`.
`LDFLAGS`/`LDLIBS` are honored at link time. For profile-guided
optimization: `make PGO=generate`, exercise every binary's real code
paths, `make clean`, `make PGO=use` (`PROFDIR` overrides the profile
directory; with clang, run `make pgo-merge` before the use build).

## Usage

Run as root. The container root (`-m`, required) is purely assembled:
an image loop-mounted at it with `-i`, a directory bound at it with
`-i dir`, `-b` binds mounted into it (even `-b /bin:/bin
-b /usr:/usr` alone is enough to produce a rootfs-equivalent
directory), or nothing at all — a bare `-m` holds just the standard
mounts:

```sh
rootlet -m /mnt/ubuntu -i ~/ubuntu.img
rootlet -m /mnt/agent -i /srv/agent
rootlet -m /mnt/agent -b /bin:/bin -b /usr:/usr -b /etc:/etc -d
rootlet -m /mnt/bare
```

With `-i source:subpath` the session is set up one level down instead:
an image is mounted at the root and the tree is assembled at
`<root>/subpath`, which is then the chroot:

```sh
rootlet -m /mnt/agent -i ~/sparse.img:/rootfs
rootlet -m /mnt/agent -i /srv/agent:/rootfs -b /data:/data
```

The subpath is relative to the mounted root, must exist or be
creatable, and must not contain `..`. A `-b` guest of `/` is rejected:
the `/` mount itself always comes from `-i`, never from a bind. The
`-m` path
must hold no mounts when the run starts — neither as a mount point
itself nor with mounts anywhere under it (checked in
`/proc/self/mountinfo`) — because the teardown unmounts everything at
or under it on exit, so pre-existing mounts there would conflict. That
mount state is also the whole exclusion between runs: once a run has
put its mounts in the tree, any other run targeting it aborts on them.

Options:

```
-m mountpoint   absolute path used as the container root (required)
-i source       root source (optional): an image file loop-mounted at
                the root, or a directory bind-mounted there; with
                :subpath the session is set up and chrooted at
                <root>/subpath instead
-s login        login program run inside the chroot (default /usr/bin/su)
-b host[:guest] bind host path into the chroot at guest; guest defaults to
                host and must be absolute, and must not be /; a
                regular-file host is loop-mounted as an image instead of
                bound. Repeatable.
-p              run the session in new PID and mount namespaces
-d              enable debug output (same as DEBUG=1)
```

With `-p` the session runs in fresh PID and mount namespaces: `/proc` is
remounted from inside the new PID namespace, so `ps` in the chroot sees only
the container's own processes (the login shell is PID 1) and the extra mount
stays private to the session. Networking and hostname remain shared with the
host. If the kernel lacks PID namespace support, rootlet prints a warning and
falls back to a normal (non-isolated) session.

Besides the standard binds (`/dev`, `/proc`, `/sys`, …), `-b` adds
extra ones, and the target directory is created if the image lacks it:

```sh
rootlet -m /mnt/ubuntu -i ~/ubuntu.img -b /data/projects:/root/projects
```

An image file as a `-b` host is loop-mounted at the guest instead of
bound, with the filesystem type probed:

```sh
rootlet -m /mnt/agent -b /bin:/bin -b /src/sparse.erofs:/dest
```

An extra `-b` that fails to mount only warns and the run continues; a
root image that fails to attach or mount aborts the run.

Locking: an image-backed root flocks the image file, so a second run
fails instead of mounting the same image twice. Nothing else needs a
lock file.

## Requirements

- Root privileges (loop devices, `mount`, `chroot`).
- A root filesystem: an image with an ext4/ext3/ext2/f2fs (or
  erofs/squashfs, for `-b` image mounts) filesystem, or host directories
  to bind (`-i dir`) or assemble the tree with (`-b`).
