#include "job.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>
#include <thread>

#include "engine.h"
#include "exr_io.h"

namespace fs = std::filesystem;

namespace ck {

// Runs the engine for a job and records how it ended. Started detached, so it
// keeps going if Resolve is closed. Cancel = create <job>/cancel (or delete the job).
static const char* kRunner = R"PY(import os, shutil, signal, subprocess, sys, time
job, cwd, cmd = sys.argv[1], sys.argv[2], sys.argv[4:]
log = open(os.path.join(job, "engine.log"), "ab", buffering=0)
kw = {"creationflags": 0x08000000 | 0x00000200} if os.name == "nt" else {"start_new_session": True}
try:
    p = subprocess.Popen(cmd, cwd=cwd, stdout=log, stderr=subprocess.STDOUT,
                         stdin=subprocess.DEVNULL, **kw)
except OSError as e:
    log.write(("could not start the engine: %s\n" % e).encode())
    code = "start"
else:
    cancelled = False
    while p.poll() is None:
        if not cancelled and (os.path.exists(os.path.join(job, "cancel")) or not os.path.isdir(job)):
            cancelled = True
            log.write(b"\n[plugin] cancel requested\n")
            try:
                if os.name == "nt":
                    p.send_signal(signal.CTRL_BREAK_EVENT)
                else:
                    os.killpg(p.pid, signal.SIGINT)
            except Exception:
                pass
            t = time.time()
            while p.poll() is None and time.time() - t < 10:
                time.sleep(0.2)
            if p.poll() is None:
                try:
                    if os.name == "nt":
                        p.kill()
                    else:
                        os.killpg(p.pid, signal.SIGKILL)
                except Exception:
                    pass
        time.sleep(0.5)
    code = "cancelled" if cancelled else str(p.wait())
shutil.rmtree(os.path.join(job, "run"), ignore_errors=True)
try:
    with open(os.path.join(job, "exitcode.tmp"), "w") as f:
        f.write(code)
    os.replace(os.path.join(job, "exitcode.tmp"), os.path.join(job, "exitcode"))
except OSError:
    pass
)PY";

static const char* kWindowSizes[] = {"", "80", "48", "24", "8", "1"};

// ------------------------------------------------------------------ spec

std::string JobSpec::str() const {
    std::ostringstream o;
    o << "kind=" << kind << "\nstart=" << start << "\ncount=" << count << "\nlinear=" << linear << "\nhint=" << hint
      << "\nhint_first_only=" << hintFirstOnly << "\nlow_vram=" << lowVram << "\nwindow=" << window << "\ndespill=" << despill
      << "\nhint_quality=" << hintQ << "\nfps=" << fps << "\n";
    return o.str();
}

