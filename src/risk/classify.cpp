#include <algorithm>
#include <cctype>
#include <optional>

#include "risk/engine.hpp"
#include "risk/paths.hpp"
#include "risk/risk.hpp"
#include "risk/rules.hpp"

namespace dale::risk {
namespace {

constexpr std::size_t kMaxFragment = 48;

// Truncates to kMaxFragment bytes without splitting a UTF-8 character.
std::string shorten(std::string_view s) {
    std::string e = escape_control(s);
    if (e.size() <= kMaxFragment) return e;
    std::size_t k = kMaxFragment;
    while (k > 0 && (static_cast<unsigned char>(e[k]) & 0xC0) == 0x80) --k;
    return e.substr(0, k) + "…";
}

bool has_control(std::string_view s) {
    return std::any_of(s.begin(), s.end(), [](char c) {
        auto u = static_cast<unsigned char>(c);
        return (u < 0x20 && c != '\n' && c != '\t') || u == 0x7f;
    });
}

bool is_number(std::string_view s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
}

std::string unescape_echo(std::string_view s) {
    std::string o;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char n = s[i + 1];
            if (n == 'n') { o += '\n'; ++i; continue; }
            if (n == 't') { o += '\t'; ++i; continue; }
            if (n == '\\') { o += '\\'; ++i; continue; }
        }
        o += s[i];
    }
    return o;
}

// What a static echo/printf/cat (R14b) or a find (its starting points) leaves in the pipe.
Produced produced_by(const SimpleCommand& c) {
    Produced p;
    if (c.words.empty() || c.words[0].dynamic) return p;
    std::string name = base_name(c.words[0].text);
    if (name == "find") {
        std::vector<std::string> items;
        for (const Word& w : find_start_words(c.words)) items.push_back(w.text);
        p.items = items;
        return p;
    }
    bool fixed = std::none_of(c.words.begin(), c.words.end(), [](const Word& w) { return w.dynamic || w.glob; });
    if (name == "cat") {  // cat <<EOF … | sh: the text of the last heredoc or here-string
        if (!fixed) return p;
        for (std::size_t i = 1; i < c.words.size(); ++i) {
            if (c.words[i].text != "-" && c.words[i].text != "/dev/stdin") return p;
        }
        for (const Redir& r : c.redirs) {
            if (r.op == "<<" || r.op == "<<-") p.text = p.raw = r.body;
            if (r.op == "<<<") {
                p.text = r.target.dynamic ? std::optional<std::string>() : r.target.text;
                p.raw = r.target.text;
            }
        }
    } else if (name == "echo" || name == "printf") {
        std::size_t k = 1;
        bool esc = name == "printf";
        if (name == "echo") {
            while (k < c.words.size()) {
                const std::string& t = c.words[k].text;
                if (t.size() < 2 || t[0] != '-' || t.find_first_not_of("neE", 1) != std::string::npos) break;
                if (t.find('e') != std::string::npos) esc = true;
                ++k;
            }
        }
        std::string text;
        for (; k < c.words.size(); ++k) {
            if (!text.empty()) text += ' ';
            text += c.words[k].text;
        }
        p.raw = esc ? unescape_echo(text) : text;
        // With expansions the output is unknown; raw keeps only the literal parts reliable.
        if (fixed) p.text = p.raw;
    }
    if (p.text) p.items = split_words(*p.text);
    return p;
}

// Environment variables that do not change what gets executed.
bool safe_env(std::string_view name) {
    for (std::string_view v : {"LANG", "LANGUAGE", "TZ", "NO_COLOR", "COLUMNS", "LINES", "TERM"}) {
        if (name == v) return true;
    }
    return name.substr(0, 3) == "LC_";
}

}  // namespace

std::vector<std::string> split_words(std::string_view text) {
    std::vector<std::string> out;
    std::size_t k = 0;
    while (k < text.size()) {
        std::size_t b = text.find_first_not_of(" \t\n", k);
        if (b == std::string_view::npos) break;
        std::size_t e = text.find_first_of(" \t\n", b);
        if (e == std::string_view::npos) e = text.size();
        out.emplace_back(text.substr(b, e - b));
        k = e;
    }
    return out;
}

std::string fragment_of(const std::vector<Word>& w, std::size_t from) {
    std::string s;
    for (std::size_t i = from; i < w.size(); ++i) {
        if (!s.empty()) s += ' ';
        s += w[i].text;
        if (s.size() > kMaxFragment) break;
    }
    return s;
}

bool is_assign_word(const Word& w) {
    const std::string& r = w.raw;
    if (r.empty() || !(std::isalpha(static_cast<unsigned char>(r[0])) || r[0] == '_')) return false;
    std::size_t k = 1;
    while (k < r.size() && (std::isalnum(static_cast<unsigned char>(r[k])) || r[k] == '_')) ++k;
    if (k < r.size() && r[k] == '+') ++k;
    return k < r.size() && r[k] == '=';
}

