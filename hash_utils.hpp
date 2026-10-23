#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace sentrix
{

    std::uint32_t fnv1a(std::string_view input) noexcept;
    std::uint32_t murmur3_32(std::string_view input, std::uint32_t seed = 0x9747b28cU) noexcept;
    std::uint32_t hash_with_seed(std::string_view input, std::uint32_t seed) noexcept;

} // namespace sentrix