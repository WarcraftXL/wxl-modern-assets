// BLP transcode: re-encode an uncompressed-BGRA (encoding 3) image to DXT5 the Client decodes.
// Copyright (C) 2026 WarcraftXL
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

#include "BlpTranscode.hpp"

#include "Dxt.hpp"

namespace wxl::modern::assets::textures::blp
{
    namespace
    {
        constexpr uint32_t kMagicBlp2 = 0x32504C42; // 'BLP2'

        uint32_t rd32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24); }
        void     wr32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (i * 8)); }
        uint32_t mx(uint32_t a, uint32_t b) { return a > b ? a : b; }

        // BLP2 header field offsets.
        constexpr uint32_t kEncoding    = 0x08; // u8: 1 palettized, 2 DXT, 3 uncompressed BGRA
        constexpr uint32_t kAlphaDepth  = 0x09; // u8
        constexpr uint32_t kAlphaType   = 0x0A; // u8: 0 DXT1, 1 DXT3, 7 DXT5
        constexpr uint32_t kWidth       = 0x0C;
        constexpr uint32_t kHeight      = 0x10;
        constexpr uint32_t kMipOffsets  = 0x14; // u32[16]
        constexpr uint32_t kMipSizes    = 0x54; // u32[16]
    }

    bool CapBlpMips(std::span<const uint8_t> in, std::vector<uint8_t>& out, uint32_t maxEdge)
    {
        const uint8_t* b = in.data();
        const uint32_t n = static_cast<uint32_t>(in.size());
        if (n < 0x94 || rd32(b) != kMagicBlp2) return false;

        const uint32_t width = rd32(b + kWidth), height = rd32(b + kHeight);
        if (width == 0 || height == 0) return false;
        const uint32_t maxdim = mx(width, height);
        if (maxdim <= maxEdge) return false; // already within budget

        uint32_t drop = 0;
        while ((maxdim >> drop) > maxEdge) ++drop; // halvings until the larger edge fits

        uint32_t mipOff[16], mipSize[16];
        for (int i = 0; i < 16; ++i) { mipOff[i] = rd32(b + kMipOffsets + i * 4); mipSize[i] = rd32(b + kMipSizes + i * 4); }
        // The target level must exist in the chain, and the data region must be in range.
        if (drop >= 16 || mipSize[drop] == 0 || mipOff[drop] == 0) return false;
        if (mipOff[0] == 0 || mipOff[0] > n) return false;

        // Header (and palette for the palettized encoding) live before the first mip; keep them verbatim.
        const uint32_t dataStart = mipOff[0];
        out.assign(b, b + dataStart);

        uint32_t newOff[16] = {}, newSize[16] = {};
        for (uint32_t i = drop, j = 0; i < 16; ++i, ++j)
        {
            if (mipSize[i] == 0 || mipOff[i] == 0) break;
            if (mipOff[i] + mipSize[i] > n) break; // truncated source level
            newOff[j]  = static_cast<uint32_t>(out.size());
            out.insert(out.end(), b + mipOff[i], b + mipOff[i] + mipSize[i]);
            newSize[j] = mipSize[i];
        }

        wr32(out.data() + kWidth,  width  >> drop);
        wr32(out.data() + kHeight, height >> drop);
        for (int i = 0; i < 16; ++i) { wr32(out.data() + kMipOffsets + i * 4, newOff[i]); wr32(out.data() + kMipSizes + i * 4, newSize[i]); }
        return true;
    }

    bool TranscodeBlp(std::span<const uint8_t> in, std::vector<uint8_t>& out)
    {
        const uint8_t* b = in.data();
        const uint32_t n = static_cast<uint32_t>(in.size());
        if (n < 0x94 || rd32(b) != kMagicBlp2 || rd32(b + 4) != 1) return false; // not a BLP image
        if (b[kEncoding] != 3) return false;                                      // only uncompressed BGRA

        const uint32_t width = rd32(b + kWidth), height = rd32(b + kHeight);
        const uint8_t  srcAlpha = b[kAlphaDepth];
        if (width == 0 || height == 0) return false;

        uint32_t mipOff[16], mipSize[16];
        for (int i = 0; i < 16; ++i) { mipOff[i] = rd32(b + kMipOffsets + i * 4); mipSize[i] = rd32(b + kMipSizes + i * 4); }
        if (mipSize[0] == 0 || mipOff[0] == 0 || mipOff[0] > n) return false;

        // Bytes per source pixel from mip 0 (encoding 3 is normally 4-byte BGRA; tolerate 3-byte BGR).
        const uint64_t px0 = uint64_t(width) * height;
        const uint32_t bpp = static_cast<uint32_t>(mipSize[0] / px0);
        if (bpp != 3 && bpp != 4) return false;

        // Keep the source header + palette region verbatim, retag it as DXT5, then append the re-encoded mips.
        out.assign(b, b + mipOff[0]);
        out[kEncoding]   = 2; // DXT
        out[kAlphaDepth] = 8;
        out[kAlphaType]  = 7; // DXT5

        uint32_t newOff[16] = {}, newSize[16] = {};
        for (int i = 0; i < 16; ++i)
        {
            if (mipSize[i] == 0 || mipOff[i] == 0) continue;
            const uint32_t mw = mx(1u, width >> i), mh = mx(1u, height >> i);
            if (mipOff[i] + size_t(mw) * mh * bpp > n) break; // truncated source mip
            const uint8_t* src = b + mipOff[i];

            newOff[i] = static_cast<uint32_t>(out.size());
            const uint32_t bx = (mw + 3) / 4, by = (mh + 3) / 4;
            for (uint32_t ty = 0; ty < by; ++ty)
                for (uint32_t tx = 0; tx < bx; ++tx)
                {
                    uint8_t block[64];
                    for (uint32_t py = 0; py < 4; ++py)
                        for (uint32_t pxi = 0; pxi < 4; ++pxi)
                        {
                            const uint32_t sx = (tx * 4 + pxi < mw) ? tx * 4 + pxi : mw - 1; // clamp edge
                            const uint32_t sy = (ty * 4 + py  < mh) ? ty * 4 + py  : mh - 1;
                            const uint8_t* p = src + (size_t(sy) * mw + sx) * bpp;
                            uint8_t* d = block + (py * 4 + pxi) * 4;
                            d[0] = p[2]; d[1] = p[1]; d[2] = p[0];               // BGR -> RGB
                            d[3] = (bpp == 4 && srcAlpha != 0) ? p[3] : 255;     // opaque when no source alpha
                        }
                    uint8_t enc[16];
                    dxt::CompressBlock(block, enc);
                    out.insert(out.end(), enc, enc + 16);
                }
            newSize[i] = static_cast<uint32_t>(out.size()) - newOff[i];
        }

        for (int i = 0; i < 16; ++i)
        {
            wr32(out.data() + kMipOffsets + i * 4, newOff[i]);
            wr32(out.data() + kMipSizes + i * 4, newSize[i]);
        }
        return true;
    }
}