JobSpec JobSpec::parse(const std::string& s) {
    JobSpec j;
    std::istringstream in(s);
    std::string line;
    while (std::getline(in, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        try {
            if (k == "kind") j.kind = v;
            else if (k == "start") j.start = std::stoi(v);
            else if (k == "count") j.count = std::stoi(v);
            else if (k == "linear") j.linear = v == "1";
            else if (k == "hint") j.hint = v == "1";
            else if (k == "low_vram") j.lowVram = v == "1";
            else if (k == "hint_first_only") j.hintFirstOnly = v == "1";
            else if (k == "window") j.window = std::stoi(v);
            else if (k == "despill") j.despill = std::stod(v);
            else if (k == "hint_quality") j.hintQ = std::stod(v);
            else if (k == "fps") j.fps = std::stod(v);
        } catch (...) {
        }
    }
    return j;
}

// ------------------------------------------------------------------ paths

static std::string jobDir(const std::string& c, const std::string& id) { return joinPath(c, id); }

static std::string numbered(const char* stem, int idx) {
    char b[64];
    std::snprintf(b, sizeof(b), "%s_%06d.exr", stem, idx);
    return b;
}

std::string outFramePath(const std::string& c, const std::string& id, const char* kind, int outIdx) {
    std::string sub = std::string("out/src_") + kind + "_exr";
    return joinPath(joinPath(jobDir(c, id), sub), numbered(kind, outIdx));
}

// ------------------------------------------------------------------ in-memory state

struct JobMem {
    std::mutex mu;
    bool loaded = false;
    JobSpec spec;
    std::set<int> done, inflight;
    bool haveInfo = false;
    int w = 0, h = 0, x1 = 0, y1 = 0;
    bool haveStatus = false;
    JobStatus status;
    std::chrono::steady_clock::time_point statusAt;
};

struct JobTable {
    std::mutex mu;
    std::map<std::string, std::shared_ptr<JobMem>> jobs;
};
static JobTable& J() {
    static JobTable* t = new JobTable;  // never destroyed (see WriterQueue)
    return *t;
}

static std::shared_ptr<JobMem> jobMem(const std::string& c, const std::string& id) {
    std::lock_guard<std::mutex> lock(J().mu);
    auto& p = J().jobs[c + "|" + id];
    if (!p) p = std::make_shared<JobMem>();
    return p;
}

static void loadLocked(JobMem& m, const std::string& dir) {
    if (m.loaded) return;
    std::string s;
    if (!readText(joinPath(dir, "job.txt"), s)) return;  // not created (yet)
    m.spec = JobSpec::parse(s);
    if (readText(joinPath(dir, "info.txt"), s)) {
        std::istringstream in(s);
        if (in >> m.w >> m.h >> m.x1 >> m.y1) m.haveInfo = true;
    }
    std::error_code ec;
    static const std::regex re("frame_(\\d+)\\.exr");
    for (auto& e : fs::directory_iterator(joinPath(dir, "src"), ec)) {
        std::smatch mm;
        std::string n = e.path().filename().string();
        if (std::regex_match(n, mm, re)) m.done.insert(std::stoi(mm[1]));
    }
    m.loaded = true;
}

bool createJob(const std::string& c, const std::string& id, const JobSpec& spec, std::string* err) {
    std::string dir = jobDir(c, id);
    if (!makeDirs(joinPath(dir, "src")) || (spec.hint && !makeDirs(joinPath(dir, "hint")))) {
        if (err) *err = "can't create " + dir;
        return false;
    }
    if (!writeTextAtomic(joinPath(dir, "job.txt"), spec.str())) {
        if (err) *err = "can't write " + dir;
        return false;
    }
    return true;
}

// ------------------------------------------------------------------ writer threads

struct WriteTask {
    std::string cache, id;
    int idx = 0, w = 0, h = 0;
    std::vector<float> rgb, hint;
    size_t bytes() const { return (rgb.size() + hint.size()) * sizeof(float); }
};

// Heap-allocated and never destroyed: at process exit nothing waits on a
// half-destroyed mutex/condition variable (that can hang the host's quit).
struct WriterQueue {
    std::mutex mu;
    std::condition_variable cv, idle;
    std::deque<WriteTask> tasks;
    size_t bytes = 0;
    int busy = 0, workers = 0;
};
static WriterQueue& Q() {
    static WriterQueue* q = new WriterQueue;
    return *q;
}
static const size_t kMaxQueueBytes = 768u << 20;

static void writerLoop() {
    WriterQueue& q = Q();
    for (;;) {
        WriteTask t;
        {
            std::unique_lock<std::mutex> lock(q.mu);
            // Idle threads end after a few seconds, so none are left when the
            // host unloads the plugin or quits.
            if (!q.cv.wait_for(lock, std::chrono::seconds(3), [&] { return !q.tasks.empty(); })) {
                --q.workers;
                q.idle.notify_all();
                return;
            }
            t = std::move(q.tasks.front());
            q.tasks.pop_front();
            q.bytes -= t.bytes();
            ++q.busy;
        }
        std::string dir = jobDir(t.cache, t.id);
        std::string err;
        bool ok = true;
        if (!t.hint.empty()) {
            std::string hp = joinPath(joinPath(dir, "hint"), numbered("hint", t.idx));
            ok = writeExr(hp + ".tmp.exr", t.hint.data(), t.w, t.h, 1, &err, false);
            std::error_code ec;
            if (ok) fs::rename(hp + ".tmp.exr", hp, ec);
        }
        std::string fp = joinPath(joinPath(dir, "src"), numbered("frame", t.idx));
        // Temp name doesn't match frame_NNNNNN.exr, so a half-written file is never used.
        ok = ok && writeExr(fp + ".tmp.exr", t.rgb.data(), t.w, t.h, 3, &err, false);
        std::error_code ec;
        if (ok) fs::rename(fp + ".tmp.exr", fp, ec);
        ok = ok && !ec;

        auto m = jobMem(t.cache, t.id);
        bool complete = false;
        {
            std::lock_guard<std::mutex> lock(m->mu);
            m->inflight.erase(t.idx);
            if (ok) m->done.insert(t.idx);
            m->haveStatus = false;
            complete = ok && m->spec.count > 0 && (int)m->done.size() >= m->spec.count &&
                       (m->spec.kind != "roto" || fileExists(joinPath(dir, "want_track")));
        }
        if (complete) launchJob(t.cache, t.id, false, nullptr);
        {
            std::lock_guard<std::mutex> lock(q.mu);
            --q.busy;
        }
        q.idle.notify_all();
    }
}

// Call with q.mu held.
static void startWritersLocked(WriterQueue& q) {
    int want = (int)std::clamp(std::thread::hardware_concurrency() / 2, 2u, 4u);
    want = std::min(want, (int)q.tasks.size() + q.busy);
    while (q.workers < want) {
        ++q.workers;
        std::thread(writerLoop).detach();
    }
}

void waitForWrites() {
    WriterQueue& q = Q();
    std::unique_lock<std::mutex> lock(q.mu);
    q.idle.wait(lock, [&] { return q.tasks.empty() && q.busy == 0; });
}

bool frameCaptured(const std::string& c, const std::string& id, int idx) {
    auto m = jobMem(c, id);
    std::lock_guard<std::mutex> lock(m->mu);
    loadLocked(*m, jobDir(c, id));
    return m->done.count(idx) || m->inflight.count(idx);
}

bool captureFrame(const std::string& c, const std::string& id, int idx, std::vector<float>&& rgb, int w,
                  int h, int x1, int y1, std::vector<float>&& hint) {
    std::string dir = jobDir(c, id);
    auto m = jobMem(c, id);
    {
        std::lock_guard<std::mutex> lock(m->mu);
        loadLocked(*m, dir);
        if (!m->loaded || idx < 0 || idx >= m->spec.count) return false;
        if (m->done.count(idx) || m->inflight.count(idx)) return true;
        if (fileExists(joinPath(dir, "launched")) || fileExists(joinPath(dir, "exitcode"))) return false;
        if (!m->haveInfo) {
            std::ostringstream o;
            o << w << " " << h << " " << x1 << " " << y1 << "\n";
            writeTextAtomic(joinPath(dir, "info.txt"), o.str());
            m->w = w; m->h = h; m->x1 = x1; m->y1 = y1;
            m->haveInfo = true;
        } else if (w != m->w || h != m->h) {
            return false;  // size changed mid-clip
        }
        m->inflight.insert(idx);
    }
    WriteTask t;
    t.cache = c; t.id = id; t.idx = idx; t.w = w; t.h = h;
    t.rgb = std::move(rgb);
    if (m->spec.hint && hint.size() == (size_t)w * h) t.hint = std::move(hint);
    WriterQueue& q = Q();
    {
        std::lock_guard<std::mutex> lock(q.mu);
        if (q.bytes + t.bytes() > kMaxQueueBytes) {  // writers behind: drop, next pass gets it
            std::lock_guard<std::mutex> l2(m->mu);
            m->inflight.erase(idx);
            return false;
        }
        q.bytes += t.bytes();
        q.tasks.push_back(std::move(t));
        startWritersLocked(q);
    }
    q.cv.notify_one();
    return true;
}

// ------------------------------------------------------------------ launch

bool startRotoTrackLocked(const std::string& root, const std::string& id, const std::string& dir, int count,
                          std::string* err);

static void failJob(const std::string& dir, const std::string& msg) {
    std::ofstream(joinPath(dir, "engine.log"), std::ios::app) << "[plugin] " << msg << "\n";
    writeTextAtomic(joinPath(dir, "exitcode"), "plugin");
}

bool launchJob(const std::string& c, const std::string& id, bool partialOk, std::string* err) {
    std::string dir = jobDir(c, id);
    auto m = jobMem(c, id);
    std::lock_guard<std::mutex> lock(m->mu);
    loadLocked(*m, dir);
    auto fail = [&](const std::string& msg) {
        if (err) *err = msg;
        return false;
    };
    if (!m->loaded) return fail("this key was deleted; press Key clip again");
    if (fileExists(joinPath(dir, "launched"))) return true;
    if (fileExists(joinPath(dir, "exitcode"))) return fail("this job already ended; press Key clip again");
    if (!partialOk && (int)m->done.size() < m->spec.count) return fail("not all frames captured yet");
    if (m->spec.kind == "roto") {
        if ((int)m->done.size() < m->spec.count) return fail("play the whole clip once first");
        bool ok = startRotoTrackLocked(c, id, dir, m->spec.count, err);
        m->haveStatus = false;
        return ok;
    }

    // Longest unbroken run of captured frames (the whole clip when everything is there).
    int bestFirst = 0, bestN = 0, runFirst = 0, runN = 0, prev = -2;
    for (int i : m->done) {
        if (i == prev + 1) ++runN;
        else { runFirst = i; runN = 1; }
        if (runN > bestN) { bestN = runN; bestFirst = runFirst; }
        prev = i;
    }
    if (bestN < 1) return fail("no frames captured yet: play the clip in Resolve first");

    EngineConfig cfg = loadConfig();
    if (!cfg.ok) {
        failJob(dir, cfg.error);
        m->haveStatus = false;
        return fail(cfg.error);
    }

    // Move the frames into run/ numbered from 0, as the engine expects.
    std::string runSrc = joinPath(dir, "run/src"), runHint = joinPath(dir, "run/hint");
    makeDirs(runSrc);
    bool useHint = m->spec.hint;
    const int hintNeeded = m->spec.hintFirstOnly ? 1 : bestN;
    for (int i = 0; i < hintNeeded && useHint; ++i)
        useHint = fileExists(joinPath(joinPath(dir, "hint"), numbered("hint", bestFirst + i)));
    if (useHint) makeDirs(runHint);
    for (int i = 0; i < bestN; ++i) {
        std::error_code ec;
        fs::rename(joinPath(joinPath(dir, "src"), numbered("frame", bestFirst + i)),
                   joinPath(runSrc, numbered("frame", i)), ec);
        if (ec) {
            failJob(dir, "couldn't prepare frame " + std::to_string(bestFirst + i) + ": " + ec.message());
            m->haveStatus = false;
            return fail("couldn't prepare the captured frames");
        }
        if (useHint && i < hintNeeded)
            fs::rename(joinPath(joinPath(dir, "hint"), numbered("hint", bestFirst + i)),
                       joinPath(runHint, numbered("hint", i)), ec);
    }
    m->done.clear();
    std::error_code ec;
    fs::remove_all(joinPath(dir, "src"), ec);
    fs::remove_all(joinPath(dir, "hint"), ec);
    makeDirs(joinPath(dir, "out"));
    makeDirs(cfg.tempDir);

    std::string runner = joinPath(dir, "runner.py");
    writeTextAtomic(runner, kRunner);

    const JobSpec& s = m->spec;
    std::vector<std::string> argv = {cfg.python, runner, dir, cfg.engine, "--", cfg.python};
    if (cfg.backend == "mps") argv.insert(argv.end(), {cfg.wrapper, cfg.engine, "infer"});
    else argv.push_back(joinPath(cfg.engine, "infer.py"));
    char num[64];
    std::snprintf(num, sizeof(num), "%.3f", s.despill);
    std::string despill = num;
    std::snprintf(num, sizeof(num), "%g", s.fps > 0 ? s.fps : 24.0);
    std::string fps = num;
    argv.insert(argv.end(), {"--input", runSrc, "--output_dir", joinPath(dir, "out"), "--outputs", "fg",
                             "cutout", "--output_format", "exr", "--exr_dtype", "half",
                             "--despill_strength", despill, "--frame_dir_fps", fps, "--temp_dir", cfg.tempDir});
    if (!cfg.ffmpeg.empty() && fileExists(cfg.ffmpeg)) argv.insert(argv.end(), {"--ffmpeg_bin", cfg.ffmpeg});
    if (s.linear) argv.push_back("--frame_dir_linear");
    if (useHint) {
        std::snprintf(num, sizeof(num), "%.3f", s.hintQ);
        if (s.hintFirstOnly)
            argv.insert(argv.end(), {"--hint_first_frame", joinPath(runHint, numbered("hint", 0)),
                                     "--hint_quality", num, "--carry_hint_quality", num});
        else
            argv.insert(argv.end(), {"--hint_video", runHint, "--hint_quality", num});
    }
    if (s.window > 0 && s.window < 6) {
        int ch = std::atoi(kWindowSizes[s.window]);
        argv.insert(argv.end(), {"--hann_chunk", std::to_string(ch), "--hann_stride",
                                 std::to_string(std::max(1, ch / 2))});
        if (s.lowVram) argv.push_back("--low_vram");
    }
    std::map<std::string, std::string> env = {
        {"TMPDIR", cfg.tempDir}, {"PYTHONUNBUFFERED", "1"}, {"HF_HUB_OFFLINE", "1"},
        {"TRANSFORMERS_OFFLINE", "1"}, {"PYTORCH_ENABLE_MPS_FALLBACK", "1"},
        {"PATH", enginePath(cfg)}};

    {
        std::ofstream log(joinPath(dir, "engine.log"), std::ios::app);
        log << "[plugin] keying frames " << (s.start + bestFirst) << ".." << (s.start + bestFirst + bestN - 1)
            << (useHint ? (s.hintFirstOnly ? " with first-frame hint" : " with hint") : "") << "\n[plugin]";
        for (size_t i = 5; i < argv.size(); ++i) log << " " << argv[i];
        log << "\n";
    }
    writeTextAtomic(joinPath(dir, "launched"), std::to_string(bestFirst) + " " + std::to_string(bestN) + "\n");
    std::string spawnErr;
    if (!spawnDetached(argv, env, cfg.engine, &spawnErr)) {
        failJob(dir, spawnErr);
        m->haveStatus = false;
        return fail(spawnErr);
    }
    m->haveStatus = false;
    return true;
}

void cancelJob(const std::string& c, const std::string& id) {
    std::string dir = jobDir(c, id);
    if (!fs::is_directory(dir)) return;
    writeTextAtomic(joinPath(dir, "cancel"), "1");
    // Not handed to the engine yet: it ends right here.
    if (!fileExists(joinPath(dir, "launched")) && !fileExists(joinPath(dir, "exitcode")))
        writeTextAtomic(joinPath(dir, "exitcode"), "cancelled");
    auto m = jobMem(c, id);
    std::lock_guard<std::mutex> lock(m->mu);
    m->haveStatus = false;
}

void forgetJob(const std::string& c, const std::string& id) {
    if (id.empty()) return;
    cancelJob(c, id);
    removeTree(jobDir(c, id));  // a running engine notices and stops
    std::lock_guard<std::mutex> lock(J().mu);
    J().jobs.erase(c + "|" + id);
}

// ------------------------------------------------------------------ status

static std::string tailOf(const std::string& path, size_t bytes = 16384) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    f.seekg(0, std::ios::end);
    std::streamoff size = f.tellg();
    std::streamoff from = size > (std::streamoff)bytes ? size - (std::streamoff)bytes : 0;
    f.seekg(from);
    std::string s((size_t)(size - from), '\0');
    f.read(&s[0], (std::streamsize)s.size());
    return s;
}