Word make_word(std::string text, bool dynamic) {
    Word w;
    w.raw = text;
    w.text = std::move(text);
    w.dynamic = dynamic;
    return w;
}

void Engine::emit(std::string_view id, std::string_view fragment, std::string_view why_es, std::string_view why_en) {
    const Rule* r = find_rule(id);
    Finding f;
    f.level = r ? r->level : Risk::Precaucion;
    f.rule_id = std::string(id);
    f.fragment = shorten(fragment);
    f.why_es = std::string(!why_es.empty() ? why_es : (r ? r->why_es : std::string_view()));
    f.why_en = std::string(!why_en.empty() ? why_en : (r ? r->why_en : std::string_view()));
    for (const Finding& g : out_.findings) {
        if (g.rule_id == f.rule_id && g.fragment == f.fragment) return;
    }
    if (f.level > out_.level) out_.level = f.level;
    out_.findings.push_back(std::move(f));
}

void Engine::code(std::string_view text, int depth, const Stdin& def) {
    if (depth > kMaxDepth) {
        emit("PARSE", text, "demasiado anidamiento para analizarlo", "too deeply nested to check");
        return;
    }
    Script sc = parse(text, depth);
    if (!sc.errors.empty()) emit("PARSE", text);
    if (sc.has_case) emit("CASE", "case");
    for (Word w : sc.loose) nested(w);
    for (const FunctionDef& f : sc.functions) {
        for (std::size_t k = f.begin; k < f.end && k < sc.commands.size(); ++k) {
            const SimpleCommand& c = sc.commands[k];
            if (!c.words.empty() && c.words[0].text == f.name && (c.in_pipe || c.background)) {
                emit("R11", f.name + "(){ … }");
                break;
            }
        }
    }
    std::vector<Range> ranges(sc.commands.size());
    std::vector<Produced> produced(sc.commands.size());
    for (std::size_t k = 0; k < sc.commands.size(); ++k) command(sc, k, def, ranges, produced);
}

void Engine::nested(Word& w) {
    for (const std::string& sub : w.subs) {
        std::size_t n0 = names_.size();
        code(sub, w.depth + 1, Stdin{});
        for (std::size_t i = n0; i < names_.size(); ++i) {
            if (is_downloader(names_[i])) w.net = true;
        }
    }
}

// VAR=x before a command (only called when there is a command).
void Engine::env_assign(const Word& w) {
    std::string name = w.text.substr(0, w.text.find_first_of("+=["));
    if (!safe_env(name)) emit("ENV", w.text);
}

void Engine::command(const Script& sc, std::size_t k, const Stdin& def, std::vector<Range>& ranges,
                     std::vector<Produced>& produced) {
    SimpleCommand c = sc.commands[k];
    std::size_t n0 = names_.size();
    for (Word& w : c.assigns) nested(w);
    for (Word& w : c.words) nested(w);
    for (Redir& r : c.redirs) nested(r.target);
    if (!c.words.empty()) {
        for (const Word& a : c.assigns) env_assign(a);
    }
    // Brace expansion: each alternative is a word.
    std::vector<Word> words;
    for (const Word& w : c.words) {
        if (w.alts.empty()) {
            words.push_back(w);
            continue;
        }
        for (const std::string& alt : w.alts) {
            Word x = w;
            x.text = alt;
            x.alts.clear();
            words.push_back(std::move(x));
        }
    }
    c.words = std::move(words);

    Stdin in = def;
    if (!c.upstream.empty()) {
        in = Stdin{};
        in.kind = Stdin::Kind::Pipe;
        for (std::size_t u : c.upstream) {
            for (std::size_t i = ranges[u].first; i < ranges[u].second; ++i) {
                if (is_downloader(names_[i])) in.net = true;
            }
        }
        if (c.upstream.size() == 1) {
            const Produced& p = produced[c.upstream[0]];
            if (p.text) {
                in.text_known = true;
                in.text = *p.text;
            }
            if (p.raw) in.raw = *p.raw;
            in.items = p.items;
        }
    }
    for (const Redir& r : c.redirs) {
        if (r.op == "<<" || r.op == "<<-") {
            in = Stdin{};
            in.kind = Stdin::Kind::Text;
            in.text = in.raw = r.body;
            in.items = split_words(in.text);
        } else if (r.op == "<<<") {
            in = Stdin{};
            in.kind = Stdin::Kind::Text;
            in.text = in.raw = r.target.text;
            in.net = r.target.net;
            if (!r.target.dynamic) in.items = split_words(in.text);
        } else if ((r.op == "<" || r.op == "<>") && (r.fd < 0 || r.fd == 0)) {
            if (normalize_path(r.target.text, r.target.tilde) != "/dev/null") {
                in = Stdin{};
                in.kind = Stdin::Kind::File;
            }
        }
        redirect(r);
    }
    if (!c.words.empty()) argv(c.words, in, c.depth);
    ranges[k] = {n0, names_.size()};
    produced[k] = produced_by(c);
}

