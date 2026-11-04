#include "job.h"

#include <algorithm>
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
    o << "start=" << start << "\ncount=" << count << "\nlinear=" << linear << "\nhint=" << hint
      << "\nlow_vram=" << lowVram << "\nwindow=" << window << "\ndespill=" << despill
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
            if (k == "start") j.start = std::stoi(v);
            else if (k == "count") j.count = std::stoi(v);
            else if (k == "linear") j.linear = v == "1";
            else if (k == "hint") j.hint = v == "1";
            else if (k == "low_vram") j.lowVram = v == "1";
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
            complete = ok && m->spec.count > 0 && (int)m->done.size() >= m->spec.count;
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
    for (int i = 0; i < bestN && useHint; ++i)
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
        if (useHint)
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
            << (useHint ? " with hint" : "") << "\n[plugin]";
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
        if (code == "0" && fileExists(outFramePath(c, id, "cutout", 0))) {
            st.state = JobState::Done;
        } else if (code == "cancelled") {
            st.state = JobState::Cancelled;
        } else {
            st.state = JobState::Failed;
            st.message = code == "0" ? "the engine finished but wrote no frames"
                                     : lastUsefulLine(tailOf(st.logPath));
            if (st.message.empty()) st.message = "engine exit code " + code;
        }
    } else if (!launched.empty()) {
        st.state = JobState::Keying;
        st.cancelling = fileExists(joinPath(dir, "cancel"));
        std::string t = tailOf(st.logPath);
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

}  // namespace ck
