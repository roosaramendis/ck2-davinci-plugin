// CorridorKey 2.0 OFX: talking to the installed CorridorKey 2.0 engine. Pure C++17, no OFX types, so it can be tested alone.
#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace ck {

// Contents of ofx.conf written by install.sh ("key=value" lines).
struct EngineConfig {
    std::string python;   // engine venv python
    std::string engine;   // engine folder (official CorridorKey2.0 checkout)
    std::string backend;  // "cuda" (NVIDIA) or "mps" (Apple GPU via wrapper)
    std::string wrapper;  // apple_gpu.py when backend == mps
    std::string cacheDir; // where keyed clips are cached
    std::string tempDir;  // engine temp (low-VRAM feature cache)
    std::string ffmpeg;   // full path; Resolve started from the Dock/menu has a short PATH
    bool ok = false;
    std::string error;
    std::string path;     // which file was read
};

std::string configPath();
EngineConfig loadConfig();
// PATH for the engine: ffmpeg's folder + Resolve's PATH (+ Homebrew on macOS).
std::string enginePath(const EngineConfig& c);

// Run a command, calling onLine for each stdout/stderr line. If onLine returns
// false the process is stopped (SIGINT first, then killed). Returns exit code,
// or -1 if it couldn't start / was cancelled.
int runProcess(const std::vector<std::string>& argv,
               const std::map<std::string, std::string>& extraEnv,
               const std::string& cwd,
               const std::function<bool(const std::string&)>& onLine,
               std::string* startError);

// Start a process that keeps running on its own (not tied to Resolve: it
// survives Resolve closing). Output goes nowhere; the program logs by itself.
bool spawnDetached(const std::vector<std::string>& argv,
                   const std::map<std::string, std::string>& extraEnv,
                   const std::string& cwd, std::string* err);

// Small helpers
bool readText(const std::string& path, std::string& out);
bool writeTextAtomic(const std::string& path, const std::string& text);
std::string joinPath(const std::string& a, const std::string& b);
bool makeDirs(const std::string& path);
bool removeTree(const std::string& path);
bool fileExists(const std::string& path);
std::string randomId();
std::string homeDir();

} // namespace ck
