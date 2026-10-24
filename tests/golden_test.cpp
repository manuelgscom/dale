#include "doctest.h"

#include "infer/prompt.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using dale::infer::build_messages;
using dale::infer::Message;
using dale::infer::normalize_request;
using dale::infer::render_prompt;
using dale::infer::render_qwen35;

namespace {

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE_MESSAGE(in.good(), "no se puede leer " << path);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

nlohmann::json load_cases() { return nlohmann::json::parse(read_file(DALE_GOLDEN_DIR "/cases.json")); }

std::vector<Message> to_messages(const nlohmann::json& arr) {
    std::vector<Message> out;
    for (const auto& m : arr) out.push_back({m.at("role").get<std::string>(), m.at("content").get<std::string>()});
    return out;
}

}  // namespace

TEST_SUITE("infer") {

TEST_CASE("dorado: render_qwen35 coincide byte a byte con apply_chat_template") {
    const auto cases = load_cases();
    REQUIRE(cases.size() >= 6);
    for (const auto& c : cases) {
        const std::string id = c.at("id").get<std::string>();
        CAPTURE(id);
        const std::string golden = read_file(DALE_GOLDEN_DIR "/prompt_" + id + ".txt");
        CHECK(render_qwen35(to_messages(c.at("messages"))) == golden);
    }
}

TEST_CASE("dorado: build_messages devuelve los mensajes de los casos reales") {
    int checked = 0;
    for (const auto& c : load_cases()) {
        if (!c.contains("request")) continue;
        const std::string id = c.at("id").get<std::string>();
        CAPTURE(id);
        const std::string context = c.at("context").get<std::string>();
        const std::string request = c.at("request").get<std::string>();
        CHECK(normalize_request(request) == request);
        CHECK(build_messages(context, request) == to_messages(c.at("messages")));
        CHECK(render_prompt(context, request) == read_file(DALE_GOLDEN_DIR "/prompt_" + id + ".txt"));
        ++checked;
    }
    CHECK(checked >= 3);
}

TEST_CASE("dorado: casos obligatorios y petición de 1000 bytes") {
    std::vector<std::string> ids;
    for (const auto& c : load_cases()) {
        ids.push_back(c.at("id").get<std::string>());
        if (ids.back() == "larga") CHECK(c.at("request").get<std::string>().size() == dale::infer::kMaxRequestBytes);
        if (ids.back() == "real_v2_es" || ids.back() == "real_v2_en") {
            // The context is the smoke set one.
            std::string ctx = read_file(DALE_TESTS_DIR "/smoke/contexto.txt");
            REQUIRE(!ctx.empty());
            CHECK(ctx.back() == '\n');
            ctx.pop_back();
            CHECK(c.at("context").get<std::string>() == ctx);
        }
    }
    for (const char* need : {"real_v2_es", "real_v2_en", "unicode", "espacios", "tools_none", "larga"}) {
        CAPTURE(need);
        CHECK(std::find(ids.begin(), ids.end(), need) != ids.end());
    }
}

TEST_CASE("prompt: contrato v2, solo system prompt y petición") {
    const auto m = build_messages("os=linux distro=arch pm=pacman tools=none", "list files");
    REQUIRE(m.size() == 2);
    CHECK(m[0].role == "system");
    CHECK(!m[0].content.empty());
    CHECK(m[1] == Message{"user", "os=linux distro=arch pm=pacman tools=none\nlist files"});
    CHECK(dale::infer::kContractVersion == 2);
}

TEST_CASE("prompt: normalize_request") {
    CHECK(normalize_request("") == "");
    CHECK(normalize_request(" \n\t ") == "");
    CHECK(normalize_request("  lista\r\nlos   ficheros\t") == "lista los ficheros");
    CHECK(normalize_request("a  b　c d e\u0085f\x1c" "g") == "a b c d e f g");
    CHECK(normalize_request("   x ") == "x");
    // Not whitespace for Python: kept.
    CHECK(normalize_request("a​b­") == "a​b­");
    CHECK(normalize_request("ñandú  «año»") == "ñandú «año»");
    // Invalid UTF-8: bytes are copied as is.
    CHECK(normalize_request(" \xFF \xC3 ") == "\xFF \xC3");
}

TEST_CASE("prompt: render_qwen35 recorta y pinta los turnos del asistente") {
    const std::vector<Message> m = {
        {"system", " S\n"}, {"user", " u1 "}, {"assistant", " a1\n"}, {"user", "u2"}};
    CHECK(render_qwen35(m) ==
          "<|im_start|>system\nS<|im_end|>\n<|im_start|>user\nu1<|im_end|>\n"
          "<|im_start|>assistant\na1<|im_end|>\n<|im_start|>user\nu2<|im_end|>\n"
          "<|im_start|>assistant\n<think>\n\n</think>\n\n");
}

}  // TEST_SUITE
