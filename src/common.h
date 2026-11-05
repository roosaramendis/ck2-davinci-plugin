// Shared by all CorridorKey OFX nodes: host detection, pixel helpers, the
// keyed-frame cache, banners, logging. Header-only.
#pragma once

#include <algorithm>
#include <cctype>
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

#include "engine.h"
#include "exr_io.h"
#include "job.h"
#include "overlay.h"

// ============================================================ host / edition

struct HostInfo {
    std::string text;     // shown in the panel
    std::string edition;  // "Studio", "Free" or "unknown"
};

inline HostInfo detectHost() {
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
inline const std::map<std::string, std::string> kStudioOnly = {};

inline bool featureBlocked(const HostInfo& h, const std::string& feature, std::string* why) {
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

inline FrameCache gCache;

inline std::string cachedCacheDir() {
    static std::mutex mu;
    static std::string dir;
    std::lock_guard<std::mutex> lock(mu);
    if (dir.empty()) dir = ck::loadConfig().cacheDir;
    return dir;
}

// Plugin log: <cache>/plugin.log (button presses, errors, state changes).
inline void plog(const std::string& msg) {
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
inline std::string describeException() {
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

// ------------------------------------------------------------ bug report

inline std::string urlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
        else if (c == ' ') o += '+';
        else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
    }
    return o;
}

// Open a URL or folder with the system's default app (browser / file manager).
inline void openWithSystem(const std::string& target) {
    std::string err;
#ifdef _WIN32
    ck::spawnDetached({"C:\\Windows\\explorer.exe", target}, {}, "", &err);
#elif defined(__APPLE__)
    ck::spawnDetached({"/usr/bin/open", target}, {}, "", &err);
#else
    ck::spawnDetached({"/usr/bin/xdg-open", target}, {{"LD_LIBRARY_PATH", ck::kEnvUnset}}, "", &err);
#endif
}

inline std::string fileTail(const std::string& path, size_t maxLines) {
    std::string s;
    if (!ck::readText(path, s)) return "";
    std::vector<std::string> lines;
    std::stringstream ss(s);
    std::string l;
    while (std::getline(ss, l)) lines.push_back(l);
    std::string out;
    for (size_t i = lines.size() > maxLines ? lines.size() - maxLines : 0; i < lines.size(); ++i) out += lines[i] + "\n";
    return out;
}

// Opens a pre-filled GitHub issue for the plugin + the folder with its logs.
inline void reportBug(const std::string& hostText, const std::string& nodeName, const std::string& status) {
    std::string cache = ck::loadConfig().cacheDir;
    std::string body = "**What happened**\n\n(describe what you did and what you expected)\n\n**System**\n"
                       "- Host: " + hostText + "\n- Node: " + nodeName + "\n- Status: " + status +
#ifdef _WIN32
                       "\n- OS: Windows"
#elif defined(__APPLE__)
                       "\n- OS: macOS"
#else
                       "\n- OS: Linux"
#endif
                       "\n\n**Logs**\nThe plugin's log folder was just opened: please drag `plugin.log` "
                       "(and the newest job folder's engine.log) into this issue.\n\n```\n";
    std::string tail = fileTail(ck::joinPath(cache, "plugin.log"), 40);
    if (tail.size() > 4000) tail = tail.substr(tail.size() - 4000);
    body += tail + "```\n";
    std::string url = "https://github.com/roosaramendis/ck2-davinci-plugin/issues/new?labels=bug&title=" +
                      urlEncode("[" + nodeName + "] ") + "&body=" + urlEncode(body);
    openWithSystem(url);
    openWithSystem(cache);
}

// Split rows over a few threads (4K frames are slow single-threaded).
template <class F>
void parallelRows(int y1, int y2, F&& f) {
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

inline float srgbEncode(float v) {  // matches CorridorKey's signed linear_to_srgb
    float a = std::fabs(v);
    float e = a <= 0.0031308f ? a * 12.92f : 1.055f * std::pow(a, 1.0f / 2.4f) - 0.055f;
    return v < 0 ? -e : e;
}

inline float readChannel(const void* px, OFX::BitDepthEnum depth, int c) {
    switch (depth) {
    case OFX::eBitDepthUByte: return ((const unsigned char*)px)[c] / 255.0f;
    case OFX::eBitDepthUShort: return ((const unsigned short*)px)[c] / 65535.0f;
    case OFX::eBitDepthFloat: return ((const float*)px)[c];
    default: return 0.0f;
    }
}

inline void writePixel(void* px, OFX::BitDepthEnum depth, int nComp, const float rgba[4]) {
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

inline int components(OFX::PixelComponentEnum c) {
    switch (c) {
    case OFX::ePixelComponentRGBA: return 4;
    case OFX::ePixelComponentRGB: return 3;
    case OFX::ePixelComponentAlpha: return 1;
    default: return 0;
    }
}

// Image -> top-row-first float buffer with `outC` channels (3 = RGB, 1 = matte).
// Walks rows with pointers (this runs during playback, so it must be quick).
inline bool imageToBuffer(OFX::Image* img, int outC, std::vector<float>& buf, int& w, int& h,
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

inline void drawBanner(OFX::Image* dst, const OfxRectI& win, const Banner& bn) {
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


// Pass the source through (row copies when formats match).
inline void copySource(OFX::Image* dst, OFX::Image* src, const OfxRectI& win) {
    OFX::BitDepthEnum dd = dst->getPixelDepth();
    int dc = components(dst->getPixelComponents());
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

// Frames the host will actually play. The Source clip can report the whole media
// (a clip trimmed in the timeline / a Fusion comp shorter than its MediaIn), so the
// range is narrowed to the output clip, the effect's range and the timeline when the
// host reports them. `how` describes what was found (for the log).
inline OfxRangeD playableRange(OFX::ImageEffect* fx, OFX::Clip* src, OFX::Clip* dst, std::string* how) {
    OfxRangeD r = src->getFrameRange();
    std::ostringstream o;
    o << "source " << r.min << ".." << r.max;
    auto narrow = [&](const char* what, double a, double b) {
        o << ", " << what << " " << a << ".." << b;
        if (b < a || (a == 0 && b == 0)) return;  // not reported
        double lo = std::max(r.min, a), hi = std::min(r.max, b);
        // Ignore ranges that overlap the clip by less than 2 frames (Edit page
        // timeline numbers, a host's "unknown" single frame).
        if (hi - lo < 1.0) {
            o << " (ignored)";
            return;
        }
        r = {lo, hi};
    };
    try {
        OfxRangeD d = dst->getFrameRange();
        narrow("output", d.min, d.max);
    } catch (...) {
    }
    try {
        const OFX::PropertySet& ps = fx->getPropertySet();
        narrow("effect", ps.propGetDouble(kOfxImageEffectPropFrameRange, 0),
               ps.propGetDouble(kOfxImageEffectPropFrameRange, 1));
    } catch (...) {
    }
    try {
        if (OFX::getImageEffectHostDescription()->supportsTimeLineSuite) {
            double t1 = 0, t2 = 0;
            fx->timeLineGetBounds(t1, t2);
            narrow("timeline", t1, t2);
        }
    } catch (...) {
    }
    try {
        double dur = fx->getEffectDuration();
        if (dur >= 2) narrow("duration", r.min, r.min + dur - 1);
    } catch (...) {
    }
    o << " -> " << r.min << ".." << r.max;
    if (how) *how = o.str();
    return r;
}

enum OutputMode { eCutout = 0, eMatte, eForeground, eChecker, eSource };



// Draw a keyed frame (from the cache) into dst. Returns false if aborted.
inline void renderKeyed(OFX::Image* dst, const OfxRectI& rw, const OfxPointD& renderScale, const CachedFrame& f,
                        int x1, int y1, int mode, bool linear) {
    OFX::BitDepthEnum dd = dst->getPixelDepth();
    int dc = components(dst->getPixelComponents());
    const double sx = renderScale.x > 0 ? renderScale.x : 1.0;
    const double sy = renderScale.y > 0 ? renderScale.y : 1.0;
    auto enc = [linear](float v) { return linear ? v : srgbEncode(v); };
    const int cell = std::max(8, f.w / 60);
    parallelRows(rw.y1, rw.y2, [&](int ya, int yb) {
        for (int y = ya; y < yb; ++y) {
            int Y = (int)std::floor((y + 0.5) / sy) - y1;  // full-res row from bottom
            int row = f.h - 1 - Y;                          // cache rows are top first
            for (int x = rw.x1; x < rw.x2; ++x) {
                void* dp = dst->getPixelAddress(x, y);
                if (!dp) continue;
                int X = (int)std::floor((x + 0.5) / sx) - x1;
                float out[4] = {0, 0, 0, 0};
                if (X >= 0 && X < f.w && row >= 0 && row < f.h) {
                    const float* c = &f.cutout[((size_t)row * f.w + X) * 4];
                    float a = std::clamp(c[3], 0.0f, 1.0f);
                    switch (mode) {
                    case eCutout:
                        out[0] = enc(c[0]); out[1] = enc(c[1]); out[2] = enc(c[2]); out[3] = a;
                        break;
                    case eMatte:
                        out[0] = out[1] = out[2] = a; out[3] = 1.0f;
                        break;
                    case eForeground: {
                        const float* g = f.fg.empty() ? c : &f.fg[((size_t)row * f.w + X) * 4];
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
