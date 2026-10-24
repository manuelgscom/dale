#pragma once
// Internal engine of the checker: walks the simple commands and applies the policy,
// the rules (rules.hpp) and recursive unwrapping (unwrap.cpp).

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "risk/lexer.hpp"
#include "risk/risk.hpp"

namespace dale::risk {

// Where a command reads its standard input from.
struct Stdin {
    enum class Kind { None, Pipe, Text, File };
    Kind kind = Kind::None;
    bool net = false;         // curl/wget/fetch/nc upstream
    bool text_known = false;  // Pipe from a static echo/printf/cat
    std::string text;         // heredoc, here-string or echo/printf/cat text
    std::optional<std::vector<std::string>> items;  // known words of the input (for xargs)
    std::string raw;  // literal input text with expansions left as written ("" if unknown)
};

// What a command leaves in the pipe, if known without running it.
struct Produced {
    std::optional<std::string> text;                 // static echo/printf/cat
    std::optional<std::vector<std::string>> items;   // words (echo…) or find starting points
    std::optional<std::string> raw;                  // echo/printf/cat text, expansions left as written
};

class Engine {
public:
    explicit Engine(Assessment& out) : out_(out) {}

    // Classifies a piece of code. def: input for commands that have no other.
    void code(std::string_view text, int depth, const Stdin& def);

    void emit(std::string_view id, std::string_view fragment, std::string_view why_es = {},
              std::string_view why_en = {});

private:
    using Range = std::pair<std::size_t, std::size_t>;

    void command(const Script& sc, std::size_t k, const Stdin& def, std::vector<Range>& ranges,
                 std::vector<Produced>& produced);
    void nested(Word& w);
    void redirect(const Redir& r);
    void write_target(const Word& t, bool append, std::string_view fragment);
    void env_assign(const Word& w);
    void argv(const std::vector<Word>& w, const Stdin& in, int depth);
    void rules(const std::vector<Word>& w, int depth, const Stdin& in = Stdin{});

    // unwrap.cpp: returns true if the command was a wrapper or interpreter and has been handled.
    bool unwrap(const std::string& name, const std::vector<Word>& w, const Stdin& in, int depth);
    void code_word(const Word& cw, int depth, const Stdin& in, std::string_view fragment);
    void script_word(const Word& sw, std::string_view fragment);
    void stdin_code(const Stdin& in, bool shell, int depth, std::string_view fragment);
    void stdin_shell(const Stdin& in, int depth, std::string_view fragment);  // only if there is input

    friend struct Unwrapper;  // wrappers in unwrap.cpp

    Assessment& out_;
    std::vector<std::string> names_;  // commands seen, in order (to know what is upstream)
    int admin_ = 0;                   // > 0 under sudo, doas, su…
};

std::string fragment_of(const std::vector<Word>& w, std::size_t from = 0);
std::vector<std::string> split_words(std::string_view text);
bool is_assign_word(const Word& w);
Word make_word(std::string text, bool dynamic = false);

}  // namespace dale::risk
