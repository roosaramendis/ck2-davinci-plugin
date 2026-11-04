#include "exr_io.h"

#include <cstdlib>
#include <cstring>

#include "tinyexr.h"

namespace ck {

// Channel names are chosen so every reader we care about understands them:
// 1 channel -> "Y" (OpenCV reads it as greyscale, Resolve as luminance),
// 3 -> B,G,R, 4 -> A,B,G,R (EXR stores channels sorted by name).
bool writeExr(const std::string& path, const float* data, int w, int h, int channels,
              std::string* err, bool compress) {
    if (channels != 1 && channels != 3 && channels != 4) {
        if (err) *err = "unsupported channel count";
        return false;
    }
    static const char* kNames1[] = {"Y"};
    static const char* kNames3[] = {"B", "G", "R"};
    static const char* kNames4[] = {"A", "B", "G", "R"};
    static const int kSrc3[] = {2, 1, 0};     // interleaved RGB -> B,G,R planes
    static const int kSrc4[] = {3, 2, 1, 0};  // interleaved RGBA -> A,B,G,R planes
    const char** names = channels == 1 ? kNames1 : channels == 3 ? kNames3 : kNames4;
    const size_t n = (size_t)w * h;

    std::vector<std::vector<float>> planes(channels, std::vector<float>(n));
    for (int c = 0; c < channels; ++c) {
        int src = channels == 1 ? 0 : channels == 3 ? kSrc3[c] : kSrc4[c];
        float* dst = planes[c].data();
        for (size_t i = 0; i < n; ++i) dst[i] = data[i * channels + src];
    }
    std::vector<float*> ptrs(channels);
    for (int c = 0; c < channels; ++c) ptrs[c] = planes[c].data();

    EXRHeader header;
    InitEXRHeader(&header);
    EXRImage image;
    InitEXRImage(&image);
    image.num_channels = channels;
    image.images = reinterpret_cast<unsigned char**>(ptrs.data());
    image.width = w;
    image.height = h;

    std::vector<EXRChannelInfo> info(channels);
    std::vector<int> pixelTypes(channels, TINYEXR_PIXELTYPE_FLOAT);
    std::vector<int> savedTypes(channels, TINYEXR_PIXELTYPE_HALF);
    for (int c = 0; c < channels; ++c) {
        std::memset(&info[c], 0, sizeof(EXRChannelInfo));
        std::strncpy(info[c].name, names[c], 255);
    }
    header.num_channels = channels;
    header.channels = info.data();
    header.pixel_types = pixelTypes.data();
    header.requested_pixel_types = savedTypes.data();
    header.compression_type = compress ? TINYEXR_COMPRESSIONTYPE_ZIP : TINYEXR_COMPRESSIONTYPE_NONE;

    const char* e = nullptr;
    int r = SaveEXRImageToFile(&image, &header, path.c_str(), &e);
    if (r != TINYEXR_SUCCESS) {
        if (err) *err = e ? e : "SaveEXRImageToFile failed";
        if (e) FreeEXRErrorMessage(e);
        return false;
    }
    return true;
}

bool readExrRGBA(const std::string& path, std::vector<float>& rgba, int& w, int& h,
                 std::string* err) {
    float* out = nullptr;
    const char* e = nullptr;
    int r = LoadEXR(&out, &w, &h, path.c_str(), &e);
    if (r != TINYEXR_SUCCESS) {
        if (err) *err = e ? e : "LoadEXR failed";
        if (e) FreeEXRErrorMessage(e);
        return false;
    }
    rgba.assign(out, out + (size_t)w * h * 4);
    std::free(out);
    return true;
}

} // namespace ck
