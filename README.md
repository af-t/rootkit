# rootlet

A tiny chroot launcher for running a Linux distribution from a disk image,
built for Android. It attaches an image to a loop device, mounts its root
filesystem, binds the host `/dev`, `/proc`, `/sys` and `/sdcard` into it, then
`chroot`s in and starts a login shell (`su -`) on its own pseudo-terminal.
On exit it tears everything back down: it kills the session's processes,
unmounts, and detaches the loop device. The same teardown runs when rootlet is
terminated with `SIGTERM` or `SIGHUP`, or when a run fails partway. It exits
with the session's own status.

## Build

```sh
make
```

Produces a single `rootlet` binary. Compiled with `-Wall -Wextra -Werror`.
`LDFLAGS`/`LDLIBS` are honored at link time. For profile-guided
optimization: `make PGO=generate`, exercise every binary's real code
paths, `make clean`, `make PGO=use` (`PROFDIR` overrides the profile
directory; with clang, run `make pgo-merge` before the use build).

## Usage

Run as root. With no arguments the image at `$HOME/distro.img` is mounted at
`/mnt/ubuntu`:

```sh
rootlet
```

Options:

```
-i image        distro image file (default $HOME/distro.img)
-m mountpoint   absolute path to mount and chroot into (default /mnt/ubuntu)
-s login        login program run inside the chroot (default /usr/bin/su)
-b host[:guest] bind host path into the chroot at guest; guest defaults to
                host and must be absolute. Repeatable.
-p              run the session in new PID and mount namespaces
-d              enable debug output (same as DEBUG=1)
```

With `-p` the session runs in fresh PID and mount namespaces: `/proc` is
remounted from inside the new PID namespace, so `ps` in the chroot sees only
the container's own processes (the login shell is PID 1) and the extra mount
stays private to the session. Networking and hostname remain shared with the
host. If the kernel lacks PID namespace support, rootlet prints a warning and
falls back to a normal (non-isolated) session.

Besides the standard binds (`/dev`, `/proc`, `/sys`, `/sdcard`, …), `-b` adds
extra ones, and the target directory is created if the image lacks it:

```sh
rootlet -b /data/projects:/root/projects -b /opt/toolchain
```

For example, to run a second image at its own mount point with diagnostics:

```sh
rootlet -i ~/alpine.img -m /mnt/alpine -d
```

## Requirements

- Root privileges (loop devices, `mount`, `chroot`).
- A distro image with an ext4/ext3/ext2/f2fs root filesystem.
