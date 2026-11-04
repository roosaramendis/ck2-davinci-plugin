// CorridorKey 2.0 for DaVinci Resolve (and any OpenFX 1.4 host).
//
// CorridorKey 2.0 keys whole clips (it looks at many frames at once), while an
// OFX effect renders one frame at a time and must never block the host. So:
//   1. "Key clip" only starts a job (returns at once).
//   2. While you play the clip, render() hands each frame to background writer
//      threads (a banner shows "CAPTURING n/N").
//   3. When every frame is captured, the engine starts as a separate background
//      process ("KEYING 45%"); Resolve stays usable and can even be closed.
//   4. render() then shows the cached key, so playback and scrubbing are fast.
//
// Community plugin: built on Corridor Digital's CorridorKey 2.0 model (Corridor
// Key Licence 2.0) and the OpenFX SDK (BSD). Not an official Corridor product.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "ofxsImageEffect.h"
#include "ofxsMultiThread.h"

#include "engine.h"
#include "exr_io.h"
#include "job.h"
#include "overlay.h"

namespace fs = std::filesystem;

#define kPluginName "CorridorKey 2.0"
#define kPluginGrouping "Keying"
#define kPluginDescription \
    "AI green/blue-screen keyer using Corridor Digital's CorridorKey 2.0 model. " \
    "Press 'Key clip', play the clip once, and keying runs in the background. " \
    "Community plugin, not an official Corridor Digital product."
#define kPluginIdentifier "io.github.ck2gui.CorridorKey2"
#define kPluginVersionMajor 1
#define kPluginVersionMinor 1

#define kHintClip "Hint"

// params
#define kKey "keyClip"
#define kKeyNow "keyNow"
#define kStop "stopKey"
#define kRefresh "refreshStatus"
#define kTick "refreshTick"
#define kForget "forgetKey"
#define kStatus "status"
#define kOutput "outputMode"
#define kHintQ "hintQuality"
#define kDespill "despill"
#define kInputColor "inputColor"
#define kMemory "memory"
#define kLowVram "lowVram"
#define kExportDir "exportDir"
#define kExport "exportExr"
#define kHostInfo "hostInfo"
#define kEditionInfo "editionInfo"
#define kKeyData "keyData"

enum OutputMode { eCutout = 0, eMatte, eForeground, eChecker, eSource };
static const char* kWindows[] = {"Auto (from free GPU memory)", "80/40 – steadiest, most memory", "48/24",
                                 "24/12", "8/4", "1/1 – single frames, least memory"};

// ============================================================ host / edition

struct HostInfo {
    std::string text;     // shown in the panel
    std::string edition;  // "Studio", "Free" or "unknown"
};

static HostInfo detectHost() {
    HostInfo h;
    const OFX::ImageEffectHostDescription* d = OFX::getImageEffectHostDescription();
    if (!d) {
        h.text = "unknown host";
        h.edition = "unknown";
        return h;
    }
    std::string name = d->hostName, label = d->hostLabel;
    auto has = [&](const char* s) {
        std::string n = name + " " + label;
        std::transform(n.begin(), n.end(), n.begin(), ::tolower);
        return n.find(s) != std::string::npos;
    };
    // Resolve reports hostName "DaVinciResolveLite" / label "DaVinci Resolve Lite"
    // for the free edition and plain "DaVinciResolve" for Studio.
    bool resolve = has("resolve");
    if (!resolve) h.edition = "unknown";
    else if (has("lite") || has("free")) h.edition = "Free";
    else h.edition = "Studio";
    std::ostringstream o;
    o << (label.empty() ? name : label);
    if (!d->versionLabel.empty()) o << " " << d->versionLabel;
    else o << " " << d->versionMajor << "." << d->versionMinor << "." << d->versionMicro;
    o << "  [" << name << "]";
    if (resolve) o << "  →  Resolve " << h.edition;
    h.text = o.str();
    return h;
}

// Features that a given edition can't use. Empty today: everything this plugin
// does (reading frames, running the engine, caching, EXR export) works in Free and
// Studio. Kept as a table so a future Studio-only feature shows a clear message.
static const std::map<std::string, std::string> kStudioOnly = {};

static bool featureBlocked(const HostInfo& h, const std::string& feature, std::string* why) {
    auto it = kStudioOnly.find(feature);
    if (it != kStudioOnly.end() && h.edition == "Free") {
        if (why) *why = it->second + " is not available in DaVinci Resolve Free (Studio only).";
        return true;
    }
    return false;
}

// ============================================================ key data (saved in project)

struct KeyData {
    std::string id;
    int start = 0, count = 0;
    bool linear = false;
    bool valid() const { return !id.empty() && count > 0; }
    std::string str() const {
        std::ostringstream o;
        o << "id=" << id << ";start=" << start << ";count=" << count << ";linear=" << (linear ? 1 : 0);
        return o.str();
    }
    static KeyData parse(const std::string& s) {
        KeyData k;
        std::stringstream ss(s);
        std::string kv;
        while (std::getline(ss, kv, ';')) {
            auto eq = kv.find('=');
            if (eq == std::string::npos) continue;
            std::string key = kv.substr(0, eq), v = kv.substr(eq + 1);
            try {
                if (key == "id") k.id = v;
                else if (key == "start") k.start = std::stoi(v);
                else if (key == "count") k.count = std::stoi(v);
                else if (key == "linear") k.linear = v == "1";
            } catch (...) {
            }
        }
        return k;
    }
};

// ============================================================ keyed-frame cache (shared, thread safe)

struct CachedFrame {
    int w = 0, h = 0;
    std::vector<float> cutout;  // RGBA straight, scene-linear, despilled
    std::vector<float> fg;      // RGBA raw foreground (loaded on demand)
};

