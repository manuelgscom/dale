// Recursive unwrapping: wrappers, interpreters, eval/source, find -exec and xargs.
#include <algorithm>
#include <optional>

#include "risk/engine.hpp"
#include "risk/paths.hpp"
#include "risk/rules.hpp"

namespace dale::risk {
namespace {

using SV = std::string_view;

bool in_list(const std::vector<SV>& v, SV s) { return std::find(v.begin(), v.end(), s) != v.end(); }

// Full names in the list that match the given long option (exact or abbreviated).
std::vector<SV> long_matches(const std::vector<SV>& v, SV given) {
    std::vector<SV> out;
    for (SV full : v) {
        if (long_match(given, full)) out.push_back(full);
    }
    return out;
}

// Result of reading a wrapper's options.
struct Scan {
    std::size_t next = 0;                            // first operand (or inner command)
    std::vector<std::pair<std::string, Word>> opts;  // option and value
    std::vector<Word> operands;                      // only with permute
    std::string unknown;                             // first unrecognized option

    bool has(SV o) const {
        return std::any_of(opts.begin(), opts.end(), [&](const auto& p) { return p.first == o; });
    }
    const Word* value(SV o) const {
        for (const auto& p : opts) {
            if (p.first == o) return &p.second;
        }
        return nullptr;
    }
};

struct OptTable {
    SV short_arg;            // short options with an argument (attached or in the next word)
    SV short_flag;           // short options without an argument
    std::vector<SV> long_arg;
    std::vector<SV> long_flag;
};

// permute: operands do not stop option parsing (su -c, runuser).
Scan scan_opts(const std::vector<Word>& w, std::size_t from, const OptTable& t, bool permute = false) {
    Scan s;
    std::size_t k = from;
    while (k < w.size()) {
        const std::string& x = w[k].text;
        if (x == "--") {
            ++k;
            if (permute) {
                for (; k < w.size(); ++k) s.operands.push_back(w[k]);
            }
            break;
        }
        if (x.size() < 2 || x[0] != '-') {
            if (!permute) break;
            s.operands.push_back(w[k++]);
            continue;
        }
        if (x[1] == '-') {
            std::size_t eq = x.find('=');
            std::string name = x.substr(0, eq);
            auto args = long_matches(t.long_arg, name);
            auto flags = long_matches(t.long_flag, name);
            if (!args.empty()) {
                Word v;
                if (eq != std::string::npos) {
                    v = make_word(x.substr(eq + 1));
                } else if (k + 1 < w.size()) {
                    v = w[++k];
                }
                for (SV full : args) s.opts.emplace_back(std::string(full), v);
            }
            for (SV full : flags) s.opts.emplace_back(std::string(full), Word{});
            if (args.empty() && flags.empty() && s.unknown.empty()) s.unknown = x;
            ++k;
            continue;
        }
        for (std::size_t j = 1; j < x.size(); ++j) {
            char ch = x[j];
            std::string f = std::string("-") + ch;
            if (t.short_arg.find(ch) != SV::npos) {
                if (j + 1 < x.size()) {
                    s.opts.emplace_back(f, make_word(x.substr(j + 1)));
                } else if (k + 1 < w.size()) {
                    s.opts.emplace_back(f, w[++k]);
                }
                break;
            }
            if (t.short_flag.find(ch) != SV::npos) {
                s.opts.emplace_back(f, Word{});
            } else if (s.unknown.empty()) {
                s.unknown = x;
            }
        }
        ++k;
    }
    s.next = k;
    return s;
}

std::vector<Word> tail(const std::vector<Word>& w, std::size_t from) {
    return from < w.size() ? std::vector<Word>(w.begin() + static_cast<std::ptrdiff_t>(from), w.end())
                           : std::vector<Word>{};
}

std::string join_text(const std::vector<Word>& w, std::size_t from) {
    std::string s;
    for (std::size_t i = from; i < w.size(); ++i) {
        if (i > from) s += ' ';
        s += w[i].text;
    }
    return s;
}

bool any_net(const std::vector<Word>& w, std::size_t from) {
    return std::any_of(w.begin() + static_cast<std::ptrdiff_t>(std::min(from, w.size())), w.end(),
                       [](const Word& x) { return x.net; });
}

enum class Own { None, Admin, OtherUser, Chroot, Service };

// Wrappers that take only options, N fixed operands and the inner command.
struct Wrapper {
    SV name;
    OptTable opts;
    int fixed = 0;           // operands before the command (timeout: duration; chroot: dir; flock: file)
    Own own = Own::None;     // own level (R30)
    bool assigns = false;    // accepts VAR=x before the command (sudo)
    bool no_cmd_ok = false;  // with no inner command it adds nothing
};

const std::vector<Wrapper>& wrappers() {
    static const std::vector<Wrapper> t = {
        {"sudo",
         {"ughpCDrtUTR", "AbEeHiKklnPSsVvBN",
          {"--user", "--group", "--host", "--prompt", "--close-from", "--chdir", "--role", "--type", "--other-user",
           "--command-timeout", "--chroot"},
          {"--preserve-env", "--login", "--shell", "--non-interactive", "--stdin", "--askpass", "--background",
           "--set-home", "--reset-timestamp", "--remove-timestamp", "--list", "--validate", "--version", "--help",
           "--edit", "--bell", "--preserve-groups"}},
         0, Own::Admin, true, true},
        {"doas", {"uC", "nsL", {}, {}}, 0, Own::Admin, false, true},
        {"pkexec", {"", "", {"--user"}, {"--disable-internal-agent", "--keep-cwd", "--version", "--help"}}, 0,
         Own::Admin, false, true},
        {"run0",
         {"ugD", "",
          {"--user", "--group", "--chdir", "--setenv", "--unit", "--property", "--description", "--slice", "--nice",
           "--machine", "--background"},
          {"--no-ask-password", "--slice-inherit"}},
         0, Own::Admin, false, true},
        {"chroot", {"", "", {"--userspec", "--groups"}, {"--skip-chdir"}}, 1, Own::Chroot, false, true},
        // Neutral wrappers: only the options in this table; any other one is WRAP.
        {"nice", {"n", "", {}, {}}, 0, Own::None, false, true},
        {"ionice", {"cnp", "", {}, {}}, 0},
        {"nohup", {"", "", {}, {}}, 0},
        {"setsid", {"", "", {}, {}}, 0},
        {"stdbuf", {"ioe", "", {}, {}}, 0},
        {"time", {"", "", {}, {}}, 0, Own::None, false, true},
        {"command", {"", "vV", {}, {}}, 0, Own::None, false, true},
        {"builtin", {"", "", {}, {}}, 0, Own::None, false, true},
        {"exec", {"", "", {}, {}}, 0, Own::None, false, true},
        {"timeout", {"sk", "", {}, {}}, 1},
        {"flock", {"wEc", "", {"--command"}, {}}, 1},  // -c carries code and is classified
        {"taskset", {"", "acp", {}, {"--all-tasks", "--cpu-list", "--pid"}}, 1},
        {"chrt",
         {"TPD", "bdfiorRamvp", {"--sched-runtime", "--sched-period", "--sched-deadline"},
          {"--batch", "--deadline", "--fifo", "--idle", "--other", "--rr", "--reset-on-fork", "--all-tasks", "--max",
           "--pid", "--verbose"}},
         1},
        {"strace", {"oeps", "f", {}, {}}, 0},
        {"systemd-run",
         {"upEHM", "tqGrdP",
          {"--unit", "--property", "--setenv", "--description", "--slice", "--host", "--machine", "--uid", "--gid",
           "--nice", "--working-directory", "--on-calendar", "--on-active", "--service-type"},
          {"--user", "--system", "--scope", "--wait", "--pty", "--pipe", "--quiet", "--collect", "--no-block",
           "--remain-after-exit", "--same-dir", "--shell", "--no-ask-password"}},
         0, Own::Service, false, true},
    };
    return t;
}

const Wrapper* find_wrapper(SV name) {
    for (const Wrapper& w : wrappers()) {
        if (w.name == name) return &w;
    }
    return nullptr;
}

// Non-shell interpreters.
struct Interp {
    std::vector<SV> names;   // name or prefix (python3.12 → python)
    SV code_short;           // -c, -e…: the code goes in the option
    std::vector<SV> code_long;
    SV arg_short;            // options with an argument (attached or next)
    SV attached;             // options whose argument is always attached
    std::vector<SV> arg_long;
    char inplace = 0;        // perl/ruby -i
    char module = 0;         // python -m
    char script = 0;         // php -f
};

const std::vector<Interp>& interps() {
    static const std::vector<Interp> t = {
        {{"python"}, "c", {}, "WX", "", {}, 0, 'm', 0},
        {{"perl"}, "eE", {}, "IMm", "lx0CFd", {}, 'i', 0, 0},
        {{"ruby"}, "e", {}, "rIEC", "x0WT", {}, 'i', 0, 0},
        {{"node", "nodejs"}, "ep", {"--eval", "--print"}, "r", "", {"--require", "--import", "--loader"}, 0, 0, 0},
        {{"php"}, "rBRE", {}, "cdz", "", {}, 0, 0, 'f'},
    };
    return t;
}

const Interp* find_interp(SV name) {
    for (const Interp& it : interps()) {
        for (SV n : it.names) {
            if (name == n) return &it;
            if (name.size() > n.size() && name.substr(0, n.size()) == n &&
                name.find_first_not_of("0123456789.", n.size()) == SV::npos)
                return &it;
        }
    }
    return nullptr;
}

const char* const kWhyOtherUser = "se ejecuta como otro usuario";
const char* const kWhyOtherUserEn = "runs as another user";

}  // namespace

// Access to the engine's private parts for the wrappers.
struct Unwrapper {
    Engine& e;
    const std::vector<Word>& w;
    const Stdin& in;
    int depth;
    std::string name;

