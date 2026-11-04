// CorridorKey Pass: outputs one pass (cutout, matte, foreground, checker) of a
// key made by a CorridorKey 2.0 node, found by its key name. Use several Pass
// nodes to get several passes at once (an OFX node can only have one output).
// Nothing is keyed again: all Pass nodes read the same cached result.

#include "common.h"

namespace fs = std::filesystem;

#define kPassId "io.github.ck2gui.CorridorKey2Pass"
#define kPassName "CorridorKey Pass"

#define kPKeyName "keyName"
#define kPPass "pass"
#define kPStatus "status"
#define kPRefresh "refreshStatus"
#define kPTick "refreshTick"

class CorridorKeyPass : public OFX::ImageEffect {
public:
    explicit CorridorKeyPass(OfxImageEffectHandle h) : ImageEffect(h) {
        dst_ = fetchClip(kOfxImageEffectOutputClipName);
        src_ = fetchClip(kOfxImageEffectSimpleSourceClipName);
        name_ = fetchStringParam(kPKeyName);
        pass_ = fetchChoiceParam(kPPass);
        status_ = fetchStringParam(kPStatus);
        tick_ = fetchIntParam(kPTick);
    }

    void getClipPreferences(OFX::ClipPreferencesSetter& prefs) override {
        prefs.setClipComponents(*dst_, OFX::ePixelComponentRGBA);
        prefs.setOutputPremultiplication(OFX::eImageUnPreMultiplied);
    }

    bool isIdentity(const OFX::IsIdentityArguments& args, OFX::Clip*& clip, double& time) override {
        int p = 0;
        pass_->getValue(p);
        if (p == eSource && src_->isConnected()) {
            clip = src_;
            time = args.time;
            return true;
        }
        return false;
    }

    // Without a Source connected the output still has the key's size.
    bool getRegionOfDefinition(const OFX::RegionOfDefinitionArguments& args, OfxRectD& rod) override {
        ck::KeyRef ref = lookup();
        if (!ref.valid()) return false;
        ck::JobStatus st = ck::jobStatus(cachedCacheDir(), ref.id);
        if (st.state != ck::JobState::Done || st.w <= 0) return false;
        rod.x1 = st.x1;
        rod.y1 = st.y1;
        rod.x2 = st.x1 + st.w;
        rod.y2 = st.y1 + st.h;
        return true;
    }

    void render(const OFX::RenderArguments& args) override {
        std::unique_ptr<OFX::Image> dst(dst_->fetchImage(args.time));
        if (!dst) OFX::throwSuiteStatusException(kOfxStatFailed);
        std::unique_ptr<OFX::Image> src(src_->isConnected() ? src_->fetchImage(args.time) : nullptr);
        int p = 0;
        pass_->getValueAtTime(args.time, p);
        std::string name;
        name_->getValue(name);
        ck::KeyRef ref = lookup();
        auto show = [&](const std::string& text, float r, float g, float b, double prog = -1) {
            copySource(dst.get(), src.get(), args.renderWindow);
            drawBanner(dst.get(), args.renderWindow, {text, r, g, b, prog});
        };
        if (p == eSource) {
            copySource(dst.get(), src.get(), args.renderWindow);
            return;
        }
        if (ck::safeKeyName(name).empty()) return show("CORRIDORKEY PASS: TYPE THE KEY NAME IN THE INSPECTOR", 0.9f, 0.3f, 0.3f);
        if (!ref.valid()) return show("NO KEY NAMED '" + ck::safeKeyName(name) + "' YET", 0.9f, 0.3f, 0.3f);
        const std::string cache = cachedCacheDir();
        ck::JobStatus st = ck::jobStatus(cache, ref.id);
        char line[160];
        switch (st.state) {
        case ck::JobState::Done: break;
        case ck::JobState::Capturing:
            std::snprintf(line, sizeof(line), "KEY '%s' IS CAPTURING %d/%d", ck::safeKeyName(name).c_str(),
                          st.captured, st.count);
            return show(line, 1.0f, 0.8f, 0.1f, st.count ? (double)st.captured / st.count : 0.0);
        case ck::JobState::Keying:
            std::snprintf(line, sizeof(line), "KEY '%s' IS KEYING %d%%", ck::safeKeyName(name).c_str(),
                          (int)std::lround(st.progress * 100));
            return show(line, 0.3f, 0.6f, 1.0f, st.progress);
        default:
            return show("KEY '" + ck::safeKeyName(name) + "' ISN'T AVAILABLE - PRESS KEY CLIP ON ITS NODE", 1.0f, 0.55f, 0.1f);
        }
        int outIdx = (int)std::lround(args.time - ref.start) - st.keyedFirst;
        std::shared_ptr<CachedFrame> f;
        if (outIdx >= 0 && outIdx < st.keyedCount) f = gCache.get(cache, ref.id, outIdx, p == eForeground);
        if (!f) return show("THIS FRAME IS OUTSIDE THE KEYED RANGE", 1.0f, 0.55f, 0.1f);
        if (abort()) return;
        renderKeyed(dst.get(), args.renderWindow, args.renderScale, *f, st.x1, st.y1, p, ref.linear);
    }

