#include "diag_log.h"

#include <windows.h>
#include <fcntl.h>
#include <io.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "console_queue.h"
#include "diag_line.h"
#include "log_parts.h"

namespace zc {
namespace {

// One log part is capped, and a run keeps a bounded number of parts, CYCLING
// (log_parts.h carries the policy and the incident). The 43.9 MB
// single-session file that motivated this was mostly status repaints (now
// console-only), but a long show with a chatty SDK can still fill a file, and
// "the log ate the disk" is not a failure mode a broadcast tool gets to have.
constexpr long long kMaxPartBytes = 8LL * 1024 * 1024;
constexpr int kMaxParts = 4;
// Older runs' files are pruned at startup. Ten launches back is plenty of
// history for "it hung yesterday" and bounds the directory forever.
constexpr size_t kKeepRuns = 10;

// The console sink is behind a bounded, dropping queue on its own thread
// (console_queue.h, which carries the incident and the property). Writing to a
// console can block INDEFINITELY and that must never reach the main loop
// through the CRT's stdout lock.
constexpr size_t kConsoleQueueMax = 256;
// How often a run of drops is allowed to say so in the file. A wedged console
// drops every line it is handed; one note per second is enough to say "the
// console tail is incomplete, this file is not".
constexpr DWORD kDropNoteEveryMs = 1000;

// DiagFlush's handshake. It cannot ask the pipe how much is left: the pump
// sits in a blocking ReadFile on that handle, and a synchronous file object
// serialises operations, so PeekNamedPipe from another thread waits for the
// read that is waiting for data -- forever. The first cut did exactly that
// and deadlocked every exit path (found by the crash-trap selftest, where the
// FATAL box stopped appearing). Instead the flusher pushes a sentinel line
// down the same pipe and waits for the pump to acknowledge it: when the pump
// has consumed the sentinel it has by construction consumed everything
// written before it.
const char kSyncLine[] = "\x01ZCDIAGSYNC\n";

struct Diag {
  HANDLE console = nullptr;      // real console/inherited stdout, or null
  HANDLE file = INVALID_HANDLE_VALUE;
  HANDLE pipe_read = nullptr;
  std::string dir;
  std::string stamp;
  std::string path;              // current part; guarded by path_m
  std::mutex path_m;
  LogParts parts{kMaxPartBytes, kMaxParts};
  std::thread pump;
  std::thread console_pump;
  // The console hand-off owns its own lock and wait (console_queue.h): the
  // no-block-on-a-wedged-console property IS the synchronisation, so it lives
  // in one place a test can drive.
  ConsoleQueue q{kConsoleQueueMax};
  unsigned long long dropped_noted = 0;  // last count written to the file
  std::atomic<unsigned> sync_seq{0};  // sentinels the pump has consumed
};

Diag& D() {
  static Diag d;
  return d;
}

std::string Stamp() {
  SYSTEMTIME st;
  GetLocalTime(&st);
  char b[32];
  std::snprintf(b, sizeof(b), "%02u:%02u:%02u.%03u", st.wHour, st.wMinute,
                st.wSecond, st.wMilliseconds);
  return b;
}

void OpenPart(Diag& d) {
  if (d.file != INVALID_HANDLE_VALUE) CloseHandle(d.file);
  const std::string path = d.dir + LogParts::Name(d.stamp, d.parts.part());
  // FILE_APPEND_DATA + share EVERYTHING: the whole point is that another
  // process can read this file WHILE the app holds it. The v0.1.10
  // investigation could not tail the log at all -- freopen_s opens with no
  // share flags, so Get-Content and File.Open(FileShare.ReadWrite) both hit a
  // sharing violation. Appending (not write+seek) also keeps every write at
  // true EOF in order, which is the chronology guarantee v0.1.8 added.
  // CREATE_ALWAYS, not OPEN_ALWAYS: parts cycle 0..kMaxParts-1 and a reused
  // part must start empty. Appending into it instead meant "rotation" only
  // moved the growth around and the run still produced one unbounded pile.
  d.file = CreateFileA(path.c_str(), FILE_APPEND_DATA,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  std::lock_guard<std::mutex> lock(d.path_m);
  d.path = d.file == INVALID_HANDLE_VALUE ? std::string() : path;
}

void WriteFileLine(Diag& d, const std::string& text) {
  if (d.file == INVALID_HANDLE_VALUE) return;
  DWORD wrote = 0;
  WriteFile(d.file, text.data(), static_cast<DWORD>(text.size()), &wrote,
            nullptr);
  if (d.parts.Wrote(static_cast<long long>(text.size()))) {
    // next_part(), not part+1: with 4 parts the note used to point at a
    // "part 4" that never exists, on the one roll that actually wraps.
    const std::string note = "[" + Stamp() + "] [diag] log part full (" +
                             std::to_string(kMaxPartBytes / (1024 * 1024)) +
                             " MB), continuing in part " +
                             std::to_string(d.parts.next_part()) + "\n";
    WriteFile(d.file, note.data(), static_cast<DWORD>(note.size()), &wrote,
              nullptr);
    d.parts.Advance();
    OpenPart(d);
  }
}

void EnqueueConsole(Diag& d, const std::string& text) {
  // Drop, never block. A wedged console must cost the console's output, not
  // the app's main loop -- ConsoleQueue::Push is the whole guarantee.
  if (d.console != nullptr) d.q.Push(text);
}

// A drop is a hole in the console tail, and a reader comparing the two sinks
// deserves to know which one is short. Written from the pump thread with
// WriteFileLine (never printf: printf feeds the pipe this thread is draining).
void NoteConsoleDrops(Diag& d) {
  static DWORD next_ms = 0;
  const DWORD now = GetTickCount();
  const unsigned long long dropped = d.q.dropped();
  if (dropped == d.dropped_noted || now < next_ms) return;
  next_ms = now + kDropNoteEveryMs;
  const std::string note =
      "[diag] console wedged: dropped " +
      std::to_string(dropped - d.dropped_noted) +
      " console lines (" + std::to_string(dropped) +
      " this run). The file below is complete; the console is not.\n";
  d.dropped_noted = dropped;
  WriteFileLine(d, "[" + Stamp() + "] " + note);
}

void ConsolePumpThread() {
  Diag& d = D();
  for (;;) {
    std::string text;
    if (!d.q.PopWait(&text)) return;
    DWORD wrote = 0;
    WriteFile(d.console, text.data(), static_cast<DWORD>(text.size()), &wrote,
              nullptr);
  }
}

void PumpThread() {
  Diag& d = D();
  DiagSplitter split;
  char buf[4096];
  for (;;) {
    DWORD n = 0;
    if (!ReadFile(d.pipe_read, buf, sizeof(buf), &n, nullptr) || n == 0) break;
    for (const DiagChunk& c : split.Feed(buf, n)) {
      if (c.text == kSyncLine) {
        d.sync_seq.fetch_add(1);  // DiagFlush's handshake; never emitted
        continue;
      }
      if (c.to_file) WriteFileLine(d, "[" + Stamp() + "] " + c.text);
      if (c.to_console) EnqueueConsole(d, c.text);
    }
    NoteConsoleDrops(d);
  }
  const DiagChunk tail = split.Drain();
  if (tail.to_file) WriteFileLine(d, "[" + Stamp() + "] " + tail.text + "\n");
  if (tail.to_console) EnqueueConsole(d, tail.text);
}

// Keep the newest kKeepRuns log files; delete the rest. Cheap enough to do at
// every launch and it bounds the directory without a maintenance story.
void PruneOldLogs(const std::string& dir) {
  struct Entry {
    FILETIME t;
    std::string name;
  };
  std::vector<Entry> files;
  WIN32_FIND_DATAA fd{};
  const HANDLE h = FindFirstFileA((dir + "\\zcomms-*.log").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return;
  do {
    if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
      files.push_back({fd.ftLastWriteTime, fd.cFileName});
    }
  } while (FindNextFileA(h, &fd));
  FindClose(h);
  if (files.size() <= kKeepRuns) return;
  std::sort(files.begin(), files.end(), [](const Entry& a, const Entry& b) {
    return CompareFileTime(&a.t, &b.t) > 0;  // newest first
  });
  for (size_t i = kKeepRuns; i < files.size(); ++i) {
    DeleteFileA((dir + "\\" + files[i].name).c_str());
  }
}

std::atomic<const char*> g_main_phase{"starting"};
std::atomic<long long> g_main_phase_since{0};

}  // namespace

void StartDiagnostics() {
  Diag& d = D();

  // The console sink, in the same priority order BindStdio used: an inherited
  // or redirected stdout first (pipes and scripted runs), then the parent
  // terminal (dev runs). What changed is that neither one SUPPRESSES the file
  // any more -- that early return is what left the v0.1.10 hang with no log.
  const HANDLE inherited = GetStdHandle(STD_OUTPUT_HANDLE);
  if (inherited != nullptr && inherited != INVALID_HANDLE_VALUE) {
    // DUPLICATE, do not just keep the value. The freopen("NUL") below closes
    // the CRT's fd 1, which closes THIS handle, and Windows hands the freed
    // slot straight back to the next CreateHandle -- which is the diagnostic
    // pipe's write end. Holding the raw value therefore aimed the console
    // sink at our own pipe: the pump read what it had just written and the
    // log grew 50 MB of the same three startup lines in six seconds. Caught
    // on the first live run of this change; it is exactly the kind of
    // recycled-handle bug that reads as impossible in review.
    if (!DuplicateHandle(GetCurrentProcess(), inherited, GetCurrentProcess(),
                         &d.console, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
      d.console = nullptr;
    }
  } else if (AttachConsole(ATTACH_PARENT_PROCESS)) {
    d.console = CreateFileA("CONOUT$", GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_EXISTING, 0, nullptr);
    if (d.console == INVALID_HANDLE_VALUE) d.console = nullptr;
    FILE* f = nullptr;
    freopen_s(&f, "CONIN$", "r", stdin);  // hotkeys
  }

  char appdata[MAX_PATH] = {};
  if (GetEnvironmentVariableA("APPDATA", appdata, sizeof(appdata)) != 0) {
    d.dir = std::string(appdata) + "\\ZComms";
    CreateDirectoryA(d.dir.c_str(), nullptr);
    d.dir += "\\logs";
    CreateDirectoryA(d.dir.c_str(), nullptr);
    PruneOldLogs(d.dir);
    SYSTEMTIME st;
    GetLocalTime(&st);
    char s[32];
    std::snprintf(s, sizeof(s), "%04u%02u%02u-%02u%02u%02u", st.wYear,
                  st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    d.stamp = s;
    OpenPart(d);
  }

  if (d.file == INVALID_HANDLE_VALUE && d.console == nullptr) return;

  // A generous pipe: the main thread must never block writing diagnostics.
  // At the post-fix line rate (status repaints no longer reach it and the
  // rest is event-driven) this is minutes of headroom even if both sinks stop.
  HANDLE pipe_write = nullptr;
  if (!CreatePipe(&d.pipe_read, &pipe_write, nullptr, 1 << 20)) return;

  // Point BOTH CRT streams and the Win32 std handles at the pipe. freopen to
  // NUL first so stdout/stderr are guaranteed to own a real fd -- in a
  // Windows-subsystem exe launched from Explorer they own none, and _dup2
  // needs a target.
  FILE* f = nullptr;
  freopen_s(&f, "NUL", "w", stdout);
  freopen_s(&f, "NUL", "w", stderr);
  const int pipe_fd =
      _open_osfhandle(reinterpret_cast<intptr_t>(pipe_write), _O_WRONLY);
  if (pipe_fd < 0) return;
  // _open_osfhandle transferred ownership of pipe_write to the CRT, so the
  // dups below are what keep the write end alive; _close(pipe_fd) closes the
  // original handle and pipe_write must not be touched again.
  if (_fileno(stdout) >= 0) _dup2(pipe_fd, _fileno(stdout));
  if (_fileno(stderr) >= 0) _dup2(pipe_fd, _fileno(stderr));
  _close(pipe_fd);
  const HANDLE live =
      reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stdout)));
  if (live != INVALID_HANDLE_VALUE) {
    SetStdHandle(STD_OUTPUT_HANDLE, live);
    SetStdHandle(STD_ERROR_HANDLE, live);
  }

