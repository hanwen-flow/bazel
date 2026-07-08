// Copyright 2020 The Bazel Authors. All rights reserved.
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

package com.google.devtools.build.lib.runtime.commands.info;

import com.google.common.base.Supplier;
import com.google.common.flogger.GoogleLogger;
import com.google.devtools.build.lib.analysis.config.BuildConfigurationValue;
import com.google.devtools.build.lib.runtime.CommandEnvironment;
import com.google.devtools.build.lib.runtime.InfoItem;
import com.google.devtools.build.lib.vfs.FileSystemUtils;
import com.google.devtools.build.lib.vfs.Path;
import java.io.IOException;

/** Info item for server_pid. */
public final class ServerPidInfoItem extends InfoItem {
  private static final GoogleLogger logger = GoogleLogger.forEnclosingClass();

  public ServerPidInfoItem(String productName) {
    super("server_pid", productName + " process id", false);
  }

  @Override
  public byte[] get(
      Supplier<BuildConfigurationValue> configurationSupplier, CommandEnvironment env) {
    // Under CRIU checkpoint/restore the server runs in a PID namespace, so
    // ProcessHandle.current().pid() is only the namespace-local pid. The launcher publishes the
    // server's host pid to server/server.host_pid; report that so the value matches the pid the
    // client (and the host) sees. Fall back to our own pid when the file is absent (normal mode).
    long hostPid = readHostPid(env);
    return print(hostPid > 0 ? hostPid : ProcessHandle.current().pid());
  }

  /** Returns the host pid recorded in server/server.host_pid, or -1 if it is absent/unreadable. */
  private static long readHostPid(CommandEnvironment env) {
    Path hostPidFile = env.getRuntime().getServerDirectory().getChild("server.host_pid");
    try {
      if (!hostPidFile.exists()) {
        return -1;
      }
      return Long.parseLong(new String(FileSystemUtils.readContentAsLatin1(hostPidFile)).trim());
    } catch (IOException | NumberFormatException e) {
      logger.atInfo().withCause(e).log("Could not read host pid from %s", hostPidFile);
      return -1;
    }
  }
}
