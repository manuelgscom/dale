#pragma once
// Self-contained incremental SHA-256 (FIPS 180-4).

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace dale::fetch {

class Sha256 {
public:
    Sha256();
    void update(const void* data, std::size_t size);
    void update(std::string_view data) { update(data.data(), data.size()); }
    // Lowercase hex. The object must not be reused after final().
    std::string final();

private:
    std::array<std::uint32_t, 8> state_{};
    std::array<unsigned char, 64> block_{};
    std::size_t block_len_ = 0;
    std::uint64_t total_bytes_ = 0;
};

std::string sha256_hex(std::string_view data);

// Reads the file in 1 MiB chunks. nullopt if it cannot be read.
std::optional<std::string> sha256_file(const std::string& path);

}  // namespace dale::fetch