    void wrap_unknown(const Scan& s) {
        if (s.unknown.empty()) return;
        e.emit("WRAP", name + " " + s.unknown, "opción no reconocida de " + name, "unrecognised option of " + name);
    }

    void own(Own o) {
        switch (o) {
            case Own::Admin: e.emit("R30", name); break;
            case Own::OtherUser: e.emit("R30", name, kWhyOtherUser, kWhyOtherUserEn); break;
            case Own::Chroot:
                e.emit("R30", name, "se ejecuta dentro de otro sistema (chroot)", "runs inside another root filesystem");
                break;
            case Own::Service:
                e.emit("R30", name, "lanza el comando como un servicio del sistema", "runs the command as a system service");
                break;
            case Own::None: break;
        }
    }

    void inner(const std::vector<Word>& cmd, const Stdin& inner_in, bool admin) {
        if (admin) ++e.admin_;
        e.argv(cmd, inner_in, depth);
        if (admin) --e.admin_;
    }

    void inner_code(const Word& cw, bool admin) {
        if (admin) ++e.admin_;
        e.code_word(cw, depth, in, fragment_of(w));
        if (admin) --e.admin_;
    }

    bool plain(const Wrapper& wr) {
        Scan s = scan_opts(w, 1, wr.opts);
        std::size_t k = s.next;
        for (int i = 0; i < wr.fixed && k < w.size(); ++i) ++k;
        if (wr.name == "flock") {  // flock FILE -c 'code'
            Scan more = scan_opts(w, k, wr.opts);
            if (s.unknown.empty()) s.unknown = more.unknown;
            for (auto& o : more.opts) s.opts.push_back(std::move(o));
            k = more.next;
        }
        wrap_unknown(s);
        own(wr.own);
        bool admin = wr.own == Own::Admin;
        if (wr.name == "flock") {
            for (SV o : {"-c", "--command"}) {
                if (const Word* v = s.value(o)) inner_code(*v, false);
            }
        }
        if (wr.name == "strace") {
            if (const Word* v = s.value("-o")) e.write_target(*v, false, "strace -o " + v->text);
        }
        if (wr.name == "command" && (s.has("-v") || s.has("-V"))) return true;
        // taskset/chrt -p and strace -p act on another process: there is no inner command.
        if ((wr.name == "taskset" || wr.name == "chrt") && (s.has("-p") || s.has("--pid"))) {
            e.rules(w, depth);
            return true;
        }
        if (wr.name == "chrt" && (s.has("-m") || s.has("--max"))) return true;
        if (wr.name == "sudo") {
            for (SV o : {"-e", "--edit", "-l", "--list", "-v", "--validate", "-k", "-K", "-V", "--version", "--help",
                         "--remove-timestamp", "--reset-timestamp"}) {
                if (s.has(o)) return true;
            }
        }
        std::size_t first_assign = k;
        if (wr.assigns) {
            while (k < w.size() && is_assign_word(w[k])) ++k;
        }
        if (k >= w.size()) {
            if (!wr.no_cmd_ok && wr.own == Own::None) e.emit("R31", name);
            // sudo -s/-i, doas -s, chroot… without a command open a shell that reads the input.
            if (wr.own == Own::Admin || wr.own == Own::Chroot) shell_input(admin);
            return true;
        }
        for (std::size_t i = first_assign; i < k; ++i) e.env_assign(w[i]);
        inner(tail(w, k), in, admin);
        return true;
    }

