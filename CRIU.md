# Testing CRIU checkpoint/restore of the Bazel server

This is an **experimental** mode (Linux only) in which the Bazel launcher starts
the server inside a user + PID + mount namespace and transparently checkpoints
and restores a [CRIU](https://criu.org) checkpoint of a warm server under
`$output_base/criu/`. It is enabled by passing the `--criu`
startup option. See the implementation in `src/main/cpp/blaze_criu.{h,cc}` and
the overview in [`site/en/run/client-server.md`](site/en/run/client-server.md).

This document explains how to build it and exercise the full
start → checkpoint → restore cycle by hand.

## What the launcher does

When `--criu` is set, the launcher:

* starts the server in a fresh user+PID+mount namespace under a small
  persistent init, so the process tree has reproducible low PIDs that rootless
  CRIU can re-create on restore (the network namespace is shared with the host,
  so clients still reach the gRPC port over loopback);
* forces `--max_idle_secs=0` so the warm server never idles out;
* **auto-restores** `$output_base/criu/` before cold-starting, if a checkpoint
  is present;
* rewrites the server's on-disk identity to its *host* pid so a host-side client
  attaches, and skips the `/proc` start-time check.

The persistent init also serves a small **checkpoint control socket** at
`$output_base/bazel-criu.sock`. A separate `checkpoint` invocation asks that
init to run `criu dump` from *inside* the namespace — where the init holds
`CAP_CHECKPOINT_RESTORE` over its own user namespace and CRIU sees a
self-consistent mount view. That is what makes the dump succeed **rootless** (no
`sudo`), with the images owned by your user (no `chown`), and without having to
special-case the host's inherited snap/squashfs/FUSE mounts.

Before it starts, restores, or checkpoints a namespaced server, the launcher
runs a **preflight check** of the roadblocks below and, if any fails, exits with
an actionable message naming the knob and the command to fix it (rather than an
opaque `unshare`/`criu` error deep inside the namespace setup). Pass
`--client_debug` to see the observed state of every check logged even on
success.

## Prerequisites

* A Linux kernel with **unprivileged user namespaces** enabled. Common blockers,
  each caught by the preflight check:
  * AppArmor's `kernel.apparmor_restrict_unprivileged_userns` (Ubuntu 23.10+).
    If `cat /proc/sys/kernel/apparmor_restrict_unprivileged_userns` prints `1`,
    enable them with
    `sudo sysctl -w kernel.apparmor_restrict_unprivileged_userns=0`.
  * The older `kernel.unprivileged_userns_clone` knob on some distros
    (`sudo sysctl -w kernel.unprivileged_userns_clone=1`); on most modern distros
    it is on by default — `cat /proc/sys/kernel/unprivileged_userns_clone`
    should print `1`.
  * `user.max_user_namespaces` must be non-zero.
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
"$BAZEL" --criu --output_base="$OB" info server_pid	
```

The reported `server_pid` is the server's **host** pid, not its namespace-local
one. The server itself only sees its small namespace-local pid (e.g. `7`) via
`ProcessHandle`. The launcher resolves the host pid from `/proc` and publishes
it to `$OB/server/server.host_pid`; the CRIU-mode server reads that file and
stamps the host pid into `server_info.rawproto` itself (so the file is correct
on its first write, never advertising the namespace-local pid), and the
`server_pid` info item reads the same file. So `info server_pid` matches the pid
you see for the `java` process on the host — even across a restore, when the
host pid changes but the namespace-local pid does not. On restore, the launcher
publishes the new host pid and pokes the resumed JVM with `SIGWINCH`; the
server's signal handler re-reads `server.host_pid` and rewrites
`server_info.rawproto` with the new host pid, and the launcher waits for that
before attaching.

To confirm the server really lives in a PID namespace, inspect `/proc`
directly. Both the persisted init and the server carry `--output_base=$OB` on
their command line, which is the most robust thing to match on (the server's
`argv[0]` is `bazel(<workspace>)`, with parentheses that confuse `pgrep -f`
regexes):

```sh
# The init and the server both show up on the host as normal processes ...
pgrep -af -- "--output_base=$OB"

# ... and the server's /proc status lists more than one NSpid entry: the host
# pid and the small namespace-local pid. (The java process is the server; the
# other match is the bazel-dev init.)
for p in $(pgrep -f -- "--output_base=$OB"); do
  grep -q '^NSpid:.*[[:space:]].*[[:space:]]' /proc/$p/status && \
    echo "host pid $p:" && grep NSpid /proc/$p/status
done
# NSpid:	<host_pid>	<small namespace-local pid>
```

Run a build to warm the server, and confirm a second invocation reattaches
instead of restarting (it should not print "Starting local ... server"):

```sh
"$BAZEL" --criu --output_base="$OB" build //...
"$BAZEL" --criu --output_base="$OB" info server_pid   # same pid, instant
```

## 3. Take a checkpoint

The launcher takes the checkpoint for you, rootless, by asking the in-namespace
init to run `criu dump`. Run the `checkpoint` command; it short-circuits in the
launcher (it never reaches the server as a normal command):

```sh
"$BAZEL" --criu --output_base="$OB" checkpoint
# => criu: requesting checkpoint+stop -> .../criu
# => criu: checkpointed and stopped   (the server and its namespace are gone)
```

By default `checkpoint` dumps the server **and then tears it down** (e.g. to
free the machine before snapshotting it). To dump but leave the warm server
running (`--leave-running`), pass `--leave_running`:

```sh
"$BAZEL" --criu --output_base="$OB" checkpoint --leave_running
# => criu: requesting checkpoint -> .../criu
# => criu: checkpointed          (the server is still up and reattachable)
```

The `checkpoint` command requires CRIU mode; run it with `--criu`
set, as above. (It replaces the older `BAZEL_CRIU_CHECKPOINT` environment
variable.)

The dump runs inside the namespace, so:

* **no `sudo`** — the init holds `CAP_CHECKPOINT_RESTORE` over its own user
  namespace, and CRIU runs with `--unprivileged`;
* the image files in `$OB/criu/` are **owned by you** (no `chown` needed);
* the host's snap/squashfs/FUSE mounts appear as external rather than something
  CRIU must serialize, so **no `--enable-fs` workaround** is required.

The launcher records the checkpointed namespace-local pid in `$OB/criu/ns-pid`
so restore can locate the revived server.

`criu` must be on the launcher's `PATH` (it is the launcher process that the
init inherits its environment from). If `criu` lives somewhere non-standard
(e.g. `/usr/local/sbin`), add it to `PATH` or set `BAZEL_CRIU_BINARY` to its
absolute path before the checkpoint invocation.

If the checkpoint fails (`criu: checkpoint failed: ...`), read `$OB/criu/dump.log`.
Common causes:

* `Some file locks are hold by dumping tasks` — the launcher already passes
  `--file-locks`; if you still see this, an unexpected lock holder is in the
  tree.
* a deleted-but-mapped or volatile-`/tmp` native library. Two loaders extract
  `.so`s: bazel's own `JniLoader` (e.g. `libunix_jni.so`) and netty's loader
  (e.g. `libnetty_transport_native_epoll`). In CRIU mode the launcher points
  both at the persistent `$OB/jni/` dir and disables their delete-after-load, so
  the libs stay file-backed at a stable path that survives a reboot. If restore
  fails with `Can't open file tmp/libnetty...so`, the netty redirect did not
  take effect; confirm the relevant flags reached the server JVM with
  `tr '\0' '\n' < /proc/$(pgrep -f -- "A-server.*--output_base=$OB" | head -1)/cmdline | grep -e netty -e java.library.path`
  (expect `io.netty.native.workdir=$OB/jni`,
  `io.netty.native.deleteLibAfterLoading=false`), and that the libs are present
  under `$OB/jni/`.
* `criu: no namespaced server is serving the control socket` — there is no warm
  `--criu` server for this `--output_base`; start one first
  (step 2).

## 4. Tear the server down, then auto-restore

Kill the running server (and its init) to simulate a reboot / evicted runner.
The server runs as your user (it was started rootless), so no `sudo` is needed.
Matching on `--output_base=$OB` reaches both the server and its init in one go:

```sh
pkill -f -- "--output_base=$OB"          # the server and its persisted init
pgrep -af -- "--output_base=$OB"         # should be empty now
```

(Alternatively, the plain `checkpoint` command from step 3 dumps *and* tears the
server down in one step.)

The image and `ns-pid` file are still on disk under `$OB/criu/`. The next
ordinary invocation should detect there is no server, **restore the checkpoint**,
and reattach to the revived server — you will see a
`No server running; restoring CRIU checkpoint.` message, followed by
`Restored; server reachable at host pid <N>`:

```sh
"$BAZEL" --criu --output_base="$OB" info server_pid
# restores, then prints the revived server's host pid -- the same <N> as in the
# "Restored; server reachable at host pid <N>" line above (a new host pid, since
# restore re-creates the process; the namespace-local pid is unchanged)

"$BAZEL" --criu --output_base="$OB" build //...
# runs against the restored warm server; analysis cache is hot
```

Confirm the revived server is again in a PID namespace and is a *different* host
process than before the checkpoint:

```sh
for p in $(pgrep -f -- "--output_base=$OB"); do
  grep -q '^NSpid:.*[[:space:]].*[[:space:]]' /proc/$p/status && grep NSpid /proc/$p/status
done
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
* **Client starts a fresh server instead of attaching after restore.** The host
  pid did not propagate into `server_info.rawproto`. Check that
  `$OB/server/server.host_pid` holds the restored `java` process's host pid and
  that `server_info.rawproto`'s pid field matches it (the launcher pokes the
  server with `SIGWINCH` to rewrite rawproto from `server.host_pid` and waits up
  to 30s for it to converge before attaching), and that the restored `java`
  process is findable under the recorded `ns-pid`.
* **`UnsupportedClassVersionError` in the server log.** The
  `--server_javabase` does not match the JDK `bazel-dev` was built with; point it
  at the right JDK (step 1).
* **`unshare` / namespace errors at startup.** Unprivileged user namespaces are
  disabled; enable them (see Prerequisites).
* **Reset everything:** `rm -rf "$OB"` and start over from step 2.

## Notes

* This mode is a research experiment. Multi-server and cleanup edge cases are not
  hardened; use a dedicated `--output_base` as shown above.
* The in-namespace, rootless checkpoint design is taken from the original
  `horapha` wrapper; the `criu/ns-pid` layout is shared with it, so a checkpoint
  produced by `horapha checkpoint` is also restorable by this launcher, and vice
  versa.
