#include "infer/parser.hpp"

#include <array>
#include <nlohmann/json.hpp>

namespace dale::infer {

namespace {

using nlohmann::json;

constexpr std::size_t kMaxCommandBytes = 1000;
constexpr std::size_t kMaxExplanationBytes = 600;

std::string_view trim_ascii(std::string_view s) {
    constexpr std::string_view ws = " \t\n\r\f\v";
    const auto b = s.find_first_not_of(ws);
    if (b == std::string_view::npos) return {};
    return s.substr(b, s.find_last_not_of(ws) - b + 1);
}

// C0 controls, DEL and C1 (U+0080–U+009F, C2 80–C2 9F in UTF-8). JSON already guarantees UTF-8.
bool has_control(std::string_view s) {
    for (std::size_t i = 0; i < s.size(); ++i) {
        const auto c = static_cast<unsigned char>(s[i]);
        if (c < 0x20 || c == 0x7F) return true;
        if (c == 0xC2 && i + 1 < s.size()) {
            const auto n = static_cast<unsigned char>(s[i + 1]);
            if (n >= 0x80 && n <= 0x9F) return true;
        }
    }
    return false;
}

ParseResult fail(std::string error, std::string detail) {
    ParseResult r;
    r.error = std::move(error);
    r.detail = std::move(detail);
    return r;
}

ParseResult schema(std::string detail) { return fail("invalid_schema", std::move(detail)); }

}  // namespace

ParseResult parse_response(std::string_view raw) {
    const std::string_view text = trim_ascii(raw);
    if (text.empty()) return fail("invalid_json", "empty output");
    if (text.front() != '{') return fail("invalid_json", "output does not start with '{'");

    // Duplicate keys in the top-level object: nlohmann silently keeps the last one.
    std::string duplicate;
    std::array<bool, 3> seen{};
    std::string extra;
    static constexpr std::array<std::string_view, 3> kKeys = {"type", "command", "explanation"};
    const json::parser_callback_t cb = [&](int depth, json::parse_event_t event, json& parsed) {
        if (depth == 1 && event == json::parse_event_t::key) {
            const auto& key = parsed.get_ref<const std::string&>();
            bool known = false;
            for (std::size_t k = 0; k < kKeys.size(); ++k) {
                if (key == kKeys[k]) {
                    known = true;
                    if (seen[k] && duplicate.empty()) duplicate = key;
                    seen[k] = true;
                }
            }
            if (!known && extra.empty()) extra = key;
        }
        return true;
    };

    json doc;
    try {
        doc = json::parse(text.begin(), text.end(), cb);
    } catch (const json::parse_error& e) {
        return fail("invalid_json", "malformed JSON at byte " + std::to_string(e.byte));
    } catch (const json::exception& e) {
        // e.g. out_of_range 406 on number overflow (1e999): must not abort dale.
        return fail("invalid_json", std::string("malformed JSON: ") + e.what());
    }
    if (!doc.is_object()) return fail("invalid_json", "top level is not an object");

    if (!duplicate.empty()) return schema("duplicate key: " + duplicate);
    if (!extra.empty()) return schema("unexpected key: " + extra);
    for (std::size_t k = 0; k < kKeys.size(); ++k) {
        if (!seen[k]) return schema("missing key: " + std::string(kKeys[k]));
    }
    for (const auto key : kKeys) {
        if (!doc[std::string(key)].is_string()) return schema("key is not a string: " + std::string(key));
    }

    const auto& type = doc["type"].get_ref<const std::string&>();
    const auto& command = doc["command"].get_ref<const std::string&>();
    const auto& explanation = doc["explanation"].get_ref<const std::string&>();

    Suggestion s;
    if (type == "command") {
        s.kind = Suggestion::Kind::Command;
        if (trim_ascii(command).empty()) return schema("empty command");
        if (command.size() > kMaxCommandBytes) return schema("command too long");
        if (has_control(command)) return schema("control character in command");
        s.command = std::string(trim_ascii(command));
    } else if (type == "refuse" || type == "unsure") {
        s.kind = type == "refuse" ? Suggestion::Kind::Refuse : Suggestion::Kind::Unsure;
        if (!command.empty()) return schema("command must be empty for type " + type);
    } else {
        return schema("unknown type");
    }

    if (trim_ascii(explanation).empty()) return schema("empty explanation");
    if (explanation.size() > kMaxExplanationBytes) return schema("explanation too long");
    if (has_control(explanation)) return schema("control character in explanation");
    s.explanation = std::string(trim_ascii(explanation));

    ParseResult r;
    r.suggestion = std::move(s);
    return r;
}

}  // namespace dale::infer
