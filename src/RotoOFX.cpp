// CorridorKey Roto: pick characters in a picker window (click / right-click /
// drag rectangles), SAM 2.1 tracks them through the clip in the background, and
// the node outputs a hint matte for CorridorKey's Hint input, a garbage-filled
// source, or an overlay to check it.
//
//   1. "Pick character…" opens the picker on the current frame (live preview).
//   2. "Track clip", then play the clip once (frames are captured while playing,
//      like Key clip). Tracking runs in the background.

#include "common.h"

namespace fs = std::filesystem;

#define kRotoId "io.github.ck2gui.CorridorKey2Roto"
#define kRotoName "CorridorKey Roto"

#define kRPick "pickCharacter"
#define kRTrack "trackClip"
#define kRStop "stopTrack"
#define kRRefresh "refreshStatus"
#define kRForget "forgetRoto"
#define kRStatus "status"
#define kROutput "rotoOutput"
#define kRGrow "grow"
#define kRSoften "soften"
#define kRMargin "margin"
#define kRFeather "feather"
#define kRScreen "screen"
#define kRModel "model"
#define kRInput "inputColor"
#define kRData "rotoData"
#define kRTick "refreshTick"

enum RotoOut { eOverlay = 0, eHint, eGarbage, eRSource };
static const char* kModels[][2] = {{"facebook/sam2.1-hiera-large", "SAM 2.1 Large – best edges"},
                                   {"facebook/sam2.1-hiera-base-plus", "SAM 2.1 Base+ – balanced"},
                                   {"facebook/sam2.1-hiera-small", "SAM 2.1 Small – faster"},
                                   {"facebook/sam2.1-hiera-tiny", "SAM 2.1 Tiny – fastest"}};

// 8-bit display image for the picker window (PPM: Qt reads it, no extra libraries).
static bool writePPM(const std::string& path, const std::vector<float>& rgb, int w, int h, bool linear) {
    std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary);
        if (!f) return false;
        f << "P6\n" << w << " " << h << "\n255\n";
        std::vector<unsigned char> row((size_t)w * 3);
        for (int y = 0; y < h; ++y) {
            for (int i = 0; i < w * 3; ++i) {
                float v = rgb[(size_t)y * w * 3 + i];
                if (linear) v = srgbEncode(v);
                row[i] = (unsigned char)std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f);
            }
            f.write((const char*)row.data(), (std::streamsize)row.size());
        }
        if (!f) return false;
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    return !ec;
}

// ------------------------------------------------------------------ masks

struct Mask {
    int w = 0, h = 0;
    std::vector<float> v;  // top row first, 0..1
};

class MaskCache {
public:
    std::shared_ptr<Mask> get(const std::string& path) {
        std::error_code ec;
        auto t = fs::last_write_time(path, ec);
        if (ec) return nullptr;
        std::string key = path + "@" + std::to_string(t.time_since_epoch().count());
        std::lock_guard<std::mutex> lock(mu_);
        auto it = map_.find(key);
        if (it != map_.end()) return it->second;
        auto m = std::make_shared<Mask>();
        std::vector<float> rgba;
        if (!ck::readExrRGBA(path, rgba, m->w, m->h, nullptr)) return nullptr;
        m->v.resize((size_t)m->w * m->h);
        for (size_t i = 0; i < m->v.size(); ++i) m->v[i] = rgba[i * 4];
        map_[key] = m;
        order_.push_front(key);
        while (order_.size() > 24) {
            map_.erase(order_.back());
            order_.pop_back();
        }
        return m;
    }

private:
    std::mutex mu_;
    std::map<std::string, std::shared_ptr<Mask>> map_;
    std::list<std::string> order_;
};
static MaskCache gMasks;

// Field of W x H floats, top row first.
struct Field {
    int w = 0, h = 0;
    std::vector<float> v;
    float& at(int x, int y) { return v[(size_t)y * w + x]; }
};

static void transpose(const Field& in, Field& out) {
    out.w = in.h;
    out.h = in.w;
    out.v.resize(in.v.size());
    parallelRows(0, in.h, [&](int a, int b) {
        for (int y = a; y < b; ++y)
            for (int x = 0; x < in.w; ++x) out.v[(size_t)x * out.w + y] = in.v[(size_t)y * in.w + x];
    });
}