class FrameCache {
public:
    std::shared_ptr<CachedFrame> get(const std::string& cacheDir, const std::string& id, int outIdx, bool needFg) {
        std::string key = id + "#" + std::to_string(outIdx);
        std::lock_guard<std::mutex> lock(mu_);
        auto it = map_.find(key);
        std::shared_ptr<CachedFrame> f;
        if (it != map_.end()) {
            f = it->second;
            order_.remove(key);
        } else {
            f = std::make_shared<CachedFrame>();
            if (!ck::readExrRGBA(ck::outFramePath(cacheDir, id, "cutout", outIdx), f->cutout, f->w, f->h, nullptr))
                return nullptr;
            map_[key] = f;
        }
        order_.push_front(key);
        if (needFg && f->fg.empty()) {
            int w = 0, h = 0;
            if (!ck::readExrRGBA(ck::outFramePath(cacheDir, id, "fg", outIdx), f->fg, w, h, nullptr) ||
                w != f->w || h != f->h)
                f->fg.clear();
        }
        while (order_.size() > 16) {
            map_.erase(order_.back());
            order_.pop_back();
        }
        return f;
    }
    void forget(const std::string& id) {
        std::lock_guard<std::mutex> lock(mu_);
        for (auto it = map_.begin(); it != map_.end();) {
            if (it->first.rfind(id + "#", 0) == 0) {
                order_.remove(it->first);
                it = map_.erase(it);
            } else {
                ++it;
            }
        }
    }

private:
    std::mutex mu_;
    std::map<std::string, std::shared_ptr<CachedFrame>> map_;
    std::list<std::string> order_;
};

static FrameCache gCache;

static std::string cachedCacheDir() {
    static std::mutex mu;
    static std::string dir;
    std::lock_guard<std::mutex> lock(mu);
    if (dir.empty()) dir = ck::loadConfig().cacheDir;
    return dir;
}

// Plugin log: <cache>/plugin.log (button presses, errors, state changes).
static void plog(const std::string& msg) {
    static std::mutex* mu = new std::mutex;
    std::lock_guard<std::mutex> lock(*mu);
    std::string dir = cachedCacheDir();
    ck::makeDirs(dir);
    std::ofstream f(ck::joinPath(dir, "plugin.log"), std::ios::app);
    std::time_t t = std::time(nullptr);
    char ts[32];
    std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
    f << ts << "  " << msg << "\n";
}

// What went wrong, for the log (call inside a catch block).
static std::string describeException() {
    try {
        throw;
    } catch (const OFX::Exception::Suite& e) {
        return "host refused (OFX status " + std::to_string((int)e.status()) + ")";
    } catch (const std::exception& e) {
        return std::string("exception: ") + e.what();
    } catch (...) {
        return "unknown exception";
    }
}

// Split rows over a few threads (4K frames are slow single-threaded).
template <class F>
static void parallelRows(int y1, int y2, F&& f) {
    int n = y2 - y1;
    int T = (int)std::min(16u, std::max(1u, std::thread::hardware_concurrency()));
    T = std::min(T, n / 32);
    if (T <= 1) {
        f(y1, y2);
        return;
    }
    std::vector<std::thread> th;
    int chunk = (n + T - 1) / T;
    for (int i = 0; i < T; ++i) {
        int a = y1 + i * chunk, b = std::min(y2, a + chunk);
        if (a < b) th.emplace_back([&f, a, b] { f(a, b); });
    }
    for (auto& t : th) t.join();
}

// ============================================================ pixel helpers

static inline float srgbEncode(float v) {  // matches CorridorKey's signed linear_to_srgb
    float a = std::fabs(v);
    float e = a <= 0.0031308f ? a * 12.92f : 1.055f * std::pow(a, 1.0f / 2.4f) - 0.055f;
    return v < 0 ? -e : e;
}

static inline float readChannel(const void* px, OFX::BitDepthEnum depth, int c) {
    switch (depth) {
    case OFX::eBitDepthUByte: return ((const unsigned char*)px)[c] / 255.0f;
    case OFX::eBitDepthUShort: return ((const unsigned short*)px)[c] / 65535.0f;
    case OFX::eBitDepthFloat: return ((const float*)px)[c];
    default: return 0.0f;
    }
}

static inline void writePixel(void* px, OFX::BitDepthEnum depth, int nComp, const float rgba[4]) {
    for (int c = 0; c < nComp; ++c) {
        float v = nComp == 1 ? rgba[3] : rgba[c];
        switch (depth) {
        case OFX::eBitDepthUByte:
            ((unsigned char*)px)[c] = (unsigned char)std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f);
            break;
        case OFX::eBitDepthUShort:
            ((unsigned short*)px)[c] = (unsigned short)std::lround(std::clamp(v, 0.0f, 1.0f) * 65535.0f);
            break;
        case OFX::eBitDepthFloat: ((float*)px)[c] = v; break;
        default: break;
        }
    }
}

static int components(OFX::PixelComponentEnum c) {
    switch (c) {
    case OFX::ePixelComponentRGBA: return 4;
    case OFX::ePixelComponentRGB: return 3;
    case OFX::ePixelComponentAlpha: return 1;
    default: return 0;
    }
}