    void shell_input(bool admin) {
        if (admin) ++e.admin_;
        e.stdin_shell(in, depth, fragment_of(w));
        if (admin) --e.admin_;
    }

    bool env() {
        OptTable t{"uCS", "i", {"--split-string"}, {}};
        Scan s = scan_opts(w, 1, t);
        wrap_unknown(s);
        std::size_t k = s.next, first_assign = k;
        while (k < w.size() && (w[k].text == "-" || is_assign_word(w[k]))) ++k;
        const Word* split = s.value("-S");
        if (!split) split = s.value("--split-string");
        if (split || k < w.size()) {
            for (std::size_t i = first_assign; i < k; ++i) {
                if (w[i].text != "-") e.env_assign(w[i]);
            }
        }
        if (split) {
            Word cw = *split;
            for (std::size_t i = k; i < w.size(); ++i) {
                cw.text += " " + w[i].raw;
                cw.net = cw.net || w[i].net;
                cw.dynamic = cw.dynamic || w[i].dynamic;
            }
            e.code_word(cw, depth, in, fragment_of(w));
            return true;
        }
        if (k < w.size()) inner(tail(w, k), in, false);
        return true;
    }

    bool su() {
        OptTable t{"csgGwu", "lmpPf",
                   {"--command", "--shell", "--group", "--supp-group", "--whitelist-environment", "--user",
                    "--session-command"},
                   {"--login", "--preserve-environment", "--pty", "--fast"}};
        own(Own::OtherUser);
        if (name == "runuser") {
            Scan s = scan_opts(w, 1, t);
            if (s.has("-u") || s.has("--user")) {
                wrap_unknown(s);
                if (s.next < w.size()) {
                    inner(tail(w, s.next), in, true);
                } else {
                    shell_input(true);
                }
                return true;
            }
        }
        Scan s = scan_opts(w, 1, t, true);
        wrap_unknown(s);
        bool has_code = false;
        for (SV o : {"-c", "--command", "--session-command"}) {
            if (const Word* v = s.value(o)) {
                inner_code(*v, true);
                has_code = true;
            }
        }
        if (!has_code) shell_input(true);  // su, su - : the shell reads the input
        return true;
    }