  d.pump = std::thread(PumpThread);
  d.pump.detach();
  if (d.console != nullptr) {
    d.console_pump = std::thread(ConsolePumpThread);
    d.console_pump.detach();
  }
}

bool DiagHasConsole() { return D().console != nullptr; }

std::string DiagLogPath() {
  Diag& d = D();
  std::lock_guard<std::mutex> lock(d.path_m);
  return d.path;
}

void DiagFlush(int timeout_ms) {
  Diag& d = D();
  if (d.pipe_read == nullptr) {
    std::fflush(nullptr);
    return;
  }
  // Wait for the pipe to actually drain. Without this, TerminateProcess kills
  // the pump with the FATAL line still in flight -- exactly the line the crash
  // trap exists to preserve. The wait is a sentinel handshake, never a peek at
  // the pipe (see kSyncLine).
  const unsigned want = d.sync_seq.load() + 1;
  std::fputs(kSyncLine, stdout);
  std::fflush(nullptr);
  const DWORD deadline = GetTickCount() + static_cast<DWORD>(timeout_ms);
  while (d.sync_seq.load() < want && GetTickCount() < deadline) Sleep(2);
  if (d.file != INVALID_HANDLE_VALUE) FlushFileBuffers(d.file);
}

void DiagEmergency(const std::string& line) {
  const std::string text = "[" + Stamp() + "] " + line + "\n";
  OutputDebugStringA(text.c_str());
  const std::string path = DiagLogPath();
  if (path.empty()) return;
  // Its OWN handle: the pump's could be behind a wedged pipe, and printf could
  // be blocked in the CRT stdout lock by whoever is hung. FILE_APPEND_DATA
  // writes are atomic at EOF, so this interleaves safely with the pump.
  const HANDLE h = CreateFileA(
      path.c_str(), FILE_APPEND_DATA,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return;
  DWORD wrote = 0;
  WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &wrote, nullptr);
  CloseHandle(h);
}

void SetMainPhase(const char* literal) {
  g_main_phase.store(literal);
  g_main_phase_since.store(static_cast<long long>(GetTickCount64()));
}

std::string MainPhaseDescription() {
  const char* p = g_main_phase.load();
  const long long since = g_main_phase_since.load();
  const long long age =
      since == 0 ? 0 : static_cast<long long>(GetTickCount64()) - since;
  return std::string(p == nullptr ? "?" : p) + " (" + std::to_string(age) +
         " ms)";
}

}  // namespace zc

