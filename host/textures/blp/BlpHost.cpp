// Host face for blp: serves an uncompressed-BGRA texture as a DXT5 texture the Client reads.
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

#include "Host.hpp"

#include "core/Logger.hpp"

#include "../../../shared/textures/blp/BlpTranscode.hpp"

#include <cctype>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

// Fires on every .blp open. A texture in the uncompressed-BGRA encoding (3) the Client cannot decode is
// re-encoded to DXT5; every other texture is served raw (the transcode declines fast after the header).
namespace
{
    bool EndsWithCI(std::string_view s, std::string_view suffix)
    {
        if (suffix.size() > s.size()) return false;
        const size_t off = s.size() - suffix.size();
        for (size_t i = 0; i < suffix.size(); ++i)
            if (std::tolower(static_cast<unsigned char>(s[off + i])) !=
                std::tolower(static_cast<unsigned char>(suffix[i]))) return false;
        return true;
    }

    // Larger edge a served texture is capped to, by content class. Terrain tilesets serve at native
    // 2048 (the client widens its boot-time mip scratch to fit 2048-wide chains, WidenMipScratch;
    // the visible tileset set is small so the extra footprint is bounded). Every other texture keeps
    // the 1024 cap: modern model/WMO art is the main consumer of the client's tight 32-bit address
    // space, and serving it all at 2048 starves the managed texture pool (allocation failures render
    // the client's solid default texture).
    constexpr uint32_t kMaxTilesetEdge = 2048;
    constexpr uint32_t kMaxTextureEdge = 1024;

    bool IsTilesetPath(std::string_view name)
    {
        static constexpr std::string_view prefix = "tileset";
        if (name.size() < prefix.size() + 1) return false;
        for (size_t i = 0; i < prefix.size(); ++i)
            if (std::tolower(static_cast<unsigned char>(name[i])) != prefix[i]) return false;
        return name[prefix.size()] == '\\' || name[prefix.size()] == '/';
    }

    bool TransformBlp(std::string_view name, std::span<const uint8_t> raw, std::vector<uint8_t>& out)
    {
        if (!EndsWithCI(name, ".blp")) return false;

        // Cap oversized textures first (drops the top mip level), then transcode if the encoding needs it.
        // Tileset side maps (_h height, _s specular) stay at 1024: the consumers read one scalar channel,
        // and every extra 2048 chain multiplies the boot-time streaming volume and pool pressure.
        const bool sideMap = EndsWithCI(name, "_h.blp") || EndsWithCI(name, "_s.blp");
        const uint32_t maxEdge = (IsTilesetPath(name) && !sideMap) ? kMaxTilesetEdge : kMaxTextureEdge;
        std::vector<uint8_t> capped;
        std::span<const uint8_t> src = raw;
        const bool didCap = wxl::modern::assets::textures::blp::CapBlpMips(raw, capped, maxEdge);
        if (didCap) src = capped;

        std::vector<uint8_t> transcoded;
        if (wxl::modern::assets::textures::blp::TranscodeBlp(src, transcoded))
        {
            out = std::move(transcoded);
            wxl::core::log::Printf("modern-blp: %.*s %sBGRA->DXT5 (%u -> %u bytes)",
                int(name.size()), name.data(), didCap ? "capped+" : "",
                uint32_t(raw.size()), uint32_t(out.size()));
            return true;
        }
        if (didCap)
        {
            out = std::move(capped);
            wxl::core::log::Printf("modern-blp: %.*s capped to %u (%u -> %u bytes)",
                int(name.size()), name.data(), kMaxTextureEdge, uint32_t(raw.size()), uint32_t(out.size()));
            return true;
        }
        return false;
    }

    struct Registrar
    {
        Registrar() { wxl::host::RegisterTransform("modern-blp", &TransformBlp); }
    } g_registrar;
}