    bool ssh() {
        OptTable t{"bcDEeFIiJLlmOoPpQRSWwB", "46AaCfGgKkMNnqsTtVvXxYy", {}, {}};
        Scan s = scan_opts(w, 1, t);
        wrap_unknown(s);
        std::size_t k = s.next;
        std::string host = k < w.size() ? w[k].text : std::string();
        e.emit("R30", "ssh " + host, "se ejecuta en otra máquina", "runs on another machine");
        for (const auto& [opt, v] : s.opts) {  // -o ProxyCommand=… / LocalCommand=…
            if (opt != "-o") continue;
            std::string code = ssh_option_code(v.text);
            if (code.empty()) continue;
            e.emit("R14c", fragment_of(w));
            e.code(code, depth + 1, Stdin{});
        }
        ++k;
        if (k < w.size() && w[k].text == "--") ++k;
        if (k < w.size()) {
            if (any_net(w, k)) e.emit("R14", fragment_of(w));
            e.code(join_text(w, k), depth + 1, in);
        } else {
            shell_input(false);  // ssh host without a command: the remote shell reads the input
        }
        return true;
    }

    bool watch() {
        OptTable t{"nq", "dtbecxpwrgCh", {"--interval", "--equexit"},
                   {"--differences", "--no-title", "--beep", "--errexit", "--chgexit", "--color", "--no-color",
                    "--exec", "--precise", "--no-wrap", "--no-linewrap"}};
        Scan s = scan_opts(w, 1, t);
        wrap_unknown(s);
        if (s.next >= w.size()) {
            e.emit("R31", name);
            return true;
        }
        if (any_net(w, s.next)) e.emit("R14", fragment_of(w));
        e.code(join_text(w, s.next), depth + 1, Stdin{});
        return true;
    }

