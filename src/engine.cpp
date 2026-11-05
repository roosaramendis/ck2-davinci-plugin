#include "engine.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <crt_externs.h>  // plugins (bundles) must not use the `environ` symbol on macOS
#define CK_ENVIRON (*_NSGetEnviron())
#else
extern char** environ;
#define CK_ENVIRON environ
#endif
#endif

namespace fs = std::filesystem;

namespace ck {

std::string homeDir() {
#ifdef _WIN32
    const char* h = std::getenv("USERPROFILE");
#else
    const char* h = std::getenv("HOME");
#endif
    return h ? h : ".";
}

std::string configPath() {
    if (const char* p = std::getenv("CK2_OFX_CONFIG")) return p;
#ifdef _WIN32
    const char* app = std::getenv("APPDATA");
    return joinPath(joinPath(app ? app : homeDir(), "ck2-davinci-plugin"), "ofx.conf");
#elif defined(__APPLE__)
    return joinPath(homeDir(), "Library/Application Support/ck2-davinci-plugin/ofx.conf");
#else
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    std::string base = (xdg && *xdg) ? xdg : joinPath(homeDir(), ".config");
    return joinPath(joinPath(base, "ck2-davinci-plugin"), "ofx.conf");
#endif
}

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

EngineConfig loadConfig() {
    EngineConfig c;
    c.path = configPath();
    std::ifstream f(c.path);
    if (!f) {
        c.error = "Not connected to an engine yet (" + c.path + " missing). "
                  "Run the plugin's install.sh again (or install.sh --connect-only), "
                  "or create that file with one line: engine=<CorridorKey2.0 folder>";
        return c;
    }
    std::string line;
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        if (k == "python") c.python = v;
        else if (k == "engine") c.engine = v;
        else if (k == "backend") c.backend = v;
        else if (k == "wrapper") c.wrapper = v;
        else if (k == "cache_dir") c.cacheDir = v;
        else if (k == "temp_dir") c.tempDir = v;
        else if (k == "ffmpeg") c.ffmpeg = v;
        else if (k == "roto_daemon") c.rotoScript = v;
        else if (k == "roto_picker") c.pickerScript = v;
        else if (k == "sam_model") c.samModel = v;
    }
    // Only "engine=" is required; the venv Python is found inside it.
    if (c.python.empty() && !c.engine.empty()) {
#ifdef _WIN32
        c.python = joinPath(c.engine, "venv\\Scripts\\python.exe");
        if (!fileExists(c.python)) c.python = joinPath(c.engine, ".venv\\Scripts\\python.exe");
#else
        c.python = joinPath(c.engine, "venv/bin/python");
        if (!fileExists(c.python)) c.python = joinPath(c.engine, ".venv/bin/python");
#endif
    }
    if (c.backend.empty()) {
#ifdef __APPLE__
        c.backend = "mps";
#else
        c.backend = "cuda";
#endif
    }
    if (c.python.empty() || !fileExists(c.python)) {
        c.error = "Engine Python not found (" + c.python + "). Install the CorridorKey 2.0 engine, then run install.sh --connect-only.";
    } else if (c.engine.empty() || !fileExists(joinPath(c.engine, "infer.py"))) {
        c.error = "CorridorKey 2.0 engine not found in " + c.engine + ".";
    } else if (c.backend == "mps" && !fileExists(c.wrapper)) {
        c.error = "Apple GPU wrapper missing (" + c.wrapper + ").";
    } else {
        c.ok = true;
    }
    if (c.rotoScript.empty()) c.rotoScript = (fs::path(c.path).parent_path() / "roto_daemon.py").string();
    if (c.pickerScript.empty()) c.pickerScript = (fs::path(c.path).parent_path() / "roto_picker.py").string();
    if (c.samModel.empty()) {
#ifdef __APPLE__
        c.samModel = "facebook/sam2.1-hiera-base-plus";
#else
        c.samModel = "facebook/sam2.1-hiera-large";
#endif
    }
    if (c.cacheDir.empty()) {
#ifdef _WIN32
        const char* la = std::getenv("LOCALAPPDATA");
        c.cacheDir = joinPath(la ? la : homeDir(), "corridorkey-ofx");
#elif defined(__APPLE__)
        c.cacheDir = joinPath(homeDir(), "Library/Caches/corridorkey-ofx");
#else
        c.cacheDir = joinPath(homeDir(), ".cache/corridorkey-ofx");
#endif
    }
    if (c.tempDir.empty()) c.tempDir = joinPath(c.cacheDir, "tmp");
    return c;
}

