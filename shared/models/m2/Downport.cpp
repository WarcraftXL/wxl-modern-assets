// Reshapes a source MD20 image onto the client M2 contract before the native parser reads it.
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

#include "Downport.hpp"

#include "core/Logger.hpp"
#include "structure/m2/M2Format.hpp"

#include "Animations.hpp"
#include "Cameras.hpp"
#include "Contract.hpp"
#include "Particles.hpp"
#include "Ribbons.hpp"
#include "Textures.hpp"

#include <cctype>
#include <string_view>

namespace wxl::modern::assets::m2::downport
{
    namespace fmt = wxl::structure::m2;

    namespace
    {
        bool StartsWithCI(std::string_view value, std::string_view prefix)
        {
            if (prefix.size() > value.size()) return false;
            for (size_t i = 0; i < prefix.size(); ++i)
            {
                const char aRaw = value[i] == '/' ? '\\' : value[i];
                const char bRaw = prefix[i] == '/' ? '\\' : prefix[i];
                const auto a = static_cast<unsigned char>(aRaw);
                const auto b = static_cast<unsigned char>(bRaw);
                if (std::tolower(a) != std::tolower(b)) return false;
            }
            return true;
        }

        bool RepairsTextureLoops(std::string_view name)
        {
            return StartsWithCI(name, "item\\") ||
                   StartsWithCI(name, "creature\\") ||
                   StartsWithCI(name, "character\\");
        }
    }

    /**
     * @brief Reports whether a buffer is an MD20 image in the source version range this module reshapes.
     * @param buffer  Model bytes.
     * @param size    Buffer length.
     * @return True if the image is in the source range.
     */
    bool IsConvertible(const void* buffer, uint32_t size)
    {
        if (!buffer || size < sizeof(fmt::M2Header)) return false;
        const auto* md = static_cast<const fmt::M2Header*>(buffer);
        if (md->magic != fmt::kMagicMD20) return false; // chunked container or not an .m2
        return md->version >= kSourceVersionMin && md->version <= kSourceVersionMax;
    }

    /**
     * @brief Logs a model's header (version, flags, counts).
     * @param buffer  Model bytes.
     * @param size    Buffer length.
     */
    void Inspect(const void* buffer, uint32_t size)
    {
        if (!buffer || size < sizeof(fmt::M2Header)) return;
        const auto* md = static_cast<const fmt::M2Header*>(buffer);
        if (md->magic != fmt::kMagicMD20)
        {
            const uint8_t* p = static_cast<const uint8_t*>(buffer);
            WLOG_INFO("modern-m2: non-MD20 image (magic %02X%02X%02X%02X), skipped", p[0], p[1], p[2], p[3]);
            return;
        }
        WLOG_INFO("modern-m2: MD20 v=%u size=%u flags=0x%X | seq=%u bones=%u verts=%u skins=%u colors=%u tex=%u mat=%u cam=%u ribbon=%u particle=%u",
                  md->version, size, md->globalFlags,
                  md->sequences.count, md->bones.count, md->vertices.count, md->numSkinProfiles,
                  md->colors.count, md->textures.count, md->materials.count,
                  md->cameras.count, md->ribbonEmitters.count, md->particleEmitters.count);
    }

    /**
     * @brief Computes the byte count the reshaped image needs.
     * @param buffer  Source model bytes.
     * @param size    Source length.
     * @return Source size, plus two bytes for the synthesized texture-coordinate-combos tail when needed.
     */
    uint32_t WorkSize(const void* buffer, uint32_t size, std::string_view name)
    {
        if (!IsConvertible(buffer, size)) return size;
        const auto* md = static_cast<const fmt::M2Header*>(buffer);

        uint32_t workSize = size;
        if (textures::NeedsCoordCombos(md))
            workSize += 2;
        if (RepairsTextureLoops(name))
            workSize += textures::TextureTransformLoopRepairExtraBytes(md, size, workSize, name);
        return workSize;
    }

    /**
     * @brief Reshapes an already-copied image in place onto the client contract.
     * @param work      Work buffer holding the copied source bytes.
     * @param origSize  Length of the copied source.
     * @param workSize  Total work-buffer length.
     * @return True on success; false when work is not a source image or workSize is too small for the grow.
     */
    bool ProcessInPlace(void* work, uint32_t origSize, uint32_t workSize, std::string_view name)
    {
        if (!work || workSize < origSize) return false;

        // work holds the copied source bytes; gate on them (the version is rewritten to the client
        // value at the end, so a second pass over the same buffer is a no-op).
        auto* md = reinterpret_cast<fmt::M2Header*>(work);
        if (md->magic != fmt::kMagicMD20) return false;
        if (md->version < kSourceVersionMin || md->version > kSourceVersionMax) return false;

        const bool grow = textures::NeedsCoordCombos(md);
        uint32_t appendOffset = origSize;
        if (grow) appendOffset += 2;
        const uint32_t loopExtra = RepairsTextureLoops(name)
            ? textures::TextureTransformLoopRepairExtraBytes(md, origSize, appendOffset) : 0;
        if (workSize < appendOffset + loopExtra) return false; // caller under-sized the buffer

        // Each theme compacts within the original body size (the body keeps its byte positions).
        cameras::Compact(md);
        particles::Compact(md, origSize);
        ribbons::Compact(md, origSize);
        animations::Fix(md);

        if (grow)
            textures::SynthCoordCombos(md, origSize); // the appended entry lives at the original tail

        if (RepairsTextureLoops(name))
        {
            const uint32_t repaired = textures::RepairTextureTransformLoops(md, origSize, appendOffset, workSize, name);
            if (repaired)
                WLOG_INFO("modern-m2: '%.*s' repaired %u pre-parse texture-transform loop(s)",
                          int(name.size()), name.data(), repaired);
        }

        // Stage, do not finalize: the body now matches the client contract, but the version carries the
        // staging bit so the DLL recognizes a compacted image and finalizes it (the client accepts only the
        // exact native version, so a staged image is inert until the DLL clears the bit).
        md->version = fmt::kClientVersion | kStagedVersionBit;
        return true;
    }
}
