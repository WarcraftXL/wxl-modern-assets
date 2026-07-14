#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace wxl::modern::assets::m2::equipment
{
    void FixObjectSkinTextureTypes(std::string_view name, std::vector<uint8_t>& model);
    bool ApplyHelmOffset(std::string_view name, std::vector<uint8_t>& model);
}
