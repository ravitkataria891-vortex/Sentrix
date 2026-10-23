#include "hash_utils.hpp"

namespace sentrix {
namespace {

std::uint32_t rotate_left(std::uint32_t value, int count) noexcept {
    return (value << count) | (value >> (32 - count));
}

std::uint32_t load_le_32(const unsigned char* data) noexcept {
    return static_cast<std::uint32_t>(data[0]) |
           (static_cast<std::uint32_t>(data[1]) << 8U) |
           (static_cast<std::uint32_t>(data[2]) << 16U) |
           (static_cast<std::uint32_t>(data[3]) << 24U);
}

} // namespace

std::uint32_t fnv1a(std::string_view input) noexcept {
    std::uint32_t hash = 2166136261U;
    for (unsigned char byte : input) {
        hash ^= byte;
        hash *= 16777619U;
    }
    return hash;
}

std::uint32_t murmur3_32(std::string_view input, std::uint32_t seed) noexcept {
    constexpr std::uint32_t c1 = 0xcc9e2d51U;
    constexpr std::uint32_t c2 = 0x1b873593U;

    const auto* data = reinterpret_cast<const unsigned char*>(input.data());
    const std::size_t length = input.size();
    const std::size_t blocks = length / 4U;
    std::uint32_t hash = seed;

    for (std::size_t i = 0; i < blocks; ++i) {
        std::uint32_t k = load_le_32(data + i * 4U);
        k *= c1;
        k = rotate_left(k, 15);
        k *= c2;

        hash ^= k;
        hash = rotate_left(hash, 13);
        hash = hash * 5U + 0xe6546b64U;
    }

    const unsigned char* tail = data + blocks * 4U;
    std::uint32_t k1 = 0;
    switch (length & 3U) {
        case 3: k1 ^= static_cast<std::uint32_t>(tail[2]) << 16U; [[fallthrough]];
        case 2: k1 ^= static_cast<std::uint32_t>(tail[1]) << 8U; [[fallthrough]];
        case 1:
            k1 ^= static_cast<std::uint32_t>(tail[0]);
            k1 *= c1;
            k1 = rotate_left(k1, 15);
            k1 *= c2;
            hash ^= k1;
            break;
        default: break;
    }

    hash ^= static_cast<std::uint32_t>(length);
    hash ^= hash >> 16U;
    hash *= 0x85ebca6bU;
    hash ^= hash >> 13U;
    hash *= 0xc2b2ae35U;
    hash ^= hash >> 16U;
    return hash;
}

std::uint32_t hash_with_seed(std::string_view input, std::uint32_t seed) noexcept {
    return murmur3_32(input, seed);
}

} // namespace sentrix