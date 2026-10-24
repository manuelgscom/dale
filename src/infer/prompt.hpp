#pragma once
// Prompt defined by the model contract.

#include <string>
#include <string_view>
#include <vector>

namespace dale::infer {

constexpr int kContractVersion = 2;
constexpr std::size_t kMaxRequestBytes = 1000;  // after normalize_request

struct Message {
    std::string role;  // system | user | assistant
    std::string content;
    bool operator==(const Message&) const = default;
};

// Newlines to spaces, runs of whitespace (including Unicode ones such as U+00A0) collapsed to
// one, and trimmed on both sides.
std::string normalize_request(std::string_view request);

// System prompt and the user message "<context_line>\n<request>" (contract v2, no examples).
// request already normalized.
std::vector<Message> build_messages(std::string_view context_line, std::string_view request);

// Equivalent to apply_chat_template(..., add_generation_prompt=True, enable_thinking=False).
std::string render_qwen35(const std::vector<Message>& messages);

// render_qwen35(build_messages(context_line, normalize_request(request))).
std::string render_prompt(std::string_view context_line, std::string_view request);

}  // namespace dale::infer