// Image -> top-row-first float buffer with `outC` channels (3 = RGB, 1 = matte).
// Walks rows with pointers (this runs during playback, so it must be quick).
static bool imageToBuffer(OFX::Image* img, int outC, std::vector<float>& buf, int& w, int& h,
                          OfxRectI& bounds) {
    bounds = img->getBounds();
    w = bounds.x2 - bounds.x1;
    h = bounds.y2 - bounds.y1;
    int nc = components(img->getPixelComponents());
    OFX::BitDepthEnum depth = img->getPixelDepth();
    if (w <= 0 || h <= 0 || nc == 0) return false;
    int bpc = depth == OFX::eBitDepthFloat ? 4 : depth == OFX::eBitDepthUShort ? 2 : depth == OFX::eBitDepthUByte ? 1 : 0;
    if (!bpc) return false;
    const size_t px = (size_t)nc * bpc;
    buf.assign((size_t)w * h * outC, 0.0f);
    bool alphaVaries = false;
    if (outC == 1 && nc == 4) {  // matte from alpha, unless alpha is solid (then luminance)
        for (int y = bounds.y1; y < bounds.y2 && !alphaVaries; ++y) {
            const char* row = (const char*)img->getPixelAddress(bounds.x1, y);
            if (!row) continue;
            for (int x = 0; x < w; ++x)
                if (readChannel(row + x * px, depth, 3) < 0.999f) { alphaVaries = true; break; }
        }
    }
    for (int y = bounds.y2 - 1, r = 0; y >= bounds.y1; --y, ++r) {
        const char* row = (const char*)img->getPixelAddress(bounds.x1, y);
        if (!row) continue;
        float* o = &buf[(size_t)r * w * outC];
        if (outC == 3 && nc >= 3 && depth == OFX::eBitDepthFloat) {  // the common case in Resolve
            const float* f = (const float*)row;
            for (int x = 0; x < w; ++x, f += nc, o += 3) { o[0] = f[0]; o[1] = f[1]; o[2] = f[2]; }
            continue;
        }
        for (int x = 0; x < w; ++x, o += outC) {
            const char* p = row + x * px;
            if (outC == 3) {
                if (nc >= 3) {
                    for (int c = 0; c < 3; ++c) o[c] = readChannel(p, depth, c);
                } else {
                    o[0] = o[1] = o[2] = readChannel(p, depth, 0);
                }
            } else {
                if (nc == 1) o[0] = readChannel(p, depth, 0);
                else if (nc == 4 && alphaVaries) o[0] = readChannel(p, depth, 3);
                else
                    o[0] = 0.2126f * readChannel(p, depth, 0) + 0.7152f * readChannel(p, depth, 1) +
                           0.0722f * readChannel(p, depth, 2);
            }
        }
    }
    return true;
}

// ============================================================ banner (status drawn into the picture)

struct Banner {
    std::string text;
    float r = 1, g = 1, b = 1;  // accent colour
    double progress = -1;       // 0..1 draws a bar
};

static void drawBanner(OFX::Image* dst, const OfxRectI& win, const Banner& bn) {
    if (bn.text.empty()) return;
    OfxRectI db = dst->getBounds();
    int W = db.x2 - db.x1, H = db.y2 - db.y1;
    int s = std::max(1, W / 560);
    int pad = 4 * s;
    int textW = (int)bn.text.size() * 6 * s - s, textH = 7 * s;
    int barH = bn.progress >= 0 ? 3 * s : 0;
    int boxW = std::min(W - 4 * s, textW + 2 * pad + 2 * s);
    int boxH = std::min(H, textH + 2 * pad + (barH ? barH + pad : 0));
    int bx1 = db.x1 + 2 * s, by2 = db.y2 - 2 * s;  // top-left corner (OFX y points up)
    int bx2 = bx1 + boxW, by1 = by2 - boxH;
    OFX::BitDepthEnum depth = dst->getPixelDepth();
    int nc = components(dst->getPixelComponents());
    for (int y = std::max(win.y1, by1); y < std::min(win.y2, by2); ++y) {
        int ly = by2 - 1 - y;
        for (int x = std::max(win.x1, bx1); x < std::min(win.x2, bx2); ++x) {
            void* p = dst->getPixelAddress(x, y);
            if (!p) continue;
            int lx = x - bx1;
            float px[4];
            for (int c = 0; c < 4; ++c) px[c] = c < nc ? readChannel(p, depth, c) : 1.0f;
            for (int c = 0; c < 3; ++c) px[c] = 0.08f + 0.2f * std::clamp(px[c], 0.0f, 1.0f);
            px[3] = 1.0f;
            if (lx < 2 * s) {
                px[0] = bn.r; px[1] = bn.g; px[2] = bn.b;
            } else {
                int tx = (lx - 2 * s - pad), ty = ly - pad;
                if (tx >= 0 && ty >= 0 && ck::textPixel(bn.text, tx / s, ty / s)) {
                    px[0] = px[1] = px[2] = 0.95f;
                }
                int barTop = pad + textH + pad;
                if (barH && ly >= barTop && ly < barTop + barH && tx >= 0 && tx < boxW - 2 * pad - 2 * s) {
                    bool on = tx < bn.progress * (boxW - 2 * pad - 2 * s);
                    px[0] = on ? bn.r : 0.3f; px[1] = on ? bn.g : 0.3f; px[2] = on ? bn.b : 0.3f;
                }
            }
            writePixel(p, depth, nc, px);
        }
    }
}

static std::string statusText(const ck::JobStatus& st) {
    std::ostringstream o;
    switch (st.state) {
    case ck::JobState::Missing: return "Not keyed yet. Press 'Key clip'.";
    case ck::JobState::Capturing:
        o << "Capturing frames: " << st.captured << " of " << st.count
          << ". Play the clip once from start to end; keying starts by itself. "
             "(Or press 'Key captured frames now'.)";
        return o.str();
    case ck::JobState::Keying:
        if (st.cancelling) return "Stopping…";
        o << "Keying in the background: " << (int)std::lround(st.progress * 100)
          << "%. You can keep working. Press 'Update status' to refresh.";
        return o.str();
    case ck::JobState::Done:
        o << "✓ Keyed " << st.keyedCount << " frames at " << st.w << "×" << st.h << ".";
        return o.str();
    case ck::JobState::Failed: return "✗ Keying failed: " + st.message + "  (log: " + st.logPath + ")";
    case ck::JobState::Cancelled: return "Stopped. Press 'Key clip' to start again.";
    }
    return "";
}

// ============================================================ the effect

