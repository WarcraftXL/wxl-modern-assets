// Host face for wmo: serves a source WMO (root or group) as Client-shaped WMO bytes.
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

#include "../../../shared/common/Chunk.hpp"
#include "../../../shared/models/wmo/WmoChunks.hpp"
#include "../../../shared/models/wmo/WmoTranslate.hpp"

#include <cctype>
#include <span>
#include <string>
#include <vector>

// A WMO ships as a root file (<name>.wmo) and N group files (<name>_NNN.wmo); the client opens each one
// on its own. This transform fires on every .wmo open, decides root vs group from the chunk that follows
// MVER (MOHD = root, MOGP = group), and down-converts the bytes the host already read. Nothing is read
// from sibling files, so no archive mount is needed. A file already Client-shaped is served unchanged.
namespace
{
    namespace mwmo = wxl::modern::assets::wmo;
    namespace iff  = wxl::modern::assets::common::iff;

    /**
     * @brief Reports whether `s` ends with `suffix`, case-insensitively.
     * @param s       string to test
     * @param suffix  suffix to match
     * @return true if `s` ends with `suffix`
     */
    bool EndsWithCI(std::string_view s, std::string_view suffix)
    {
        if (suffix.size() > s.size()) return false;
        const size_t off = s.size() - suffix.size();
        for (size_t i = 0; i < suffix.size(); ++i)
            if (std::tolower(static_cast<unsigned char>(s[off + i])) !=
                std::tolower(static_cast<unsigned char>(suffix[i]))) return false;
        return true;
    }

    // Magic of the chunk that follows MVER: MOHD for a root, MOGP for a group, 0 if unreadable.
    uint32_t SecondChunkMagic(std::span<const uint8_t> raw)
    {
        if (raw.size() < 12 || iff::Rd32(raw.data()) != mwmo::kMVER) return 0;
        const uint32_t mverLen = 8 + iff::Rd32(raw.data() + 4);
        if (mverLen + 8 > raw.size()) return 0;
        return iff::Rd32(raw.data() + mverLen);
    }

    // FileDataID -> path adapter for the WMO translate: routes to the host resolver (the DB2 path tables a
    // module registers). Cold; called once per unresolved material texture reference.
    bool ResolveThunk(void* /*user*/, uint32_t fileDataId, std::string& outPath)
    {
        return wxl::host::ResolveFdid(fileDataId, outPath);
    }

    bool TransformWmo(std::string_view name, std::span<const uint8_t> raw, std::vector<uint8_t>& out)
    {
        if (!EndsWithCI(name, ".wmo")) return false;

        const uint32_t second = SecondChunkMagic(raw);
        if (second == mwmo::kMOHD)
        {
            wxl::core::log::Printf("modern-wmo root: %.*s", int(name.size()), name.data());
            // Source WMOs name their textures directly (MOTX); a FileDataID-based source resolves through here.
            mwmo::ResolveCtx rc{ &ResolveThunk, nullptr };
            return mwmo::TranslateWmoRoot(raw, rc, out);
        }
        if (second == mwmo::kMOGP)
        {
            // Diagnostic: the exterior terrain is culled when the camera is under a WMO group that lacks the
            // EXTERIOR flag (0x8). Log each group's flags + name so a culling-under-arch report can be tied to
            // the exact WMO group and its source flags. (MOGP flags = u32 at the 0x44 header +0x08.)
            const uint32_t mverLen = 8 + iff::Rd32(raw.data() + 4);
            if (mverLen + 8 + 0x0C <= raw.size())
            {
                const uint32_t flags = iff::Rd32(raw.data() + mverLen + 8 + 0x08);
                wxl::core::log::Printf("modern-wmo grp: %.*s flags=0x%08X ext=%d int=%d",
                    int(name.size()), name.data(), flags, int((flags & 0x8) != 0), int((flags & 0x2000) != 0));
            }
            return mwmo::TranslateWmoGroup(raw, out);
        }

        return false; // not a recognizable WMO root or group
    }

    // File-scope registrar: self-registers the transform before the host serve loop starts.
    struct Registrar
    {
        Registrar() { wxl::host::RegisterTransform("modern-wmo", &TransformWmo); }
    } g_registrar;
}