void Engine::redirect(const Redir& r) {
    const std::string& op = r.op;
    std::string frag = op + r.target.text;
    if (op == ">" || op == ">|" || op == "&>" || op == "<>") {
        write_target(r.target, false, frag);
    } else if (op == ">>" || op == "&>>") {
        write_target(r.target, true, frag);
    } else if (op == ">&" && !is_number(r.target.text) && r.target.text != "-") {
        write_target(r.target, false, frag);
    }
}

void Engine::write_target(const Word& t, bool append, std::string_view fragment) {
    if (t.proc_sub) return;  // >(cmd): the code has already been classified
    std::string p = normalize_path(t.text, t.tilde);
    // The target is only known if the only expansion is a leading $HOME.
    if (t.glob || (t.dynamic && p.find_first_of("$`") != std::string::npos)) {
        emit("WRITE", fragment);
        return;
    }
    if (p == "/dev/null" || p == "/dev/stdout" || p == "/dev/stderr" || p == "/dev/tty" || p == "/dev/fd/1" ||
        p == "/dev/fd/2")
        return;
    if (p == "/proc/sysrq-trigger") {
        emit("R13", fragment);
    } else if (in_D(p)) {
        emit("R6", fragment);
    } else if (in_F(p)) {
        emit(append ? "R7b" : "R7", fragment);
    } else if (p.rfind("/proc/sys/", 0) == 0 || p.rfind("/sys/", 0) == 0) {
        emit("R26", fragment);
    } else {
        emit("WRITE", fragment);
    }
}

void Engine::argv(const std::vector<Word>& w, const Stdin& in, int depth) {
    if (w.empty()) return;
    const Word& w0 = w[0];
    if (w0.dynamic || w0.glob) {
        names_.emplace_back("$");
        emit(w0.net ? "R14" : "DYN", w0.text);
        return;
    }
    std::string name = base_name(w0.text);
    names_.push_back(name);
    if (name == "openssl" && w.size() > 1 && w[1].text == "s_client") names_.emplace_back("s_client");
    if (unwrap(name, w, in, depth)) return;
    rules(w, depth, in);
}

void Engine::rules(const std::vector<Word>& w, int depth, const Stdin& in) {
    Cmd c = make_cmd(w, admin_ > 0);
    c.input = in.raw;
    std::string frag = fragment_of(w);
    // Only --help and/or --version: escalation rules do not apply.
    if (!c.args.empty() && std::all_of(c.args.begin(), c.args.end(), [](const Word& a) {
            return a.text == "--help" || a.text == "--version";
        })) {
        if (!read_only(c)) {
            emit("R31", frag, "el verificador no puede confirmar que solo lea información",
                 "the checker cannot confirm it only reads information");
        }
        return;
    }
    std::size_t before = out_.findings.size();
    for (const auto& [target, append] : write_values(c)) write_target(target, append, c.name + " " + target.text);
    for (const std::string& code_text : code_values(c)) {
        emit("R14c", frag);
        code(code_text, depth + 1, Stdin{});
    }
    bool extra = out_.findings.size() > before;  // already writes or runs code: R31 is not needed
    bool matched = false;
    for (const Rule& r : rule_table()) {
        if (!r.match || !rule_applies(r, c.name)) continue;
        if (r.otherwise && matched) continue;
        if (r.match(c)) {
            emit(r.id, frag, r.why_es, r.why_en);
            matched = true;
        }
    }
    if (matched || extra || read_only(c)) return;
    if (known_command(c.name)) {
        emit("R31", frag, "el verificador no puede confirmar que solo lea información",
             "the checker cannot confirm it only reads information");
    } else if (w[0].text.find('/') != std::string::npos) {
        emit("R31", frag, "ejecuta un programa o script local", "runs a local program or script");
    } else {
        emit("R31", frag);
    }
}

Assessment classify(std::string_view command) {
    Assessment a;
    Engine e(a);
    if (command.size() > kMaxInput) {
        e.emit("SIZE", command);
        return a;
    }
    if (has_control(command)) {
        for (char c : command) {
            auto u = static_cast<unsigned char>(c);
            if ((u < 0x20 && c != '\n' && c != '\t') || u == 0x7f) {
                e.emit("CTRL", std::string(1, c));
                break;
            }
        }
    }
    e.code(command, 0, Stdin{});
    return a;
}

std::string_view to_string(Risk level) {
    switch (level) {
        case Risk::Seguro: return "seguro";
        case Risk::Precaucion: return "precaucion";
        case Risk::Peligro: return "peligro";
    }
    return "precaucion";
}

std::string escape_control(std::string_view text) {
    static const char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        auto u = static_cast<unsigned char>(c);
        if ((u < 0x20 && c != '\n' && c != '\t') || u == 0x7f) {
            out += "\\x";
            out += kHex[u >> 4];
            out += kHex[u & 0x0f];
        } else {
            out += c;
        }
    }
    return out;
}

}  // namespace dale::risk