class CorridorKeyPlugin : public OFX::ImageEffect {
public:
    explicit CorridorKeyPlugin(OfxImageEffectHandle handle) : ImageEffect(handle) {
        dst_ = fetchClip(kOfxImageEffectOutputClipName);
        src_ = fetchClip(kOfxImageEffectSimpleSourceClipName);
        hint_ = getContext() == OFX::eContextGeneral ? fetchClip(kHintClip) : nullptr;
        status_ = fetchStringParam(kStatus);
        keyData_ = fetchStringParam(kKeyData);
        tick_ = fetchIntParam(kTick);
        output_ = fetchChoiceParam(kOutput);
        hintQ_ = fetchDoubleParam(kHintQ);
        despill_ = fetchDoubleParam(kDespill);
        inputColor_ = fetchChoiceParam(kInputColor);
        memory_ = fetchChoiceParam(kMemory);
        lowVram_ = fetchBooleanParam(kLowVram);
        exportDir_ = fetchStringParam(kExportDir);
        plog("node created in " + detectHost().text + (hint_ ? " (Fusion/general: has Hint input)" : " (filter)"));
    }

    void render(const OFX::RenderArguments& args) override;
    bool isIdentity(const OFX::IsIdentityArguments& args, OFX::Clip*& clip, double& time) override;
    void getClipPreferences(OFX::ClipPreferencesSetter& prefs) override;
    void changedParam(const OFX::InstanceChangedArgs& args, const std::string& name) override;

private:
    void keyClip();
    void keyNow();
    void stopKey();
    void refresh();
    void forgetKey();
    void exportExr();
    // Param writes can be refused by a host; never let that abort a button.
    void setStatus(const std::string& v) {
        try {
            status_->setValue(v);
        } catch (...) {
            plog("couldn't show status (" + describeException() + "): " + v);
        }
    }
    void setKeyData(const std::string& v) {
        {
            std::lock_guard<std::mutex> lock(keyMu_);
            keyMem_ = v;
        }
        try {
            keyData_->setValue(v);  // saved with the project
        } catch (...) {
            plog("couldn't store key data in the project (" + describeException() + ")");
        }
        bumpTick();
    }
    void bumpTick() {
        try {
            int t = 0;
            tick_->getValue(t);
            tick_->setValue(t + 1);  // makes the host redraw
        } catch (...) {
            plog("couldn't trigger a redraw (" + describeException() + ")");
        }
    }
    KeyData keyData() {
        std::string s;
        try {
            keyData_->getValue(s);
        } catch (...) {
        }
        std::lock_guard<std::mutex> lock(keyMu_);
        if (s.empty() && !keyMem_.empty()) return KeyData::parse(keyMem_);
        return KeyData::parse(s);
    }
    std::mutex keyMu_;
    std::string keyMem_;
    void copySource(OFX::Image* dst, OFX::Image* src, const OfxRectI& win);
    void capture(const KeyData& k, int idx, OFX::Image* src, double time);

    OFX::Clip *dst_, *src_, *hint_;
    OFX::StringParam *status_, *keyData_, *exportDir_;
    OFX::IntParam* tick_;
    OFX::ChoiceParam *output_, *inputColor_, *memory_;
    OFX::DoubleParam *hintQ_, *despill_;
    OFX::BooleanParam* lowVram_;
};

void CorridorKeyPlugin::getClipPreferences(OFX::ClipPreferencesSetter& prefs) {
    prefs.setClipComponents(*dst_, OFX::ePixelComponentRGBA);
    prefs.setOutputPremultiplication(OFX::eImageUnPreMultiplied);
}

bool CorridorKeyPlugin::isIdentity(const OFX::IsIdentityArguments& args, OFX::Clip*& clip, double& time) {
    int mode = 0;
    output_->getValue(mode);
    if (mode == eSource) {
        clip = src_;
        time = args.time;
        return true;
    }
    return false;
}

void CorridorKeyPlugin::copySource(OFX::Image* dst, OFX::Image* src, const OfxRectI& win) {
    OFX::BitDepthEnum dd = dst->getPixelDepth();
    int dc = components(dst->getPixelComponents());
    // Fast path: same format, source covers the window -> copy whole rows.
    if (src && src->getPixelDepth() == dd && components(src->getPixelComponents()) == dc) {
        OfxRectI sb = src->getBounds();
        if (sb.x1 <= win.x1 && sb.x2 >= win.x2 && sb.y1 <= win.y1 && sb.y2 >= win.y2) {
            int bpc = dd == OFX::eBitDepthFloat ? 4 : dd == OFX::eBitDepthUShort ? 2 : 1;
            size_t bytes = (size_t)(win.x2 - win.x1) * dc * bpc;
            parallelRows(win.y1, win.y2, [&](int a, int b) {
                for (int y = a; y < b; ++y) {
                    void* dp = dst->getPixelAddress(win.x1, y);
                    void* sp = src->getPixelAddress(win.x1, y);
                    if (dp && sp) std::memcpy(dp, sp, bytes);
                }
            });
            return;
        }
    }
    parallelRows(win.y1, win.y2, [&](int a, int b) {
        for (int y = a; y < b; ++y)
            for (int x = win.x1; x < win.x2; ++x) {
                void* dp = dst->getPixelAddress(x, y);
                if (!dp) continue;
                float px[4] = {0, 0, 0, 1};
                void* sp = src ? src->getPixelAddress(x, y) : nullptr;
                if (sp) {
                    int sc = components(src->getPixelComponents());
                    for (int c = 0; c < std::min(sc, 4); ++c) px[c] = readChannel(sp, src->getPixelDepth(), c);
                    if (sc == 1) px[0] = px[1] = px[2] = px[3] = readChannel(sp, src->getPixelDepth(), 0);
                }
                writePixel(dp, dd, dc, px);
            }
    });
}

// Hand this frame (full resolution only) to the background writers.
void CorridorKeyPlugin::capture(const KeyData& k, int idx, OFX::Image* src, double time) {
    std::string cache = cachedCacheDir();
    if (ck::frameCaptured(cache, k.id, idx)) return;
    std::vector<float> rgb, hint;
    int w = 0, h = 0;
    OfxRectI b;
    if (!imageToBuffer(src, 3, rgb, w, h, b)) return;
    if (hint_ && hint_->isConnected()) {
        std::unique_ptr<OFX::Image> himg(hint_->fetchImage(time));
        int hw = 0, hh = 0;
        OfxRectI hb;
        if (!himg || !imageToBuffer(himg.get(), 1, hint, hw, hh, hb) || hw != w || hh != h) hint.clear();
    }
    ck::captureFrame(cache, k.id, idx, std::move(rgb), w, h, b.x1, b.y1, std::move(hint));
}