static std::string lastUsefulLine(const std::string& log) {
    std::istringstream in(log);
    std::string line, last, err;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty()) continue;
        last = line;
        if (line.find("Error") != std::string::npos || line.find("error") != std::string::npos ||
            line.find("[plugin]") == 0)
            err = line;
    }
    return err.empty() ? last : err;
}

JobStatus jobStatus(const std::string& c, const std::string& id, bool fresh) {
    std::string dir = jobDir(c, id);
    auto m = jobMem(c, id);
    std::lock_guard<std::mutex> lock(m->mu);
    auto now = std::chrono::steady_clock::now();
    if (!fresh && m->haveStatus && now - m->statusAt < std::chrono::milliseconds(500)) return m->status;

    JobStatus st;
    st.logPath = joinPath(dir, "engine.log");
    loadLocked(*m, dir);
    if (!m->loaded) {
        m->status = st;  // Missing
        m->haveStatus = true;
        m->statusAt = now;
        return st;
    }
    st.count = m->spec.count;
    st.w = m->w; st.h = m->h; st.x1 = m->x1; st.y1 = m->y1;
    std::string launched, code;
    if (readText(joinPath(dir, "launched"), launched)) {
        std::istringstream in(launched);
        in >> st.keyedFirst >> st.keyedCount;
    }
    if (readText(joinPath(dir, "exitcode"), code)) {
        while (!code.empty() && (code.back() == '\n' || code.back() == '\r')) code.pop_back();
        bool produced = m->spec.kind == "roto" ? fileExists(trackMaskPath(c, id, 0))
                                               : fileExists(outFramePath(c, id, "cutout", 0));
        if (code == "0" && produced) {
            st.state = JobState::Done;
        } else if (code == "cancelled") {
            st.state = JobState::Cancelled;
        } else {
            st.state = JobState::Failed;
            st.message = code == "0" ? "finished without writing any frames"
                                     : lastUsefulLine(tailOf(st.logPath));
            if (st.message.empty()) st.message = "engine exit code " + code;
        }
    } else if (!launched.empty()) {
        st.state = JobState::Keying;
        st.cancelling = fileExists(joinPath(dir, "cancel"));
        std::string t = tailOf(st.logPath);
        size_t mark = t.rfind("[plugin]");  // only this run's progress (a re-track appends)
        if (mark != std::string::npos) t = t.substr(mark);
        static const std::regex re("window (\\d+)/(\\d+)");
        double p = 0.0;
        for (std::sregex_iterator it(t.begin(), t.end(), re), end; it != end; ++it) {
            double i = std::stod((*it)[1]), n = std::max(1.0, std::stod((*it)[2]));
            p = (i - 0.5) / n;
        }
        st.progress = std::clamp(p, 0.0, 0.99);
        st.message = lastUsefulLine(t);
    } else {
        st.state = JobState::Capturing;
        st.captured = (int)m->done.size();
    }
    m->status = st;
    m->haveStatus = true;
    m->statusAt = now;
    return st;
}

