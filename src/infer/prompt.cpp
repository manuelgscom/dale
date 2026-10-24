#include "infer/prompt.hpp"

#include <cstdint>

namespace dale::infer {

namespace {

// System prompt of contract v2: the minimal prompt of the fine-tuned model, no examples.
// Any change bumps the version and regenerates tests/golden.
constexpr std::string_view kSystemPrompt =
    "Turn the request into one bash command for the Linux system described in the first line of "
    "the user message.\n"
    "Reply with only JSON {\"type\":\"command|refuse|unsure\",\"command\":\"...\",\"explanation\":\"...\"}: "
    "\"command\" is \"\" unless type is command; \"explanation\" is one short sentence in the "
    "language of the request.";

// Decodes the UTF-8 code point at s[i]. An invalid sequence counts as a single non-space
// byte.
std::uint32_t decode(std::string_view s, std::size_t i, std::size_t& len) {
    const auto b0 = static_cast<unsigned char>(s[i]);
    len = 1;
    if (b0 < 0x80) return b0;
    std::size_t n = 0;
    std::uint32_t cp = 0;
    if ((b0 & 0xE0) == 0xC0) {
        n = 2;
        cp = b0 & 0x1Fu;
    } else if ((b0 & 0xF0) == 0xE0) {
        n = 3;
        cp = b0 & 0x0Fu;
    } else if ((b0 & 0xF8) == 0xF0) {
        n = 4;
        cp = b0 & 0x07u;
    } else {
        return 0xFFFFFFFFu;
    }
    if (i + n > s.size()) return 0xFFFFFFFFu;
    for (std::size_t k = 1; k < n; ++k) {
        const auto b = static_cast<unsigned char>(s[i + k]);
        if ((b & 0xC0) != 0x80) return 0xFFFFFFFFu;
        cp = (cp << 6) | (b & 0x3Fu);
    }
    len = n;
    return cp;
}

// Same whitespace as Python's str.isspace() (and therefore as Jinja's trim).
bool is_space(std::uint32_t cp) {
    return (cp >= 0x09 && cp <= 0x0D) || (cp >= 0x1C && cp <= 0x20) || cp == 0x85 || cp == 0xA0 ||
           cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 || cp == 0x2029 ||
           cp == 0x202F || cp == 0x205F || cp == 0x3000;
}

// Equivalent to Python's str.strip().
std::string_view trim(std::string_view s) {
    std::size_t begin = s.size(), end = 0;
    for (std::size_t i = 0, len = 0; i < s.size(); i += len) {
        if (!is_space(decode(s, i, len))) {
            if (begin == s.size()) begin = i;
            end = i + len;
        }
    }
    return begin < end ? s.substr(begin, end - begin) : std::string_view{};
}

}  // namespace

std::string normalize_request(std::string_view request) {
    // Equivalent to Python's " ".join(request.split()).
    std::string out;
    out.reserve(request.size());
    bool pending_space = false;
    for (std::size_t i = 0, len = 0; i < request.size(); i += len) {
        if (is_space(decode(request, i, len))) {
            pending_space = true;
            continue;
        }
        if (pending_space && !out.empty()) out += ' ';
        pending_space = false;
        out.append(request.substr(i, len));
    }
    return out;
}

std::vector<Message> build_messages(std::string_view context_line, std::string_view request) {
    std::vector<Message> messages;
    messages.push_back({"system", std::string(kSystemPrompt)});
    std::string user(context_line);
    user += '\n';
    user += request;
    messages.push_back({"user", std::move(user)});
    return messages;
}

std::string render_qwen35(const std::vector<Message>& messages) {
    // Subset of the Qwen/Qwen3.5-0.8B chat_template.jinja without tools or images, with
    // add_generation_prompt=True and enable_thinking=False. Assistant content carries no
    // <think> blocks of its own.
    std::size_t last_query = messages.size();
    for (std::size_t i = messages.size(); i-- > 0;) {
        if (messages[i].role == "user") {
            last_query = i;
            break;
        }
    }
    std::string out;
    for (std::size_t i = 0; i < messages.size(); ++i) {
        const Message& m = messages[i];
        const std::string_view content = trim(m.content);
        if (m.role == "system") {
            if (i == 0) {
                out += "<|im_start|>system\n";
                out += content;
                out += "<|im_end|>\n";
            }
        } else if (m.role == "user") {
            out += "<|im_start|>user\n";
            out += content;
            out += "<|im_end|>\n";
        } else if (m.role == "assistant") {
            // Only turns after the last user request get an (empty) <think> block.
            out += "<|im_start|>assistant\n";
            if (last_query != messages.size() && i > last_query) out += "<think>\n\n</think>\n\n";
            out += content;
            out += "<|im_end|>\n";
        }
    }
    out += "<|im_start|>assistant\n<think>\n\n</think>\n\n";
    return out;
}

std::string render_prompt(std::string_view context_line, std::string_view request) {
    return render_qwen35(build_messages(context_line, normalize_request(request)));
}

}  // namespace dale::infer
