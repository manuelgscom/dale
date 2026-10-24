#include "risk/lexer.hpp"

#include <algorithm>
#include <utility>

namespace dale::risk {
namespace {

constexpr std::size_t npos = std::string_view::npos;

bool is_blank(char c) { return c == ' ' || c == '\t'; }
bool is_meta(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == ';' || c == '&' || c == '|' || c == '(' ||
           c == ')' || c == '<' || c == '>';
}
bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_name_start(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
bool is_name_char(char c) { return is_name_start(c) || is_digit(c); }

std::size_t find_close(std::string_view s, std::size_t open, char oc, char cc);

// s[k] is '"'; returns the index after the closing quote, or npos.
std::size_t skip_dquote(std::string_view s, std::size_t k) {
    ++k;
    while (k < s.size()) {
        char c = s[k];
        if (c == '\\') {
            k += 2;
        } else if (c == '"') {
            return k + 1;
        } else if (c == '$' && k + 1 < s.size() && (s[k + 1] == '(' || s[k + 1] == '{')) {
            std::size_t j = find_close(s, k + 1, s[k + 1], s[k + 1] == '(' ? ')' : '}');
            if (j == npos) return npos;
            k = j + 1;
        } else if (c == '`') {
            ++k;
            while (k < s.size() && s[k] != '`') k += (s[k] == '\\') ? 2 : 1;
            if (k >= s.size()) return npos;
            ++k;
        } else {
            ++k;
        }
    }
    return npos;
}

// Index of the character that closes s[open], honoring quotes and nesting; npos if unclosed.
std::size_t find_close(std::string_view s, std::size_t open, char oc, char cc) {
    int level = 0;
    std::size_t k = open;
    while (k < s.size()) {
        char c = s[k];
        if (c == '\\') {
            k += 2;
            continue;
        }
        if (c == '\'') {
            std::size_t j = s.find('\'', k + 1);
            if (j == npos) return npos;
            k = j + 1;
            continue;
        }
        if (c == '"') {
            k = skip_dquote(s, k);
            if (k == npos) return npos;
            continue;
        }
        if (c == '`') {
            ++k;
            while (k < s.size() && s[k] != '`') k += (s[k] == '\\') ? 2 : 1;
            if (k >= s.size()) return npos;
            ++k;
            continue;
        }
        if (c == oc) {
            ++level;
        } else if (c == cc) {
            if (--level == 0) return k;
        }
        ++k;
    }
    return npos;
}

void append_utf8(std::string& out, unsigned long cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | ((cp >> 18) & 0x07));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool is_assignment(std::string_view raw) {
    if (raw.empty() || !is_name_start(raw[0])) return false;
    std::size_t k = 1;
    while (k < raw.size() && is_name_char(raw[k])) ++k;
    if (k < raw.size() && raw[k] == '[') {
        std::size_t j = raw.find(']', k);
        if (j == npos) return false;
        k = j + 1;
    }
    if (k < raw.size() && raw[k] == '+') ++k;
    return k < raw.size() && raw[k] == '=';
}

// Brace expansion: t is the text with a mark on each unquoted { , }.
using Marked = std::vector<std::pair<char, bool>>;

bool expand_braces(const Marked& t, std::vector<Marked>& out, std::size_t limit) {
    for (std::size_t i = 0; i < t.size(); ++i) {
        if (!(t[i].second && t[i].first == '{')) continue;
        int level = 0;
        std::size_t close = 0;
        std::vector<std::size_t> commas;
        for (std::size_t k = i + 1; k < t.size() && close == 0; ++k) {
            if (!t[k].second) continue;
            if (t[k].first == '{') {
                ++level;
            } else if (t[k].first == '}') {
                if (level == 0) close = k;
                else --level;
            } else if (t[k].first == ',' && level == 0) {
                commas.push_back(k);
            }
        }
        if (close == 0) continue;
        if (commas.empty()) {
            for (std::size_t k = i + 1; k + 1 < close; ++k) {
                if (t[k].first == '.' && t[k + 1].first == '.') return false;  // {1..9}: not expanded
            }
            continue;
        }
        commas.push_back(close);
        std::size_t from = i + 1;
        for (std::size_t c : commas) {
            Marked n(t.begin(), t.begin() + static_cast<std::ptrdiff_t>(i));
            n.insert(n.end(), t.begin() + static_cast<std::ptrdiff_t>(from), t.begin() + static_cast<std::ptrdiff_t>(c));
            n.insert(n.end(), t.begin() + static_cast<std::ptrdiff_t>(close + 1), t.end());
            if (!expand_braces(n, out, limit)) return false;
            from = c + 1;
        }
        return true;
    }
    out.push_back(t);
    return out.size() <= limit;
}

struct Stop {
    bool paren = false, brace = false, esac = false;
};

class Parser {
public:
    Parser(std::string_view s, int depth) : s_(s), depth_(depth) {}

    Script run() {
        parse_list(Stop{}, {});
        if (!pending_.empty()) error("heredoc sin cierre");
        return std::move(out_);
    }

    // Walks the whole text as if it were inside double quotes (heredoc body, ${..}).
    void scan_body(Word& w) {
        if (depth_ > kMaxDepth) {
            error("demasiado anidamiento");
            return;
        }
        while (!eof()) {
            char c = at();
            if (c == '\\') {
                char n = at(1);
                if (i_ + 1 < s_.size() && (n == '$' || n == '`' || n == '\\' || n == '\n')) {
                    if (n != '\n') w.text += n;
                    i_ += 2;
                } else {
                    w.text += c;
                    ++i_;
                }
            } else if (c == '$') {
                parse_dollar(w, true);
            } else if (c == '`') {
                parse_backtick(w);
            } else {
                w.text += c;
                ++i_;
            }
        }
    }

    std::vector<std::string>& errors() { return out_.errors; }

private:
    struct Pending {
        std::size_t cmd, redir;
        std::string delim;
        bool strip, expands;
    };

    std::string_view s_;
    std::size_t i_ = 0;
    int depth_;
    Script out_;
    std::vector<Pending> pending_;

    bool eof() const { return i_ >= s_.size(); }
    char at(std::size_t k = 0) const { return i_ + k < s_.size() ? s_[i_ + k] : '\0'; }

    void error(const std::string& m) {
        if (std::find(out_.errors.begin(), out_.errors.end(), m) == out_.errors.end())
            out_.errors.push_back(m);
    }

    void merge_errors(const std::vector<std::string>& errs) {
        for (const auto& e : errs) error(e);
    }

    // Blanks, line continuations and comments (only called between tokens).
    void skip_blanks() {
        while (!eof()) {
            char c = at();
            if (is_blank(c)) {
                ++i_;
            } else if (c == '\\' && at(1) == '\n') {
                i_ += 2;
            } else if (c == '#') {
                while (!eof() && at() != '\n') ++i_;
            } else {
                break;
            }
        }
    }

    void skip_space_nl() {
        for (;;) {
            skip_blanks();
            if (!eof() && at() == '\n') {
                consume_newline();
            } else {
                break;
            }
        }
    }

    void consume_newline() {
        ++i_;
        if (!pending_.empty()) read_heredocs();
    }

    void read_heredocs() {
        for (const Pending& p : pending_) {
            std::string body;
            bool found = false;
            while (!eof()) {
                std::size_t end = s_.find('\n', i_);
                std::string_view line = s_.substr(i_, end == npos ? npos : end - i_);
                i_ = (end == npos) ? s_.size() : end + 1;
                std::string_view check = line;
                if (p.strip) {
                    while (!check.empty() && check[0] == '\t') check.remove_prefix(1);
                }
                if (check == p.delim) {
                    found = true;
                    break;
                }
                body += p.strip ? check : line;
                body += '\n';
            }
            if (!found) error("heredoc sin cierre");
            if (p.expands) {
                Word w;
                w.depth = depth_;
                Parser sub(body, depth_);
                sub.scan_body(w);
                merge_errors(sub.errors());
                if (!w.subs.empty()) out_.loose.push_back(std::move(w));
            }
            out_.commands[p.cmd].redirs[p.redir].body = std::move(body);
        }
        pending_.clear();
    }

    // Reserved word at the start of a command (without consuming it), or empty.
    std::string_view peek_reserved() const {
        std::size_t k = i_;
        while (k < s_.size()) {
            char c = s_[k];
            bool ok = (c >= 'a' && c <= 'z') || c == '!' || c == '{' || c == '}' || c == '[' || c == ']';
            if (!ok) break;
            ++k;
        }
        if (k == i_) return {};
        if (k < s_.size() && !is_meta(s_[k])) return {};
        std::string_view w = s_.substr(i_, k - i_);
        static const char* const kWords[] = {"if",   "then",  "elif", "else",     "fi",   "while", "until",
                                             "do",   "done",  "for",  "select",   "case", "esac",  "in",
                                             "!",    "{",     "}",    "[[",       "]]",   "time",  "coproc",
                                             "function"};
        for (const char* r : kWords) {
            if (w == r) return w;
        }
        return {};
    }

    bool enter_group() {
        if (depth_ + 1 > kMaxDepth) {
            error("demasiado anidamiento");
            i_ = s_.size();
            return false;
        }
        ++depth_;
        return true;
    }

    void parse_list(Stop stop, const std::vector<std::size_t>& inherited) {
        for (;;) {
            std::size_t before = i_;
            skip_space_nl();
            if (eof()) return;
            char c = at();
            if (c == ')') {
                if (stop.paren) return;
                error("paréntesis sin abrir");
                ++i_;
                continue;
            }
            if (c == ';' && (at(1) == ';' || at(1) == '&')) {
                if (stop.esac) return;
                error("';;' fuera de case");
                i_ += 2;
                continue;
            }
            std::string_view rw = peek_reserved();
            if (stop.brace && rw == "}") return;
            if (stop.esac && rw == "esac") return;

            std::size_t b = out_.commands.size();
            parse_pipeline(inherited);
            std::size_t e = out_.commands.size();
            skip_blanks();
            if (eof()) return;
            c = at();
            if (c == ';' && at(1) != ';' && at(1) != '&') {
                ++i_;
            } else if ((c == '&' && at(1) == '&') || (c == '|' && at(1) == '|')) {
                i_ += 2;
            } else if (c == '&' && at(1) != '>') {
                ++i_;
                for (std::size_t k = b; k < e; ++k) out_.commands[k].background = true;
            } else if (c == '\n') {
                consume_newline();
            }
            if (i_ == before) {
                error("carácter inesperado");
                ++i_;
            }
        }
    }

    void parse_pipeline(const std::vector<std::size_t>& inherited) {
        std::size_t b = out_.commands.size();
        std::vector<std::size_t> prev;
        int stages = 0;
        for (;;) {
            std::vector<std::size_t> up = inherited;
            up.insert(up.end(), prev.begin(), prev.end());
            std::size_t sb = out_.commands.size();
            parse_command(up);
            std::size_t se = out_.commands.size();
            skip_blanks();
            if (at() == '|' && at(1) != '|' && !eof()) {
                i_ += (at(1) == '&') ? 2 : 1;
                for (std::size_t k = sb; k < se; ++k) prev.push_back(k);
                ++stages;
                skip_space_nl();
                if (eof()) {
                    error("tubería incompleta");
                    break;
                }
                continue;
            }
            break;
        }
        if (stages > 0) {
            for (std::size_t k = b; k < out_.commands.size(); ++k) out_.commands[k].in_pipe = true;
        }
    }

    void parse_command(const std::vector<std::size_t>& up) {
        for (;;) {
            skip_blanks();
            std::string_view rw = peek_reserved();
            if (rw == "if" || rw == "then" || rw == "elif" || rw == "else" || rw == "while" || rw == "until" ||
                rw == "do" || rw == "!" || rw == "time" || rw == "coproc") {
                i_ += rw.size();
                if (rw == "time") {
                    skip_blanks();
                    if (at() == '-' && at(1) == 'p' && (i_ + 2 >= s_.size() || is_meta(at(2)))) i_ += 2;
                }
                continue;
            }
            if (rw == "fi" || rw == "done" || rw == "esac" || rw == "}" || rw == "in" || rw == "]]") {
                i_ += rw.size();
                continue;
            }
            break;
        }
        if (eof()) return;
        if (at() == '(') {
            if (at(1) == '(') {
                std::size_t j = find_close(s_, i_, '(', ')');
                if (j == npos) {
                    error("paréntesis sin cerrar");
                    i_ = s_.size();
                    return;
                }
                scan_loose(s_.substr(i_, j + 1 - i_));
                i_ = j + 1;
                parse_trailing_redirs(up);
                return;
            }
            ++i_;
            if (!enter_group()) return;
            parse_list(Stop{true, false, false}, up);
            --depth_;
            if (!eof() && at() == ')') {
                ++i_;
            } else {
                error("paréntesis sin cerrar");
            }
            parse_trailing_redirs(up);
            return;
        }
        std::string_view rw = peek_reserved();
        if (rw == "{") {
            ++i_;
            if (!enter_group()) return;
            parse_list(Stop{false, true, false}, up);
            --depth_;
            if (peek_reserved() == "}") {
                ++i_;
            } else {
                error("llave sin cerrar");
            }
            parse_trailing_redirs(up);
            return;
        }
        if (rw == "[[") return parse_test(up);
        if (rw == "for" || rw == "select") return parse_for(rw.size());
        if (rw == "case") return parse_case(up);
        if (rw == "function") return parse_function_kw();
        parse_simple(up);
    }

    // Text with expansions that is not a command: its $(..) are kept to be classified.
    void scan_loose(std::string_view text) {
        Word w;
        w.depth = depth_;
        Parser sub(text, depth_);
        sub.scan_body(w);
        merge_errors(sub.errors());
        if (!w.subs.empty()) out_.loose.push_back(std::move(w));
    }

    void parse_trailing_redirs(const std::vector<std::size_t>& up) {
        SimpleCommand c;
        c.upstream = up;
        c.depth = depth_;
        std::vector<Pending> pend;
        for (;;) {
            skip_blanks();
            int fd = -1;
            std::size_t len = 0;
            std::string op = redirect_at(fd, len);
            if (op.empty()) break;
            parse_redir(c, fd, op, len, pend);
        }
        push_command(std::move(c), pend);
    }

    void push_command(SimpleCommand c, std::vector<Pending>& pend) {
        if (c.words.empty() && c.assigns.empty() && c.redirs.empty()) return;
        out_.commands.push_back(std::move(c));
        for (Pending& p : pend) {
            p.cmd = out_.commands.size() - 1;
            pending_.push_back(std::move(p));
        }
    }

    void parse_test(const std::vector<std::size_t>& up) {
        i_ += 2;
        SimpleCommand c;
        c.upstream = up;
        c.depth = depth_;
        Word first;
        first.text = first.raw = "[[";
        first.depth = depth_;
        c.words.push_back(first);
        for (;;) {
            skip_blanks();
            if (eof() || at() == '\n') {
                error("[[ sin cerrar");
                break;
            }
            if (peek_reserved() == "]]") {
                i_ += 2;
                break;
            }
            Word w = read_word(true);
            if (w.raw.empty()) {
                ++i_;
                continue;
            }
            c.words.push_back(std::move(w));
        }
        std::vector<Pending> pend;
        push_command(std::move(c), pend);
    }

    void parse_for(std::size_t kw) {
        i_ += kw;
        skip_blanks();
        if (at() == '(' && at(1) == '(') {
            std::size_t j = find_close(s_, i_, '(', ')');
            if (j == npos) {
                error("paréntesis sin cerrar");
                i_ = s_.size();
                return;
            }
            scan_loose(s_.substr(i_, j + 1 - i_));
            i_ = j + 1;
            return;
        }
        for (;;) {
            skip_blanks();
            if (eof() || at() == ';' || at() == '\n' || peek_reserved() == "do") return;
            if (is_meta(at()) && !((at() == '<' || at() == '>') && at(1) == '(')) return;
            Word w = read_word(false);
            if (w.raw.empty()) {
                ++i_;
                continue;
            }
            if (!w.subs.empty()) out_.loose.push_back(std::move(w));
        }
    }

    void parse_case(const std::vector<std::size_t>& up) {
        i_ += 4;
        out_.has_case = true;
        skip_blanks();
        Word subject = read_word(false);
        if (!subject.subs.empty()) out_.loose.push_back(std::move(subject));
        skip_space_nl();
        if (peek_reserved() != "in") {
            error("case sin in");
            return;
        }
        i_ += 2;
        for (;;) {
            skip_space_nl();
            if (eof()) {
                error("case sin esac");
                return;
            }
            if (peek_reserved() == "esac") {
                i_ += 4;
                parse_trailing_redirs(up);
                return;
            }
            if (at() == '(') ++i_;
            for (;;) {
                skip_blanks();
                if (eof() || at() == '\n' || at() == ';') {
                    error("patrón de case mal formado");
                    return;
                }
                if (at() == ')') {
                    ++i_;
                    break;
                }
                if (at() == '|') {
                    ++i_;
                    continue;
                }
                Word w = read_word(false);
                if (w.raw.empty()) {
                    error("patrón de case mal formado");
                    return;
                }
                if (!w.subs.empty()) out_.loose.push_back(std::move(w));
            }
            parse_list(Stop{false, false, true}, up);
            skip_blanks();
            if (at() == ';' && at(1) == ';') {
                i_ += 2;
                if (at() == '&') ++i_;
            } else if (at() == ';' && at(1) == '&') {
                i_ += 2;
            }
        }
    }

    void parse_function_kw() {
        i_ += 8;
        skip_blanks();
        Word name = read_word(false);
        skip_blanks();
        if (at() == '(') {
            std::size_t k = i_ + 1;
            while (k < s_.size() && is_blank(s_[k])) ++k;
            if (k < s_.size() && s_[k] == ')') i_ = k + 1;
        }
        define_function(name.text);
    }

    void define_function(const std::string& name) {
        skip_space_nl();
        std::size_t b = out_.commands.size();
        parse_command({});
        out_.functions.push_back({name, b, out_.commands.size()});
    }

    // Redirection at the current position (without consuming it): returns the operator or empty.
    std::string redirect_at(int& fd, std::size_t& len) const {
        std::size_t k = i_;
        fd = -1;
        if (k < s_.size() && is_digit(s_[k])) {
            std::size_t j = k;
            int v = 0;
            while (j < s_.size() && is_digit(s_[j])) {
                v = std::min(v * 10 + (s_[j] - '0'), 100000);
                ++j;
            }
            if (j >= s_.size() || (s_[j] != '<' && s_[j] != '>')) return {};
            fd = v;
            k = j;
        }
        if (k >= s_.size()) return {};
        auto next = [&](std::size_t o) { return k + o < s_.size() ? s_[k + o] : '\0'; };
        std::string op;
        char c = s_[k];
        if (c == '&' && fd < 0 && next(1) == '>') {
            op = next(2) == '>' ? "&>>" : "&>";
        } else if (c == '<') {
            if (fd < 0 && next(1) == '(') return {};
            if (next(1) == '<' && next(2) == '<') op = "<<<";
            else if (next(1) == '<' && next(2) == '-') op = "<<-";
            else if (next(1) == '<') op = "<<";
            else if (next(1) == '>') op = "<>";
            else if (next(1) == '&') op = "<&";
            else op = "<";
        } else if (c == '>') {
            if (fd < 0 && next(1) == '(') return {};
            if (next(1) == '>') op = ">>";
            else if (next(1) == '|') op = ">|";
            else if (next(1) == '&') op = ">&";
            else op = ">";
        } else {
            return {};
        }
        len = (k - i_) + op.size();
        return op;
    }

    void parse_redir(SimpleCommand& c, int fd, const std::string& op, std::size_t len, std::vector<Pending>& pend) {
        i_ += len;
        Redir r;
        r.fd = fd;
        r.op = op;
        skip_blanks();
        if (!eof() && (!is_meta(at()) || ((at() == '<' || at() == '>') && at(1) == '('))) r.target = read_word(false);
        if (r.target.raw.empty()) {
            error("redirección sin destino");
            return;
        }
        if (op == "<<" || op == "<<-") {
            r.body_expands = !r.target.quoted;
            pend.push_back({0, c.redirs.size(), r.target.text, op == "<<-", r.body_expands});
        }
        c.redirs.push_back(std::move(r));
    }

    bool next_is_close_paren() const {
        std::size_t k = i_ + 1;
        while (k < s_.size() && is_blank(s_[k])) ++k;
        return k < s_.size() && s_[k] == ')';
    }

    void parse_simple(const std::vector<std::size_t>& up) {
        SimpleCommand c;
        c.upstream = up;
        c.depth = depth_;
        std::vector<Pending> pend;
        for (;;) {
            skip_blanks();
            if (eof()) break;
            char ch = at();
            if (ch == '\n' || ch == ';' || ch == ')' || ch == '|') break;
            if (ch == '&' && at(1) != '>') break;
            int fd = -1;
            std::size_t len = 0;
            std::string op = redirect_at(fd, len);
            if (!op.empty()) {
                parse_redir(c, fd, op, len, pend);
                continue;
            }
            if (ch == '(') {
                if (c.words.size() == 1 && c.assigns.empty() && c.redirs.empty() && next_is_close_paren()) {
                    i_ = s_.find(')', i_) + 1;
                    define_function(c.words[0].text);
                    return;
                }
                error("paréntesis inesperado");
                ++i_;
                continue;
            }
            Word w = read_word(false);
            if (w.raw.empty()) {
                error("carácter inesperado");
                ++i_;
                continue;
            }
            if (c.words.empty() && is_assignment(w.raw)) {
                c.assigns.push_back(std::move(w));
            } else {
                c.words.push_back(std::move(w));
            }
        }
        push_command(std::move(c), pend);
    }

    Word read_word(bool test_mode) {
        Word w;
        w.depth = depth_;
        std::size_t start = i_;
        w.tilde = at() == '~';
        std::vector<std::size_t> braces;  // positions in w.text of unquoted { , }
        while (!eof()) {
            char c = at();
            if (is_blank(c) || c == '\n') break;
            if (is_meta(c)) {
                if ((c == '<' || c == '>') && at(1) == '(' && i_ == start) {
                    std::size_t j = find_close(s_, i_ + 1, '(', ')');
                    if (j == npos) {
                        error("paréntesis sin cerrar");
                        w.subs.emplace_back(s_.substr(i_ + 2));
                        j = s_.size() - 1;
                    } else {
                        w.subs.emplace_back(s_.substr(i_ + 2, j - i_ - 2));
                    }
                    w.text += s_.substr(i_, j + 1 - i_);
                    w.dynamic = w.proc_sub = true;
                    i_ = j + 1;
                    continue;
                }
                if (c == '(' && !test_mode && i_ > start && s_[i_ - 1] == '=' &&
                    is_assignment(s_.substr(start, i_ - start))) {
                    std::size_t j = find_close(s_, i_, '(', ')');
                    if (j == npos) {
                        error("paréntesis sin cerrar");
                        j = s_.size() - 1;
                    }
                    Parser sub(s_.substr(i_, j + 1 - i_), depth_ + 1);
                    sub.scan_body(w);
                    merge_errors(sub.errors());
                    i_ = j + 1;
                    continue;
                }
                if (!test_mode || i_ > start) break;
            }
            switch (c) {
                case '\\':
                    if (at(1) == '\n') {
                        i_ += 2;
                    } else if (i_ + 1 >= s_.size()) {
                        w.text += '\\';
                        ++i_;
                    } else {
                        w.text += at(1);
                        w.quoted = true;
                        i_ += 2;
                    }
                    break;
                case '\'': {
                    w.quoted = true;
                    std::size_t j = s_.find('\'', i_ + 1);
                    if (j == npos) {
                        error("comillas sin cerrar");
                        w.text += s_.substr(i_ + 1);
                        i_ = s_.size();
                    } else {
                        w.text += s_.substr(i_ + 1, j - i_ - 1);
                        i_ = j + 1;
                    }
                    break;
                }
                case '"':
                    parse_dquote(w);
                    break;
                case '$':
                    parse_dollar(w, false);
                    break;
                case '`':
                    parse_backtick(w);
                    break;
                case '*':
                case '?':
                    w.glob = true;
                    w.text += c;
                    ++i_;
                    break;
                case '[': {
                    std::size_t k = i_ + 1;
                    while (k < s_.size() && !is_meta(s_[k]) && s_[k] != ']') ++k;
                    if (k < s_.size() && s_[k] == ']' && k > i_ + 1) w.glob = true;
                    w.text += c;
                    ++i_;
                    break;
                }
                default:
                    if (!test_mode && (c == '{' || c == ',' || c == '}')) braces.push_back(w.text.size());
                    w.text += c;
                    ++i_;
            }
        }
        w.raw = std::string(s_.substr(start, i_ - start));
        if (braces.size() >= 2) {
            Marked m;
            for (std::size_t k = 0, b = 0; k < w.text.size(); ++k) {
                bool special = b < braces.size() && braces[b] == k;
                if (special) ++b;
                m.emplace_back(w.text[k], special);
            }
            std::vector<Marked> out;
            if (!expand_braces(m, out, 64)) {
                error("expansión de llaves demasiado compleja");
            } else if (out.size() > 1) {
                for (const Marked& x : out) {
                    std::string t;
                    for (const auto& pc : x) t += pc.first;
                    w.alts.push_back(std::move(t));
                }
            }
        }
        return w;
    }

    void parse_dquote(Word& w) {
        w.quoted = true;
        ++i_;
        for (;;) {
            if (eof()) {
                error("comillas sin cerrar");
                return;
            }
            char c = at();
            if (c == '"') {
                ++i_;
                return;
            }
            if (c == '\\') {
                char n = at(1);
                if (i_ + 1 < s_.size() && (n == '$' || n == '`' || n == '"' || n == '\\' || n == '\n')) {
                    if (n != '\n') w.text += n;
                    i_ += 2;
                } else {
                    w.text += c;
                    ++i_;
                }
            } else if (c == '$') {
                parse_dollar(w, true);
            } else if (c == '`') {
                parse_backtick(w);
            } else {
                w.text += c;
                ++i_;
            }
        }
    }

    void parse_backtick(Word& w) {
        std::size_t k = i_ + 1;
        std::string content;
        bool closed = false;
        while (k < s_.size()) {
            char c = s_[k];
            if (c == '\\' && k + 1 < s_.size()) {
                char n = s_[k + 1];
                if (n == '`' || n == '\\' || n == '$') {
                    content += n;
                } else {
                    content += c;
                    content += n;
                }
                k += 2;
            } else if (c == '`') {
                closed = true;
                break;
            } else {
                content += c;
                ++k;
            }
        }
        if (!closed) error("comillas sin cerrar");
        std::size_t end = closed ? k + 1 : s_.size();
        w.text += s_.substr(i_, end - i_);
        w.subs.push_back(std::move(content));
        w.dynamic = true;
        i_ = end;
    }

    void parse_dollar(Word& w, bool in_dq) {
        char n = at(1);
        if (i_ + 1 >= s_.size()) {
            w.text += '$';
            ++i_;
            return;
        }
        if (n == '(') {
            std::size_t j = find_close(s_, i_ + 1, '(', ')');
            bool arith = at(2) == '(';
            std::size_t end = (j == npos) ? s_.size() : j + 1;
            if (j == npos) error("paréntesis sin cerrar");
            std::string_view inner = s_.substr(i_ + 2, (j == npos ? s_.size() : j) - i_ - 2);
            if (arith) {
                Parser sub(inner, depth_ + 1);
                sub.scan_body(w);
                merge_errors(sub.errors());
            } else {
                w.subs.emplace_back(inner);
            }
            w.text += s_.substr(i_, end - i_);
            w.dynamic = true;
            i_ = end;
            return;
        }
        if (n == '{') {
            std::size_t j = find_close(s_, i_ + 1, '{', '}');
            std::size_t end = (j == npos) ? s_.size() : j + 1;
            if (j == npos) error("llave sin cerrar");
            std::string_view inner = s_.substr(i_ + 2, (j == npos ? s_.size() : j) - i_ - 2);
            Word tmp;
            Parser sub(inner, depth_ + 1);
            sub.scan_body(tmp);
            merge_errors(sub.errors());
            for (auto& sc : tmp.subs) w.subs.push_back(std::move(sc));
            w.text += s_.substr(i_, end - i_);
            w.dynamic = true;
            i_ = end;
            return;
        }
        if (n == '\'' && !in_dq) {
            parse_ansi(w);
            return;
        }
        if (n == '"' && !in_dq) {
            ++i_;
            parse_dquote(w);
            return;
        }
        if (is_name_start(n)) {
            std::size_t k = i_ + 1;
            while (k < s_.size() && is_name_char(s_[k])) ++k;
            w.text += s_.substr(i_, k - i_);
            w.dynamic = true;
            i_ = k;
            return;
        }
        if (is_digit(n) || n == '@' || n == '*' || n == '#' || n == '?' || n == '$' || n == '!' || n == '-') {
            w.text += s_.substr(i_, 2);
            w.dynamic = true;
            i_ += 2;
            return;
        }
        w.text += '$';
        ++i_;
    }

    // $'...' with ANSI-C escapes.
    void parse_ansi(Word& w) {
        w.quoted = true;
        std::size_t k = i_ + 2;
        bool closed = false, cut = false;  // bash cuts the string at the first \0
        auto put = [&](char ch) {
            if (ch == '\0') cut = true;
            if (!cut) w.text += ch;
        };
        while (k < s_.size()) {
            char c = s_[k];
            if (c == '\'') {
                closed = true;
                ++k;
                break;
            }
            if (c != '\\' || k + 1 >= s_.size()) {
                put(c);
                ++k;
                continue;
            }
            char e = s_[k + 1];
            k += 2;
            switch (e) {
                case 'n': put('\n'); break;
                case 't': put('\t'); break;
                case 'r': put('\r'); break;
                case 'a': put('\a'); break;
                case 'b': put('\b'); break;
                case 'f': put('\f'); break;
                case 'v': put('\v'); break;
                case 'e':
                case 'E': put('\x1b'); break;
                case 'x':
                case 'u':
                case 'U': {
                    std::size_t maxd = e == 'x' ? 2 : (e == 'u' ? 4 : 8);
                    unsigned long v = 0;
                    std::size_t d = 0;
                    while (d < maxd && k < s_.size() && hex_value(s_[k]) >= 0) {
                        v = v * 16 + static_cast<unsigned long>(hex_value(s_[k]));
                        ++k;
                        ++d;
                    }
                    if (d == 0) {
                        put('\\');
                        put(e);
                    } else if (e == 'x') {
                        put(static_cast<char>(v));
                    } else {
                        if (v == 0) cut = true;
                        if (!cut) append_utf8(w.text, v);
                    }
                    break;
                }
                case 'c':
                    if (k < s_.size()) {
                        put(static_cast<char>(s_[k] & 0x1f));
                        ++k;
                    }
                    break;
                default:
                    if (e >= '0' && e <= '7') {
                        unsigned v = static_cast<unsigned>(e - '0');
                        for (int d = 0; d < 2 && k < s_.size() && s_[k] >= '0' && s_[k] <= '7'; ++d, ++k)
                            v = v * 8 + static_cast<unsigned>(s_[k] - '0');
                        put(static_cast<char>(v & 0xff));
                    } else if (e == '\\' || e == '\'' || e == '"' || e == '?') {
                        put(e);
                    } else {
                        put('\\');
                        put(e);
                    }
            }
        }
        if (!closed) error("comillas sin cerrar");
        i_ = k;
    }
};

}  // namespace

Script parse(std::string_view text, int depth) {
    Parser p(text, depth);
    return p.run();
}

}  // namespace dale::risk