// ------------------------------------------------------------------ roto

std::string rotoRoot(const std::string& c) { return joinPath(c, "roto"); }
// Roto jobs use rotoRoot(cache) as their "cache" for the shared job functions.
std::string previewMaskPath(const std::string& c, const std::string& id, int idx) {
    return joinPath(joinPath(joinPath(c, id), "preview"), numbered("mask", idx));
}
std::string trackMaskPath(const std::string& c, const std::string& id, int idx) {
    return joinPath(joinPath(joinPath(c, id), "masks"), numbered("mask", idx));
}

static long long nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

static double fileAgeSeconds(const std::string& p) {
    std::error_code ec;
    auto t = fs::last_write_time(p, ec);
    if (ec) return 1e9;
    auto now = decltype(t)::clock::now();
    return std::chrono::duration<double>(now - t).count();
}

bool ensureRotoDaemon(const std::string& root, std::string* err) {
    // root = rotoRoot(cache). Alive if its heartbeat is fresh.
    if (fileAgeSeconds(joinPath(root, "daemon.alive")) < 10) return true;
    // Just started one? (loading torch takes a while before the first heartbeat)
    if (fileAgeSeconds(joinPath(root, "daemon.starting")) < 60) return true;
    EngineConfig cfg = loadConfig();
    if (!cfg.ok) {
        if (err) *err = cfg.error;
        return false;
    }
    if (!fileExists(cfg.rotoScript)) {
        if (err) *err = "roto_daemon.py not found next to " + cfg.path + " (run install.sh again)";
        return false;
    }
    makeDirs(root);
    writeTextAtomic(joinPath(root, "daemon.starting"), std::to_string(nowMs()));
    std::vector<std::string> argv = {cfg.python, cfg.rotoScript, root, cfg.samModel};
    std::map<std::string, std::string> env = {{"PYTHONUNBUFFERED", "1"}, {"PYTORCH_ENABLE_MPS_FALLBACK", "1"},
                                              {"PATH", enginePath(cfg)}};
    return spawnDetached(argv, env, root, err);
}

