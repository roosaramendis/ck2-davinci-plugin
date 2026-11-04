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

#include "common.h"

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
#define kKeyName "keyName"
#define kHintMode "hintMode"

static const char* kWindows[] = {"Auto (from free GPU memory)", "80/40 – steadiest, most memory", "48/24",
                                 "24/12", "8/4", "1/1 – single frames, least memory"};

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
        keyName_ = fetchStringParam(kKeyName);
        hintMode_ = hint_ ? fetchChoiceParam(kHintMode) : nullptr;
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
    void capture(const KeyData& k, int idx, OFX::Image* src, double time);

    OFX::Clip *dst_, *src_, *hint_;
    OFX::StringParam *status_, *keyData_, *exportDir_, *keyName_;
    OFX::ChoiceParam* hintMode_;
    void publishName(const KeyData& k);
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

    if (abort()) return;
    renderKeyed(dst.get(), args.renderWindow, args.renderScale, *f, st.x1, st.y1, mode, k.linear);
}

void CorridorKeyPlugin::changedParam(const OFX::InstanceChangedArgs& args, const std::string& name) {
    // No check of args.reason: Resolve's Fusion page doesn't report button presses
    // as "user edits", and buttons can only be pressed by the user anyway.
    if (name == kKeyName) {
        try {
            KeyData k = keyData();
            if (k.valid()) publishName(k);
        } catch (...) {
            plog("key name change failed: " + describeException());
        }
        return;
    }
    static const char* reasons[] = {"user", "plugin", "time"};
    if (name == kKey || name == kKeyNow || name == kStop || name == kForget || name == kExport)
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

void CorridorKeyPlugin::publishName(const KeyData& k) {
    std::string name;
    try {
        keyName_->getValue(name);
    } catch (...) {
    }
    if (ck::safeKeyName(name).empty()) {
        name = "key-" + k.id.substr(0, 4);
        try {
            keyName_->setValue(name);
        } catch (...) {
            plog("couldn't fill in the key name (" + describeException() + ")");
        }
    }
    if (ck::registerKeyName(cachedCacheDir(), name, k.id, k.start, k.count, k.linear))
        plog("key " + k.id + " published as '" + ck::safeKeyName(name) + "'");
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
    if (hintMode_) {
        int hm = 0;
        hintMode_->getValue(hm);
        spec.hintFirstOnly = hm == 1;
    }
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
    publishName(k);
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

    OFX::StringParamDescriptor* kn = desc.defineStringParam(kKeyName);
    kn->setLabel("Key name");
    kn->setHint("CorridorKey Pass nodes and the 'CorridorKey Outputs' macro find this key by its name. "
                "Filled in automatically on the first 'Key clip'; you can rename it.");
    kn->setDefault("");
    kn->setAnimates(false);
    kn->setEvaluateOnChange(false);
    page->addChild(*kn);

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

    if (context == OFX::eContextGeneral) {
        OFX::ChoiceParamDescriptor* hm = desc.defineChoiceParam(kHintMode);
        hm->setLabel("Hint is");
        hm->appendOption("A matte for every frame (e.g. from CorridorKey Roto)");
        hm->appendOption("First frame only (CorridorKey carries it forward)");
        hm->setDefault(0);
        hm->setAnimates(false);
        hm->setEvaluateOnChange(false);
        page->addChild(*hm);
    }

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

OFX::PluginFactory* makePassFactory();
OFX::PluginFactory* makeRotoFactory();

namespace OFX {
namespace Plugin {
void getPluginIDs(OFX::PluginFactoryArray& ids) {
    static CorridorKeyPluginFactory p(kPluginIdentifier, kPluginVersionMajor, kPluginVersionMinor);
    ids.push_back(&p);
    ids.push_back(makePassFactory());
    ids.push_back(makeRotoFactory());
}
}  // namespace Plugin
}  // namespace OFX
