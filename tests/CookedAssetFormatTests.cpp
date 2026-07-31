#include "CookedAssetFormat.h"

#include <cassert>
#include <cstdint>

int main() {
    using namespace SGE::Cooked;
    Header header;
    header.headerSize = sizeof(Header);
    header.fileSize = 4096;
    header.primitiveOffset = 256;
    header.primitiveCount = 2;
    header.materialOffset = 512;
    header.materialCount = 1;
    header.textureOffset = 768;
    header.textureCount = 1;
    header.clipOffset = 1024;
    header.clipCount = 1;
    header.stringOffset = 1280;
    header.stringSize = 128;
    header.payloadOffset = 1536;
    header.payloadSize = header.fileSize - header.payloadOffset;
    assert(HeaderValid(header, 4096));

    Header badMagic = header;
    badMagic.magic = 0;
    assert(!HeaderValid(badMagic, 4096));

    Header overflow = header;
    overflow.primitiveOffset = 4080;
    overflow.primitiveCount = 2;
    assert(!HeaderValid(overflow, 4096));

    assert(RangeValid(4000, 96, 4096));
    assert(!RangeValid(4000, 97, 4096));
    assert(!ArrayValid(0, UINT64_MAX, 2, 4096));
    return 0;
}
