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
package com.google.devtools.build.lib.server.signal;

import com.google.common.base.Preconditions;
import sun.misc.Signal;
import sun.misc.SignalHandler;

/**
 * A facade around {@link sun.misc.Signal} that invokes a callback when a named signal is delivered.
 *
 * <p>Used for out-of-band pokes from the launcher to the server. The default disposition of the
 * chosen signal should be harmless (e.g. {@code SIGWINCH}, which is ignored by default) so that a
 * delivery before the handler is installed cannot terminate the process.
 *
 * <p>We wrap {@code sun.misc} rather than using it directly because it is deprecated and referencing
 * it elsewhere provokes compiler warnings; keeping the usage confined to this package localizes it.
 */
public final class ControlSignalHandler {

  private final Signal signal;
  private SignalHandler oldHandler;

  /**
   * Installs {@code callback} as the handler for {@code signalName} (e.g. {@code "WINCH"}). Until
   * {@link #uninstall} is called, each delivery of that signal runs {@code callback} on a signal
   * dispatch thread.
   */
  public ControlSignalHandler(String signalName, Runnable callback) {
    this.signal = new Signal(signalName);
    this.oldHandler = Signal.handle(signal, sig -> callback.run());
  }

  /** Restores the previous handler for the signal. */
  public synchronized void uninstall() {
    Preconditions.checkNotNull(oldHandler, "uninstall() already called");
    Signal.handle(signal, oldHandler);
    oldHandler = null;
  }
}