// Exact squared Euclidean distance transform (Felzenszwalb & Huttenlocher), per row.
static void edtRows(Field& f) {
    parallelRows(0, f.h, [&](int a, int b) {
        const int n = f.w;
        std::vector<float> d(n), z(n + 1);
        std::vector<int> v(n);
        for (int y = a; y < b; ++y) {
            float* row = &f.v[(size_t)y * n];
            int k = 0;
            v[0] = 0;
            z[0] = -1e30f;
            z[1] = 1e30f;
            for (int q = 1; q < n; ++q) {
                float s;
                while (true) {
                    int p = v[k];
                    s = ((row[q] + (float)q * q) - (row[p] + (float)p * p)) / (2.0f * q - 2.0f * p);
                    if (s <= z[k] && k > 0) --k;
                    else break;
                }
                if (s <= z[k]) {  // k == 0
                    v[0] = q;
                    z[0] = -1e30f;
                    z[1] = 1e30f;
                    continue;
                }
                ++k;
                v[k] = q;
                z[k] = s;
                z[k + 1] = 1e30f;
            }
            k = 0;
            for (int q = 0; q < n; ++q) {
                while (z[k + 1] < q) ++k;
                d[q] = (float)(q - v[k]) * (q - v[k]) + row[v[k]];
            }
            std::copy(d.begin(), d.end(), row);
        }
    });
}

static void rowBox(Field& f, int r) {
    if (r <= 0) return;
    parallelRows(0, f.h, [&](int a, int b) {
        std::vector<float> tmp;
        for (int y = a; y < b; ++y) {
            float* row = &f.v[(size_t)y * f.w];
            tmp.assign(row, row + f.w);
            double sum = 0;
            int n = f.w;
            auto px = [&](int i) { return tmp[std::clamp(i, 0, n - 1)]; };
            for (int i = -r; i <= r; ++i) sum += px(i);
            for (int x = 0; x < n; ++x) {
                row[x] = (float)(sum / (2 * r + 1));
                sum += px(x + r + 1) - px(x - r);
            }
        }
    });
}

// Grow (r > 0) or shrink (r < 0) a hard mask by r pixels with a round brush.
static void dilate(Field& f, int r) {
    if (r == 0) return;
    const bool grow = r > 0;
    const float r2 = (float)r * r;
    const float inf = 1e20f;
    Field t;
    // Distance to the nearest "inside" pixel (grow) or "outside" pixel (shrink).
    for (float& x : f.v) x = ((x >= 0.5f) == grow) ? 0.0f : inf;
    edtRows(f);
    transpose(f, t);
    edtRows(t);
    transpose(t, f);
    for (float& x : f.v) x = grow ? (x <= r2 ? 1.0f : 0.0f) : (x <= r2 ? 0.0f : 1.0f);
}

static void blur(Field& f, double sigma) {  // three box passes ~ gaussian
    int r = (int)std::lround(sigma);
    if (r <= 0) return;
    Field t;
    for (int i = 0; i < 3; ++i) rowBox(f, r);
    transpose(f, t);
    for (int i = 0; i < 3; ++i) rowBox(t, r);
    transpose(t, f);
}

// Mask -> hard edge at the output size, grown/shrunk then softened (sizes in output pixels).
static Field shapeMask(const Mask& m, int W, int H, int grow, double soften) {
    Field f;
    f.w = W;
    f.h = H;
    f.v.assign((size_t)W * H, 0.0f);
    const double sx = (double)m.w / W, sy = (double)m.h / H;
    parallelRows(0, H, [&](int a, int b) {
        for (int y = a; y < b; ++y) {
            double my = (y + 0.5) * sy - 0.5;
            int y0 = std::clamp((int)std::floor(my), 0, m.h - 1), y1 = std::min(y0 + 1, m.h - 1);
            float fy = (float)std::clamp(my - y0, 0.0, 1.0);
            for (int x = 0; x < W; ++x) {
                double mx = (x + 0.5) * sx - 0.5;
                int x0 = std::clamp((int)std::floor(mx), 0, m.w - 1), x1 = std::min(x0 + 1, m.w - 1);
                float fx = (float)std::clamp(mx - x0, 0.0, 1.0);
                float top = m.v[(size_t)y0 * m.w + x0] * (1 - fx) + m.v[(size_t)y0 * m.w + x1] * fx;
                float bot = m.v[(size_t)y1 * m.w + x0] * (1 - fx) + m.v[(size_t)y1 * m.w + x1] * fx;
                f.v[(size_t)y * W + x] = (top * (1 - fy) + bot * fy) >= 0.5f ? 1.0f : 0.0f;
            }
        }
    });
    dilate(f, grow);
    blur(f, soften / 2.0);
    return f;
}