void CorridorKeyPlugin::render(const OFX::RenderArguments& args) {
    std::unique_ptr<OFX::Image> dst(dst_->fetchImage(args.time));
    if (!dst) OFX::throwSuiteStatusException(kOfxStatFailed);
    std::unique_ptr<OFX::Image> src(src_->isConnected() ? src_->fetchImage(args.time) : nullptr);

    int mode = 0;
    output_->getValueAtTime(args.time, mode);
    KeyData k = keyData();
    if (mode == eSource) {
        copySource(dst.get(), src.get(), args.renderWindow);
        return;
    }
    if (!k.valid()) {
        copySource(dst.get(), src.get(), args.renderWindow);
        drawBanner(dst.get(), args.renderWindow, {"CORRIDORKEY: PRESS KEY CLIP IN THE INSPECTOR", 0.9f, 0.3f, 0.3f});
        return;
    }
    const std::string cache = cachedCacheDir();
    const ck::JobStatus st = ck::jobStatus(cache, k.id);
    const int idx = (int)std::lround(args.time - k.start);
    {
        static std::mutex* mu = new std::mutex;
        static auto* seen = new std::map<std::string, int>;
        std::lock_guard<std::mutex> lock(*mu);
        int& last = (*seen)[k.id];
        if (last != (int)st.state + 1) {
            static const char* names[] = {"missing", "capturing", "keying", "done", "failed", "stopped"};
            last = (int)st.state + 1;
            plog("job " + k.id + " is " + names[(int)st.state] +
                 (st.state == ck::JobState::Failed ? ": " + st.message : "") +
                 " (render t=" + std::to_string(args.time) + " scale=" + std::to_string(args.renderScale.x) + ")");
        }
    }
    const bool fullRes = std::fabs(args.renderScale.x - 1.0) < 1e-6 && std::fabs(args.renderScale.y - 1.0) < 1e-6;
    char line[160];

    if (st.state != ck::JobState::Done) {
        copySource(dst.get(), src.get(), args.renderWindow);
        Banner bn;
        switch (st.state) {
        case ck::JobState::Capturing:
            if (src && idx >= 0 && idx < k.count && fullRes) capture(k, idx, src.get(), args.time);
            if (!fullRes) {
                bn = {"CORRIDORKEY: SET PLAYBACK TO FULL RESOLUTION (PROXY OFF) TO CAPTURE", 1.0f, 0.75f, 0.1f};
            } else {
                std::snprintf(line, sizeof(line), "CAPTURING %d/%d - PLAY THE CLIP ONCE",
                              std::min(st.captured + 1, st.count), st.count);
                bn = {line, 1.0f, 0.8f, 0.1f, st.count ? (double)st.captured / st.count : 0.0};
            }
            break;
        case ck::JobState::Keying:
            std::snprintf(line, sizeof(line), st.cancelling ? "STOPPING..." : "KEYING %d%% (BACKGROUND)",
                          (int)std::lround(st.progress * 100));
            bn = {line, 0.3f, 0.6f, 1.0f, st.progress};
            break;
        case ck::JobState::Failed:
            bn = {"KEYING FAILED - SEE STATUS IN THE INSPECTOR", 1.0f, 0.25f, 0.25f};
            break;
        case ck::JobState::Cancelled:
            bn = {"STOPPED - PRESS KEY CLIP TO START AGAIN", 0.6f, 0.6f, 0.6f};
            break;
        default:
            bn = {"KEY MISSING (CACHE CLEARED?) - PRESS KEY CLIP", 1.0f, 0.55f, 0.1f};
            break;
        }
        drawBanner(dst.get(), args.renderWindow, bn);
        return;
    }

    int outIdx = idx - st.keyedFirst;
    std::shared_ptr<CachedFrame> f;
    if (outIdx >= 0 && outIdx < st.keyedCount) f = gCache.get(cache, k.id, outIdx, mode == eForeground);
    if (!f) {
        copySource(dst.get(), src.get(), args.renderWindow);
        drawBanner(dst.get(), args.renderWindow,
                   {outIdx >= 0 && outIdx < st.keyedCount ? "KEY MISSING (CACHE CLEARED?) - PRESS KEY CLIP"
                                                          : "THIS FRAME IS OUTSIDE THE KEYED RANGE",
                    1.0f, 0.55f, 0.1f});
        return;
    }

    OFX::BitDepthEnum dd = dst->getPixelDepth();
    int dc = components(dst->getPixelComponents());
    const double sx = args.renderScale.x > 0 ? args.renderScale.x : 1.0;
    const double sy = args.renderScale.y > 0 ? args.renderScale.y : 1.0;
    const bool lin = k.linear;
    auto enc = [lin](float v) { return lin ? v : srgbEncode(v); };
    const int cell = std::max(8, f->w / 60);

    if (abort()) return;
    const OfxRectI rw = args.renderWindow;
    parallelRows(rw.y1, rw.y2, [&](int ya, int yb) {
    for (int y = ya; y < yb; ++y) {
        int Y = (int)std::floor((y + 0.5) / sy) - st.y1;   // full-res row from bottom
        int row = f->h - 1 - Y;                            // cache rows are top first
        for (int x = args.renderWindow.x1; x < args.renderWindow.x2; ++x) {
            void* dp = dst->getPixelAddress(x, y);
            if (!dp) continue;
            int X = (int)std::floor((x + 0.5) / sx) - st.x1;
            float out[4] = {0, 0, 0, 0};
            if (X >= 0 && X < f->w && row >= 0 && row < f->h) {
                const float* c = &f->cutout[((size_t)row * f->w + X) * 4];
                float a = std::clamp(c[3], 0.0f, 1.0f);
                switch (mode) {
                case eCutout:
                    out[0] = enc(c[0]); out[1] = enc(c[1]); out[2] = enc(c[2]); out[3] = a;
                    break;
                case eMatte:
                    out[0] = out[1] = out[2] = a; out[3] = 1.0f;
                    break;
                case eForeground: {
                    const float* g = f->fg.empty() ? c : &f->fg[((size_t)row * f->w + X) * 4];
                    out[0] = enc(g[0]); out[1] = enc(g[1]); out[2] = enc(g[2]); out[3] = a;
                    break;
                }
                case eChecker: {
                    float bg = ((X / cell + Y / cell) % 2) ? 0.55f : 0.8f;
                    for (int i = 0; i < 3; ++i) out[i] = std::clamp(enc(c[i]), 0.0f, 1.0f) * a + bg * (1 - a);
                    out[3] = 1.0f;
                    break;
                }
                default: break;
                }
            }
            writePixel(dp, dd, dc, out);
        }
    }
    });
}

