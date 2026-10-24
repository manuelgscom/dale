#pragma once
// Escalation rule table, policy ids and read-only whitelist.

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "risk/lexer.hpp"
#include "risk/risk.hpp"

namespace dale::risk {

// View of an already unwrapped simple command, with options parsed GNU-style:
// mixed with operands, "--" ends them and combined short options are expanded (-rf → -r -f).
struct Cmd {
    std::string name;                                   // command basename
    std::vector<Word> args;                             // arguments after the name
    std::vector<std::string> flags;                     // "-r", "--recursive" (without "=value")
    std::vector<std::pair<std::string, Word>> values;   // option with a value ("-o", file)
    std::vector<Word> operands;
    std::vector<std::string> paths;                     // normalized operands
    bool admin = false;                                 // under sudo, doas, su…
    std::string input;                                  // literal stdin text (Stdin::raw), "" if unknown

    bool has(std::string_view flag) const;
    bool has_prefix(std::string_view prefix) const;
    const Word* value(std::string_view opt) const;      // nullptr if absent
    std::string op(std::size_t i) const;                // text of operand i, or ""
};

Cmd make_cmd(const std::vector<Word>& argv, bool admin);

struct Rule {
    std::string_view id;
    std::vector<std::string_view> commands;  // empty: policy or rule emitted by the code
    Risk level;
    bool (*match)(const Cmd&);               // nullptr: emitted by the code (redirections, wrappers…)
    bool otherwise;                          // only if no other rule for this command has matched before
    std::string_view why_es, why_en;
};

const std::vector<Rule>& rule_table();
const Rule* find_rule(std::string_view id);
bool rule_applies(const Rule& r, std::string_view name);
bool known_command(std::string_view name);  // appears in the table or in the whitelist

// Read-only whitelist with its condition.
bool read_only(const Cmd& c);

// Long option given (e.g. "--rec") as a GNU abbreviation of full ("--recursive"): at least
// two letters after "--". Conservative: if it matches several, it matches all of them.
bool long_match(std::string_view given, std::string_view full);

// Options whose value is a file written to (sort -o, curl -o, find -fprint…): target and whether it appends.
std::vector<std::pair<Word, bool>> write_values(const Cmd& c);
// Options whose value is code (rsync -e, tar --to-command, git rebase --exec, trap…).
std::vector<std::string> code_values(const Cmd& c);
// Code in an ssh -o (ProxyCommand=…, LocalCommand=…), or empty.
std::string ssh_option_code(std::string_view opt);
// find starting points ("." by default).
std::vector<Word> find_start_words(const std::vector<Word>& argv);

bool is_downloader(std::string_view name);   // curl, wget, fetch, nc…
bool is_shell(std::string_view name);        // bash, sh, dash, zsh, ksh
std::string base_name(std::string_view path);

}  // namespace dale::risk