// Median screen colour outside the keep area (like ck2-gui's garbage fill).
static void screenColour(const std::vector<float>& rgb, int W, int H, const Field& keep, int screenMode, float out[3]) {
    std::vector<float> ch[3];
    int step = std::max(1, std::min(W, H) / 180);
    for (int y = 0; y < H; y += step)
        for (int x = 0; x < W; x += step)
            if (keep.v[(size_t)y * W + x] < 0.01f)
                for (int c = 0; c < 3; ++c) ch[c].push_back(rgb[((size_t)y * W + x) * 3 + c]);
    if (ch[0].size() < 64) {
        for (int c = 0; c < 3; ++c) ch[c].clear();
        for (int y = 0; y < H; y += step)
            for (int x = 0; x < W; x += step)
                for (int c = 0; c < 3; ++c) ch[c].push_back(rgb[((size_t)y * W + x) * 3 + c]);
    }
    auto median = [](std::vector<float> v) {
        if (v.empty()) return 0.0f;
        std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
        return v[v.size() / 2];
    };
    int scr = screenMode;  // 0 auto, 1 green, 2 blue
    if (scr == 0) {
        std::vector<float> gd, bd;
        for (size_t i = 0; i < ch[0].size(); ++i) {
            gd.push_back(ch[1][i] - std::max(ch[0][i], ch[2][i]));
            bd.push_back(ch[2][i] - std::max(ch[0][i], ch[1][i]));
        }
        scr = median(gd) >= median(bd) ? 1 : 2;
    }
    int c0 = scr == 1 ? 1 : 2, o1 = scr == 1 ? 0 : 0, o2 = scr == 1 ? 2 : 1;
    std::vector<float> sel[3];
    for (size_t i = 0; i < ch[0].size(); ++i)
        if (ch[c0][i] > ch[o1][i] * 1.15f && ch[c0][i] > ch[o2][i] * 1.15f)
            for (int c = 0; c < 3; ++c) sel[c].push_back(ch[c][i]);
    for (int c = 0; c < 3; ++c) out[c] = median(sel[0].size() >= 64 ? sel[c] : ch[c]);
}

// ------------------------------------------------------------------ the node

struct RotoData {
    std::string id;
    int start = 0, count = 0;
    bool valid() const { return !id.empty() && count > 0; }
    std::string str() const {
        return "id=" + id + ";start=" + std::to_string(start) + ";count=" + std::to_string(count);
    }
    static RotoData parse(const std::string& s) {
        RotoData r;
        std::stringstream ss(s);
        std::string kv;
        while (std::getline(ss, kv, ';')) {
            auto eq = kv.find('=');
            if (eq == std::string::npos) continue;
            std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
            try {
                if (k == "id") r.id = v;
                else if (k == "start") r.start = std::stoi(v);
                else if (k == "count") r.count = std::stoi(v);
            } catch (...) {
            }
        }
        return r;
    }
};

class CorridorKeyRoto : public OFX::ImageEffect {
public:
    explicit CorridorKeyRoto(OfxImageEffectHandle h) : ImageEffect(h) {
        dst_ = fetchClip(kOfxImageEffectOutputClipName);
        src_ = fetchClip(kOfxImageEffectSimpleSourceClipName);
        status_ = fetchStringParam(kRStatus);
        data_ = fetchStringParam(kRData);
        tick_ = fetchIntParam(kRTick);
        output_ = fetchChoiceParam(kROutput);
        grow_ = fetchIntParam(kRGrow);
        soften_ = fetchDoubleParam(kRSoften);
        margin_ = fetchIntParam(kRMargin);
        feather_ = fetchDoubleParam(kRFeather);
        screen_ = fetchChoiceParam(kRScreen);
        model_ = fetchChoiceParam(kRModel);
        input_ = fetchChoiceParam(kRInput);
    }

    void getClipPreferences(OFX::ClipPreferencesSetter& prefs) override {
        prefs.setClipComponents(*dst_, OFX::ePixelComponentRGBA);
        prefs.setOutputPremultiplication(OFX::eImageUnPreMultiplied);
    }

    bool isIdentity(const OFX::IsIdentityArguments& args, OFX::Clip*& clip, double& time) override {
        int o = 0;
        output_->getValue(o);
        if (o == eRSource) {
            clip = src_;
            time = args.time;
            return true;
        }
        return false;
    }