    bool xargs() {
        OptTable t{"IndPaELs", "0rt", {}, {"--null", "--no-run-if-empty", "--verbose"}};
        Scan s = scan_opts(w, 1, t);
        wrap_unknown(s);
        std::vector<Word> cmd = tail(w, s.next);
        if (cmd.empty()) cmd.push_back(make_word("echo"));
        std::string rep;
        if (const Word* v = s.value("-I")) rep = v->text;

        // Known input (static echo/printf/cat, heredoc, here-string or find starting points).
        bool known = in.items.has_value() && !s.has("-a");
        std::vector<std::string> items = known ? *in.items : std::vector<std::string>{};
        if (known && !rep.empty() && (in.text_known || in.kind == Stdin::Kind::Text)) {
            items.clear();  // with -I each item is a whole line
            std::size_t b = 0;
            while (b <= in.text.size()) {
                std::size_t e2 = in.text.find('\n', b);
                if (e2 == std::string::npos) e2 = in.text.size();
                std::string line = in.text.substr(b, e2 - b);
                if (line.find_first_not_of(" \t") != std::string::npos) items.push_back(line);
                b = e2 + 1;
            }
        }
        std::string inner_name = base_name(cmd[0].text);
        if (in.net && (is_shell(inner_name) || find_interp(inner_name))) e.emit("R14", fragment_of(w));
        std::vector<Word> out;
        out.push_back(cmd[0]);
        if (!rep.empty() && cmd[0].text.find(rep) != std::string::npos) out[0].dynamic = true;
        bool replaced = false;
        for (std::size_t i = 1; i < cmd.size(); ++i) {
            if (!rep.empty() && cmd[i].text.find(rep) != std::string::npos) {
                replaced = true;
                if (known) {
                    for (const auto& it : items) {
                        Word x = cmd[i];
                        std::string& tx = x.text;
                        for (std::size_t p = tx.find(rep); p != std::string::npos; p = tx.find(rep, p + it.size()))
                            tx.replace(p, rep.size(), it);
                        out.push_back(x);
                    }
                } else {
                    Word x = cmd[i];
                    x.dynamic = true;
                    out.push_back(x);
                }
            } else {
                out.push_back(cmd[i]);
            }
        }
        if (!replaced) {
            if (known) {
                for (const auto& it : items) out.push_back(make_word(it));
            } else {
                out.push_back(make_word("<entrada>", true));
            }
        }
        inner(out, Stdin{}, false);
        return true;
    }

    // find … -exec cmd {} ; : the inner command is classified with {} replaced by the starting points.
    bool find() {
        std::vector<Word> starts = find_start_words(w);
        std::size_t k = 1;
        bool had_exec = false;
        while (k < w.size()) {
            const std::string& t = w[k].text;
            if (t != "-exec" && t != "-execdir" && t != "-ok" && t != "-okdir") {
                ++k;
                continue;
            }
            had_exec = true;
            ++k;
            std::vector<Word> cmd;
            while (k < w.size()) {
                const std::string& x = w[k].text;
                if (x == ";" || (x == "+" && !cmd.empty() && cmd.back().text == "{}")) {
                    ++k;
                    break;
                }
                cmd.push_back(w[k++]);
            }
            std::vector<Word> sub;
            for (const Word& x : cmd) {
                if (x.text.find("{}") == std::string::npos) {
                    sub.push_back(x);
                    continue;
                }
                for (const Word& st : starts) {
                    Word y = x;
                    for (std::size_t p = y.text.find("{}"); p != std::string::npos;
                         p = y.text.find("{}", p + st.text.size()))
                        y.text.replace(p, 2, st.text);
                    y.dynamic = y.dynamic || st.dynamic;
                    y.glob = y.glob || st.glob;
                    y.tilde = st.tilde;
                    sub.push_back(y);
                }
            }
            if (sub.empty()) {
                e.emit("PARSE", fragment_of(w));
            } else {
                inner(sub, Stdin{}, false);
            }
        }
        if (!had_exec) return false;
        e.rules(w, depth);  // with -exec it is not on the whitelist: at least caution
        return true;
    }

    // Replaces the placeholders ({}, {/}, {.}…) with each path, or appends the paths at the end.
    static std::vector<Word> with_paths(const std::vector<Word>& cmd, const std::vector<Word>& paths) {
        std::vector<Word> out{cmd[0]};
        bool placeholder = false;
        for (std::size_t i = 1; i < cmd.size(); ++i) {
            const Word& x = cmd[i];
            std::size_t b = x.text.find('{'), en = x.text.find('}');
            if (b == std::string::npos || en == std::string::npos || en < b || en - b > 3) {
                out.push_back(x);
                continue;
            }
            placeholder = true;
            for (const Word& p : paths) {
                Word y = x;
                y.text.replace(b, en - b + 1, p.text);
                y.dynamic = y.dynamic || p.dynamic;
                y.tilde = p.tilde;
                out.push_back(y);
            }
        }
        if (!placeholder) out.insert(out.end(), paths.begin(), paths.end());
        return out;
    }

