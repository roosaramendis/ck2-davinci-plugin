// One "Key clip" job: frames captured while Resolve plays the clip, then the
// CorridorKey engine runs in the background. Everything lives in
// <cache>/<id>/ so a job survives Resolve restarting. No OFX types here.
//
//   job.txt        settings chosen when Key clip was pressed
//   info.txt       frame size/offset (from the first captured frame)
//   src/, hint/    captured frames (index = frame - start)
//   launched       "first count": which captured frames went to the engine
//   run/           frames moved here for the engine, deleted afterwards
//   engine.log     engine output;  exitcode: "0", a number, or "cancelled"
//   out/src_cutout_exr, out/src_fg_exr   the result
#pragma once

#include <string>
#include <vector>

namespace ck {

struct JobSpec {
    std::string kind = "key";   // "key" (CorridorKey) or "roto" (SAM 2 tracking)
    int start = 0, count = 0;   // host frame range at the time of Key clip
    bool linear = false;        // input is scene-linear
    bool hint = false;          // a Hint clip is connected
    bool hintFirstOnly = false; // use only the first frame of the hint (engine carries it forward)
    bool lowVram = false;
    int window = 0;             // 0 = auto, else hann chunk size
    double despill = 0.5, hintQ = 0.95, fps = 24.0;
    std::string str() const;
    static JobSpec parse(const std::string& s);
};

enum class JobState { Missing, Capturing, Keying, Done, Failed, Cancelled };

struct JobStatus {
    JobState state = JobState::Missing;
    int captured = 0, count = 0;      // capturing
    double progress = 0.0;            // keying, 0..1
    bool cancelling = false;
    int keyedFirst = 0, keyedCount = 0;  // done: which frames (index from start) are keyed
    int w = 0, h = 0, x1 = 0, y1 = 0;    // frame size and offset (pixels)
    std::string message;              // failure reason / last engine line
    std::string logPath;
};

bool createJob(const std::string& cacheDir, const std::string& id, const JobSpec& spec, std::string* err);
JobStatus jobStatus(const std::string& cacheDir, const std::string& id, bool fresh = false);

// Queue one frame for writing (returns at once). rgb/hint are top-row-first
// float buffers (hint may be empty). When the last frame lands the engine starts.
// Returns false if the frame was not taken (queue full, size mismatch, keying started).
bool captureFrame(const std::string& cacheDir, const std::string& id, int idx,
                  std::vector<float>&& rgb, int w, int h, int x1, int y1, std::vector<float>&& hint);
bool frameCaptured(const std::string& cacheDir, const std::string& id, int idx);  // or being written
// Tell the job which frame the host is drawing while capturing. When playback
// jumps from the end of the captured frames back to their start (it looped),
// the job starts on those frames even if the host reported a longer clip.
bool notePlayback(const std::string& cacheDir, const std::string& id, int idx);
void waitForWrites();  // tests

// Start the engine now on the longest unbroken run of captured frames.
bool launchJob(const std::string& cacheDir, const std::string& id, bool partialOk, std::string* err);
void cancelJob(const std::string& cacheDir, const std::string& id);
void forgetJob(const std::string& cacheDir, const std::string& id);

// ------------------------------------------------------------------ roto (SAM 2)
// Roto jobs live in <cache>/roto/<id>/ and are handled by roto_daemon.py.
std::string rotoRoot(const std::string& cacheDir);
bool ensureRotoDaemon(const std::string& rotoRoot, std::string* err);
// Opens the picker window (frame image already written by the node).
bool openPicker(const std::string& rotoRoot, const std::string& id, int idx, const std::string& model,
                std::string* err);
bool pickerOpen(const std::string& rotoRoot, const std::string& id);
struct Prompts {
    bool valid = false;
    int frame = 0, objects = 0;
};
Prompts readPrompts(const std::string& rotoRoot, const std::string& id);
struct PreviewState {
    bool pending = false;  // asked, no answer yet
    std::string error;
};
PreviewState previewState(const std::string& rotoRoot, const std::string& id);
std::string previewDir(const std::string& rotoRoot, const std::string& id);
std::string previewMaskPath(const std::string& rotoRoot, const std::string& id, int idx);
std::string trackMaskPath(const std::string& rotoRoot, const std::string& id, int idx);
// Track the picked selection. Starts now if frames are ready, else waits for playback to
// capture them; pressed again while capturing, it tracks the captured part right away.
enum class TrackStart { Failed, Started, WaitingForFrames };
TrackStart requestTrack(const std::string& rotoRoot, const std::string& id, std::string* err);
bool rotoDaemonAlive(const std::string& rotoRoot);
bool trackWanted(const std::string& rotoRoot, const std::string& id);

// Key names: the CorridorKey node publishes its key under a short name
// (<cache>/names/<name>.key) so Pass nodes / the Fusion macro can find it.
std::string safeKeyName(const std::string& name);
bool registerKeyName(const std::string& cacheDir, const std::string& name, const std::string& id, int start,
                     int count, bool linear);
struct KeyRef {
    std::string id;
    int start = 0, count = 0;
    bool linear = false;
    bool valid() const { return !id.empty() && count > 0; }
};
KeyRef lookupKeyName(const std::string& cacheDir, const std::string& name);  // cached ~0.5 s

std::string outFramePath(const std::string& cacheDir, const std::string& id, const char* kind, int outIdx);

}  // namespace ck
