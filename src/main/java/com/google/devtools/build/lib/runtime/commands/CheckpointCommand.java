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
package com.google.devtools.build.lib.runtime.commands;

import static com.google.devtools.build.lib.runtime.Command.BuildPhase.NONE;

import com.google.common.flogger.GoogleLogger;
import com.google.devtools.build.lib.concurrent.PooledInterner;
import com.google.devtools.build.lib.events.Event;
import com.google.devtools.build.lib.runtime.BlazeCommand;
import com.google.devtools.build.lib.runtime.BlazeCommandResult;
import com.google.devtools.build.lib.runtime.BlazeServerStartupOptions;
import com.google.devtools.build.lib.runtime.Command;
import com.google.devtools.build.lib.runtime.CommandEnvironment;
import com.google.devtools.build.lib.server.FailureDetails;
import com.google.devtools.build.lib.server.FailureDetails.CheckpointCommand.Code;
import com.google.devtools.build.lib.server.FailureDetails.FailureDetail;
import com.google.devtools.build.lib.util.DebugLoggerConfigurator;
import com.google.devtools.build.lib.vfs.Path;
import com.google.devtools.common.options.OptionsParser;
import com.google.devtools.common.options.OptionsParsingResult;
import java.io.IOException;

/**
 * The 'blaze checkpoint' command: prepares the server to be CRIU-checkpointed by the launcher.
 *
 * <p>Under {@code --criu} the launcher checkpoints a warm server. That must not happen while a real
 * command is executing, and the checkpointed image should be small and self-consistent. This
 * command runs on the server like any other, so the dispatcher's exclusive command lock guarantees
 * no other command runs concurrently. While holding that lock it returns memory to the OS (full GC
 * plus interner shrinking) and flushes buffered log streams, then hands control to the launcher
 * through a sentinel file and parks until the launcher removes it.
 */
@Command(
    name = "checkpoint",
    buildPhase = NONE,
    allowResidue = false,
    mustRunInWorkspace = false,
    hidden = true,
    shortDescription = "Quiesces the %{product} server so the launcher can CRIU-checkpoint it.",
    help =
        "Drives the server side of a CRIU checkpoint: takes the exclusive command lock, returns"
            + " memory to the OS, flushes logs, then parks until the launcher has taken the"
            + " checkpoint. Only meaningful under the --criu startup option; invoked by the"
            + " launcher's 'checkpoint' command, not typically by hand.")
public final class CheckpointCommand implements BlazeCommand {
  private static final GoogleLogger logger = GoogleLogger.forEnclosingClass();

  // Basename (under $output_base/server) of the file that hands control to the launcher. The server
  // creates it once it is quiescent and holding the command lock; the launcher waits for it to
  // appear, then takes the criu dump. On a plain checkpoint the launcher removes it to release the
  // server; on a restore the in-namespace init removes it before the server resumes. Must match
  // kCheckpointSentinelName in blaze_criu.cc.
  private static final String SENTINEL_FILE = "checkpoint.sentinel";

  // How often the parked server checks whether the sentinel has been removed.
  private static final long POLL_INTERVAL_MILLIS = 50;

  @Override
  public void editOptions(OptionsParser optionsParser) {}

  @Override
  public BlazeCommandResult exec(CommandEnvironment env, OptionsParsingResult options) {
    boolean criuMode =
        env.getRuntime()
            .getStartupOptionsProvider()
            .getOptions(BlazeServerStartupOptions.class)
            .getCriu();
    if (!criuMode) {
      String message =
          "The 'checkpoint' command requires CRIU mode (pass the --criu startup option).";
      env.getReporter().handle(Event.error(message));
      return createFailure(message, Code.NOT_CRIU_MODE);
    }

    // Return memory to the OS so the checkpoint image is as small as possible. Mirrors
    // GcAndInternerShrinkingIdleTask: a full GC followed by shrinking the interner pools.
    System.gc();
    PooledInterner.shrinkAll();

    // Flush buffered log streams (e.g. the server's jvm.out) so nothing is stranded in an
    // in-memory buffer that a stop-checkpoint would discard, or that a restore would re-emit.
    DebugLoggerConfigurator.flushServerLog();

    Path sentinel = env.getRuntime().getServerDirectory().getChild(SENTINEL_FILE);
    try {
      sentinel.getOutputStream().close();
      logger.atInfo().log("CRIU: created checkpoint sentinel %s", sentinel);
    } catch (IOException e) {
      String message = "Failed to create checkpoint sentinel " + sentinel + ": " + e.getMessage();
      env.getReporter().handle(Event.error(message));
      return createFailure(message, Code.SENTINEL_IO_FAILURE);
    }

    logger.atInfo().log("CRIU: parked holding the command lock; awaiting launcher checkpoint.");
    try {
      // Park until the sentinel is removed. This holds the command lock so no other command can run
      // while the launcher checkpoints us. Across a checkpoint/restore the in-namespace init removes
      // the sentinel before the process resumes, so on resume the file is already gone and this loop
      // exits on its next tick, before the restored server serves any real command.
      while (sentinel.exists()) {
        Thread.sleep(POLL_INTERVAL_MILLIS);
      }
    } catch (InterruptedException e) {
      Thread.currentThread().interrupt();
      // A stop-checkpoint tears the server down with the command still parked; on the leave-running
      // path the launcher removes the sentinel to end the wait cleanly, so an interrupt here is an
      // out-of-band cancellation, not the normal exit.
      try {
        sentinel.delete();
      } catch (IOException ignored) {
        // Best effort: a leftover sentinel only makes the next checkpoint's wait a no-op.
      }
      return createFailure("checkpoint interrupted", Code.INTERRUPTED);
    } catch (IOException e) {
      String message = "Failed to poll checkpoint sentinel " + sentinel + ": " + e.getMessage();
      env.getReporter().handle(Event.error(message));
      return createFailure(message, Code.SENTINEL_IO_FAILURE);
    }

    logger.atInfo().log("CRIU: launcher checkpoint complete; releasing the command lock.");
    return BlazeCommandResult.success();
  }

  private static BlazeCommandResult createFailure(String message, Code code) {
    return BlazeCommandResult.failureDetail(
        FailureDetail.newBuilder()
            .setMessage(message)
            .setCheckpointCommand(FailureDetails.CheckpointCommand.newBuilder().setCode(code))
            .build());
  }
}