    // fd PATTERN PATH -x cmd: like find -exec, with the search paths.
    bool fd() {
        std::size_t k = 1;
        while (k < w.size() && !in_list({"-x", "--exec", "-X", "--exec-batch"}, w[k].text)) ++k;
        if (k >= w.size()) return false;
        Cmd before = make_cmd(std::vector<Word>(w.begin(), w.begin() + static_cast<std::ptrdiff_t>(k)), false);
        std::vector<Word> paths(before.operands.size() > 1 ? before.operands.begin() + 1 : before.operands.end(),
                                before.operands.end());
        for (const auto& [opt, v] : before.values) {
            if (long_match(opt, "--search-path")) paths.push_back(v);
        }
        if (paths.empty()) paths.push_back(make_word("."));
        std::vector<Word> cmd;
        for (std::size_t i = k + 1; i < w.size() && w[i].text != ";"; ++i) cmd.push_back(w[i]);
        if (!cmd.empty()) inner(with_paths(cmd, paths), Stdin{}, false);
        e.rules(w, depth);  // with -x/-X it is not on the whitelist: at least caution
        return true;
    }

    bool busybox() {
        if (w.size() < 2 || w[1].text.empty() || w[1].text[0] == '-') {
            e.rules(w, depth);
            return true;
        }
        e.argv(tail(w, 1), in, depth);
        return true;
    }

    bool unshare() {
        std::size_t k = 1;
        while (k < w.size() && w[k].text.size() > 1 && w[k].text[0] == '-' && w[k].text != "--") ++k;
        if (k < w.size() && w[k].text == "--") ++k;
        if (k >= w.size()) {
            e.emit("R31", name, "abre un intérprete interactivo", "opens an interactive interpreter");
            shell_input(false);
            return true;
        }
        inner(tail(w, k), in, false);
        return true;
    }

    bool script() {
        OptTable t{"cTIOBEm", "aefqk",
                   {"--command", "--log-timing", "--log-in", "--log-out", "--log-io", "--echo", "--logging-format",
                    "--output-limit"},
                   {"--append", "--return", "--flush", "--force", "--quiet"}};
        Scan s = scan_opts(w, 1, t);
        wrap_unknown(s);
        bool append = s.has("-a") || s.has("--append");
        if (s.next < w.size()) {
            e.write_target(w[s.next], append, "script " + w[s.next].text);
        } else {
            e.write_target(make_word("typescript"), append, "script typescript");
        }
        const Word* c = s.value("-c");
        if (!c) c = s.value("--command");
        if (c) {
            e.code_word(*c, depth, in, fragment_of(w));
        } else {
            e.emit("R31", name, "abre un intérprete interactivo", "opens an interactive interpreter");
            shell_input(false);
        }
        return true;
    }

    // at/batch read code from the input to run it later.
    bool at() {
        OptTable t{"fqt", "mMlrdcbvVK", {}, {}};
        Scan s = scan_opts(w, 1, t);
        wrap_unknown(s);
        if (s.has("-l") || s.has("-r") || s.has("-d") || s.has("-c")) {
            e.rules(w, depth);
            return true;
        }
        std::string frag = fragment_of(w);
        e.emit("R14c", frag, "programa código para ejecutarlo más tarde", "schedules code to run later");
        if (!s.has("-f") && in.kind != Stdin::Kind::None) e.stdin_code(in, true, depth, frag);
        return true;
    }

    // parallel cmd ::: args: "cmd args" is classified as code, as parallel does.
    bool parallel() {
        std::size_t sep = 1;
        while (sep < w.size() && w[sep].text.rfind(":::", 0) != 0) ++sep;
        OptTable t{"jPSNnd", "kvqu0",
                   {"--jobs", "--sshlogin", "--colsep", "--delimiter", "--max-args", "--joblog", "--results", "--tmpdir"},
                   {"--keep-order", "--verbose", "--quiet", "--null", "--dry-run", "--ungroup", "--line-buffer",
                    "--progress", "--bar"}};
        std::vector<Word> head(w.begin(), w.begin() + static_cast<std::ptrdiff_t>(sep));
        Scan s = scan_opts(head, 1, t);
        wrap_unknown(s);
        std::vector<Word> cmd = tail(head, s.next);
        std::vector<Word> args;
        bool known = true;
        if (sep < w.size()) {
            for (std::size_t i = sep; i < w.size(); ++i) {
                const std::string& x = w[i].text;
                if (x.rfind("::::", 0) == 0) known = false;  // arguments from a file
                if (x.rfind(":::", 0) == 0) continue;
                args.push_back(w[i]);
            }
        } else if (in.items) {
            for (const auto& it : *in.items) args.push_back(make_word(it));
        } else {
            known = false;
        }
        if (!known || args.empty()) args = {make_word("$PARALLEL_ARG", true)};
        if (args.size() > 64) args.resize(64);
        for (const Word& a : args) {
            std::vector<Word> line = cmd.empty() ? std::vector<Word>{a} : with_paths(cmd, {a});
            std::string text;
            for (std::size_t i = 0; i < line.size(); ++i) text += (i ? " " : "") + line[i].text;
            e.code(text, depth + 1, Stdin{});
        }
        return true;
    }