    void render(const OFX::RenderArguments& args) override;
    void changedParam(const OFX::InstanceChangedArgs& args, const std::string& name) override;

private:
    std::string root() { return ck::rotoRoot(cachedCacheDir()); }
    RotoData data() {
        std::string s;
        try {
            data_->getValue(s);
        } catch (...) {
        }
        std::lock_guard<std::mutex> lock(mu_);
        if (s.empty() && !mem_.empty()) s = mem_;
        return RotoData::parse(s);
    }
    void setData(const std::string& v) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            mem_ = v;
        }
        try {
            data_->setValue(v);
        } catch (...) {
            plog("roto: couldn't store data in the project (" + describeException() + ")");
        }
        bump();
    }
    void setStatus(const std::string& v) {
        if (v.rfind("\xe2\x9c\x97", 0) == 0) plog("roto: " + v);  // log every ✗ message
        try {
            status_->setValue(v);
        } catch (...) {
            plog("roto: couldn't show status: " + v);
        }
    }
    void bump() {
        try {
            int t = 0;
            tick_->getValue(t);
            tick_->setValue(t + 1);
        } catch (...) {
        }
    }
    std::string modelName() {
        int m = 0;
        model_->getValue(m);
        return kModels[std::clamp(m, 0, 3)][0];
    }
    bool ensureJob(RotoData& r, std::string* err);
    void pick(double time);
    void track();
    void refresh();

    OFX::Clip *dst_, *src_;
    OFX::StringParam *status_, *data_;
    OFX::IntParam *tick_, *grow_, *margin_;
    OFX::DoubleParam *soften_, *feather_;
    OFX::ChoiceParam *output_, *screen_, *model_, *input_;
    std::mutex mu_;
    std::string mem_;
};

bool CorridorKeyRoto::ensureJob(RotoData& r, std::string* err) {
    if (r.valid() && ck::jobStatus(root(), r.id, true).state != ck::JobState::Missing) return true;
    ck::EngineConfig cfg = ck::loadConfig();
    if (!cfg.ok) {
        if (err) *err = cfg.error;
        return false;
    }
    if (!src_->isConnected()) {
        if (err) *err = "Connect the clip to this node's input first (in Fusion: drag from the MediaIn's output "
                        "to the Roto node's yellow input; the same clip also stays connected to CorridorKey)";
        return false;
    }
    OfxRangeD range = src_->getFrameRange();
    int t0 = (int)std::ceil(range.min - 1e-5), t1 = (int)std::floor(range.max + 1e-5);
    ck::JobSpec spec;
    spec.kind = "roto";
    spec.start = t0;
    spec.count = t1 - t0 + 1;
    int lin = 0;
    input_->getValue(lin);
    spec.linear = lin == 1;
    if (spec.count < 1) {
        if (err) *err = "the host reported an empty frame range";
        return false;
    }
    r = RotoData{ck::randomId(), t0, spec.count};
    if (!ck::createJob(root(), r.id, spec, err)) return false;
    setData(r.str());
    plog("roto job " + r.id + " created: " + std::to_string(spec.count) + " frames from " + std::to_string(t0));
    return true;
}

void CorridorKeyRoto::pick(double time) {
    std::string err;
    RotoData r = data();
    if (!ensureJob(r, &err)) {
        setStatus("✗ " + err);
        return;
    }
    ck::JobStatus st = ck::jobStatus(root(), r.id, true);
    if (st.state == ck::JobState::Keying) {
        setStatus("✗ Tracking is running; press Stop first to change the selection.");
        return;
    }
    std::unique_ptr<OFX::Image> img(src_->fetchImage(time));
    std::vector<float> rgb;
    int w = 0, h = 0;
    OfxRectI b;
    if (!img || !imageToBuffer(img.get(), 3, rgb, w, h, b)) {
        setStatus("✗ Couldn't read this frame from the host.");
        return;
    }
    int idx = (int)std::lround(time - r.start);
    if (idx < 0 || idx >= r.count) {
        setStatus("✗ The playhead is outside the clip.");
        return;
    }
    int lin = 0;
    input_->getValue(lin);
    std::string pv = ck::previewDir(root(), r.id);
    ck::makeDirs(pv);
    char name[64];
    std::snprintf(name, sizeof(name), "src_%06d.exr", idx);  // for SAM 2 (full quality)
    std::string exr = ck::joinPath(pv, name);
    if (!ck::writeExr(exr + ".tmp.exr", rgb.data(), w, h, 3, &err, false)) {
        setStatus("✗ Couldn't save the frame: " + err);
        return;
    }
    std::error_code ec;
    fs::rename(exr + ".tmp.exr", exr, ec);
    std::snprintf(name, sizeof(name), "view_%06d.ppm", idx);  // for the picker window
    if (!writePPM(ck::joinPath(pv, name), rgb, w, h, lin == 1)) {
        setStatus("✗ Couldn't save the frame for the picker.");
        return;
    }
    if (!ck::openPicker(root(), r.id, idx, modelName(), &err)) {
        setStatus("✗ Couldn't open the picker: " + err);
        return;
    }
    plog("roto picker opened on frame " + std::to_string(idx));
    setStatus("Picker window open (frame " + std::to_string(idx) + "): click / drag on what to keep, "
              "then 'Use this selection'. Then press 'Track clip'.");
    bump();
}