void CorridorKeyPlugin::changedParam(const OFX::InstanceChangedArgs& args, const std::string& name) {
    // No check of args.reason: Resolve's Fusion page doesn't report button presses
    // as "user edits", and buttons can only be pressed by the user anyway.
    static const char* reasons[] = {"user", "plugin", "time"};
    if (name == kKey || name == kKeyNow || name == kStop || name == kRefresh || name == kForget || name == kExport)
        plog("button '" + name + "' (reason: " + reasons[std::clamp((int)args.reason, 0, 2)] + ")");
    try {
        if (name == kKey) keyClip();
        else if (name == kKeyNow) keyNow();
        else if (name == kStop) stopKey();
        else if (name == kRefresh) refresh();
        else if (name == kForget) forgetKey();
        else if (name == kExport) exportExr();
    } catch (...) {
        std::string why = describeException();
        plog("button '" + name + "' failed: " + why);
        setStatus("✗ Internal error (" + why + "). See plugin.log.");
    }
}

void CorridorKeyPlugin::refresh() {
    KeyData k = keyData();
    if (k.valid()) {
        ck::JobStatus st = ck::jobStatus(cachedCacheDir(), k.id, true);
        setStatus(statusText(st));
    } else {
        setStatus("Not keyed yet. Press 'Key clip'.");
    }
    bumpTick();
}

void CorridorKeyPlugin::forgetKey() {
    KeyData k = keyData();
    if (k.valid()) {
        gCache.forget(k.id);
        ck::forgetJob(cachedCacheDir(), k.id);
    }
    setKeyData("");
    setStatus("Not keyed yet. Press 'Key clip'.");
}

void CorridorKeyPlugin::keyClip() {
    HostInfo host = detectHost();
    std::string why;
    if (featureBlocked(host, "key", &why)) {
        sendMessage(OFX::Message::eMessageError, "", why);
        return;
    }
    ck::EngineConfig cfg = ck::loadConfig();
    if (!cfg.ok) {
        plog("config problem: " + cfg.error);
        setStatus("✗ " + cfg.error);
        sendMessage(OFX::Message::eMessageError, "", "CorridorKey 2.0: " + cfg.error);
        return;
    }
    if (!src_->isConnected()) {
        plog("Key clip: nothing connected to Source");
        setStatus("✗ Nothing connected to the Source input.");
        sendMessage(OFX::Message::eMessageError, "", "Nothing connected to the Source input.");
        return;
    }
    OfxRangeD range = src_->getFrameRange();
    // Fusion reports e.g. 0..361.9999 for frames 0-361, Resolve/others 0..361.
    // (0.9999 is 1e-4 short of the next frame, so the tolerance must be smaller.)
    int t0 = (int)std::ceil(range.min - 1e-5), t1 = (int)std::floor(range.max + 1e-5);
    int n = t1 - t0 + 1;
    plog("Key clip: source frame range " + std::to_string(range.min) + ".." + std::to_string(range.max));
    if (n < 1 || n > 1000000) {
        setStatus("✗ The host reported an invalid frame range for this clip.");
        sendMessage(OFX::Message::eMessageError, "", "The host reported an invalid frame range for this clip.");
        return;
    }
    plog("Key clip: " + std::to_string(n) + " frames from " + std::to_string(t0) + "; clearing the old key");
    forgetKey();  // drop this node's previous key/job

    ck::JobSpec spec;
    spec.start = t0;
    spec.count = n;
    int v = 0;
    inputColor_->getValue(v);
    spec.linear = v == 1;
    memory_->getValue(spec.window);
    lowVram_->getValue(spec.lowVram);
    despill_->getValue(spec.despill);
    hintQ_->getValue(spec.hintQ);
    // Resolve's Fusion page doesn't provide frame-rate properties (asking throws),
    // and the engine only uses it for metadata, so fall back quietly.
    double fps = 0;
    try { fps = src_->getFrameRate(); } catch (...) {}
    if (!(fps > 0)) try { fps = getFrameRate(); } catch (...) {}
    spec.fps = fps > 0 ? fps : 24.0;
    spec.hint = hint_ && hint_->isConnected();
    plog("Key clip: settings read (fps " + std::to_string(spec.fps) + (spec.hint ? ", with hint" : "") + ")");

    KeyData k;
    k.id = ck::randomId();
    k.start = t0;
    k.count = n;
    k.linear = spec.linear;
    std::string err;
    if (!ck::createJob(cfg.cacheDir, k.id, spec, &err)) {
        plog("Key clip: couldn't create the job: " + err);
        setStatus("✗ " + err);
        sendMessage(OFX::Message::eMessageError, "", "CorridorKey 2.0: " + err);
        return;
    }
    plog("job " + k.id + " created: " + std::to_string(n) + " frames, cache " + cfg.cacheDir);
    setKeyData(k.str());  // host re-renders: capturing starts with this frame
    setStatus(statusText(ck::jobStatus(cfg.cacheDir, k.id, true)));
}