    bool shell() {
        std::size_t k = 1;
        bool has_c = false, read_stdin = false;
        std::string unknown;
        while (k < w.size()) {
            const std::string& t = w[k].text;
            if (t == "--" || t == "-") {
                ++k;
                break;
            }
            if (t.size() < 2 || (t[0] != '-' && t[0] != '+')) break;
            if (t[0] == '-' && t[1] == '-') {
                if (t == "--rcfile" || t == "--init-file") {
                    k += 2;
                    continue;
                }
                if (long_matches({"--login", "--norc", "--noprofile", "--posix", "--restricted", "--verbose",
                                  "--noediting", "--debugger", "--help", "--version", "--dump-strings",
                                  "--dump-po-strings", "--pretty-print"},
                                 t)
                        .empty() &&
                    unknown.empty())
                    unknown = t;
                ++k;
                continue;
            }
            std::size_t skip = 0;
            for (std::size_t j = 1; j < t.size(); ++j) {
                char ch = t[j];
                if (ch == 'c') {
                    has_c = true;
                } else if (ch == 'o' || ch == 'O') {
                    ++skip;
                } else if (ch == 's') {
                    read_stdin = true;
                } else if (SV("abefhiklmnprtuvxBCEHPT").find(ch) == SV::npos && unknown.empty()) {
                    unknown = t;
                }
            }
            k += 1 + skip;
        }
        if (!unknown.empty()) {
            e.emit("WRAP", name + " " + unknown, "opción no reconocida de " + name, "unrecognised option of " + name);
        }
        std::string frag = fragment_of(w);
        if (has_c) {
            if (k >= w.size()) {
                e.emit("PARSE", frag);
            } else {
                e.code_word(w[k], depth, in, frag);
            }
            return true;
        }
        bool dev_stdin = k < w.size() && in_list({"/dev/stdin", "/proc/self/fd/0", "/dev/fd/0"}, w[k].text);
        if (k < w.size() && !read_stdin && !dev_stdin) {
            e.script_word(w[k], frag);
        } else {
            e.stdin_code(in, true, depth, frag);
        }
        return true;
    }

    bool interp(const Interp& it) {
        std::size_t k = 1;
        std::optional<Word> code, script;
        bool inplace = false, module = false, forced_stdin = false;
        while (k < w.size() && !code && !script && !module) {
            const std::string& t = w[k].text;
            if (t == "--") {
                ++k;
                break;
            }
            if (t == "-") {
                forced_stdin = true;
                ++k;
                break;
            }
            if (t.size() < 2 || t[0] != '-') break;
            if (t[1] == '-') {
                std::size_t eq = t.find('=');
                std::string nm = t.substr(0, eq);
                if (!long_matches(it.code_long, nm).empty()) {
                    if (eq != std::string::npos) {
                        code = make_word(t.substr(eq + 1));
                    } else if (k + 1 < w.size()) {
                        code = w[++k];
                    }
                } else if (!long_matches(it.arg_long, nm).empty() && eq == std::string::npos) {
                    ++k;
                }
                ++k;
                continue;
            }
            for (std::size_t j = 1; j < t.size(); ++j) {
                char ch = t[j];
                std::string rest = t.substr(j + 1);
                auto take = [&]() -> std::optional<Word> {
                    if (!rest.empty()) return make_word(rest);
                    if (k + 1 < w.size()) return w[++k];
                    return std::nullopt;
                };
                if (it.code_short.find(ch) != SV::npos) {
                    code = take();
                    if (!code) code = make_word("");
                    break;
                }
                if (it.inplace && ch == it.inplace) {
                    inplace = true;
                    break;
                }
                if (it.module && ch == it.module) {
                    module = true;
                    break;
                }
                if (it.script && ch == it.script) {
                    script = take();
                    break;
                }
                if (it.arg_short.find(ch) != SV::npos) {
                    take();
                    break;
                }
                if (it.attached.find(ch) != SV::npos) break;
            }
            ++k;
        }
        std::string frag = fragment_of(w);
        if (inplace) {
            e.emit("R29", frag);
            // The remaining words are the edited files (or a script and then the files).
            for (std::size_t j = k; j < w.size(); ++j) {
                if (in_F(normalize_path(w[j].text, w[j].tilde))) {
                    e.emit("R7d", frag);
                    break;
                }
            }
        }
        if (code) {
            e.emit(code->net ? "R14" : "R14c", frag);
            return true;
        }
        if (module) {
            e.emit("R31", frag, "ejecuta un módulo que el verificador no puede revisar",
                   "runs a module the checker cannot review");
            return true;
        }
        if (!script && !forced_stdin && k < w.size()) script = w[k];
        if (script) {
            e.script_word(*script, frag);
        } else {
            e.stdin_code(in, false, depth, frag);
        }
        return true;
    }