void CorridorKeyRoto::track() {
    std::string err;
    RotoData r = data();
    if (!r.valid()) {
        setStatus("✗ Press 'Pick character…' first.");
        return;
    }
    ck::Prompts pr = ck::readPrompts(root(), r.id);
    if (!pr.valid) {
        setStatus(ck::pickerOpen(root(), r.id) ? "✗ Press 'Use this selection' in the picker window first."
                                               : "✗ Press 'Pick character…' and choose what to keep first.");
        return;
    }
    if (!ck::requestTrack(root(), r.id, &err)) {
        setStatus("✗ " + err);
        return;
    }
    plog("roto track requested: " + std::to_string(pr.objects) + " object(s) from frame " + std::to_string(pr.frame));
    refresh();
}

void CorridorKeyRoto::refresh() {
    RotoData r = data();
    if (!r.valid()) {
        setStatus("Go to a frame where the character is clear and press 'Pick character…'.");
        bump();
        return;
    }
    ck::JobStatus st = ck::jobStatus(root(), r.id, true);
    ck::Prompts pr = ck::readPrompts(root(), r.id);
    std::ostringstream o;
    switch (st.state) {
    case ck::JobState::Capturing:
        if (ck::trackWanted(root(), r.id))
            o << "Capturing frames " << st.captured << "/" << st.count << ": play the clip once; tracking starts by itself.";
        else if (ck::pickerOpen(root(), r.id))
            o << "Picker window is open: choose what to keep, then 'Use this selection'.";
        else if (pr.valid)
            o << pr.objects << " object(s) picked on frame " << pr.frame << ". Press 'Track clip'.";
        else
            o << "Press 'Pick character…' and choose what to keep.";
        break;
    case ck::JobState::Keying:
        o << (st.cancelling ? "Stopping…" : "Tracking in the background: ") << (int)std::lround(st.progress * 100) << "%";
        break;
    case ck::JobState::Done: o << "✓ Tracked " << st.keyedCount << " frames. Output: pick Hint matte or Garbage-filled."; break;
    case ck::JobState::Failed: o << "✗ Tracking failed: " << st.message; break;
    case ck::JobState::Cancelled: o << "Stopped. Press 'Track clip' to try again."; break;
    default: o << "Go to a frame where the character is clear and press 'Pick character…'."; break;
    }
    setStatus(o.str());
    bump();
}

void CorridorKeyRoto::changedParam(const OFX::InstanceChangedArgs& args, const std::string& name) {
    bool button = name == kRPick || name == kRTrack || name == kRStop || name == kRRefresh || name == kRForget;
    if (!button) return;
    if (name != kRRefresh) plog("roto button '" + name + "'");
    try {
        if (name == kRPick) pick(args.time);
        else if (name == kRTrack) track();
        else if (name == kRRefresh) refresh();
        else if (name == kRStop) {
            RotoData r = data();
            if (r.valid()) ck::cancelJob(root(), r.id);
            refresh();
        } else if (name == kRForget) {
            RotoData r = data();
            if (r.valid()) ck::forgetJob(root(), r.id);
            setData("");
            setStatus("Go to a frame where the character is clear and press 'Pick character…'.");
        }
    } catch (...) {
        std::string why = describeException();
        plog("roto button '" + name + "' failed: " + why);
        setStatus("✗ Internal error (" + why + "). See plugin.log.");
    }
}