void CorridorKeyPlugin::keyNow() {
    KeyData k = keyData();
    if (!k.valid()) {
        sendMessage(OFX::Message::eMessageError, "", "Press 'Key clip' and play the clip first.");
        return;
    }
    ck::waitForWrites();
    std::string err;
    if (!ck::launchJob(cachedCacheDir(), k.id, true, &err)) {
        plog("Key captured frames now failed: " + err);
        sendMessage(OFX::Message::eMessageError, "", "CorridorKey 2.0: " + err);
    }
    refresh();
}

void CorridorKeyPlugin::stopKey() {
    KeyData k = keyData();
    if (k.valid()) ck::cancelJob(cachedCacheDir(), k.id);
    refresh();
}

void CorridorKeyPlugin::exportExr() {
    std::string out;
    exportDir_->getValue(out);
    KeyData k = keyData();
    std::string cache = cachedCacheDir();
    ck::JobStatus st = k.valid() ? ck::jobStatus(cache, k.id, true) : ck::JobStatus{};
    if (st.state != ck::JobState::Done) {
        sendMessage(OFX::Message::eMessageError, "", "Key the clip first (wait until keying is done).");
        return;
    }
    if (out.empty()) {
        sendMessage(OFX::Message::eMessageError, "", "Choose an export folder first.");
        return;
    }
    ck::makeDirs(out);
    progressStart("CorridorKey 2.0: exporting EXRs", "ck2export");
    std::vector<float> rgba, alpha;
    int done = 0;
    for (int i = 0; i < st.keyedCount; ++i) {
        int w = 0, h = 0;
        std::string err;
        std::string cut = ck::outFramePath(cache, k.id, "cutout", i);
        if (!ck::readExrRGBA(cut, rgba, w, h, &err)) break;
        alpha.resize((size_t)w * h);
        for (size_t p = 0; p < alpha.size(); ++p) alpha[p] = rgba[p * 4 + 3];
        char name[96];
        int frame = k.start + st.keyedFirst + i;
        std::snprintf(name, sizeof(name), "corridorkey_alpha.%06d.exr", frame);
        ck::writeExr(ck::joinPath(out, name), alpha.data(), w, h, 1, &err);
        std::error_code ec;
        std::snprintf(name, sizeof(name), "corridorkey_cutout.%06d.exr", frame);
        fs::copy_file(cut, ck::joinPath(out, name), fs::copy_options::overwrite_existing, ec);
        std::snprintf(name, sizeof(name), "corridorkey_fg.%06d.exr", frame);
        fs::copy_file(ck::outFramePath(cache, k.id, "fg", i), ck::joinPath(out, name),
                      fs::copy_options::overwrite_existing, ec);
        ++done;
        if (!progressUpdate((double)(i + 1) / st.keyedCount)) break;
    }
    progressEnd();
    setStatus("Exported " + std::to_string(done) + " frames (alpha, fg, cutout; scene-linear EXR) to " + out);
}

// ============================================================ factory

mDeclarePluginFactory(CorridorKeyPluginFactory, {}, {});

void CorridorKeyPluginFactory::describe(OFX::ImageEffectDescriptor& desc) {
    desc.setLabels(kPluginName, kPluginName, kPluginName);
    desc.setPluginGrouping(kPluginGrouping);
    desc.setPluginDescription(kPluginDescription);
    desc.addSupportedContext(OFX::eContextFilter);
    desc.addSupportedContext(OFX::eContextGeneral);
    desc.addSupportedBitDepth(OFX::eBitDepthFloat);
    desc.addSupportedBitDepth(OFX::eBitDepthUShort);
    desc.addSupportedBitDepth(OFX::eBitDepthUByte);
    desc.setSingleInstance(false);
    desc.setHostFrameThreading(false);
    desc.setSupportsMultiResolution(true);
    desc.setSupportsTiles(false);
    desc.setTemporalClipAccess(false);
    desc.setRenderTwiceAlways(false);
    desc.setSupportsMultipleClipPARs(false);
    desc.setRenderThreadSafety(OFX::eRenderFullySafe);
}