std::string enginePath(const EngineConfig& c) {
#ifdef _WIN32
    const char sep = ';';
#else
    const char sep = ':';
#endif
    std::string out;
    auto add = [&](const std::string& d) {
        if (d.empty()) return;
        if (!out.empty()) out += sep;
        out += d;
    };
    if (!c.ffmpeg.empty()) add(fs::path(c.ffmpeg).parent_path().string());
    if (const char* p = std::getenv("PATH")) add(p);
#ifdef __APPLE__
    add("/opt/homebrew/bin");
    add("/usr/local/bin");
#endif
    return out;
}

std::string joinPath(const std::string& a, const std::string& b) {
    return (fs::path(a) / fs::path(b)).string();
}

bool readText(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream o;
    o << f.rdbuf();
    out = o.str();
    return true;
}

bool writeTextAtomic(const std::string& path, const std::string& text) {
    std::string tmp = path + ".tmp" + randomId();
    {
        std::ofstream f(tmp, std::ios::binary);
        if (!f) return false;
        f << text;
        if (!f) return false;
    }
    std::error_code ec;
    for (int i = 0; i < 50; ++i) {  // Windows refuses to replace a file another process has open
        fs::rename(tmp, path, ec);
        if (!ec) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    std::error_code ec2;
    fs::remove(tmp, ec2);
    return false;
}

bool makeDirs(const std::string& path) {
    std::error_code ec;
    fs::create_directories(path, ec);
    return fs::is_directory(path, ec);
}

bool removeTree(const std::string& path) {
    std::error_code ec;
    fs::remove_all(path, ec);
    return !ec;
}

bool fileExists(const std::string& path) {
    std::error_code ec;
    return fs::exists(path, ec);
}

std::string randomId() {
    std::random_device rd;
    std::mt19937_64 g(rd() ^ (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count());
    std::ostringstream o;
    o << std::hex << g();
    return o.str();
}

// ------------------------------------------------------------------ processes

#ifdef _WIN32

static std::wstring widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n ? n - 1 : 0, L'\0');
    if (n) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

static std::wstring quoteArg(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring q = L"\"";
    for (size_t i = 0; i < a.size(); ++i) {
        size_t bs = 0;
        while (i < a.size() && a[i] == L'\\') { ++bs; ++i; }
        if (i == a.size()) { q.append(bs * 2, L'\\'); break; }
        if (a[i] == L'"') { q.append(bs * 2 + 1, L'\\'); q += L'"'; }
        else { q.append(bs, L'\\'); q += a[i]; }
    }
    return q + L"\"";
}

// Environment block for the child = Resolve's environment + extraEnv.
// (SetEnvironmentVariable would change Resolve's own environment.)
static std::wstring envBlock(const std::map<std::string, std::string>& extraEnv) {
    std::map<std::wstring, std::wstring, bool (*)(const std::wstring&, const std::wstring&)> vars(
        [](const std::wstring& x, const std::wstring& y) { return _wcsicmp(x.c_str(), y.c_str()) < 0; });
    if (wchar_t* cur = GetEnvironmentStringsW()) {
        for (const wchar_t* e = cur; *e; e += wcslen(e) + 1) {
            std::wstring kv(e);
            size_t eq = kv.find(L'=', 1);  // entries like "=C:=C:\" start with '='
            if (eq != std::wstring::npos) vars[kv.substr(0, eq)] = kv.substr(eq + 1);
        }
        FreeEnvironmentStringsW(cur);
    }
    for (auto& [k, v] : extraEnv) {
        if (v == kEnvUnset) vars.erase(widen(k));
        else vars[widen(k)] = widen(v);
    }
    std::wstring block;
    for (auto& [k, v] : vars) { block += k; block += L'='; block += v; block += L'\0'; }
    block += L'\0';
    return block;
}

int runProcess(const std::vector<std::string>& argv, const std::map<std::string, std::string>& extraEnv,
               const std::string& cwd, const std::function<bool(const std::string&)>& onLine,
               std::string* startError) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) { if (startError) *startError = "CreatePipe failed"; return -1; }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr; si.hStdError = wr; si.hStdInput = nullptr;
    std::wstring cmd;
    for (auto& a : argv) { if (!cmd.empty()) cmd += L' '; cmd += quoteArg(widen(a)); }
    std::wstring env = envBlock(extraEnv);
    std::wstring wcwd = widen(cwd);

    // A job object kills the engine and anything it started (ffmpeg) on cancel,
    // and also if Resolve itself goes away.
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof(li));
    }
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
                             env.data(), cwd.empty() ? nullptr : wcwd.c_str(), &si, &pi);
    CloseHandle(wr);
    if (!ok) {
        CloseHandle(rd);
        if (job) CloseHandle(job);
        if (startError) *startError = "could not start " + argv[0];
        return -1;
    }
    if (job) AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);

    std::string buf; char chunk[4096]; bool cancelled = false, exited = false;
    auto flushLines = [&]() {
        size_t pos;
        while ((pos = buf.find_first_of("\r\n")) != std::string::npos) {
            std::string line = buf.substr(0, pos); buf.erase(0, pos + 1);
            if (!line.empty() && !onLine(line)) return false;
        }
        return true;
    };
    while (!cancelled) {
        DWORD avail = 0, n = 0;
        if (!PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr)) break;  // child closed pipe
        if (avail > 0) {
            if (!ReadFile(rd, chunk, std::min<DWORD>(avail, sizeof(chunk)), &n, nullptr) || n == 0) break;
            buf.append(chunk, n);
            if (!flushLines()) cancelled = true;
            continue;
        }
        if (exited) break;
        // Nothing to read: let the caller check for Cancel, then wait a bit.
        if (!onLine("")) { cancelled = true; break; }
        exited = WaitForSingleObject(pi.hProcess, 250) == WAIT_OBJECT_0;  // one more drain pass
    }
    if (cancelled) {
        if (job) TerminateJobObject(job, 1);
        else TerminateProcess(pi.hProcess, 1);
    } else if (!buf.empty()) {
        onLine(buf);
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread); CloseHandle(rd);
    if (job) CloseHandle(job);
    return cancelled ? -1 : (int)code;
}

