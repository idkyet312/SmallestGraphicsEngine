#include "TerrainStampBake.h"

#include <cstdio>
#include <cstring>

namespace {

uint32_t Crc32(const unsigned char* data, size_t length, uint32_t crc = 0) {
    static uint32_t table[256];
    static bool built = false;
    if (!built) {
        for (uint32_t n = 0; n < 256; ++n) {
            uint32_t c = n;
            for (int k = 0; k < 8; ++k)
                c = (c & 1u) ? (0xedb88320u ^ (c >> 1)) : (c >> 1);
            table[n] = c;
        }
        built = true;
    }
    crc = crc ^ 0xffffffffu;
    for (size_t i = 0; i < length; ++i)
        crc = table[(crc ^ data[i]) & 0xffu] ^ (crc >> 8);
    return crc ^ 0xffffffffu;
}

void PushBE32(std::vector<unsigned char>& out, uint32_t value) {
    out.push_back(static_cast<unsigned char>((value >> 24) & 0xffu));
    out.push_back(static_cast<unsigned char>((value >> 16) & 0xffu));
    out.push_back(static_cast<unsigned char>((value >> 8) & 0xffu));
    out.push_back(static_cast<unsigned char>(value & 0xffu));
}

void PushChunk(std::vector<unsigned char>& out, const char tag[4],
               const unsigned char* data, size_t length) {
    PushBE32(out, static_cast<uint32_t>(length));
    const size_t crcStart = out.size();
    out.insert(out.end(), tag, tag + 4);
    if (length) out.insert(out.end(), data, data + length);
    const uint32_t crc = Crc32(out.data() + crcStart, out.size() - crcStart);
    PushBE32(out, crc);
}

}  // namespace

bool WriteGray16PNG(const std::filesystem::path& path,
                    const std::vector<uint16_t>& gray,
                    uint32_t width, uint32_t height,
                    unsigned char* (*compress)(unsigned char*, int, int*, int)) {
    if (!compress || width == 0 || height == 0) return false;
    if (gray.size() != static_cast<size_t>(width) * height) return false;

    // Raw scanlines: one filter byte (0 = None) then big-endian 16-bit samples.
    std::vector<unsigned char> raw;
    raw.reserve(static_cast<size_t>(height) * (1 + static_cast<size_t>(width) * 2));
    for (uint32_t y = 0; y < height; ++y) {
        raw.push_back(0);
        const uint16_t* row = gray.data() + static_cast<size_t>(y) * width;
        for (uint32_t x = 0; x < width; ++x) {
            raw.push_back(static_cast<unsigned char>(row[x] >> 8));
            raw.push_back(static_cast<unsigned char>(row[x] & 0xffu));
        }
    }

    int compressedLength = 0;
    unsigned char* compressed = compress(
        raw.data(), static_cast<int>(raw.size()), &compressedLength, 8);
    if (!compressed || compressedLength <= 0) return false;

    std::vector<unsigned char> png;
    const unsigned char signature[8] = { 137, 'P', 'N', 'G', 13, 10, 26, 10 };
    png.insert(png.end(), signature, signature + 8);

    unsigned char ihdr[13];
    ihdr[0] = static_cast<unsigned char>((width >> 24) & 0xffu);
    ihdr[1] = static_cast<unsigned char>((width >> 16) & 0xffu);
    ihdr[2] = static_cast<unsigned char>((width >> 8) & 0xffu);
    ihdr[3] = static_cast<unsigned char>(width & 0xffu);
    ihdr[4] = static_cast<unsigned char>((height >> 24) & 0xffu);
    ihdr[5] = static_cast<unsigned char>((height >> 16) & 0xffu);
    ihdr[6] = static_cast<unsigned char>((height >> 8) & 0xffu);
    ihdr[7] = static_cast<unsigned char>(height & 0xffu);
    ihdr[8] = 16;   // bit depth
    ihdr[9] = 0;    // colour type 0 = grayscale
    ihdr[10] = 0;   // deflate
    ihdr[11] = 0;   // adaptive filtering
    ihdr[12] = 0;   // no interlace
    PushChunk(png, "IHDR", ihdr, sizeof(ihdr));
    PushChunk(png, "IDAT", compressed, static_cast<size_t>(compressedLength));
    PushChunk(png, "IEND", nullptr, 0);

    free(compressed);

    FILE* file = nullptr;
    if (fopen_s(&file, path.string().c_str(), "wb") != 0 || !file) return false;
    const size_t written = fwrite(png.data(), 1, png.size(), file);
    fclose(file);
    return written == png.size();
}
