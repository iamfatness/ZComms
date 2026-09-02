// The diagnostic stream: where printf goes, and the one instrument that still
// works when the process is wedged.
//
// This replaces BindStdio()'s "pick ONE destination" policy, which cost a
// whole investigation. v0.1.10 hung on the owner's machine (WER: AppHangB1,
// settings drawer open, ~4 minutes in) and left ZERO trace, because the run
// was launched from a shell: BindStdio saw a valid inherited STD_OUTPUT_HANDLE
// and returned early, so no file under %APPDATA%\ZComms\logs was ever opened.
// The evidence for a hang IS the log; it must exist unconditionally.
//
// Shape now: stdout and stderr both go into a pipe, and a pump thread fans
// each line out to the log file (always) and to the console (when one exists).
// Three properties fall out of that, each of which was a real defect:
//
//  1. The file is ALWAYS written, console or not.
//  2. Only the pump ever touches the file, and it opens it with full sharing
//     (_SH_DENYNO). The old freopen_s(..., "a") opened without share flags,
//     so Get-Content -- and even File.Open with FileShare.ReadWrite -- failed
//     with a sharing violation. During a hang the log is the evidence and it
//     was unreadable. One appending writer also KEEPS the chronology guarantee
//     that the old "a"/"a" split was there to protect (v0.1.8: a "w"/"a" pair
//     gave stdout and stderr independent file positions and shuffled a FATAL
//     line into the middle of the first field crash log) -- and strengthens
//     it, because there is now exactly one file position in the process.
//  3. The app no longer writes to conhost on its own threads. A console
//     stalled by anything -- a user selecting text in QuickEdit mode, a pipe
//     whose reader stopped -- used to block whoever called printf, IN the CRT
//     stdout lock, which is a hang the old watchdog could not even report
//     because it reported by calling printf. Now that write happens on the
//     console thread behind a bounded queue that drops rather than blocks.
#pragma once

#include <cstdint>
#include <string>

namespace zc {

// Routes stdout/stderr for the whole process. Call once, before anything
// prints. Never fails in a way the caller must handle: with no console and no
// %APPDATA% the app still runs, silently.
void StartDiagnostics();

// True when a real console is attached: gates the _kbhit/_getch hotkeys and
// the status meter, which is decoration for a human watching a terminal.
bool DiagHasConsole();

// The log file currently being written, or "" if none could be opened.
std::string DiagLogPath();

// Push everything queued through to the log file and wait, bounded, for the
// pump to land it. Every exit here is TerminateProcess (sdk.dll fastfails in
// its own DLL_PROCESS_DETACH), which would otherwise kill the pump with the
// last lines -- including FATAL -- still in the pipe.
void DiagFlush(int timeout_ms);

// A line that must survive a wedged diagnostic stream. Written with WriteFile
// straight to its own appending handle on the log file, taking no CRT lock and
// no pipe, plus OutputDebugStringA so a live debugger or DebugView sees it.
// This is what the watchdogs use: the failure they exist to report is exactly
// the failure that can make printf never return.
//
// It can land slightly out of order relative to buffered printf output. That
// is the trade; an out-of-order stall line beats no stall line.
void DiagEmergency(const std::string& line);

// --- What the main loop is doing ------------------------------------------
// Set from the main loop as it moves between phases; read by BOTH watchdogs so
// a UI-thread stall report can say what the other loop was busy with. The
// argument must be a string literal (stored as a bare pointer, read from
// another thread with no lock -- literals have static storage and never move).
void SetMainPhase(const char* literal);

// "publish state (1240 ms)" -- the current phase and how long it has run.
std::string MainPhaseDescription();

}  // namespace zc