bool spawnDetached(const std::vector<std::string>& argv, const std::map<std::string, std::string>& extraEnv,
                   const std::string& cwd, std::string* err) {
    STARTUPINFOW si{}; si.cb = sizeof(si);
    std::wstring cmd;
    for (auto& a : argv) { if (!cmd.empty()) cmd += L' '; cmd += quoteArg(widen(a)); }
    std::wstring env = envBlock(extraEnv);
    std::wstring wcwd = widen(cwd);
    PROCESS_INFORMATION pi{};
    DWORD flags = DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP | CREATE_UNICODE_ENVIRONMENT;
    // Leave Resolve's job object if allowed, so closing Resolve doesn't kill the keying.
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, flags | CREATE_BREAKAWAY_FROM_JOB,
                             env.data(), cwd.empty() ? nullptr : wcwd.c_str(), &si, &pi);
    if (!ok)
        ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, flags, env.data(),
                            cwd.empty() ? nullptr : wcwd.c_str(), &si, &pi);
    if (!ok) { if (err) *err = "could not start " + argv[0]; return false; }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

#else  // POSIX (Linux, macOS)

// Shared by runProcess/spawnDetached: everything prepared before fork().
struct ChildPrep {
    std::vector<std::string> envStrings;
    std::vector<char*> envp, args;
    long maxFd = 65536;
    ChildPrep(const std::vector<std::string>& argv, const std::map<std::string, std::string>& extraEnv) {
        std::map<std::string, std::string> vars;
        for (char** e = CK_ENVIRON; e && *e; ++e) {
            const char* eq = std::strchr(*e, '=');
            if (eq) vars[std::string(*e, eq - *e)] = eq + 1;
        }
        for (auto& [k, v] : extraEnv) {
            if (v == kEnvUnset) vars.erase(k);
            else vars[k] = v;
        }
        for (auto& [k, v] : vars) envStrings.push_back(k + "=" + v);
        for (auto& e : envStrings) envp.push_back(const_cast<char*>(e.c_str()));
        envp.push_back(nullptr);
        for (auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
        args.push_back(nullptr);
        maxFd = sysconf(_SC_OPEN_MAX);
        if (maxFd < 0 || maxFd > 65536) maxFd = 65536;
    }
};

bool spawnDetached(const std::vector<std::string>& argv, const std::map<std::string, std::string>& extraEnv,
                   const std::string& cwd, std::string* err) {
    ChildPrep prep(argv, extraEnv);
    pid_t pid = fork();
    if (pid < 0) { if (err) *err = "fork failed"; return false; }
    if (pid == 0) {
        setsid();  // own session: not killed with Resolve, no terminal
        if (fork() != 0) _exit(0);  // grandchild is re-parented to init: no zombie
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) { dup2(devnull, 0); dup2(devnull, 1); dup2(devnull, 2); }
        for (long fd = 3; fd < prep.maxFd; ++fd) close((int)fd);
        if (!cwd.empty() && chdir(cwd.c_str()) != 0) _exit(126);
        execve(prep.args[0], prep.args.data(), prep.envp.data());
        _exit(127);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    return true;
}

int runProcess(const std::vector<std::string>& argv, const std::map<std::string, std::string>& extraEnv,
               const std::string& cwd, const std::function<bool(const std::string&)>& onLine,
               std::string* startError) {
    // Everything the child needs is prepared here: after fork() in a multithreaded
    // host only async-signal-safe calls are allowed (no setenv/malloc).
    std::map<std::string, std::string> vars;
    for (char** e = CK_ENVIRON; e && *e; ++e) {
        const char* eq = std::strchr(*e, '=');
        if (eq) vars[std::string(*e, eq - *e)] = eq + 1;
    }
    for (auto& [k, v] : extraEnv) vars[k] = v;
    std::vector<std::string> envStrings;
    for (auto& [k, v] : vars) envStrings.push_back(k + "=" + v);
    std::vector<char*> envp, args;
    for (auto& e : envStrings) envp.push_back(const_cast<char*>(e.c_str()));
    envp.push_back(nullptr);
    for (auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    long maxFd = sysconf(_SC_OPEN_MAX);
    if (maxFd < 0 || maxFd > 65536) maxFd = 65536;

    int fds[2];
    if (pipe(fds) != 0) { if (startError) *startError = "pipe failed"; return -1; }
    pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); if (startError) *startError = "fork failed"; return -1; }
    if (pid == 0) {
        setpgid(0, 0);  // own process group so we can signal the engine and its children
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) dup2(devnull, STDIN_FILENO);
        for (long fd = 3; fd < maxFd; ++fd) close((int)fd);  // don't inherit Resolve's files
        if (!cwd.empty() && chdir(cwd.c_str()) != 0) _exit(126);
        execve(args[0], args.data(), envp.data());
        _exit(127);
    }
    close(fds[1]);
    std::string buf; char chunk[4096]; bool cancelled = false;
    for (;;) {
        pollfd p{fds[0], POLLIN, 0};
        int r = poll(&p, 1, 500);
        if (r < 0) break;
        if (r == 0) {  // let the caller poll for cancel even when the engine is quiet
            if (!onLine(std::string())) { cancelled = true; break; }
            continue;
        }
        ssize_t n = read(fds[0], chunk, sizeof(chunk));
        if (n <= 0) break;
        buf.append(chunk, (size_t)n);
        size_t pos;
        while ((pos = buf.find_first_of("\r\n")) != std::string::npos) {
            std::string line = buf.substr(0, pos); buf.erase(0, pos + 1);
            if (!line.empty() && !onLine(line)) { cancelled = true; break; }
        }
        if (cancelled) break;
    }
    if (cancelled) {
        kill(-pid, SIGINT);  // Python cleans up temp files on KeyboardInterrupt
        for (int i = 0; i < 100; ++i) {
            int st; if (waitpid(pid, &st, WNOHANG) == pid) { close(fds[0]); return -1; }
            usleep(100000);
        }
        kill(-pid, SIGKILL);
    } else if (!buf.empty()) {
        onLine(buf);
    }
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    if (cancelled) return -1;
    if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        if (code == 127 && startError) *startError = "could not start " + argv[0];
        return code;
    }
    return -1;
}

#endif

} // namespace ck
