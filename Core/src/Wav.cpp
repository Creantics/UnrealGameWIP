#include "ftcore/Wav.h"

#include <cstring>

namespace ftcore {

namespace {

std::uint32_t le16(std::string_view b, std::size_t at) {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(b[at])) |
           static_cast<std::uint32_t>(static_cast<unsigned char>(b[at + 1])) << 8;
}

std::uint32_t le32(std::string_view b, std::size_t at) {
    return le16(b, at) | le16(b, at + 2) << 16;
}

bool fail(std::string* error, const char* what) {
    if (error) *error = what;
    return false;
}

constexpr std::uint32_t kFormatPcm = 1;
constexpr std::uint32_t kFormatFloat = 3;
constexpr std::uint32_t kFormatExtensible = 0xFFFE;

} // namespace

bool readWav(std::string_view bytes, WavInfo& info, std::vector<float>* samples, std::string* error) {
    info = WavInfo{};
    if (samples) samples->clear();
    if (bytes.size() < 12 || bytes.substr(0, 4) != "RIFF" || bytes.substr(8, 4) != "WAVE")
        return fail(error, "not a RIFF WAVE file");

    std::uint32_t format = 0;
    bool haveFmt = false;
    std::string_view data;
    std::size_t at = 12;
    while (at + 8 <= bytes.size()) {
        const std::string_view id = bytes.substr(at, 4);
        const std::uint32_t size = le32(bytes, at + 4);
        const std::size_t body = at + 8;
        if (size > bytes.size() - body) {
            // A truncated data chunk is common in the wild; read what is there. Anything else is broken.
            if (id != "data") return fail(error, "a chunk runs past the end of the file");
        }
        const std::string_view chunk = bytes.substr(body, size);
        if (id == "fmt ") {
            if (chunk.size() < 16) return fail(error, "fmt chunk is too short");
            format = le16(chunk, 0);
            info.channels = le16(chunk, 2);
            info.sampleRate = le32(chunk, 4);
            info.bitsPerSample = le16(chunk, 14);
            if (format == kFormatExtensible) {
                if (chunk.size() < 26) return fail(error, "extensible fmt chunk is too short");
                format = le16(chunk, 24); // first two bytes of the subformat GUID
            }
            haveFmt = true;
        } else if (id == "data") {
            data = chunk;
        } else if (id == "smpl" && chunk.size() >= 36 + 24 && le32(chunk, 28) > 0) {
            info.hasLoop = true;
            info.loopStart = le32(chunk, 36 + 8);
            info.loopEnd = le32(chunk, 36 + 12);
        }
        at = body + size + (size & 1);
    }
    if (!haveFmt) return fail(error, "no fmt chunk");
    if (data.data() == nullptr) return fail(error, "no data chunk");
    if (info.channels == 0) return fail(error, "zero channels");
    if (info.sampleRate == 0) return fail(error, "zero sample rate");

    if (format == kFormatFloat && info.bitsPerSample == 32) info.isFloat = true;
    else if (format != kFormatPcm) return fail(error, "not PCM or 32-bit float (compressed formats aren't read)");
    else if (info.bitsPerSample != 16 && info.bitsPerSample != 24 && info.bitsPerSample != 32)
        return fail(error, "PCM must be 16, 24 or 32 bit");

    const std::uint32_t bytesPerSample = info.bitsPerSample / 8;
    const std::uint64_t frameBytes = static_cast<std::uint64_t>(bytesPerSample) * info.channels;
    info.frames = data.size() / frameBytes;
    if (!samples) return true;

    const std::uint64_t count = info.frames * info.channels;
    samples->resize(static_cast<std::size_t>(count));
    const unsigned char* p = reinterpret_cast<const unsigned char*>(data.data());
    float* out = samples->data();
    for (std::uint64_t i = 0; i < count; ++i, p += bytesPerSample) {
        if (info.isFloat) {
            float v;
            std::memcpy(&v, p, 4);
            out[i] = v;
        } else if (bytesPerSample == 2) {
            const std::int16_t v = static_cast<std::int16_t>(p[0] | p[1] << 8);
            out[i] = static_cast<float>(v) / 32768.f;
        } else if (bytesPerSample == 3) {
            std::int32_t v = p[0] | p[1] << 8 | p[2] << 16;
            if (v & 0x800000) v -= 0x1000000;
            out[i] = static_cast<float>(v) / 8388608.f;
        } else {
            std::uint32_t u = static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8 |
                              static_cast<std::uint32_t>(p[2]) << 16 | static_cast<std::uint32_t>(p[3]) << 24;
            std::int32_t v;
            std::memcpy(&v, &u, 4);
            out[i] = static_cast<float>(static_cast<double>(v) / 2147483648.0);
        }
    }
    return true;
}

} // namespace ftcore