    void changedParam(const OFX::InstanceChangedArgs&, const std::string& name) override {
        if (name != kPRefresh && name != kPKeyName) return;
        try {
            ck::KeyRef ref = lookup();
            std::string text;
            std::string kn;
            name_->getValue(kn);
            if (!ref.valid()) {
                text = ck::safeKeyName(kn).empty() ? "Type the key name shown on the CorridorKey 2.0 node."
                                                   : "No key named '" + ck::safeKeyName(kn) + "' yet.";
            } else {
                ck::JobStatus st = ck::jobStatus(cachedCacheDir(), ref.id, true);
                std::ostringstream o;
                switch (st.state) {
                case ck::JobState::Done:
                    o << "✓ Using key '" << ck::safeKeyName(kn) << "': " << st.keyedCount << " frames, " << st.w
                      << "×" << st.h;
                    break;
                case ck::JobState::Capturing: o << "Key is capturing frames (" << st.captured << "/" << st.count << ")."; break;
                case ck::JobState::Keying: o << "Key is keying: " << (int)std::lround(st.progress * 100) << "%."; break;
                default: o << "That key isn't available; press Key clip on its node."; break;
                }
                text = o.str();
            }
            status_->setValue(text);
        } catch (...) {
            plog("Pass node: status update failed (" + describeException() + ")");
        }
        try {
            int t = 0;
            tick_->getValue(t);
            tick_->setValue(t + 1);
        } catch (...) {
        }
    }

private:
    ck::KeyRef lookup() {
        std::string name;
        name_->getValue(name);
        return ck::lookupKeyName(cachedCacheDir(), name);
    }

    OFX::Clip *dst_, *src_;
    OFX::StringParam *name_, *status_;
    OFX::ChoiceParam* pass_;
    OFX::IntParam* tick_;
};

mDeclarePluginFactory(CorridorKeyPassFactory, {}, {});

void CorridorKeyPassFactory::describe(OFX::ImageEffectDescriptor& desc) {
    desc.setLabels(kPassName, kPassName, kPassName);
    desc.setPluginGrouping("Keying");
    desc.setPluginDescription(
        "One pass (cutout, matte, foreground or checker) of a key made by a CorridorKey 2.0 node, "
        "found by its key name. Use several to get several passes at once.");
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

void CorridorKeyPassFactory::describeInContext(OFX::ImageEffectDescriptor& desc, OFX::ContextEnum) {
    OFX::ClipDescriptor* src = desc.defineClip(kOfxImageEffectSimpleSourceClipName);
    src->addSupportedComponent(OFX::ePixelComponentRGBA);
    src->addSupportedComponent(OFX::ePixelComponentRGB);
    src->setSupportsTiles(false);
    src->setOptional(true);
    OFX::ClipDescriptor* dst = desc.defineClip(kOfxImageEffectOutputClipName);
    dst->addSupportedComponent(OFX::ePixelComponentRGBA);
    dst->setSupportsTiles(false);

    OFX::PageParamDescriptor* page = desc.definePageParam("Controls");

    OFX::StringParamDescriptor* name = desc.defineStringParam(kPKeyName);
    name->setLabel("Key name");
    name->setHint("The 'Key name' shown on the CorridorKey 2.0 node.");
    name->setDefault("");
    name->setAnimates(false);
    page->addChild(*name);

    OFX::ChoiceParamDescriptor* pass = desc.defineChoiceParam(kPPass);
    pass->setLabel("Pass");
    pass->appendOption("Cutout (RGBA, despilled)");
    pass->appendOption("Matte (alpha)");
    pass->appendOption("Foreground (raw colour + alpha)");
    pass->appendOption("Checker preview");
    pass->appendOption("Source (bypass)");
    pass->setDefault(eCutout);
    pass->setAnimates(false);
    page->addChild(*pass);

    OFX::PushButtonParamDescriptor* ref = desc.definePushButtonParam(kPRefresh);
    ref->setLabel("Update status");
    page->addChild(*ref);

    OFX::StringParamDescriptor* status = desc.defineStringParam(kPStatus);
    status->setLabel("Status");
    status->setStringType(OFX::eStringTypeSingleLine);
    status->setDefault("Type the key name shown on the CorridorKey 2.0 node.");
    status->setAnimates(false);
    status->setEvaluateOnChange(false);
    page->addChild(*status);

    OFX::IntParamDescriptor* tick = desc.defineIntParam(kPTick);
    tick->setLabel("Refresh");
    tick->setIsSecret(true);
    tick->setAnimates(false);
    tick->setIsPersistant(false);
    tick->setDefault(0);
}

OFX::ImageEffect* CorridorKeyPassFactory::createInstance(OfxImageEffectHandle h, OFX::ContextEnum) {
    return new CorridorKeyPass(h);
}

OFX::PluginFactory* makePassFactory() {
    static CorridorKeyPassFactory p(kPassId, 1, 0);
    return &p;
}