std::string previewDir(const std::string& root, const std::string& id) {
    return joinPath(joinPath(root, id), "preview");
}

bool pickerOpen(const std::string& root, const std::string& id) {
    return fileAgeSeconds(joinPath(joinPath(root, id), "picker.open")) < 6;
}

bool openPicker(const std::string& root, const std::string& id, int idx, const std::string& model,
                std::string* err) {
    if (pickerOpen(root, id)) return true;  // already open (it stays on top)
    EngineConfig cfg = loadConfig();
    if (!cfg.ok) {
        if (err) *err = cfg.error;
        return false;
    }
    if (!fileExists(cfg.pickerScript)) {
        if (err) *err = "roto_picker.py not found next to " + cfg.path + " (run install.sh again)";
        return false;
    }
    if (!ensureRotoDaemon(root, err)) return false;  // start loading SAM 2 right away
    std::string dir = joinPath(root, id);
    writeTextAtomic(joinPath(dir, "picker.open"), "starting");
    std::vector<std::string> argv = {cfg.python, cfg.pickerScript, dir, std::to_string(idx), cfg.rotoScript, model};
    // Resolve's own library/Qt settings must not leak into the picker's Qt.
    std::map<std::string, std::string> env = {
        {"PATH", enginePath(cfg)},       {"LD_LIBRARY_PATH", kEnvUnset},   {"QT_PLUGIN_PATH", kEnvUnset},
        {"QML2_IMPORT_PATH", kEnvUnset}, {"QT_QPA_PLATFORM_PLUGIN_PATH", kEnvUnset},
        {"PYTHONPATH", kEnvUnset},       {"PYTHONHOME", kEnvUnset},        {"DYLD_LIBRARY_PATH", kEnvUnset},
        {"DYLD_FRAMEWORK_PATH", kEnvUnset}};
    return spawnDetached(argv, env, dir, err);
}

