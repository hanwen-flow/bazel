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

#include "src/main/cpp/blaze_criu.h"

#include <cstdlib>

#include "src/main/cpp/blaze_util.h"
#include "src/main/cpp/blaze_util_platform.h"
#include "src/main/cpp/util/errors.h"
#include "src/main/cpp/util/exit_code.h"
#include "src/main/cpp/util/file.h"
#include "src/main/cpp/util/logging.h"
#include "src/main/cpp/util/numbers.h"
#include "src/main/cpp/util/strings.h"

namespace blaze {

using std::map;
using std::string;
using std::vector;

namespace {
// Subdirectory of output_base holding CRIU checkpoint images.
constexpr char kCriuImagesSubdir[] = "criu";
// File (inside the images dir) recording the checkpointed namespace-local pid,
// written at checkpoint time. Restore uses it to find the restored server
// without trusting bazel's mutable server.pid.txt. Matches horapha's
// serverinfo.CheckpointPIDName.
constexpr char kNsPidFile[] = "ns-pid";
// Filesystem types CRIU should tolerate rather than refuse to dump/restore.
// The server's mount namespace inherits the host's FUSE/squashfs/autofs mounts
// (e.g. snap packages under /snap) locked from the outer user namespace, so we
// cannot detach them; --enable-fs makes CRIU ignore them instead of failing
// with "unsupported id". A build never touches these mounts. The matching flag
// must be passed to the manual `criu dump` too (see CRIU.md).
constexpr char kCriuEnableFs[] = "squashfs,fuse,autofs";
}  // namespace

bool CriuModeActive() {
#ifdef __linux__
  return ExistsEnv("BAZEL_CRIU");
#else
  return false;
#endif
}

bool CriuCheckpointExists(const blaze_util::Path &output_base) {
  return blaze_util::PathExists(
      output_base.GetRelative(kCriuImagesSubdir).GetRelative(kNsPidFile));
}

}  // namespace blaze

#ifndef __linux__

// On non-Linux platforms CRIU mode is never active; these are unreachable
// stubs to keep the launcher linking on every platform.
namespace blaze {

int ExecuteDaemonInNamespace(const blaze_util::Path &, const vector<string> &,
                             const map<string, EnvVarValue> &,
                             const blaze_util::Path &, bool,
                             const blaze_util::Path &, const blaze_util::Path &,
                             BlazeServerStartup **) {
  BAZEL_DIE(blaze_exit_code::INTERNAL_ERROR)
      << "BAZEL_CRIU is only supported on Linux.";
  return -1;
}

bool CriuRestore(const blaze_util::Path &) { return false; }

}  // namespace blaze

#else  // __linux__

#include <dirent.h>
#include <fcntl.h>
#include <sched.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#ifndef environ
extern char **environ;
#endif