    bool eval() {
        std::string frag = fragment_of(w);
        e.emit(any_net(w, 1) ? "R14" : "R14c", frag);
        e.code(join_text(w, 1), depth + 1, in);
        return true;
    }

    bool source() {
        std::string frag = fragment_of(w);
        if (w.size() >= 2) {
            const Word& f = w[1];
            if (f.net) {
                e.emit("R14", frag);
                return true;
            }
            if (in_list({"/dev/stdin", "-", "/proc/self/fd/0", "/dev/fd/0"}, f.text) && in.kind != Stdin::Kind::None) {
                e.stdin_code(in, true, depth, frag);
                if (in.net) return true;
            }
        }
        e.emit("R14c", frag);
        return true;
    }
};

bool Engine::unwrap(const std::string& name, const std::vector<Word>& w, const Stdin& in, int depth) {
    Unwrapper u{*this, w, in, depth, name};
    if (const Wrapper* wr = find_wrapper(name)) return u.plain(*wr);
    if (name == "env") return u.env();
    if (name == "su" || name == "runuser") return u.su();
    if (name == "ssh") return u.ssh();
    if (name == "watch") return u.watch();
    if (name == "xargs") return u.xargs();
    if (name == "find") return u.find();
    if (name == "fd" || name == "fdfind") return u.fd();
    if (name == "busybox") return u.busybox();
    if (name == "unshare") return u.unshare();
    if (name == "script") return u.script();
    if (name == "at" || name == "batch") return u.at();
    if (name == "parallel") return u.parallel();
    if (is_shell(name)) return u.shell();
    if (const Interp* it = find_interp(name)) return u.interp(*it);
    if (name == "eval") return u.eval();
    if (name == "source" || name == ".") return u.source();
    return false;
}

void Engine::code_word(const Word& cw, int depth, const Stdin& in, std::string_view fragment) {
    if (cw.net) {
        emit("R14", fragment);
    } else if (cw.dynamic) {
        emit("R14c", fragment);
    }
    code(cw.text, depth + 1, in);
}

void Engine::script_word(const Word& sw, std::string_view fragment) {
    if (sw.net) {
        emit("R14", fragment);
    } else if (sw.proc_sub || sw.dynamic) {
        emit("R14c", fragment);
    } else {
        emit("R31", fragment, "ejecuta un script que el verificador no puede revisar",
             "runs a script the checker cannot review");
    }
}

void Engine::stdin_shell(const Stdin& in, int depth, std::string_view fragment) {
    if (in.kind != Stdin::Kind::None) stdin_code(in, true, depth, fragment);
}

void Engine::stdin_code(const Stdin& in, bool shell, int depth, std::string_view fragment) {
    switch (in.kind) {
        case Stdin::Kind::Pipe:
            if (in.net) {
                emit("R14", fragment);
            } else if (shell && in.text_known) {
                code(in.text, depth + 1, Stdin{});  // R14b: echo 'code' | sh
            } else {
                emit("R14c", fragment);
            }
            break;
        case Stdin::Kind::Text:
            if (in.net) {
                emit("R14", fragment);
            } else if (shell) {
                code(in.text, depth + 1, Stdin{});
            } else {
                emit("R14c", fragment);
            }
            break;
        case Stdin::Kind::File:
            emit("R14c", fragment);
            break;
        case Stdin::Kind::None:
            emit("R31", fragment, "abre un intérprete interactivo", "opens an interactive interpreter");
            break;
    }
}

}  // namespace dale::risk