Prompts readPrompts(const std::string& root, const std::string& id) {
    Prompts p;
    std::string s;
    if (!readText(joinPath(joinPath(root, id), "prompts.json"), s)) return p;
    static const std::regex fr("\"frame\"\\s*:\\s*(-?\\d+)");
    std::smatch m;
    if (!std::regex_search(s, m, fr)) return p;
    p.frame = std::stoi(m[1]);
    size_t pos = 0;  // one "points" key per object
    while ((pos = s.find("\"points\"", pos)) != std::string::npos) {
        ++p.objects;
        ++pos;
    }
    p.valid = p.objects > 0;
    return p;
}

static long long jsonSeq(const std::string& s) {
    static const std::regex re("\"seq\"\\s*:\\s*(\\d+)");
    std::smatch m;
    return std::regex_search(s, m, re) ? std::stoll(m[1]) : 0;
}

PreviewState previewState(const std::string& root, const std::string& id) {
    PreviewState p;
    std::string pv = previewDir(root, id), req, done;
    if (!readText(joinPath(pv, "request.json"), req)) return p;
    readText(joinPath(pv, "done.json"), done);
    p.pending = jsonSeq(req) > jsonSeq(done);
    static const std::regex re("\"error\"\\s*:\\s*\"([^\"]*)\"");
    std::smatch m;
    if (!p.pending && std::regex_search(done, m, re)) p.error = m[1];
    return p;
}

