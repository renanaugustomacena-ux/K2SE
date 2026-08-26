#pragma once

// Raw Win32 logging. Deliberately no CRT file I/O:
// on process exit Windows terminates every other thread BEFORE calling
// DLL_PROCESS_DETACH, so a thread killed while holding the CRT file lock would
// deadlock us there.
//
// Destination: %LOCALAPPDATA%\K2SE\k2se.log -- never the game directory.
//
// Logging rules (see DESIGN.md 3.7):
//   - log every write we perform, unconditionally
//   - log every REFUSAL to write, unconditionally (a refusal is the *absence*
//     of a write, so "log all writes" alone misses exactly the silent-failure
//     case that users report as "nothing happens")
//   - verbose tracing is gated on a marker FILE next to the DLL, never on an
//     env var and never on anything a shipped script could switch on.

namespace k2se {
namespace log {

// Opens the log and writes the identity block. Safe to call from DllMain.
void Init();
void Shutdown();

void Write(const char* line);
void Writef(const char* fmt, ...);

// Only emitted when the marker file K2SE_DIAGNOSTIC exists beside the DLL.
void Trace(const char* fmt, ...);
bool DiagnosticEnabled();

}  // namespace log
}  // namespace k2se
