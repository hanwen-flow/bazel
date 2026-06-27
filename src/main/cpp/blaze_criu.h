// Copyright 2026 The Bazel Authors. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// blaze_criu folds the experimental "horapha" CRIU wrapper into the launcher
// itself: when the BAZEL_CRIU environment variable is set, the launcher starts
// the bazel server inside a fresh user+PID+mount namespace under a tiny
// persistent init, and transparently restores a previously-saved CRIU
// checkpoint from $output_base/criu/ when no server is running.
//
// The PID namespace gives the server a stable, low namespace-local pid that
// rootless CRIU can re-create exactly on restore; the (shared host) network
// namespace keeps the server's gRPC port reachable from ordinary host clients.
//
// Taking a checkpoint (criu dump) is driven from *inside* the namespace: the
// persistent init serves a small control socket on $output_base, and when a
// host-side launcher (invoked with BAZEL_CRIU_CHECKPOINT set) connects and asks
// it to dump, the init runs `criu dump --unprivileged` against the server. criu
// runs inside the user namespace it owns (where it holds CAP_CHECKPOINT_RESTORE
// and sees a self-consistent mount namespace), which is what lets a rootless,
// unprivileged dump succeed without root, without a chown, and without having
// to special-case the host's inherited snap/squashfs/FUSE mounts.
//
// Everything here is a no-op on non-Linux platforms and whenever BAZEL_CRIU is
// unset, so the normal launcher behavior is unchanged.

#ifndef BAZEL_SRC_MAIN_CPP_BLAZE_CRIU_H_
#define BAZEL_SRC_MAIN_CPP_BLAZE_CRIU_H_

#include <map>
#include <string>
#include <vector>

#include "src/main/cpp/blaze_util.h"
#include "src/main/cpp/blaze_util_platform.h"
#include "src/main/cpp/util/path.h"

namespace blaze {

// Returns true if launcher-driven CRIU checkpoint/restore is active, i.e. the
// BAZEL_CRIU environment variable is set and we are on a platform that supports
// it (Linux). When active the launcher:
//   - forces --max_idle_secs=0 so the warm server never self-terminates;
//   - starts the server inside a PID namespace under a persistent init;
//   - auto-restores $output_base/criu/ before starting a fresh server;
//   - drops the /proc starttime check in VerifyServerProcess.
bool CriuModeActive();

// Returns true if this invocation is a request to checkpoint the running
// server rather than to run a normal bazel command, i.e. the
// BAZEL_CRIU_CHECKPOINT environment variable is set (and we are on Linux). When
// true, Main short-circuits into CriuCheckpoint instead of RunLauncher.
bool CriuCheckpointRequested();

// Asks the in-namespace init serving output_base's control socket to checkpoint
// the running server into $output_base/criu/ (criu dump, rootless, from inside
// the namespace). If BAZEL_CRIU_CHECKPOINT=stop, the server is also torn down
// after the dump. `install_md5` identifies the binary that produced the server
// (it is the install_base's basename); it is recorded alongside the images so a
// later restore can reject a checkpoint taken by a different binary. Returns a
// process exit code (0 on success). Prints progress and any error to stderr.
int CriuCheckpoint(const blaze_util::Path &output_base,
                   const std::string &install_md5);

// Returns true if a usable CRIU checkpoint exists for output_base AND it was
// taken by the binary identified by `install_md5` (the install_base basename):
// the images dir must contain the recorded namespace-local pid that restore
// needs, and its recorded install key must match. A checkpoint from a different
// binary is treated as absent, so the launcher cold-starts the new binary
// rather than reviving a stale server.
bool CriuCheckpointExists(const blaze_util::Path &output_base,
                          const std::string &install_md5);

// Starts the bazel server inside a fresh user+PID+mount namespace, mirroring
// the contract of ExecuteDaemon: it spawns `exe args_vector` via the daemonize
// helper, but does so as the child of a persistent PID-1 init that outlives the
// launcher and holds the namespace (and the warm server) open for a later
// checkpoint.
//
// Unlike the plain ExecuteDaemon, this blocks until the server is up and its
// server_info.rawproto has been rewritten to advertise the server's *host* pid
// (so ordinary host clients can attach over loopback), and returns that host
// pid. *server_startup is set to an always-alive stub: liveness is established
// by the subsequent gRPC connect rather than by an inherited socket.
//
// Crashes (BAZEL_DIE) on unrecoverable failure.
int ExecuteDaemonInNamespace(
    const blaze_util::Path &exe, const std::vector<std::string> &args_vector,
    const std::map<std::string, EnvVarValue> &env,
    const blaze_util::Path &daemon_output, bool daemon_output_append,
    const blaze_util::Path &binaries_dir, const blaze_util::Path &server_dir,
    BlazeServerStartup **server_startup);

// Restores the bazel server from $output_base/criu/ inside a fresh namespace
// under a persistent init (criu restore --restore-detached), then rewrites the
// restored server's on-disk identity (server_info.rawproto pid + starttime) to
// its new host pid so a host-side client can attach. Returns true on success.
bool CriuRestore(const blaze_util::Path &output_base);

}  // namespace blaze

#endif  // BAZEL_SRC_MAIN_CPP_BLAZE_CRIU_H_