bool trackWanted(const std::string& root, const std::string& id) {
    return fileExists(joinPath(joinPath(root, id), "want_track"));
}

// Called with the job's lock held, frames complete (or kept from an earlier track).
bool startRotoTrackLocked(const std::string& root, const std::string& id, const std::string& dir, int count,
                          std::string* err) {
    std::string prompt;
    if (!readText(joinPath(dir, "prompts.json"), prompt)) {
        if (err) *err = "press 'Pick character…' first";
        return false;
    }
    std::error_code ec;
    fs::remove(joinPath(dir, "exitcode"), ec);
    fs::remove(joinPath(dir, "cancel"), ec);
    fs::remove_all(joinPath(dir, "masks"), ec);
    // prompts.json is {"frame": f, "objects": [...], "model": "..."}; add seq + count.
    std::string body = prompt.substr(prompt.find('{') + 1);
    std::ostringstream o;
    o << "{\"seq\": " << nowMs() << ", \"kind\": \"track\", \"count\": " << count << ", " << body;
    writeTextAtomic(joinPath(dir, "launched"), "0 " + std::to_string(count) + "\n");
    std::ofstream(joinPath(dir, "engine.log"), std::ios::app) << "[plugin] tracking requested\n";
    if (!writeTextAtomic(joinPath(dir, "request.json"), o.str())) {
        if (err) *err = "can't write the track request";
        return false;
    }
    fs::remove(joinPath(dir, "want_track"), ec);
    return ensureRotoDaemon(root, err);
}

