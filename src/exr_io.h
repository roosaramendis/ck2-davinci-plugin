// Minimal EXR read/write via tinyexr (top-row-first, interleaved floats).
#pragma once
#include <string>
#include <vector>

namespace ck {

// channels = 1, 3 or 4. Writes half floats. compress=false is much faster
// (used for the temporary captured frames).
bool writeExr(const std::string& path, const float* data, int w, int h, int channels,
              std::string* err, bool compress = true);

// Always returns RGBA (missing channels: RGB=0, A=1; single-channel files fill RGB).
bool readExrRGBA(const std::string& path, std::vector<float>& rgba, int& w, int& h,
                 std::string* err);

} // namespace ck