void CorridorKeyPluginFactory::describeInContext(OFX::ImageEffectDescriptor& desc, OFX::ContextEnum context) {
    OFX::ClipDescriptor* src = desc.defineClip(kOfxImageEffectSimpleSourceClipName);
    src->addSupportedComponent(OFX::ePixelComponentRGBA);
    src->addSupportedComponent(OFX::ePixelComponentRGB);
    src->setSupportsTiles(false);
    if (context == OFX::eContextGeneral) {
        OFX::ClipDescriptor* hint = desc.defineClip(kHintClip);
        hint->addSupportedComponent(OFX::ePixelComponentRGBA);
        hint->addSupportedComponent(OFX::ePixelComponentAlpha);
        hint->setOptional(true);
        hint->setSupportsTiles(false);
        hint->setIsMask(false);
    }
    OFX::ClipDescriptor* dst = desc.defineClip(kOfxImageEffectOutputClipName);
    dst->addSupportedComponent(OFX::ePixelComponentRGBA);
    dst->setSupportsTiles(false);

    OFX::PageParamDescriptor* page = desc.definePageParam("Controls");
    HostInfo host = detectHost();

    OFX::PushButtonParamDescriptor* key = desc.definePushButtonParam(kKey);
    key->setLabel("Key clip");
    key->setHint("Starts a key: play the clip once so every frame is captured, then CorridorKey 2.0 "
                 "runs in the background. Press again after changing settings or the clip.");
    page->addChild(*key);

    OFX::PushButtonParamDescriptor* now = desc.definePushButtonParam(kKeyNow);
    now->setLabel("Key captured frames now");
    now->setHint("Don't wait for the whole clip: key the frames captured so far (the longest unbroken run).");
    page->addChild(*now);

    OFX::PushButtonParamDescriptor* stop = desc.definePushButtonParam(kStop);
    stop->setLabel("Stop keying");
    page->addChild(*stop);

    OFX::PushButtonParamDescriptor* ref = desc.definePushButtonParam(kRefresh);
    ref->setLabel("Update status");
    ref->setHint("Refresh the status line and redraw the viewer.");
    page->addChild(*ref);

    OFX::StringParamDescriptor* status = desc.defineStringParam(kStatus);
    status->setLabel("Status");
    status->setStringType(OFX::eStringTypeSingleLine);  // a Label type shows its name in Fusion, not the text
    status->setDefault("Not keyed yet. Press 'Key clip'.");
    status->setAnimates(false);
    status->setEvaluateOnChange(false);
    page->addChild(*status);

    OFX::ChoiceParamDescriptor* output = desc.defineChoiceParam(kOutput);
    output->setLabel("Output");
    output->appendOption("Cutout (RGBA, despilled)");
    output->appendOption("Matte (alpha)");
    output->appendOption("Foreground (raw colour + alpha)");
    output->appendOption("Checker preview");
    output->appendOption("Source (bypass)");
    output->setDefault(eCutout);
    output->setAnimates(false);
    page->addChild(*output);

    OFX::ChoiceParamDescriptor* ic = desc.defineChoiceParam(kInputColor);
    ic->setLabel("Input is");
    ic->appendOption("Display-referred (Rec.709 / sRGB timeline)");
    ic->appendOption("Scene-linear");
    ic->setHint("Match your timeline colour space. Output is returned in the same space.");
    ic->setDefault(0);
    ic->setAnimates(false);
    ic->setEvaluateOnChange(false);
    page->addChild(*ic);

    OFX::DoubleParamDescriptor* despill = desc.defineDoubleParam(kDespill);
    despill->setLabel("Despill");
    despill->setRange(0, 1);
    despill->setDisplayRange(0, 1);
    despill->setDefault(0.5);
    despill->setAnimates(false);
    despill->setEvaluateOnChange(false);
    despill->setHint("Used for the next 'Key clip'.");
    page->addChild(*despill);

    OFX::DoubleParamDescriptor* hq = desc.defineDoubleParam(kHintQ);
    hq->setLabel("Hint trust");
    hq->setRange(0, 1);
    hq->setDisplayRange(0, 1);
    hq->setDefault(0.95);
    hq->setAnimates(false);
    hq->setEvaluateOnChange(false);
    hq->setHint(context == OFX::eContextGeneral
                    ? "How much to trust the optional Hint input (0.95 clean matte, 0.75 rough, 0.5 loose)."
                    : "Only used when a Hint input is connected (Fusion page).");
    page->addChild(*hq);

    OFX::ChoiceParamDescriptor* mem = desc.defineChoiceParam(kMemory);
    mem->setLabel("Temporal window");
    for (auto* w : kWindows) mem->appendOption(w);
    mem->setDefault(0);
    mem->setAnimates(false);
    mem->setEvaluateOnChange(false);
    page->addChild(*mem);

    OFX::BooleanParamDescriptor* lv = desc.defineBooleanParam(kLowVram);
    lv->setLabel("Low-VRAM mode");
    lv->setDefault(false);
    lv->setAnimates(false);
    lv->setEvaluateOnChange(false);
    lv->setHint("Only with a manual window. Uses disk instead of GPU memory (slower).");
    page->addChild(*lv);

    OFX::PushButtonParamDescriptor* forget = desc.definePushButtonParam(kForget);
    forget->setLabel("Forget key");
    forget->setHint("Delete this node's cached key from disk.");
    page->addChild(*forget);

    OFX::StringParamDescriptor* ed = desc.defineStringParam(kExportDir);
    ed->setLabel("EXR export folder");
    ed->setStringType(OFX::eStringTypeDirectoryPath);
    ed->setAnimates(false);
    ed->setEvaluateOnChange(false);
    page->addChild(*ed);

    OFX::PushButtonParamDescriptor* ex = desc.definePushButtonParam(kExport);
    ex->setLabel("Export EXRs");
    ex->setHint("Writes corridorkey_alpha / _fg / _cutout.######.exr (scene-linear) for Fusion or Nuke.");
    page->addChild(*ex);

    OFX::StringParamDescriptor* hi = desc.defineStringParam(kHostInfo);
    hi->setLabel("Host");
    hi->setStringType(OFX::eStringTypeSingleLine);
    hi->setEnabled(false);  // read-only; a Label type shows its name in Fusion, not the text
    hi->setDefault(host.text);
    hi->setAnimates(false);
    hi->setEvaluateOnChange(false);
    page->addChild(*hi);

    OFX::StringParamDescriptor* edn = desc.defineStringParam(kEditionInfo);
    edn->setLabel("Edition");
    edn->setStringType(OFX::eStringTypeSingleLine);
    edn->setEnabled(false);  // read-only; a Label type shows its name in Fusion, not the text
    edn->setDefault(kStudioOnly.empty()
                        ? (host.edition == "unknown" ? "All features available."
                                                     : "All features work in Resolve " + host.edition + ".")
                        : "Some features need Resolve Studio.");
    edn->setAnimates(false);
    edn->setEvaluateOnChange(false);
    page->addChild(*edn);

    OFX::StringParamDescriptor* kd = desc.defineStringParam(kKeyData);
    kd->setLabel("Key data");
    kd->setIsSecret(true);
    kd->setAnimates(false);
    kd->setDefault("");

    OFX::IntParamDescriptor* tick = desc.defineIntParam(kTick);
    tick->setLabel("Refresh");
    tick->setIsSecret(true);
    tick->setAnimates(false);
    tick->setIsPersistant(false);
    tick->setDefault(0);
}

OFX::ImageEffect* CorridorKeyPluginFactory::createInstance(OfxImageEffectHandle handle, OFX::ContextEnum) {
    return new CorridorKeyPlugin(handle);
}

namespace OFX {
namespace Plugin {
void getPluginIDs(OFX::PluginFactoryArray& ids) {
    static CorridorKeyPluginFactory p(kPluginIdentifier, kPluginVersionMajor, kPluginVersionMinor);
    ids.push_back(&p);
}
}  // namespace Plugin
}  // namespace OFX