namespace blaze {
namespace {

// Returns s with leading/trailing whitespace removed.
string Stripped(const string &s) {
  string out = s;
  blaze_util::StripWhitespace(&out);
  return out;
}

// Writes "criu: <msg>: <errno string>\n" to stderr. For use in the post-fork
// child paths, where BAZEL_LOG is not fork-safe; the init's stderr is still
// connected to the launcher's terminal until it detaches, so these surface.
void ChildPerror(const char *msg) {
  int e = errno;
  const char *es = strerror(e);
  char buf[256];
  int n = snprintf(buf, sizeof(buf), "criu: %s: %s\n", msg, es);
  if (n > 0) {
    (void)!write(STDERR_FILENO, buf, n < static_cast<int>(sizeof(buf))
                                         ? n
                                         : static_cast<int>(sizeof(buf)) - 1);
  }
}

// An always-alive startup stub. In CRIU mode the server lives in a PID
// namespace under a persistent init, so we cannot use the inherited-socket
// liveness trick of SocketBlazeServerStartup. Instead the subsequent gRPC
// connect (ConnectOrDie) decides whether startup succeeded.
class AlwaysAliveServerStartup : public BlazeServerStartup {
 public:
  bool IsStillAlive() override { return true; }
};

// Writes the whole of `content` to the file at `path` (used for the
// /proc/self/{setgroups,uid_map,gid_map} userns files). Returns false on any
// short write or error. Async-signal-safe: only open/write/close.
bool WriteProcFile(const char *path, const string &content) {
  int fd = open(path, O_WRONLY | O_CLOEXEC);
  if (fd < 0) {
    ChildPerror(path);
    return false;
  }
  ssize_t n = write(fd, content.data(), content.size());
  bool ok = n == static_cast<ssize_t>(content.size());
  if (!ok) {
    ChildPerror(path);
  }
  close(fd);
  return ok;
}

// Maps host uid/gid to root (0) inside the user namespace newly created by the
// process `pid`, using the single-mapping exception that needs no privilege.
//
// This MUST be done by `pid`'s parent (which still has privilege over the new,
// nested user namespace), not by `pid` itself: once a process is inside the new
// userns it is mapped to "nobody" and cannot write its own map. setgroups must
// be denied before gid_map may be written unprivileged.
bool WriteUserNamespaceMappings(pid_t pid, uid_t uid, gid_t gid) {
  char path[64];
  char buf[64];

  snprintf(path, sizeof(path), "/proc/%d/uid_map", pid);
  int len = snprintf(buf, sizeof(buf), "0 %d 1", uid);
  if (!WriteProcFile(path, string(buf, len))) {
    return false;
  }

  snprintf(path, sizeof(path), "/proc/%d/setgroups", pid);
  if (!WriteProcFile(path, "deny")) {
    return false;
  }

  snprintf(path, sizeof(path), "/proc/%d/gid_map", pid);
  len = snprintf(buf, sizeof(buf), "0 %d 1", gid);
  if (!WriteProcFile(path, string(buf, len))) {
    return false;
  }
  return true;
}

// Mounts a private /proc inside the new mount + PID namespace so that the init
// and CRIU see the namespaced process view. Mirrors horapha's mountProc().
//
// Note: we cannot strip the host's FUSE/squashfs/snap mounts here — they are
// inherited locked from the outer user namespace and cannot be unmounted or
// overmounted from our nested namespace. Instead, CRIU is told to tolerate
// those filesystems with --enable-fs (see kCriuEnableFs) on both dump and
// restore.
bool MountPrivateProc() {
  if (mount("", "/", "", MS_REC | MS_PRIVATE, nullptr) != 0) {
    ChildPerror("mount / rprivate");
    return false;
  }
  if (mount("proc", "/proc", "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC,
            nullptr) != 0) {
    ChildPerror("mount /proc");
    return false;
  }
  return true;
}

// Converts wait(2) status to a process exit code (128+signal for signals).
int WaitStatusToCode(int status) {
  if (WIFEXITED(status)) {
    return WEXITSTATUS(status);
  }
  if (WIFSIGNALED(status)) {
    return 128 + WTERMSIG(status);
  }
  return 1;
}

// Closes every inherited file descriptor >= 3 except `keep`. The launcher holds
// the output-base lock (and other fds) on descriptors that would otherwise be
// inherited by the persistent init and the server it forks — and an inherited
// flock would block every future client on the output base forever. We scan
// /proc/self/fd so we close exactly the open ones. Fork-safe: only
// opendir/readdir/close.
void CloseInheritedFds(int keep) {
  DIR *d = opendir("/proc/self/fd");
  if (d == nullptr) {
    // Fallback: blind-close a conservative range.
    for (int fd = 3; fd < 1024; fd++) {
      if (fd != keep) {
        close(fd);
      }
    }
    return;
  }
  int dir_fd = dirfd(d);
  for (struct dirent *e = readdir(d); e != nullptr; e = readdir(d)) {
    int fd = 0;
    // Manual atoi to avoid pulling non-fork-safe helpers; entries are "." etc.
    bool numeric = e->d_name[0] != '\0';
    for (const char *p = e->d_name; *p; p++) {
      if (*p < '0' || *p > '9') {
        numeric = false;
        break;
      }
      fd = fd * 10 + (*p - '0');
    }
    if (numeric && fd >= 3 && fd != keep && fd != dir_fd) {
      close(fd);
    }
  }
  closedir(d);
}

// Builds a NULL-terminated argv/envp array from C++ containers. The backing
// strings must outlive the returned pointer array.
vector<char *> ToCStringArray(const vector<string> &items) {
  vector<char *> out;
  out.reserve(items.size() + 1);
  for (const string &s : items) {
    out.push_back(const_cast<char *>(s.c_str()));
  }
  out.push_back(nullptr);
  return out;
}

vector<string> EnvToStrings(const map<string, EnvVarValue> &env) {
  vector<string> out;
  for (const auto &kv : env) {
    if (kv.second.action == EnvVarAction::SET) {
      out.push_back(kv.first + "=" + kv.second.value);
    }
  }
  return out;
}

// The body of the persistent PID-1 init. Never returns: it forks the
// foreground command (argv/envp), waits for it, reports the foreground exit
// code to the launcher over status_wfd, then keeps reaping the (daemonized or
// restored) server tree until the namespace empties, so the namespace stays
// alive for a later checkpoint. Runs in a freshly forked process and performs
// no heap allocation (the argv/envp arrays are built by the caller before the
// fork), using only fork/exec/wait/mount/open/write to stay fork-safe.
[[noreturn]] void RunInit(char *const argv[], char *const envp[],
                          int status_wfd) {
  // Drop every inherited fd except the status pipe — crucially the launcher's
  // output-base lock, which the persistent init and server would otherwise hold
  // forever, blocking all future clients. Do this first, before we fork the
  // server, so the server does not inherit them either.
  CloseInheritedFds(status_wfd);

  // Detach from the launcher's session/terminal so shell signals (Ctrl-C) do
  // not reach the persisted init or the server tree it holds.
  setsid();

  if (!MountPrivateProc()) {
    char code = 1;
    (void)!write(status_wfd, &code, 1);
    _exit(1);
  }

  pid_t fg = fork();
  if (fg < 0) {
    ChildPerror("fork foreground");
    char code = 1;
    (void)!write(status_wfd, &code, 1);
    _exit(1);
  }
  if (fg == 0) {
    // execvpe (not execve) so a bare command name like "criu" is resolved
    // against PATH. The fresh-start path passes an absolute daemonize path,
    // which execvpe handles too.
    execvpe(argv[0], argv, envp);
    ChildPerror(argv[0]);
    _exit(127);  // exec failed
  }

  // Reaper loop. Record the foreground command's status when it is reaped,
  // report it to the launcher, then keep reaping the remaining tree (the
  // persisted server) until the namespace empties.
  int fg_code = 1;
  bool reported = false;
  while (true) {
    int status;
    pid_t w = waitpid(-1, &status, 0);
    if (w < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;  // ECHILD: namespace is empty.
    }
    if (w == fg && !reported) {
      fg_code = WaitStatusToCode(status);
      char code = static_cast<char>(fg_code);
      (void)!write(status_wfd, &code, 1);
      close(status_wfd);
      reported = true;

      // Detach stdio so the launcher's shell sees EOF and the init runs quietly.
      int devnull = open("/dev/null", O_RDWR);
      if (devnull >= 0) {
        dup2(devnull, 0);
        dup2(devnull, 1);
        dup2(devnull, 2);
        if (devnull > 2) {
          close(devnull);
        }
      }
    }
  }
  if (!reported) {
    char code = static_cast<char>(fg_code);
    (void)!write(status_wfd, &code, 1);
  }
  _exit(fg_code);
}

// Starts a persistent PID-1 init in fresh user+PID+mount namespaces that runs
// `fg_argv` (with environment `fg_env`) as its foreground command. Returns the
// foreground command's exit code via *fg_code, the init's *host* pid via
// *init_host_pid (so callers can identify our PID namespace unambiguously), and
// true on a successful launch.
//
// The init outlives this call (it holds the namespace and the server open), so
// we deliberately do not wait for it; we only reap the throwaway userns-setup
// child. Mirrors horapha's nsrun.Run + runInit.
bool RunInNamespace(const vector<string> &fg_argv, const vector<string> &fg_env,
                    int *fg_code, int *init_host_pid) {
  // Build the NULL-terminated argv/envp arrays BEFORE forking, so the post-fork
  // code paths perform no heap allocation and stay async-fork-safe. The backing
  // string vectors must outlive the fork.
  vector<char *> argv = ToCStringArray(fg_argv);
  vector<char *> envp = ToCStringArray(fg_env);
  const uid_t uid = getuid();
  const gid_t gid = getgid();

  // status_pipe: the init reports the foreground exit code (one byte) to us.
  // map_ready/map_request: synchronize the user-namespace setup. The setup
  // child creates the userns, signals us (1 byte) so we can write its uid/gid
  // map from out here — which MUST be done by the parent, since a process
  // inside a freshly created (nested) userns is "nobody" and cannot write its
  // own map — then we signal back (1 byte) that the maps are in place.
  // info_pipe: the setup child sends us the init's host pid (4 bytes), so we
  // can identify our own server's PID namespace and not confuse it with another
  // namespaced server that happens to share the same ns-local pid.
  int status_pipe[2];
  int map_ready[2];    // parent -> child: maps written, proceed
  int map_request[2];  // child -> parent: userns created, please write maps
  int info_pipe[2];    // child -> parent: init host pid
  if (pipe(status_pipe) != 0 || pipe(map_ready) != 0 ||
      pipe(map_request) != 0 || pipe(info_pipe) != 0) {
    BAZEL_LOG(USER) << "criu: pipe failed: " << blaze_util::GetLastErrorString();
    return false;
  }

  pid_t setup = fork();
  if (setup < 0) {
    BAZEL_LOG(USER) << "criu: fork failed: " << blaze_util::GetLastErrorString();
    return false;
  }

  if (setup == 0) {
    // Throwaway userns-setup child (stays in the host PID namespace). It creates
    // the user namespace, waits for the parent to write its uid/gid map, then
    // unshares the PID and mount namespaces so its first child (the init)
    // becomes PID 1 of the new PID namespace. It forks that init and exits; the
    // init reparents to a host subreaper and persists.
    close(status_pipe[0]);
    close(map_ready[1]);
    close(map_request[0]);
    close(info_pipe[0]);

    char b = 1;
    if (unshare(CLONE_NEWUSER) != 0) {
      ChildPerror("unshare(CLONE_NEWUSER)");
      (void)!write(status_pipe[1], &b, 1);
      _exit(1);
    }
    // Ask the parent to write our maps, and wait until it has.
    (void)!write(map_request[1], &b, 1);
    if (read(map_ready[0], &b, 1) != 1 || b != 0) {
      // Parent failed to write the maps (it already logged why).
      (void)!write(status_pipe[1], &b, 1);
      _exit(1);
    }
    close(map_ready[0]);
    close(map_request[1]);

    if (unshare(CLONE_NEWNS | CLONE_NEWPID) != 0) {
      ChildPerror("unshare(CLONE_NEWNS|CLONE_NEWPID)");
      char code = 1;
      (void)!write(status_pipe[1], &code, 1);
      _exit(1);
    }

    // This fork runs in the host PID namespace (we have only unshared, not yet
    // entered, the new PID namespace), so `init` is the init's HOST pid. The
    // init itself becomes PID 1 inside the new namespace.
    pid_t init = fork();
    if (init < 0) {
      ChildPerror("fork init");
      char code = 1;
      (void)!write(status_pipe[1], &code, 1);
      _exit(1);
    }
    if (init == 0) {
      RunInit(argv.data(), envp.data(), status_pipe[1]);  // never returns
    }
    // Tell the launcher the init's host pid, then exit. The init keeps
    // status_pipe[1] open and reparents to a host subreaper.
    int init_pid = static_cast<int>(init);
    (void)!write(info_pipe[1], &init_pid, sizeof(init_pid));
    close(info_pipe[1]);
    close(status_pipe[1]);
    _exit(0);
  }

  // Launcher (parent).
  close(status_pipe[1]);
  close(map_ready[0]);
  close(map_request[1]);
  close(info_pipe[1]);

  // Wait for the setup child to create its userns, then write its uid/gid map.
  char req = 0;
  bool mapped = false;
  if (read(map_request[0], &req, 1) == 1) {
    mapped = WriteUserNamespaceMappings(setup, uid, gid);
  }
  char ack = mapped ? 0 : 1;
  (void)!write(map_ready[1], &ack, 1);
  close(map_ready[1]);
  close(map_request[0]);

  // Receive the init's host pid.
  int init_pid = 0;
  ssize_t got = read(info_pipe[0], &init_pid, sizeof(init_pid));
  close(info_pipe[0]);

  // Reap the throwaway setup child, then await the init's report.
  int setup_status;
  while (waitpid(setup, &setup_status, 0) < 0 && errno == EINTR) {
  }

  char code = 0;
  ssize_t n = read(status_pipe[0], &code, 1);
  close(status_pipe[0]);
  if (!mapped) {
    return false;  // mapping failure already logged by WriteProcFile/ChildPerror
  }
  if (n != 1) {
    BAZEL_LOG(USER) << "criu: namespaced init exited before reporting status.";
    return false;
  }
  if (got != sizeof(init_pid) || init_pid <= 0) {
    BAZEL_LOG(USER) << "criu: did not learn the namespaced init's host pid.";
    return false;
  }
  *fg_code = static_cast<int>(code);
  *init_host_pid = init_pid;
  return true;
}

// ---------------------------------------------------------------------------
// serverinfo: making a namespaced server reachable by a host-side client.
// Ported from horapha's internal/serverinfo.
// ---------------------------------------------------------------------------

// Reads field 22 (start time, jiffies since boot) of /proc/<pid>/stat as a
// decimal string. The comm field (2nd) is parenthesized and may contain
// spaces, so we split after the last ')'.
bool ReadStartTime(int pid, string *start_time) {
  string statline;
  if (!blaze_util::ReadFile("/proc/" + std::to_string(pid) + "/stat",
                            &statline)) {
    return false;
  }
  string::size_type rparen = statline.rfind(')');
  if (rparen == string::npos) {
    return false;
  }
  // blaze_util::Split skips empty subsections, so the leading space after ')'
  // does not produce a spurious token. After ')' the first field is #3
  // (state); start time is field #22, i.e. index 19 counting from #3.
  vector<string> fields = blaze_util::Split(statline.substr(rparen + 1), ' ');
  const size_t kStartTimeIndexAfterComm = 19;
  if (fields.size() <= kStartTimeIndexAfterComm) {
    return false;
  }
  *start_time = fields[kStartTimeIndexAfterComm];
  return true;
}

// Returns the innermost-namespace pid for a host pid, and whether the process
// is actually in a nested PID namespace (the NSpid line has >1 field).
bool InnerNsPid(int host_pid, int *inner) {
  string status;
  if (!blaze_util::ReadFile("/proc/" + std::to_string(host_pid) + "/status",
                            &status)) {
    return false;
  }
  for (const string &line : blaze_util::Split(status, '\n')) {
    if (line.compare(0, 6, "NSpid:") != 0) {
      continue;
    }
    // The NSpid line lists the pid in each pid namespace from outermost (host)
    // to innermost; the last (tab/space-separated) field is the innermost pid.
    // A single field means the process is not in a nested namespace.
    vector<string> fields = blaze_util::Split(Stripped(line.substr(6)), '\t');
    if (fields.size() < 2) {
      return false;
    }
    return blaze_util::safe_strto32(Stripped(fields.back()), inner);
  }
  return false;
}

// Returns the comm (executable name) of a host pid.
string ReadComm(int host_pid) {
  string comm;
  if (!blaze_util::ReadFile("/proc/" + std::to_string(host_pid) + "/comm",
                            &comm)) {
    return "";
  }
  return Stripped(comm);
}

// Returns the PID-namespace identity of a host pid as the symlink target of
// /proc/<pid>/ns/pid (e.g. "pid:[4026533181]"), or "" on error. Two processes
// share a PID namespace iff these strings are equal.
string PidNamespaceOf(int host_pid) {
  char target[256];
  string link = "/proc/" + std::to_string(host_pid) + "/ns/pid";
  ssize_t n = readlink(link.c_str(), target, sizeof(target) - 1);
  if (n < 0) {
    return "";
  }
  return string(target, n);
}

// Scans /proc and returns the host pid of the process whose innermost namespace
// pid equals ns_pid, whose comm matches want_comm (if non-empty), AND which
// lives in the SAME PID namespace as ns_namespace. The namespace match is
// essential: several independent namespaced servers can each have the same
// low ns-local pid (e.g. 3), so ns_pid alone is ambiguous — ns_namespace
// (the PID-namespace identity of our own init) disambiguates to our server.
bool HostPidForNsPid(int ns_pid, const string &want_comm,
                     const string &ns_namespace, int *host_pid) {
  DIR *proc = opendir("/proc");
  if (proc == nullptr) {
    return false;
  }
  bool found = false;
  for (struct dirent *e = readdir(proc); e != nullptr; e = readdir(proc)) {
    int candidate;
    if (!blaze_util::safe_strto32(e->d_name, &candidate)) {
      continue;  // not a pid dir
    }
    int inner;
    if (!InnerNsPid(candidate, &inner)) {
      continue;
    }
    // candidate != ns_pid guards against matching the process in its own (host)
    // namespace where the two are trivially equal.
    if (inner != ns_pid || candidate == ns_pid) {
      continue;
    }
    if (!want_comm.empty() && ReadComm(candidate) != want_comm) {
      continue;
    }
    if (!ns_namespace.empty() && PidNamespaceOf(candidate) != ns_namespace) {
      continue;  // a different namespaced server that happens to share ns_pid
    }
    *host_pid = candidate;
    found = true;
    break;
  }
  closedir(proc);
  return found;
}

// Polls HostPidForNsPid until a matching process appears or the timeout
// elapses. After a restore the tree is reparented asynchronously, so the
// server may take a moment to settle.
bool WaitHostPidForNsPid(int ns_pid, const string &want_comm,
                         const string &ns_namespace, int timeout_secs,
                         int *host_pid) {
  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(timeout_secs);
  while (true) {
    if (HostPidForNsPid(ns_pid, want_comm, ns_namespace, host_pid)) {
      return true;
    }
    if (std::chrono::steady_clock::now() > deadline) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
}

// Rewrites field 1 (pid, a varint) of the ServerInfo proto at `path` to new_pid,
// leaving every other field byte-for-byte intact. ServerInfo declares pid as
// field 1 and protobuf serializes fields in order, so it is first (leading byte
// 0x08 == (field 1 << 3) | wiretype 0). Ported from horapha's
// PatchRawprotoPID.
bool PatchRawprotoPid(const blaze_util::Path &path, int new_pid) {
  // daemonize returns as soon as the pid file exists, but the server writes
  // server_info.rawproto a little later during startup. Wait for it to appear
  // (and be non-empty) before patching.
  string data;
  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (true) {
    if (blaze_util::ReadFile(path, &data) && !data.empty()) {
      break;
    }
    if (std::chrono::steady_clock::now() > deadline) {
      BAZEL_LOG(USER) << "criu: timed out waiting for server_info.rawproto: "
                      << path.AsPrintablePath();
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  if (static_cast<unsigned char>(data[0]) != 0x08) {
    BAZEL_LOG(USER) << "criu: rawproto bad lead byte (size=" << data.size()
                    << ")";
    return false;
  }
  // Skip the existing varint value.
  size_t i = 1;
  while (i < data.size()) {
    unsigned char b = static_cast<unsigned char>(data[i]);
    i++;
    if ((b & 0x80) == 0) {
      break;
    }
  }
  string rest = data.substr(i);

  string out;
  out.push_back(0x08);
  uint64_t v = static_cast<uint64_t>(new_pid);
  while (v >= 0x80) {
    out.push_back(static_cast<char>((v & 0x7f) | 0x80));
    v >>= 7;
  }
  out.push_back(static_cast<char>(v));
  out += rest;

  // Write atomically so a reader never sees a half-written proto.
  blaze_util::Path tmp(path.AsNativePath() + ".bazel-criu.tmp");
  if (!blaze_util::WriteFile(out, tmp)) {
    BAZEL_LOG(USER) << "criu: rawproto tmp write failed: "
                    << tmp.AsPrintablePath();
    return false;
  }
  if (rename(tmp.AsNativePath().c_str(), path.AsNativePath().c_str()) != 0) {
    BAZEL_LOG(USER) << "criu: rawproto rename failed: "
                    << blaze_util::GetLastErrorString();
    return false;
  }
  return true;
}

// Makes the server identified by host_pid reachable by a host-side client, by
// writing host_pid into rawproto's pid field and host_pid's current start time
// into server.starttime. Use after a restore (new pid, new start time) or after
// a fresh namespaced start (the server advertised its namespace-local pid).
bool RewriteServerIdentity(const blaze_util::Path &server_dir, int host_pid) {
  string start_time;
  if (!ReadStartTime(host_pid, &start_time)) {
    BAZEL_LOG(USER) << "criu: could not read start time of host pid "
                    << host_pid;
    return false;
  }
  if (!PatchRawprotoPid(server_dir.GetRelative("server_info.rawproto"),
                        host_pid)) {
    BAZEL_LOG(USER) << "criu: could not patch server_info.rawproto pid";
    return false;
  }
  if (!blaze_util::WriteFile(start_time,
                             server_dir.GetRelative("server.starttime"))) {
    BAZEL_LOG(USER) << "criu: could not write server.starttime";
    return false;
  }
  return true;
}

// Resolves the server's namespace-local pid to its host pid (within the PID
// namespace identified by ns_namespace) and rewrites the on-disk identity
// files. Returns the host pid, or -1 on failure.
int MakeReachable(int ns_pid, const string &ns_namespace,
                  const blaze_util::Path &server_dir, int timeout_secs) {
  int host_pid;
  if (!WaitHostPidForNsPid(ns_pid, "java", ns_namespace, timeout_secs,
                           &host_pid)) {
    BAZEL_LOG(USER) << "criu: could not find host pid for namespace pid "
                    << ns_pid;
    return -1;
  }
  if (!RewriteServerIdentity(server_dir, host_pid)) {
    BAZEL_LOG(USER) << "criu: could not rewrite server identity for host pid "
                    << host_pid;
    return -1;
  }
  return host_pid;
}

// Reads a pid from a "<n>\n" file.
bool ReadPidFile(const blaze_util::Path &path, int *pid) {
  string bufstr;
  return blaze_util::ReadFile(path, &bufstr, 32) &&
         blaze_util::safe_strto32(Stripped(bufstr), pid);
}

string CriuBinary() {
  string criu = GetEnv("BAZEL_CRIU_BINARY");
  return criu.empty() ? "criu" : criu;
}

}  // namespace

int ExecuteDaemonInNamespace(const blaze_util::Path &exe,
                             const vector<string> &args_vector,
                             const map<string, EnvVarValue> &env,
                             const blaze_util::Path &daemon_output,
                             bool daemon_output_append,
                             const blaze_util::Path &binaries_dir,
                             const blaze_util::Path &server_dir,
                             BlazeServerStartup **server_startup) {
  const blaze_util::Path pid_file = server_dir.GetRelative(kServerPidFile);
  const string daemonize = binaries_dir.GetRelative("daemonize").AsNativePath();

  // Same daemonize invocation as ExecuteDaemon, minus the cgroup/systemd
  // options: a rootless user namespace cannot drive systemd-run, and CRIU does
  // not coexist cleanly with cgroup placement here.
  vector<string> fg_argv = {daemonize, "-l", daemon_output.AsNativePath(), "-p",
                            pid_file.AsNativePath()};
  if (daemon_output_append) {
    fg_argv.push_back("-a");
  }
  fg_argv.push_back("--");
  fg_argv.push_back(exe.AsNativePath());
  fg_argv.insert(fg_argv.end(), args_vector.begin(), args_vector.end());

  int fg_code = 0;
  int init_host_pid = 0;
  if (!RunInNamespace(fg_argv, EnvToStrings(env), &fg_code, &init_host_pid) ||
      fg_code != 0) {
    BAZEL_DIE(blaze_exit_code::INTERNAL_ERROR)
        << "criu: failed to start namespaced server (daemonize exit "
        << fg_code << "). See " << daemon_output.AsPrintablePath();
  }

  // daemonize has exited, so the pid file exists and holds the server's
  // *namespace-local* pid. Resolve it to the host pid (within our init's PID
  // namespace) and rewrite the server's on-disk identity so this host-side
  // launcher (and later clients) can attach over loopback.
  int ns_pid;
  if (!ReadPidFile(pid_file, &ns_pid)) {
    BAZEL_DIE(blaze_exit_code::INTERNAL_ERROR)
        << "criu: failed to read namespaced server pid from "
        << pid_file.AsPrintablePath();
  }
  const string ns_namespace = PidNamespaceOf(init_host_pid);
  int host_pid =
      MakeReachable(ns_pid, ns_namespace, server_dir, /*timeout_secs=*/10);
  if (host_pid < 0) {
    BAZEL_DIE(blaze_exit_code::INTERNAL_ERROR)
        << "criu: could not make namespaced server (ns pid " << ns_pid
        << ") reachable.";
  }

  *server_startup = new AlwaysAliveServerStartup();
  return host_pid;
}

bool CriuRestore(const blaze_util::Path &output_base) {
  const blaze_util::Path images_dir =
      output_base.GetRelative(kCriuImagesSubdir);
  const blaze_util::Path server_dir = output_base.GetRelative("server");

  // The namespace-local pid recorded at checkpoint time (not bazel's mutable
  // server.pid.txt) is how we locate the restored server afterwards.
  int ns_pid;
  if (!ReadPidFile(images_dir.GetRelative(kNsPidFile), &ns_pid)) {
    BAZEL_LOG(USER) << "criu: no checkpoint pid at "
                    << images_dir.GetRelative(kNsPidFile).AsPrintablePath();
    return false;
  }

  BAZEL_LOG(USER) << "Restoring server (ns pid " << ns_pid << ") from "
                  << images_dir.AsPrintablePath() << " ...";

  // criu restore runs as the foreground command of a fresh namespace; the init
  // adopts the --restore-detached tree and then persists, holding the namespace
  // open exactly as a fresh namespaced start would.
  vector<string> fg_argv = {
      CriuBinary(),
      "restore",
      "--restore-detached",
      "--images-dir",
      images_dir.AsNativePath(),
      // Bazel writes files with restrictive modes; skip criu's rwx sanity check.
      "--skip-file-rwx-check",
      // Raise the ghost-file size limit for any large deleted-but-mapped files.
      "--ghost-limit",
      "1000000000",
      // The server's clients are transient and reconnect; close TCP rather than
      // restore the command-port connections.
      "--tcp-close",
      // Allow checkpointing processes with a controlling terminal.
      "--shell-job",
      // Relax checks for rootless operation on the shared host network ns.
      "--unprivileged",
      // Tolerate the inherited, locked FUSE/squashfs/snap mounts (see
      // kCriuEnableFs). Must match the flag used for `criu dump`.
      "--enable-fs",
      kCriuEnableFs,
      "--log-file",
      images_dir.GetRelative("restore.log").AsNativePath(),
      "-v4",
  };

  // criu inherits the launcher's environment.
  vector<string> env;
  for (char **e = environ; e != nullptr && *e != nullptr; ++e) {
    env.push_back(*e);
  }

  int fg_code = 0;
  int init_host_pid = 0;
  if (!RunInNamespace(fg_argv, env, &fg_code, &init_host_pid) || fg_code != 0) {
    BAZEL_LOG(USER) << "criu: restore failed (exit " << fg_code << "); see "
                    << images_dir.GetRelative("restore.log").AsPrintablePath();
    return false;
  }

  // The restored server has a new host pid and start time; point the client's
  // validation files at it. Allow longer for the reparent to settle.
  const string ns_namespace = PidNamespaceOf(init_host_pid);
  int host_pid =
      MakeReachable(ns_pid, ns_namespace, server_dir, /*timeout_secs=*/30);
  if (host_pid < 0) {
    return false;
  }
  BAZEL_LOG(USER) << "Restored; server reachable at host pid " << host_pid;
  return true;
}

}  // namespace blaze

#endif  // __linux__
