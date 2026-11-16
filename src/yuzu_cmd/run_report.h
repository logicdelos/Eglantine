// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>
#include <string_view>

#include "common/common_types.h"
#include "common/run_outcome.h"

// The JSON file of --report: what the run did, written once when it ends. The numbers come from
// the Nx2Bottleneck collector and the exit reason from Common::RunOutcome; this only checks the
// destination, gathers the rest and writes the file (to a temporary name first, then renamed).
namespace RunReport {

enum class PrepareResult {
    Ready,
    /// The destination exists, or its directory does not: the caller's mistake (exit code 64).
    BadDestination,
    /// The directory refused a temporary file (exit code 74).
    IoError,
};

/// Checks `destination` (UTF-8) before anything is launched and creates the empty temporary file
/// next to it, so a destination that cannot be written fails now and not after a long run. An
/// existing destination is refused rather than overwritten. `message` says what is wrong.
[[nodiscard]] PrepareResult Prepare(const std::string& destination, std::string& message);

/// Records what is only known at the start: the environment variables that tune the emulator and
/// the settings as they are (after the config file and the command line were applied).
void CaptureStart();

/// The program id of the loaded game, for the report's title_id.
void SetTitleId(u64 program_id);

/// Writes the report for a run that ended for `reason` with `code`. `shutdown_complete` is false
/// when the process is being ended without the teardown having finished, which also marks the
/// numbers incomplete. Safe to call from two threads (the second call does nothing and returns
/// the first one's result); returns false, after saying why on stderr, when nothing was written.
/// Does nothing and returns true when Prepare was never called.
[[nodiscard]] bool Publish(Common::RunOutcome::Reason reason, int code, bool shutdown_complete,
                           std::string_view detail);

/// Ends the process after a failure that cannot return to the frontend's own shutdown (a window
/// constructor, a network callback). In an unattended run the failure is recorded as a startup
/// error and the report is written first, and the exit code is the one of the reason on record;
/// otherwise the code is 1.
[[noreturn]] void ExitWithFailure(std::string_view detail);

} // namespace RunReport
