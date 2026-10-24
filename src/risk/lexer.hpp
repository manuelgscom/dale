#pragma once
// The checker's own shell lexer and parser. Turns a string into a flat list
// of simple commands; groupings are flattened and nested code ($(..), `..`,
// <(..), >(..)) is kept as text to be classified recursively later.

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace dale::risk {

inline constexpr std::size_t kMaxInput = 8192;  // over 8 KiB: not analyzed
inline constexpr int kMaxDepth = 8;             // allowed nesting levels

struct Word {
    std::string text;               // unquoted; expansions keep their text ($X, $(..))
    std::string raw;                // source text as is
    bool dynamic = false;           // $VAR, ${..}, $(..), `..`, $((..)), <(..) or >(..)
    bool glob = false;              // unquoted * ? [..]
    bool quoted = false;            // some part quoted or escaped
    bool proc_sub = false;          // contains <(..) or >(..)
    bool net = false;               // its nested code uses curl/wget/… (set by the classifier)
    bool tilde = true;              // the leading ~ is expanded (not quoted)
    std::vector<std::string> alts;  // result of brace expansion ({a,b}), if any
    int depth = 0;                  // nesting level where it appears
    std::vector<std::string> subs;  // code of $(..), `..`, <(..) and >(..), in order
};

struct Redir {
    int fd = -1;                // explicit descriptor (2> → 2); -1 if none
    std::string op;             // > >> >| < <> &> &>> >& <& << <<- <<<
    Word target;                // target; in a heredoc, the delimiter
    std::string body;           // heredoc body
    bool body_expands = false;  // heredoc with an unquoted delimiter
};

struct SimpleCommand {
    std::vector<Word> assigns;          // prefix assignments (VAR=x)
    std::vector<Word> words;            // name and arguments
    std::vector<Redir> redirs;
    std::vector<std::size_t> upstream;  // upstream commands in the same pipeline
    bool in_pipe = false;
    bool background = false;
    int depth = 0;
};

struct FunctionDef {
    std::string name;
    std::size_t begin = 0, end = 0;  // range in commands of the body
};

struct Script {
    std::vector<SimpleCommand> commands;
    std::vector<FunctionDef> functions;
    std::vector<Word> loose;          // words that are not commands (for header, case…)
    std::vector<std::string> errors;  // non-empty: could not be fully parsed
    bool has_case = false;
};

// depth: nesting level of the text itself (0 for the top-level command).
Script parse(std::string_view text, int depth = 0);

}  // namespace dale::risk