void CorridorKeyRoto::render(const OFX::RenderArguments& args) {
    std::unique_ptr<OFX::Image> dst(dst_->fetchImage(args.time));
    if (!dst) OFX::throwSuiteStatusException(kOfxStatFailed);
    std::unique_ptr<OFX::Image> src(src_->isConnected() ? src_->fetchImage(args.time) : nullptr);
    int out = 0;
    output_->getValue(out);
    const OfxRectI rw = args.renderWindow;
    if (out == eRSource) return copySource(dst.get(), src.get(), rw);

    RotoData r = data();
    const std::string rr = root();
    ck::JobStatus st = r.valid() ? ck::jobStatus(rr, r.id) : ck::JobStatus{};
    const int idx = (int)std::lround(args.time - r.start);
    const bool fullRes = std::fabs(args.renderScale.x - 1.0) < 1e-6 && std::fabs(args.renderScale.y - 1.0) < 1e-6;

    // Capture frames while the user plays the clip (after Track clip was pressed).
    if (r.valid() && st.state == ck::JobState::Capturing && ck::trackWanted(rr, r.id) && src && fullRes &&
        idx >= 0 && idx < r.count && !ck::frameCaptured(rr, r.id, idx)) {
        std::vector<float> rgb, none;
        int w = 0, h = 0;
        OfxRectI b;
        if (imageToBuffer(src.get(), 3, rgb, w, h, b)) ck::captureFrame(rr, r.id, idx, std::move(rgb), w, h, b.x1, b.y1, std::move(none));
    }

    // Which mask applies to this frame?
    std::shared_ptr<Mask> mask;
    if (r.valid()) {
        if (st.state == ck::JobState::Done) mask = gMasks.get(ck::trackMaskPath(rr, r.id, idx));
        if (!mask) mask = gMasks.get(ck::previewMaskPath(rr, r.id, idx));
    }

    // Banner text for anything that isn't a finished track.
    Banner bn;
    char line[160];
    if (!r.valid()) {
        bn = {src ? "ROTO: PRESS PICK CHARACTER IN THE INSPECTOR" : "ROTO: CONNECT THE CLIP TO THIS NODE'S INPUT",
              0.9f, 0.3f, 0.3f};
    } else if (st.state == ck::JobState::Capturing && ck::trackWanted(rr, r.id)) {
        if (!fullRes) bn = {"ROTO: SET PLAYBACK TO FULL RESOLUTION TO CAPTURE", 1.0f, 0.75f, 0.1f};
        else {
            std::snprintf(line, sizeof(line), "ROTO: CAPTURING %d/%d - PLAY THE CLIP ONCE", std::min(st.captured + 1, st.count), st.count);
            bn = {line, 1.0f, 0.8f, 0.1f, st.count ? (double)st.captured / st.count : 0.0};
        }
    } else if (st.state == ck::JobState::Keying) {
        std::snprintf(line, sizeof(line), "ROTO: TRACKING %d%%", (int)std::lround(st.progress * 100));
        bn = {line, 0.3f, 0.6f, 1.0f, st.progress};
    } else if (st.state == ck::JobState::Failed) {
        bn = {"ROTO: TRACKING FAILED - SEE STATUS", 1.0f, 0.25f, 0.25f};
    } else if (st.state != ck::JobState::Done) {
        if (ck::pickerOpen(rr, r.id)) bn = {"ROTO: PICKER WINDOW IS OPEN", 0.3f, 0.6f, 1.0f};
        else if (ck::readPrompts(rr, r.id).valid) bn = {"ROTO: PICKED - PRESS TRACK CLIP", 0.4f, 0.85f, 0.4f};
        else bn = {"ROTO: PRESS PICK CHARACTER IN THE INSPECTOR", 0.6f, 0.6f, 0.6f};
    }

    OfxRectI db = dst->getBounds();
    const int W = db.x2 - db.x1, H = db.y2 - db.y1;
    const double sc = args.renderScale.x > 0 ? args.renderScale.x : 1.0;

    if (!mask || W <= 0 || H <= 0) {
        if (out == eHint) {  // neutral hint (keep everything) until there is a mask
            float white[4] = {1, 1, 1, 1};
            OFX::BitDepthEnum d = dst->getPixelDepth();
            int nc = components(dst->getPixelComponents());
            for (int y = rw.y1; y < rw.y2; ++y)
                for (int x = rw.x1; x < rw.x2; ++x)
                    if (void* p = dst->getPixelAddress(x, y)) writePixel(p, d, nc, white);
        } else {
            copySource(dst.get(), src.get(), rw);
        }
        drawBanner(dst.get(), rw, bn);
        return;
    }

    int grow = 0, margin = 0, scr = 0;
    double soften = 0, feather = 0;
    grow_->getValue(grow);
    soften_->getValue(soften);
    margin_->getValue(margin);
    feather_->getValue(feather);
    screen_->getValue(scr);
    if (abort()) return;

    OFX::BitDepthEnum dd = dst->getPixelDepth();
    int dc = components(dst->getPixelComponents());
    std::vector<float> rgb;  // source at output size, top row first
    int sw = 0, sh = 0;
    OfxRectI sb{0, 0, 0, 0};
    if (src) imageToBuffer(src.get(), 3, rgb, sw, sh, sb);
    auto srcPx = [&](int X, int Ytop, float o[3]) {  // X/Ytop in dst bounds coords
        int sx = X + db.x1 - sb.x1, sy = Ytop + (sb.y2 - db.y2);
        if (rgb.empty() || sx < 0 || sy < 0 || sx >= sw || sy >= sh) { o[0] = o[1] = o[2] = 0; return; }
        const float* p = &rgb[((size_t)sy * sw + sx) * 3];
        o[0] = p[0]; o[1] = p[1]; o[2] = p[2];
    };

    if (out == eHint) {
        Field f = shapeMask(*mask, W, H, (int)std::lround(grow * sc), soften * sc);
        parallelRows(rw.y1, rw.y2, [&](int a, int b) {
            for (int y = a; y < b; ++y) {
                int Yt = db.y2 - 1 - y;
                for (int x = rw.x1; x < rw.x2; ++x) {
                    void* p = dst->getPixelAddress(x, y);
                    if (!p) continue;
                    float m = f.v[(size_t)Yt * W + (x - db.x1)];
                    float px[4] = {m, m, m, 1.0f};
                    writePixel(p, dd, dc, px);
                }
            }
        });
    } else if (out == eGarbage) {
        Field keep = shapeMask(*mask, W, H, (int)std::lround(margin * sc), feather * sc);
        float fill[3] = {0, 0, 0};
        if (!rgb.empty() && sw == W && sh == H) screenColour(rgb, W, H, keep, scr, fill);
        parallelRows(rw.y1, rw.y2, [&](int a, int b) {
            for (int y = a; y < b; ++y) {
                int Yt = db.y2 - 1 - y;
                for (int x = rw.x1; x < rw.x2; ++x) {
                    void* p = dst->getPixelAddress(x, y);
                    if (!p) continue;
                    float k = keep.v[(size_t)Yt * W + (x - db.x1)], s[3];
                    srcPx(x - db.x1, Yt, s);
                    float px[4] = {s[0] * k + fill[0] * (1 - k), s[1] * k + fill[1] * (1 - k), s[2] * k + fill[2] * (1 - k), 1.0f};
                    writePixel(p, dd, dc, px);
                }
            }
        });
    } else {  // overlay
        Field f = shapeMask(*mask, W, H, (int)std::lround(grow * sc), soften * sc);
        parallelRows(rw.y1, rw.y2, [&](int a, int b) {
            for (int y = a; y < b; ++y) {
                int Yt = db.y2 - 1 - y;
                for (int x = rw.x1; x < rw.x2; ++x) {
                    void* p = dst->getPixelAddress(x, y);
                    if (!p) continue;
                    float m = f.v[(size_t)Yt * W + (x - db.x1)], s[3];
                    srcPx(x - db.x1, Yt, s);
                    float px[4] = {s[0] * (1 - 0.45f * m) + 0.9f * 0.45f * m, s[1] * (1 - 0.45f * m) + 0.15f * 0.45f * m,
                                   s[2] * (1 - 0.45f * m) + 0.15f * 0.45f * m, 1.0f};
                    if (m < 0.5f) { px[0] *= 0.6f; px[1] *= 0.6f; px[2] *= 0.6f; }  // dim what gets removed
                    writePixel(p, dd, dc, px);
                }
            }
        });
    }
    if (out == eOverlay || !bn.text.empty()) drawBanner(dst.get(), rw, bn);
}

