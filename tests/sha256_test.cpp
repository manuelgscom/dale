#include "doctest.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "fetch/sha256.hpp"

namespace fs = std::filesystem;
using dale::fetch::Sha256;
using dale::fetch::sha256_file;
using dale::fetch::sha256_hex;

namespace {

constexpr const char* kNist56 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";

struct TempDir {
    fs::path path;
    TempDir() {
        std::string tmpl = (fs::temp_directory_path() / "dale-sha256-test-XXXXXX").string();
        std::vector<char> buf(tmpl.begin(), tmpl.end());
        buf.push_back('\0');
        REQUIRE(mkdtemp(buf.data()) != nullptr);
        path = buf.data();
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

// Hash of data fed in pieces of chunk bytes.
std::string hash_in_chunks(const std::string& data, std::size_t chunk) {
    Sha256 h;
    for (std::size_t pos = 0; pos < data.size(); pos += chunk) {
        h.update(std::string_view(data).substr(pos, chunk));
    }
    return h.final();
}

std::string host_sha256sum(const fs::path& file) {
    const std::string cmd = "sha256sum '" + file.string() + "'";
    std::FILE* p = popen(cmd.c_str(), "r");
    REQUIRE(p != nullptr);
    char buf[65] = {};
    const std::size_t n = std::fread(buf, 1, 64, p);
    pclose(p);
    return std::string(buf, n);
}

}  // namespace

TEST_SUITE("fetch") {

TEST_CASE("sha256: vectores del NIST") {
    CHECK(sha256_hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(sha256_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(sha256_hex(kNist56) ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    CHECK(sha256_hex(std::string(1000000, 'a')) ==
          "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST_CASE("sha256: un millón de 'a' byte a byte") {
    Sha256 h;
    for (int i = 0; i < 1000000; ++i) h.update("a", 1);
    CHECK(h.final() == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST_CASE("sha256: troceos en los límites del bloque") {
    std::mt19937 rng(42);
    for (const std::size_t len : {55u, 56u, 63u, 64u, 65u, 119u, 120u, 128u, 1000u}) {
        std::string data(len, '\0');
        for (auto& c : data) c = static_cast<char>(rng() & 0xff);
        const std::string whole = sha256_hex(data);
        for (const std::size_t chunk : {1u, 55u, 56u, 63u, 64u, 65u}) {
            CAPTURE(len);
            CAPTURE(chunk);
            CHECK(hash_in_chunks(data, chunk) == whole);
        }
        // Two pieces split exactly at 55, 56, 63, 64 and 65.
        for (const std::size_t cut : {55u, 56u, 63u, 64u, 65u}) {
            if (cut > len) continue;
            Sha256 h;
            h.update(std::string_view(data).substr(0, cut));
            h.update(std::string_view(data).substr(cut));
            CAPTURE(len);
            CAPTURE(cut);
            CHECK(h.final() == whole);
        }
    }
    // Padding lengths: 55 bytes fit in one block, 56 need two.
    CHECK(sha256_hex(std::string(55, 'a')) ==
          "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318");
    CHECK(sha256_hex(std::string(56, 'a')) ==
          "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");
    CHECK(sha256_hex(std::string(64, 'a')) ==
          "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");
}

TEST_CASE("sha256_file: 3 MiB aleatorios igual que sha256sum") {
    TempDir dir;
    const fs::path file = dir.path / "random.bin";
    std::string data(3u << 20, '\0');
    std::mt19937 rng(7);
    for (auto& c : data) c = static_cast<char>(rng() & 0xff);
    // Plus a tail that does not fill a 1 MiB block.
    data += "cola";
    {
        std::ofstream out(file, std::ios::binary);
        out << data;
    }
    const auto ours = sha256_file(file.string());
    REQUIRE(ours.has_value());
    CHECK(*ours == host_sha256sum(file));
    CHECK(*ours == sha256_hex(data));
}

TEST_CASE("sha256_file: fichero inexistente") {
    TempDir dir;
    CHECK_FALSE(sha256_file((dir.path / "no-existe").string()).has_value());
}

}  // TEST_SUITE
