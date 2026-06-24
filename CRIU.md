# Testing CRIU checkpoint/restore of the Bazel server

This is an **experimental** mode (Linux only) in which the Bazel launcher starts
the server inside a user + PID + mount namespace and transparently restores a
[CRIU](https://criu.org) checkpoint of a warm server from
`$output_base/criu/`. It is enabled by setting the `BAZEL_CRIU` environment
variable. See the implementation in `src/main/cpp/blaze_criu.{h,cc}` and the
overview in [`site/en/run/client-server.md`](site/en/run/client-server.md).

This document explains how to build it and exercise the full
start → checkpoint → restore cycle by hand.

## What the launcher does (and does not) do

When `BAZEL_CRIU` is set, the launcher:

* starts the server in a fresh user+PID+mount namespace under a small
  persistent init, so the process tree has reproducible low PIDs that rootless
  CRIU can re-create on restore (the network namespace is shared with the host,
  so clients still reach the gRPC port over loopback);
* forces `--max_idle_secs=0` so the warm server never idles out;
* **auto-restores** `$output_base/criu/` before cold-starting, if a checkpoint
  is present;
* rewrites the server's on-disk identity to its *host* pid so a host-side client
  attaches, and skips the `/proc` start-time check.

It does **not** take the checkpoint for you. Producing the `criu/` image is done
out of band; the manual `criu dump` recipe below is what to use for testing.

## Prerequisites

* A Linux kernel with **unprivileged user namespaces** enabled
  (`sysctl kernel.unprivileged_userns_clone=1` on some distros; on most modern
  distros it is on by default — `cat /proc/sys/kernel/unprivileged_userns_clone`
  should print `1`).
* **`criu` >= 3.18** on `$PATH` (4.x recommended). Override the binary with
  `BAZEL_CRIU_BINARY`. Check capabilities with `sudo criu check`.
* A JDK matching the one the `bazel-dev` binary was compiled for (e.g. Java 21);
  see the `--server_javabase` note below.

## 1. Build the patched `bazel-dev`

This branch carries the `JniLoader` patch (honouring `HORAPHA_JNI_DIR`) that
keeps the server's JNI libraries file-backed on disk, which rootless CRIU
requires — a deleted-but-mapped library cannot be dumped.

```sh
cd /path/to/bazel
bazel build //src:bazel-dev
# => bazel-bin/src/bazel-dev
```

The `bazel-dev` binary is compiled for a recent JDK, but the server defaults to
Bazel's embedded JDK (often 11), which fails with `UnsupportedClassVersionError`.
Run the server on a matching JDK via `--server_javabase`. The simplest way to
apply that flag (and keep the command lines short) is a tiny wrapper:

```sh
cat > /tmp/bazel-criu <<'EOF'
#!/bin/bash
exec /path/to/bazel/bazel-bin/src/bazel-dev \
  --server_javabase=/usr/lib/jvm/java-21-openjdk-amd64 "$@"
EOF
chmod +x /tmp/bazel-criu
```

Point the variable `BAZEL` at it for the rest of this doc, and pick a dedicated
output base so you do not disturb your normal server:

```sh
export BAZEL=/tmp/bazel-criu
export OB="$HOME/.cache/bazel-criu-test"
cd ~/my/workspace        # any dir under a MODULE.bazel / WORKSPACE
```

## 2. Start a namespaced server

```sh
BAZEL_CRIU=1 "$BAZEL" --output_base="$OB" info server_pid	
```

The reported `server_pid` is the server's **namespace-local** pid — it will be a
small number (e.g. `7`), not a host pid. Confirm the server really lives in a
PID namespace:

```sh
# The bazel server shows up on the host as a normal process ...
pgrep -af 'bazel(' | grep -v grep

# ... but its /proc status lists more than one NSpid entry (host + namespace):
HOST_PID=$(pgrep -f 'bazel(.*-server' | head -1)
grep NSpid /proc/$HOST_PID/status
# NSpid:	<host_pid>	<small namespace-local pid>
```

Run a build to warm the server, and confirm a second invocation reattaches
instead of restarting (it should not print "Starting local ... server"):

```sh
BAZEL_CRIU=1 "$BAZEL" --output_base="$OB" build //...
BAZEL_CRIU=1 "$BAZEL" --output_base="$OB" info server_pid   # same pid, instant
```

## 3. Take a checkpoint

CRIU needs `CAP_CHECKPOINT_RESTORE`. The easy route for testing is to dump as
real root with `sudo`. The launcher's restore reads the checkpointed
namespace-local pid from `$OB/criu/ns-pid`, so we record it alongside the image.

```sh
mkdir -p "$OB/criu"

# The namespace-local pid the server advertises to itself lives in
# server.pid.txt (the launcher deliberately leaves it untouched).
NS_PID=$(cat "$OB/server/server.pid.txt")
HOST_PID=$(pgrep -f 'bazel(.*-server' | head -1)

# Dump the whole server tree by its HOST pid, leaving it running.
#
# --enable-fs squashfs,fuse,autofs is essential: the server's mount namespace
# inherits the host's snap (squashfs) and FUSE mounts, locked from the outer
# user namespace so they cannot be detached. Without this flag criu dump fails
# with "Error (criu/mount.c): mnt: FS mnt ./snap/... unsupported id". The flag
# tells criu to ignore those filesystems (a build never touches them). The
# launcher passes the same flag on restore.
sudo criu dump \
  --tree "$HOST_PID" \
  --images-dir "$OB/criu" \
  --leave-running \
  --shell-job \
  --tcp-close \
  --skip-file-rwx-check \
  --enable-fs squashfs,fuse,autofs \
  --ghost-limit 1000000000 \
  --log-file "$OB/criu/dump.log" -v4

# Record the namespace-local pid so the launcher can find the restored server.
echo "$NS_PID" > "$OB/criu/ns-pid"

# criu writes its image files as the user that ran the dump — here, root (mode
# 0600). There is no criu option to write them as another user. Restore runs
# criu *inside the user namespace*, where your login user is only mapped to root
# within that namespace; the image inodes are still owned by host-root on disk
# and would be unreadable. Hand them back to your user so restore can read them.
sudo chown -R "$(id -u):$(id -g)" "$OB/criu"
```

> If you prefer to avoid root (and the chown) entirely, the original `horapha`
> wrapper dumps *rootless from inside the namespace*: the in-namespace init holds
> `CAP_CHECKPOINT_RESTORE` over its own user namespace, so criu runs as your user
> and the images are owned by you from the start. This launcher does not (yet)
> drive `criu dump` itself, so the `sudo` recipe above is what to use for testing.

If `criu dump` fails, read `$OB/criu/dump.log`. Two common causes:

* `mnt: FS mnt ./snap/... unsupported id` — a snap/FUSE mount criu won't dump;
  add `--enable-fs squashfs,fuse,autofs` (already in the recipe above).
* a deleted-but-mapped file (a JNI `.so`); the `JniLoader` patch and the
  `-Dio.netty.native.deleteLibAfterLoading=false` flag the launcher injects are
  what prevent that — verify they are in effect with
  `tr '\0' '\n' < /proc/$HOST_PID/cmdline | grep -e netty -e java.library.path`.

## 4. Tear the server down, then auto-restore

Kill the running server (and its init) to simulate a reboot / evicted runner:

```sh
sudo pkill -f 'bazel(.*-server'      # the server
pkill -f 'src/bazel-dev'             # any lingering launcher/init
pgrep -af 'bazel(' | grep -v grep    # should be empty now
```

The image and `ns-pid` file are still on disk under `$OB/criu/`. The next
ordinary invocation should detect there is no server, **restore the checkpoint**,
and reattach to the revived server — you will see a
`No server running; restoring CRIU checkpoint.` message, followed by
`Restored; server reachable at host pid <N>`:

```sh
BAZEL_CRIU=1 "$BAZEL" --output_base="$OB" info server_pid
# restores, then prints the original (namespace-local) server_pid

BAZEL_CRIU=1 "$BAZEL" --output_base="$OB" build //...
# runs against the restored warm server; analysis cache is hot
```

Confirm the revived server is again in a PID namespace and is a *different* host
process than before the checkpoint:

```sh
HOST_PID=$(pgrep -f 'bazel(.*-server' | head -1)
grep NSpid /proc/$HOST_PID/status
```

## Troubleshooting

* **`No server running; restoring CRIU checkpoint.` then a cold start.** Restore
  failed; see `$OB/criu/restore.log`. Common causes: a `criu` too old, a kernel
  without `CAP_CHECKPOINT_RESTORE`, or an image taken from an incompatible
  kernel/host.
* **`criu: criu: No such file or directory` on restore.** `criu` is resolved
  against the launcher's `PATH`. If it lives somewhere non-standard (e.g.
  `/usr/local/sbin`), either add that to `PATH` or set `BAZEL_CRIU_BINARY` to its
  absolute path.
* **Client starts a fresh server instead of attaching after restore.** The
  on-disk identity rewrite did not land. Check that `$OB/server/server.starttime`
  and `server_info.rawproto` exist, and that the restored `java` process is
  findable under the recorded `ns-pid` (the launcher waits up to 30s for the
  reparent to settle).
* **`UnsupportedClassVersionError` in the server log.** The
  `--server_javabase` does not match the JDK `bazel-dev` was built with; point it
  at the right JDK (step 1).
* **`unshare` / namespace errors at startup.** Unprivileged user namespaces are
  disabled; enable them (see Prerequisites).
* **Reset everything:** `rm -rf "$OB"` and start over from step 2.

## Notes

* This mode is a research experiment. Multi-server and cleanup edge cases are not
  hardened; use a dedicated `--output_base` as shown above.
* The `criu/ns-pid` layout is shared with the original `horapha` wrapper, so a
  checkpoint produced by `horapha checkpoint` is also restorable by this
  launcher.