// ------------------------------------------------------------------ factory

mDeclarePluginFactory(CorridorKeyRotoFactory, {}, {});

void CorridorKeyRotoFactory::describe(OFX::ImageEffectDescriptor& desc) {
    desc.setLabels(kRotoName, kRotoName, kRotoName);
    desc.setPluginGrouping("Keying");
    desc.setPluginDescription(
        "Pick a character with on-screen points; SAM 2.1 tracks it through the clip. Outputs a hint "
        "matte for CorridorKey 2.0's Hint input, or the source with everything else filled with screen colour.");
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
    desc.setRenderThreadSafety(OFX::eRenderFullySafe);
}

void CorridorKeyRotoFactory::describeInContext(OFX::ImageEffectDescriptor& desc, OFX::ContextEnum) {
    OFX::ClipDescriptor* src = desc.defineClip(kOfxImageEffectSimpleSourceClipName);
    src->addSupportedComponent(OFX::ePixelComponentRGBA);
    src->addSupportedComponent(OFX::ePixelComponentRGB);
    src->setSupportsTiles(false);
    OFX::ClipDescriptor* dst = desc.defineClip(kOfxImageEffectOutputClipName);
    dst->addSupportedComponent(OFX::ePixelComponentRGBA);
    dst->setSupportsTiles(false);

    OFX::PageParamDescriptor* page = desc.definePageParam("Controls");
    auto button = [&](const char* id, const char* label, const char* hint) {
        OFX::PushButtonParamDescriptor* b = desc.definePushButtonParam(id);
        b->setLabel(label);
        b->setHint(hint);
        page->addChild(*b);
    };
    button(kRPick, "Pick character…", "Opens a window on this frame: click on what to keep, right-click on what "
                                      "to remove, drag rectangles around characters. Live preview.");
    button(kRTrack, "Track clip", "Track the picked selection through the whole clip. Play the clip once to capture it.");
    button(kRStop, "Stop", "Stop tracking.");
    button(kRRefresh, "Update status", "Refresh the status line and the viewer.");

    OFX::StringParamDescriptor* status = desc.defineStringParam(kRStatus);
    status->setLabel("Status");
    status->setStringType(OFX::eStringTypeSingleLine);
    status->setDefault("Go to a frame where the character is clear and press 'Pick character…'.");
    status->setAnimates(false);
    status->setEvaluateOnChange(false);
    page->addChild(*status);

    OFX::ChoiceParamDescriptor* out = desc.defineChoiceParam(kROutput);
    out->setLabel("Output");
    out->appendOption("Overlay – check the mask");
    out->appendOption("Hint matte – into CorridorKey's Hint input (Fusion)");
    out->appendOption("Garbage-filled source – feed CorridorKey directly");
    out->appendOption("Source (bypass)");
    out->setDefault(eHint);
    out->setAnimates(false);
    page->addChild(*out);

    OFX::IntParamDescriptor* grow = desc.defineIntParam(kRGrow);
    grow->setLabel("Hint grow (px)");
    grow->setRange(-200, 200);
    grow->setDisplayRange(-50, 50);
    grow->setDefault(0);
    grow->setAnimates(false);
    page->addChild(*grow);
    OFX::DoubleParamDescriptor* soften = desc.defineDoubleParam(kRSoften);
    soften->setLabel("Hint soften (px)");
    soften->setRange(0, 200);
    soften->setDisplayRange(0, 50);
    soften->setDefault(4);
    soften->setAnimates(false);
    page->addChild(*soften);
    OFX::IntParamDescriptor* margin = desc.defineIntParam(kRMargin);
    margin->setLabel("Garbage margin (px)");
    margin->setRange(0, 400);
    margin->setDisplayRange(0, 150);
    margin->setDefault(40);
    margin->setAnimates(false);
    page->addChild(*margin);
    OFX::DoubleParamDescriptor* feather = desc.defineDoubleParam(kRFeather);
    feather->setLabel("Garbage feather (px)");
    feather->setRange(0, 400);
    feather->setDisplayRange(0, 100);
    feather->setDefault(30);
    feather->setAnimates(false);
    page->addChild(*feather);
    OFX::ChoiceParamDescriptor* scr = desc.defineChoiceParam(kRScreen);
    scr->setLabel("Screen colour");
    scr->appendOption("Auto");
    scr->appendOption("Green");
    scr->appendOption("Blue");
    scr->setDefault(0);
    scr->setAnimates(false);
    page->addChild(*scr);

    OFX::ChoiceParamDescriptor* model = desc.defineChoiceParam(kRModel);
    model->setLabel("Model");
    for (auto& m : kModels) model->appendOption(m[1]);
#ifdef __APPLE__
    model->setDefault(1);
#else
    model->setDefault(0);
#endif
    model->setAnimates(false);
    model->setEvaluateOnChange(false);
    page->addChild(*model);
    OFX::ChoiceParamDescriptor* ic = desc.defineChoiceParam(kRInput);
    ic->setLabel("Input is");
    ic->appendOption("Display-referred (Rec.709 / sRGB timeline)");
    ic->appendOption("Scene-linear");
    ic->setDefault(0);
    ic->setAnimates(false);
    ic->setEvaluateOnChange(false);
    page->addChild(*ic);

    button(kRForget, "Forget roto", "Delete this node's tracking data from disk.");

    OFX::StringParamDescriptor* data = desc.defineStringParam(kRData);
    data->setLabel("Roto data");
    data->setIsSecret(true);
    data->setAnimates(false);
    data->setDefault("");
    OFX::IntParamDescriptor* tick = desc.defineIntParam(kRTick);
    tick->setLabel("Refresh");
    tick->setIsSecret(true);
    tick->setAnimates(false);
    tick->setIsPersistant(false);
    tick->setDefault(0);
}

OFX::ImageEffect* CorridorKeyRotoFactory::createInstance(OfxImageEffectHandle h, OFX::ContextEnum) {
    return new CorridorKeyRoto(h);
}

OFX::PluginFactory* makeRotoFactory() {
    static CorridorKeyRotoFactory p(kRotoId, 1, 0);
    return &p;
}