bool requestTrack(const std::string& root, const std::string& id, std::string* err) {
    std::string dir = joinPath(root, id);
    auto m = jobMem(root, id);
    std::lock_guard<std::mutex> lock(m->mu);
    loadLocked(*m, dir);
    if (!m->loaded) {
        if (err) *err = "roto job missing";
        return false;
    }
    if (!fileExists(joinPath(dir, "prompts.json"))) {
        if (err) *err = "press 'Pick character…' first";
        return false;
    }
    if (fileExists(joinPath(dir, "launched")) && !fileExists(joinPath(dir, "exitcode"))) {
        if (err) *err = "already tracking; press Stop first";
        return false;
    }
    m->haveStatus = false;
    // Frames already prepared by an earlier track: go straight away.
    bool ready = fileExists(joinPath(joinPath(dir, "sam"), "info.json")) || (int)m->done.size() >= m->spec.count;
    if (ready) return startRotoTrackLocked(root, id, dir, m->spec.count, err);
    std::error_code ec;
    fs::remove(joinPath(dir, "exitcode"), ec);
    fs::remove(joinPath(dir, "launched"), ec);
    writeTextAtomic(joinPath(dir, "want_track"), "1");
    return true;
}

// ------------------------------------------------------------------ key names

std::string safeKeyName(const std::string& name) {
    std::string out;
    for (unsigned char c : name) {
        if (std::isalnum(c)) out += (char)std::tolower(c);
        else if (c == '-' || c == '_') out += (char)c;
        else if (c == ' ' || c == '.') out += '_';
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    return out.substr(0, 64);
}

bool registerKeyName(const std::string& c, const std::string& name, const std::string& id, int start, int count,
                     bool linear) {
    std::string n = safeKeyName(name);
    if (n.empty() || id.empty()) return false;
    makeDirs(joinPath(c, "names"));
    std::ostringstream o;
    o << id << " " << start << " " << count << " " << (linear ? 1 : 0) << "\n";
    return writeTextAtomic(joinPath(joinPath(c, "names"), n + ".key"), o.str());
}

KeyRef lookupKeyName(const std::string& c, const std::string& name) {
    struct Entry {
        KeyRef ref;
        std::chrono::steady_clock::time_point at;
    };
    static std::mutex* mu = new std::mutex;
    static auto* cache = new std::map<std::string, Entry>;
    std::string n = safeKeyName(name);
    if (n.empty()) return {};
    auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(*mu);
    auto it = cache->find(c + "|" + n);
    if (it != cache->end() && now - it->second.at < std::chrono::milliseconds(500)) return it->second.ref;
    KeyRef r;
    std::string s;
    if (readText(joinPath(joinPath(c, "names"), n + ".key"), s)) {
        std::istringstream in(s);
        int lin = 0;
        if (in >> r.id >> r.start >> r.count >> lin) r.linear = lin == 1;
        else r = KeyRef{};
    }
    (*cache)[c + "|" + n] = {r, now};
    return r;
}

}  // namespace ck
