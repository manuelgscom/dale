#include "risk/rules.hpp"

#include <algorithm>
#include <cctype>

#include "risk/paths.hpp"

namespace dale::risk {
namespace {

using SV = std::string_view;

bool starts_with(SV s, SV p) { return s.substr(0, p.size()) == p; }
bool one_of(SV s, std::initializer_list<SV> l) { return std::find(l.begin(), l.end(), s) != l.end(); }

std::string lower(SV s) {
    std::string o(s);
    for (char& c : o) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return o;
}

// ------------------------------------------------------------ option parsing

struct OptSpec {
    std::vector<SV> names;
    SV short_arg;               // short options that take an argument
    std::vector<SV> long_arg;   // long options that take an argument (unless given with "=")
};

const std::vector<OptSpec>& opt_specs() {
    static const std::vector<OptSpec> t = {
        {{"cp", "mv", "ln"}, "tS", {"--target-directory", "--suffix"}},
        {{"install"}, "mogtS", {"--mode", "--owner", "--group", "--target-directory", "--suffix"}},
        {{"truncate"}, "sr", {"--size", "--reference"}},
        {{"kill"}, "sn", {}},
        {{"killall"}, "suoy", {"--signal", "--user", "--older-than", "--younger-than"}},
        {{"pkill", "pgrep"}, "uUgGPstF", {"--signal", "--euid", "--uid", "--group", "--pgroup", "--parent",
                                          "--session", "--terminal", "--pidfile"}},
        {{"sed"}, "efl", {"--expression", "--file", "--line-length"}},
        {{"awk", "gawk", "mawk", "nawk"}, "Ffvei", {"--field-separator", "--assign", "--file", "--source", "--include"}},
        {{"git"}, "Ccme", {"--git-dir", "--work-tree", "--namespace", "--expire"}},
        {{"docker", "podman"}, "Hc", {"--host", "--context", "--config", "--log-level", "--url", "--connection"}},
        {{"kubectl"}, "nlocfs", {"--namespace", "--selector", "--output", "--context", "--kubeconfig", "--cluster",
                                 "--user", "--server", "--filename", "--container"}},
        {{"systemctl"}, "HMtpnos", {"--host", "--machine", "--type", "--property", "--lines", "--output", "--signal",
                                    "--state", "--root", "--kill-whom", "--what", "--job-mode"}},
        {{"curl"}, "oDcdFTXHuAebrmxwKECUzYytQ",
         {"--output", "--data", "--data-raw", "--data-binary", "--data-urlencode", "--data-ascii", "--form",
          "--form-string", "--upload-file", "--request", "--header", "--user", "--user-agent", "--referer",
          "--cookie", "--cookie-jar", "--dump-header", "--max-time", "--connect-timeout", "--proxy", "--write-out",
          "--config", "--retry", "--output-dir", "--json", "--url", "--resolve", "--cacert", "--cert", "--key",
          "--stderr", "--hsts", "--alt-svc", "--trace", "--trace-ascii", "--range"}},
        {{"wget"}, "OoaPtTwUiBQlDARIXe",
         {"--output-document", "--output-file", "--append-output", "--directory-prefix", "--post-data",
          "--post-file", "--method", "--body-data", "--body-file", "--header", "--user-agent", "--tries",
          "--timeout", "--input-file", "--user", "--password", "--execute", "--config"}},
        {{"tar"}, "fCIF", {"--file", "--directory", "--to-command", "--use-compress-program", "--info-script",
                           "--new-volume-script", "--rsh-command", "--checkpoint-action", "--exclude"}},
        {{"scp"}, "cFiJloPSX", {}},
        {{"sftp"}, "BbcDFiJloPRSs", {}},
        {{"usermod"}, "cdefgGlpsuRPZ", {"--home", "--shell", "--login", "--groups", "--expiredate", "--comment"}},
        {{"chage"}, "dEImMWR", {"--expiredate", "--lastday", "--inactive", "--mindays", "--maxdays", "--warndays"}},
        {{"gpasswd"}, "adAM", {"--add", "--delete", "--administrators", "--members"}},
        {{"pv"}, "oLsNBdi", {"--output", "--rate-limit", "--size", "--name", "--buffer-size", "--watchfd",
                             "--interval"}},
        {{"bat", "batcat"}, "lmrH", {"--pager", "--language", "--style", "--theme", "--map-syntax", "--line-range"}},
        {{"fd", "fdfind"}, "edEtSjcCo", {"--extension", "--max-depth", "--min-depth", "--exact-depth", "--exclude",
                                          "--type", "--size", "--changed-within", "--changed-before", "--threads",
                                          "--color", "--owner", "--base-directory", "--search-path",
                                          "--max-results", "--batch-size", "--format"}},
        {{"crontab"}, "u", {}},
        {{"passwd"}, "xnwi", {}},
        {{"smartctl"}, "dTlbrtsoSFvn", {}},
        {{"iptables", "ip6tables"}, "tAIDRNEjpsdiomgw", {"--table", "--append", "--insert", "--delete", "--jump",
                                                        "--protocol", "--source", "--destination", "--match"}},
        {{"rsync"}, "ef", {"--rsh", "--filter"}},
        {{"date"}, "dfrs", {"--date", "--file", "--reference", "--set"}},
        {{"apt", "apt-get"}, "oct", {}},
        {{"apt-mark"}, "ocf", {"--option", "--config-file", "--file"}},
        {{"timedatectl"}, "HMp", {"--host", "--machine", "--property"}},
        {{"pip", "pip3", "pipx"}, "rcei", {"--requirement", "--constraint", "--editable", "--index-url", "--target"}},
        {{"cryptsetup"}, "dcsh", {}},
        {{"chown", "chgrp"}, "", {"--from", "--reference"}},
        {{"sort"}, "okStT", {"--output", "--key", "--field-separator", "--buffer-size", "--temporary-directory"}},
        {{"nft"}, "fI", {"--file"}},
        {{"man"}, "PLMSm", {"--pager", "--locale", "--manpath", "--sections"}},
        {{"tree"}, "oLPIH", {}},
        {{"journalctl"}, "upnobtgDMSUIF", {"--unit", "--priority", "--lines", "--output", "--since", "--until"}},
        {{"yq"}, "o", {"--output-format"}},
        {{"less", "more"}, "oOxyhbjzp", {"--log-file"}},
    };
    return t;
}

const OptSpec* spec_for(SV name) {
    for (const auto& s : opt_specs()) {
        if (std::find(s.names.begin(), s.names.end(), name) != s.names.end()) return &s;
    }
    return nullptr;
}

Word plain_word(std::string text) {
    Word w;
    w.raw = text;
    w.text = std::move(text);
    return w;
}

// ------------------------------------------------------------ predicate helpers

bool any_path(const Cmd& c, bool (*pred)(SV)) {
    return std::any_of(c.paths.begin(), c.paths.end(), [&](const std::string& p) { return pred(p); });
}

bool rec_r(const Cmd& c) { return c.has("-r") || c.has("-R") || c.has("--recursive"); }
bool rec_upper(const Cmd& c) { return c.has("-R") || c.has("--recursive"); }

// Target of cp/mv/install/ln: -t DIR or the last operand.
std::string dest_path(const Cmd& c) {
    if (const Word* t = c.value("-t")) return normalize_path(t->text, t->tilde);
    if (const Word* t = c.value("--target-directory")) return normalize_path(t->text, t->tilde);
    if (c.operands.size() >= 2) return c.paths.back();
    return {};
}

std::vector<std::string> source_paths(const Cmd& c) {
    if (c.value("-t") || c.value("--target-directory")) return c.paths;
    if (c.paths.size() < 2) return {};
    return std::vector<std::string>(c.paths.begin(), c.paths.end() - 1);
}

// Effective targets: the target and, in case it is a directory, target/basename(source).
std::vector<std::string> dest_paths(const Cmd& c) {
    std::string d = dest_path(c);
    if (d.empty()) return {};
    std::vector<std::string> out{d};
    for (const std::string& src : source_paths(c)) out.push_back(normalize_path(d + "/" + base_name(src)));
    return out;
}

bool any_dest(const Cmd& c, bool (*pred)(SV)) {
    auto d = dest_paths(c);
    return std::any_of(d.begin(), d.end(), [&](const std::string& p) { return pred(p); });
}

std::vector<std::string> dd_targets(const Cmd& c) {
    std::vector<std::string> out;
    for (const auto& a : c.args) {
        if (starts_with(a.text, "of=")) out.push_back(normalize_path(SV(a.text).substr(3)));
    }
    return out;
}

bool tee_append(const Cmd& c) { return c.has("-a") || c.has("--append"); }

// Operand that starts with an expansion ($X, ${…}, $(…), `…`) followed by "/" (R2).
// $HOME and $PWD do not count: they are normalized to ~ and ".".
bool var_slash(const Word& w) {
    const std::string& t = w.text;
    if (!w.dynamic || t.size() < 3 || (t[0] != '$' && t[0] != '`')) return false;
    std::size_t k = 0;
    std::string name;
    if (t[0] == '`') {
        k = t.find('`', 1);
        if (k == std::string::npos) return false;
        ++k;
    } else if (t[1] == '(' || t[1] == '{') {
        char open = t[1], close = open == '(' ? ')' : '}';
        int level = 0;
        for (k = 1; k < t.size(); ++k) {
            if (t[k] == open) ++level;
            if (t[k] == close && --level == 0) break;
        }
        if (k >= t.size()) return false;
        if (open == '{') {
            std::size_t e = 2;
            while (e < k && (std::isalnum(static_cast<unsigned char>(t[e])) || t[e] == '_')) ++e;
            name = t.substr(2, e - 2);
        }
        ++k;
    } else {
        k = 1;
        while (k < t.size() && (std::isalnum(static_cast<unsigned char>(t[k])) || t[k] == '_')) ++k;
        if (k == 1) ++k;  // $1, $@…
        name = t.substr(1, k - 1);
    }
    return name != "HOME" && name != "PWD" && k < t.size() && t[k] == '/';
}

// systemd unit without a suffix (.service, .socket, .target).
std::string unit_base(SV u) {
    for (SV suf : {SV(".service"), SV(".socket"), SV(".target")}) {
        if (u.size() > suf.size() && u.substr(u.size() - suf.size()) == suf) return std::string(u.substr(0, u.size() - suf.size()));
    }
    return std::string(u);
}

bool protected_unit(SV u) {
    std::string b = unit_base(u);
    return starts_with(b, "systemd-") ||
           one_of(b, {"ssh", "sshd", "dbus", "NetworkManager", "network-manager", "networking", "network", "gdm",
                      "gdm3", "sddm", "lightdm", "display-manager", "polkit", "udev"});
}

const std::vector<SV> kQueryVerbs = {"status",   "list-units", "list-unit-files", "list-timers", "is-active",
                                     "is-enabled", "is-failed", "show",           "cat"};

bool protected_process(SV p) {
    return one_of(p, {"systemd", "init", "sshd", "dbus-daemon", "Xorg", "Xwayland", "gnome-shell", "plasmashell",
                      "gdm", "sddm", "lightdm"});
}

// kill: the first -N/-SIG argument is the signal; PID 1 or -1 after it is danger.
bool kill_all(const Cmd& c) {
    const auto& a = c.args;
    if (a.empty()) return false;
    std::size_t i = 0;
    const std::string& first = a[0].text;
    if (first == "-s" || first == "-n" || first == "--signal") {
        i = 2;
    } else if (first.size() > 1 && first[0] == '-' && first != "--") {
        if (first == "-l" || first == "-L" || first == "--list" || first == "--table") return false;
        if (a.size() == 1 && first == "-1") return true;  // a lone kill -1: conservative
        i = 1;
    }
    if (i < a.size() && a[i].text == "--") ++i;
    for (; i < a.size(); ++i) {
        if (a[i].text == "1" || a[i].text == "-1") return true;
    }
    return false;
}

bool disk_listing(const Cmd& c) {
    if (c.has("-l") || c.has("--list") || c.has("-p") || c.has("--print")) return true;
    if (c.name == "sfdisk" && (c.has("-d") || c.has("--dump"))) return true;
    if (c.name == "parted" && c.operands.size() >= 2) {
        for (std::size_t i = 1; i < c.operands.size(); ++i) {
            if (!one_of(c.operands[i].text, {"print", "free", "unit", "s", "B", "kB", "MB", "MiB", "GB", "GiB",
                                             "TB", "TiB", "%", "cyl", "chs", "compact"}))
                return false;
        }
        return true;
    }
    return false;
}

bool fw_listing(const Cmd& c) {
    if (c.name == "iptables" || c.name == "ip6tables")
        return c.has("-L") || c.has("-S") || c.has("--list") || c.has("--list-rules");
    if (c.name == "nft") return c.op(0) == "list";
    if (c.name == "ufw") return c.op(0) == "status" || c.op(0) == "show" || (c.op(0) == "app" && c.op(1) == "list");
    if (c.name == "firewall-cmd") {
        if (c.flags.empty()) return false;
        return std::all_of(c.flags.begin(), c.flags.end(), [](const std::string& f) {
            return starts_with(f, "--list") || starts_with(f, "--get") || starts_with(f, "--query") ||
                   f == "--state" || f == "--zone" || f == "--permanent";
        });
    }
    return false;
}

// ---- ip / nmcli
struct IpParts {
    std::string obj, verb;
    std::vector<std::string> rest;
    bool batch = false;
};

IpParts ip_parts(const Cmd& c) {
    IpParts ip;
    std::size_t k = 0;
    const auto& a = c.args;
    while (k < a.size() && a[k].text.size() > 1 && a[k].text[0] == '-') {
        const std::string& t = a[k].text;
        if (one_of(t, {"-b", "-batch", "--batch", "-force"})) ip.batch = true;
        k += one_of(t, {"-f", "-family", "-n", "-netns", "-b", "-batch", "-rc", "-rcvbuf", "-l", "-loops"}) ? 2 : 1;
    }
    if (k < a.size()) ip.obj = a[k++].text;
    if (k < a.size()) ip.verb = a[k++].text;
    for (; k < a.size(); ++k) ip.rest.push_back(a[k].text);
    return ip;
}

bool abbrev_of(SV given, SV full) { return !given.empty() && starts_with(full, given); }

bool ip_read_only(const Cmd& c) {
    IpParts ip = ip_parts(c);
    return !ip.batch && (ip.verb.empty() || one_of(ip.verb, {"show", "list", "ls", "lst", "sh", "get"}));
}

// Leaves the machine without network (R15).
bool ip_cuts_network(const Cmd& c) {
    IpParts ip = ip_parts(c);
    auto in_rest = [&](SV x) { return std::find(ip.rest.begin(), ip.rest.end(), x) != ip.rest.end(); };
    if (abbrev_of(ip.obj, "link") && abbrev_of(ip.verb, "set")) return in_rest("down");
    if (abbrev_of(ip.obj, "route")) {
        if (abbrev_of(ip.verb, "delete")) return in_rest("default");
        if (abbrev_of(ip.verb, "flush")) return ip.rest.empty() || in_rest("default") || in_rest("all");
    }
    if (abbrev_of(ip.obj, "address") || ip.obj == "addr") return abbrev_of(ip.verb, "flush");
    return false;
}

// ifconfig with no operand or a single interface only shows the configuration.
bool ifconfig_read_only(const Cmd& c) {
    return c.operands.size() <= 1 && std::all_of(c.flags.begin(), c.flags.end(), [](const std::string& f) {
               return one_of(f, {"-a", "-s", "-v", "-S"});
           });
}

// ifconfig IF down / del ADDR / 0.0.0.0: takes the interface or its address away (R15, like ip link set down).
bool ifconfig_cuts_network(const Cmd& c) {
    for (std::size_t i = 1; i < c.operands.size(); ++i) {
        const std::string& t = c.operands[i].text;
        if (t == "down" || t == "del") return true;
        bool addr_slot = i == 1 || c.operands[i - 1].text == "inet";
        if (addr_slot && (t == "0" || t == "0.0.0.0")) return true;
    }
    return false;
}

// chpasswd reads "user:password" lines from stdin; root is only known when the input is literal
// text (echo/printf/cat, heredoc, here-string). A line that starts with "root:" changes root's password.
bool chpasswd_root(const Cmd& c) {
    SV t = c.input;
    while (!t.empty()) {
        std::size_t e = t.find('\n');
        SV line = t.substr(0, e);
        line.remove_prefix(std::min(line.find_first_not_of(" \t"), line.size()));
        if (starts_with(line, "root:")) return true;
        if (e == SV::npos) break;
        t.remove_prefix(e + 1);
    }
    return false;
}

std::vector<std::string> nmcli_ops(const Cmd& c) {
    std::vector<std::string> ops;
    for (std::size_t k = 0; k < c.args.size(); ++k) {
        const std::string& t = c.args[k].text;
        if (t.size() > 1 && t[0] == '-') {
            if (one_of(t, {"-f", "--fields", "-g", "--get-values", "-c", "--colors", "-e", "--escape", "-m", "--mode",
                           "-w", "--wait"}))
                ++k;
            continue;
        }
        ops.push_back(t);
    }
    return ops;
}

bool nmcli_read_only(const Cmd& c) {
    auto ops = nmcli_ops(c);
    if (ops.size() <= 1) return true;
    if (one_of(ops[1], {"hostname", "logging"})) return ops.size() == 2;
    if (ops[1] == "wifi") return ops.size() >= 3 && ops[2] == "list";
    return one_of(ops[1], {"status", "show", "list", "permissions", "connectivity"});
}

// Leaves the machine without network (R15): networking off, any radio off, a device disconnected or
// down (like ip link set IF down), or a connection taken down. nmcli takes any prefix of an object or
// verb and picks the first match, so "d" is "disconnect" for devices and "down" for connections.
bool nmcli_cuts_network(const Cmd& c) {
    auto ops = nmcli_ops(c);
    if (ops.size() < 2) return false;
    const std::string &obj = ops[0], &verb = ops[1];
    if (abbrev_of(obj, "networking")) return verb == "off";
    if (abbrev_of(obj, "radio")) return ops.size() >= 3 && ops[2] == "off";
    if (abbrev_of(obj, "device")) return abbrev_of(verb, "disconnect") || abbrev_of(verb, "down");
    if (abbrev_of(obj, "connection")) return abbrev_of(verb, "down");
    return false;
}

// ---- git
std::string git_sub(const Cmd& c) { return c.op(0); }

// Index in c.args of the git subcommand (skips -C/-c and their values).
std::size_t git_sub_index(const Cmd& c) {
    for (std::size_t i = 0; i < c.args.size(); ++i) {
        const std::string& t = c.args[i].text;
        if (t == "-C" || t == "-c" || t == "--git-dir" || t == "--work-tree" || t == "--namespace") {
            ++i;
            continue;
        }
        if (t.empty() || t[0] != '-') return i;
    }
    return c.args.size();
}

bool git_whole_tree(const Cmd& c) {
    for (std::size_t i = 1; i < c.operands.size(); ++i) {
        const std::string& t = c.operands[i].text;
        if (c.paths[i] == "." || one_of(t, {":/", ":", "*", ":/*", ":/."})) return true;
    }
    return false;
}

bool git_danger(const Cmd& c) {
    std::string s = git_sub(c);
    if (s == "push") {
        if (c.has("--force") || c.has("-f") || c.has("--mirror") || c.has("--delete") || c.has("-d") ||
            c.has("--prune"))
            return true;
        for (std::size_t i = 1; i < c.operands.size(); ++i) {
            if (starts_with(c.operands[i].text, "+") || starts_with(c.operands[i].text, ":")) return true;
        }
        return false;
    }
    if (s == "reset") return c.has("--hard");
    if (s == "clean") return (c.has("-f") || c.has("--force")) && (c.has("-d") || c.has("-x") || c.has("-X"));
    if (s == "checkout") return c.has("-f") || c.has("--force") || git_whole_tree(c);
    if (s == "switch") return c.has("-f") || c.has("--force") || c.has("--discard-changes");
    if (s == "rm") return (c.has("-r") || c.has("-R")) && git_whole_tree(c);
    if (s == "reflog") return c.op(1) == "expire";
    if (s == "gc") {
        const Word* v = c.value("--prune");
        return v && v->text == "now";
    }
    if (s == "prune") {
        const Word* v = c.value("--expire");
        return v && v->text == "now";
    }
    if (s == "stash") return c.op(1) == "clear";
    if (s == "filter-repo") return true;
    if (s == "restore") {
        bool staged_only = (c.has("--staged") || c.has("-S")) && !(c.has("--worktree") || c.has("-W"));
        return !staged_only && git_whole_tree(c);
    }
    if (s == "filter-branch") return true;
    if (s == "branch") return c.has("-D") || ((c.has("-d") || c.has("--delete")) && (c.has("-f") || c.has("--force")));
    return false;
}

// -c before the subcommand (git -c key=value …).
bool git_global_c(const Cmd& c) {
    for (std::size_t i = 0; i < c.args.size(); ++i) {
        const std::string& t = c.args[i].text;
        if (t == "-C" || t == "--git-dir" || t == "--work-tree" || t == "--namespace") {
            ++i;
            continue;
        }
        if (t == "-c" || starts_with(t, "-c")) return true;
        if (t.empty() || t[0] != '-') return false;
    }
    return false;
}

bool git_read_only(const Cmd& c) {
    if (git_global_c(c) || c.has_prefix("--output")) return false;
    std::string s = git_sub(c);
    if (s.empty()) return !c.flags.empty() && std::all_of(c.flags.begin(), c.flags.end(), [](const std::string& f) {
        return f == "--version" || f == "--help";
    });
    if (s == "grep") return !(c.has("-O") || c.has("--open-files-in-pager"));
    if (one_of(s, {"status", "log", "diff", "show", "blame", "ls-files", "rev-parse", "describe", "shortlog",
                   "version", "help", "cat-file", "ls-remote", "ls-tree", "rev-list", "show-ref", "merge-base",
                   "for-each-ref", "count-objects"}))
        return true;
    if (s == "reflog") return !one_of(c.op(1), {"expire", "delete"});
    if (s == "branch") {
        for (const auto& f : c.flags) {
            if (!one_of(f, {"-a", "-r", "-l", "-v", "--list", "--all", "--remotes", "--verbose", "--merged",
                            "--no-merged", "--contains", "--no-contains", "--show-current", "--sort", "--color",
                            "--no-color", "--format"}))
                return false;
        }
        return c.operands.size() == 1 || c.has("-l") || c.has("--list") || c.has("--merged") ||
               c.has("--no-merged") || c.has("--contains") || c.has("--no-contains");
    }
    if (s == "remote") return c.operands.size() == 1 || one_of(c.op(1), {"show", "get-url"});
    if (s == "tag") {
        if (c.has("-d") || c.has("--delete") || c.has("-a") || c.has("-s") || c.has("-f") || c.has("-m")) return false;
        return c.operands.size() == 1 || c.has("-l") || c.has("--list") || c.has("--contains");
    }
    if (s == "stash") return one_of(c.op(1), {"list", "show"});
    if (s == "config") {
        if (c.has_prefix("--unset") || c.has("--add") || c.has("--replace-all") || c.has("--rename-section") ||
            c.has("--remove-section") || c.has("--edit") || c.has("-e"))
            return false;
        bool get = c.has("--get") || c.has("--get-all") || c.has("--get-regexp") || c.has("--list") || c.has("-l");
        return get || c.operands.size() == 2;  // git config key: a read
    }
    return false;
}

// ---- docker / podman
// Operand i of docker/podman; docker-compose is treated as "docker compose".
std::string dk(const Cmd& c, std::size_t i) {
    if (c.name == "docker-compose") return i == 0 ? "compose" : c.op(i - 1);
    return c.op(i);
}

bool docker_danger(const Cmd& c) {
    std::string a = dk(c, 0), b = dk(c, 1);
    if (a == "compose") {
        // "down" may come after compose options with a value (-f FILE, -p NAME): look for it anywhere.
        bool down = false;
        for (std::size_t i = 1; !dk(c, i).empty(); ++i) down = down || dk(c, i) == "down";
        return down && (c.has("-v") || c.has("--volumes"));
    }
    if (a == "system" && b == "reset") return true;
    if (a == "system" && b == "prune") return c.has("-a") || c.has("--all") || c.has("--volumes");
    if (a == "volume" && one_of(b, {"prune", "rm", "remove"})) return true;
    bool rm = a == "rm" || a == "rmi" || ((a == "container" || a == "image") && one_of(b, {"rm", "remove"}));
    if (rm && (c.has("-f") || c.has("--force"))) {
        if (c.has("-a") || c.has("--all")) return true;  // podman rm/rmi -af: every container or image
        return std::any_of(c.operands.begin(), c.operands.end(), [](const Word& w) { return w.dynamic; });
    }
    return false;
}

bool docker_read_only(const Cmd& c) {
    std::string a = dk(c, 0), b = dk(c, 1);
    if (one_of(a, {"ps", "images", "logs", "inspect", "version", "info", "stats"})) return true;
    if (one_of(a, {"image", "container", "volume", "network"})) return one_of(b, {"ls", "list", "inspect"});
    return a == "compose" && b == "ps";
}

// ---- package managers
struct Pkg {
    bool removal = false, action = false;
    std::vector<std::string> pkgs;
};

Pkg pkg_info(const Cmd& c) {
    Pkg p;
    const std::string& n = c.name;
    std::string s = c.op(0);
    auto rest = [&](std::size_t from) {
        for (std::size_t i = from; i < c.operands.size(); ++i) p.pkgs.push_back(c.operands[i].text);
    };
    if (one_of(n, {"apt", "apt-get", "aptitude"})) {
        p.removal = one_of(s, {"remove", "purge", "autoremove", "autopurge"});
        p.action = p.removal || one_of(s, {"install", "reinstall", "upgrade", "full-upgrade", "dist-upgrade",
                                           "update", "build-dep", "satisfy"});
        rest(1);
    } else if (one_of(n, {"dnf", "yum", "microdnf"})) {
        p.removal = one_of(s, {"remove", "erase", "autoremove"});
        p.action = p.removal || one_of(s, {"install", "reinstall", "upgrade", "update", "downgrade", "distro-sync",
                                           "swap", "groupinstall", "groupremove", "group"});
        rest(1);
    } else if (n == "pacman") {
        p.removal = c.has("-R") || c.has("--remove");
        bool sync = (c.has("-S") || c.has("--sync")) &&
                    !(c.has("-s") || c.has("-i") || c.has("-l") || c.has("-g") || c.has("--search") ||
                      c.has("--info") || c.has("--list") || c.has("--groups"));
        p.action = p.removal || sync || c.has("-U") || c.has("--upgrade");
        rest(0);
    } else if (n == "zypper") {
        p.removal = one_of(s, {"rm", "remove"});
        p.action = p.removal || one_of(s, {"in", "install", "up", "update", "dup", "dist-upgrade", "patch"});
        rest(1);
    } else if (n == "apk") {
        p.removal = s == "del";
        p.action = p.removal || one_of(s, {"add", "upgrade", "update", "fix"});
        rest(1);
    } else if (n == "snap") {
        p.removal = s == "remove";
        p.action = p.removal || one_of(s, {"install", "refresh", "revert"});
        rest(1);
    } else if (n == "flatpak") {
        p.removal = one_of(s, {"uninstall", "remove"});
        p.action = p.removal || one_of(s, {"install", "update", "upgrade"});
        rest(1);
    } else if (n == "dpkg") {
        p.removal = c.has("-r") || c.has("-P") || c.has("--remove") || c.has("--purge");
        p.action = p.removal || c.has("-i") || c.has("--install") || c.has("--unpack") || c.has("--configure");
        rest(0);
    } else if (n == "rpm") {
        if (c.has("-q") || c.has("--query")) return p;
        p.removal = c.has("-e") || c.has("--erase");
        p.action = p.removal || c.has("-i") || c.has("-U") || c.has("-F") || c.has("--install") ||
                   c.has("--upgrade") || c.has("--freshen");
        rest(0);
    } else if (one_of(n, {"pip", "pip3", "pipx"})) {
        p.action = one_of(s, {"install", "uninstall", "reinstall", "upgrade", "upgrade-all", "uninstall-all", "inject"});
    } else if (n == "npm") {
        p.action = (c.has("-g") || c.has("--global")) &&
                   one_of(s, {"install", "i", "uninstall", "un", "remove", "rm", "r", "update", "up", "upgrade",
                              "add", "link", "unlink"});
    } else if (n == "cargo") {
        p.action = one_of(s, {"install", "uninstall"});
    }
    return p;
}

bool protected_pkg(SV raw) {
    std::string p = lower(raw);
    std::size_t cut = p.find_first_of(":=");
    if (cut != std::string::npos) p.resize(cut);
    if (one_of(p, {"libc6", "glibc", "systemd", "sudo", "bash", "coreutils", "dpkg", "apt", "rpm", "dnf", "pacman",
                   "linux-generic", "openssh-server", "network-manager", "networkmanager", "linux", "linux-lts",
                   "linux-zen", "linux-hardened"}))
        return true;
    return starts_with(p, "linux-image") || starts_with(p, "kernel") || starts_with(p, "grub");
}

bool pkg_read_only(const Cmd& c) {
    const std::string& n = c.name;
    std::string s = c.op(0);
    if (n == "apt") return one_of(s, {"list", "show", "search", "policy"});
    if (n == "apt-cache") return true;
    if (n == "apt-mark")
        return one_of(s, {"showhold", "showmanual", "showauto", "showinstall", "showremove", "showpurge"});
    if (n == "dpkg") {
        if (pkg_info(c).action || c.has_prefix("--force")) return false;
        return c.has("-l") || c.has("-L") || c.has("-s") || c.has("-S") || c.has("--list") || c.has("--listfiles") ||
               c.has("--status") || c.has("--search");
    }
    if (n == "dnf") return one_of(s, {"list", "info", "search", "repoquery", "provides"});
    if (n == "rpm") return c.has("-q") || c.has("--query");
    if (n == "pacman") {
        if (c.has("-Q") || c.has("--query")) return true;
        return c.has("-S") && (c.has("-s") || c.has("-i")) && !c.has("-y") && !c.has("-u");
    }
    if (n == "zypper") return one_of(s, {"search", "se", "info", "if"});
    if (n == "flatpak") return one_of(s, {"list", "info", "search"});
    if (n == "snap") return one_of(s, {"list", "info", "find"});
    return false;
}

// ---- find
struct FindInfo {
    std::vector<std::string> starts;
    bool del = false, exec = false, fprint = false, filter = false;
};

FindInfo find_info(const Cmd& c) {
    FindInfo f;
    std::size_t k = 0;
    const auto& a = c.args;
    while (k < a.size()) {
        const std::string& t = a[k].text;
        if (t == "-H" || t == "-L" || t == "-P" || starts_with(t, "-O")) {
            ++k;
        } else if (t == "-D") {
            k += 2;
        } else {
            break;
        }
    }
    while (k < a.size()) {
        const std::string& t = a[k].text;
        if ((!t.empty() && t[0] == '-') || t == "(" || t == "!" || t == ",") break;
        f.starts.push_back(normalize_path(t, a[k].tilde));
        ++k;
    }
    if (f.starts.empty()) f.starts.push_back(".");
    for (; k < a.size(); ++k) {
        const std::string& t = a[k].text;
        if (t == "-delete") f.del = true;
        if (one_of(t, {"-exec", "-execdir", "-ok", "-okdir"})) f.exec = true;
        if (starts_with(t, "-fprint") || t == "-fls") f.fprint = true;
        if (one_of(t, {"-name", "-iname", "-path", "-ipath", "-type", "-mtime", "-size", "-wholename", "-regex",
                       "-iregex", "-newer", "-mmin", "-amin", "-cmin", "-atime", "-ctime", "-user", "-group",
                       "-perm"}))
            f.filter = true;
    }
    return f;
}

// ---- curl / wget (R28)
bool to_stdout(const Word* w) { return w && (w->text == "-" || w->text == "/dev/null" || w->text == "/dev/stdout"); }

bool curl_r28(const Cmd& c) {
    for (SV o : {"-o", "--output", "-D", "--dump-header", "-c", "--cookie-jar"}) {
        if (c.has(o) && !to_stdout(c.value(o))) return true;
    }
    if (c.has("-K") || c.has("--config")) return true;  // the config file may write or send
    if (c.has("-O") || c.has("--remote-name") || c.has("--remote-name-all") || c.has("-J") || c.has_prefix("--trace") ||
        c.has("--libcurl") || c.has("--etag-save"))
        return true;
    if (c.has("-d") || c.has_prefix("--data") || c.has("-F") || c.has_prefix("--form") || c.has("-T") ||
        c.has("--upload-file") || c.has("--json"))
        return true;
    for (SV o : {"-X", "--request"}) {
        const Word* v = c.value(o);
        if (v && lower(v->text) != "get") return true;
    }
    return false;
}

bool wget_r28(const Cmd& c) {
    if (c.has("--spider")) {
        // no download
    } else {
        const Word* o = c.value("-O");
        if (!o) o = c.value("--output-document");
        if (!to_stdout(o)) return true;
    }
    if (c.has("-o") || c.has("--output-file") || c.has("-a") || c.has("--append-output")) return true;
    if (c.has("-e") || c.has("--execute") || c.has("--config")) return true;
    if (c.has("--post-data") || c.has("--post-file") || c.has("--body-data") || c.has("--body-file")) return true;
    const Word* m = c.value("--method");
    return m && lower(m->text) != "get";
}

// Whitelist by allowed options (read-only to stdout).
bool curl_read_only(const Cmd& c) {
    if (curl_r28(c)) return false;
    for (const std::string& f : c.flags) {
        if (f.size() == 2 && SV("sSLIifvkHAerm46xuG").find(f[1]) != SV::npos) continue;
        if (one_of(f, {"--silent", "--show-error", "--location", "--head", "--include", "--fail", "--verbose",
                       "--insecure", "--header", "--user-agent", "--referer", "--range", "--max-time",
                       "--connect-timeout", "--compressed", "--retry", "--http1.1", "--http2", "--proxy", "--user"}))
            continue;
        const Word* v = c.value(f);
        if ((f == "-o" || f == "--output") && v && v->text == "-") continue;
        if ((f == "-X" || f == "--request") && v && lower(v->text) == "get") continue;
        if ((f == "-w" || f == "--write-out") && v && v->text.find("%output") == std::string::npos) continue;
        return false;
    }
    return true;
}

bool wget_read_only(const Cmd& c) {
    if (wget_r28(c)) return false;
    for (const std::string& f : c.flags) {
        if (f.size() == 2 && SV("qSTtU46").find(f[1]) != SV::npos) continue;
        if (one_of(f, {"--spider", "--header", "--no-check-certificate", "--quiet", "--server-response", "--timeout",
                       "--tries", "--user-agent"}))
            continue;
        const Word* v = c.value(f);
        if ((f == "-O" || f == "--output-document") && v && v->text == "-") continue;
        return false;
    }
    return true;
}

bool tar_read_only(const Cmd& c) {
    bool list = c.has("-t") || c.has("--list");
    if (!c.args.empty() && !c.args[0].text.empty() && c.args[0].text[0] != '-') {
        const std::string& m = c.args[0].text;  // old style: tar tvf x.tar
        if (m.find_first_not_of("tvfzjJa") != std::string::npos) return false;
        list = list || m.find('t') != std::string::npos;
    }
    for (const std::string& f : c.flags) {
        if (!one_of(f, {"-t", "--list", "-v", "-f", "-z", "-j", "-J", "-a", "--verbose", "--file", "--gzip",
                        "--bzip2", "--xz", "--zstd", "--auto-compress", "--wildcards", "--exclude", "-C"}))
            return false;
    }
    return list;
}

// ---- sed / awk (R29)
std::string sed_program(const Cmd& c) {
    std::string prog;
    for (const auto& [k, v] : c.values) {
        if (k == "-e" || k == "--expression") prog += v.text + "\n";
    }
    if (prog.empty()) prog = c.op(0);
    return prog;
}

// Does the sed program have a w/W/e command or the w/e flag of s///?
bool sed_writes(SV p) {
    std::size_t i = 0, n = p.size();
    auto skip_delim = [&](char d) {  // skips to the unescaped delimiter d; returns false if it is missing
        while (i < n && p[i] != d) i += (p[i] == '\\') ? 2 : 1;
        if (i >= n) return false;
        ++i;
        return true;
    };
    while (i < n) {
        char c = p[i];
        if (c == ' ' || c == '\t' || c == ';' || c == '\n' || c == '{' || c == '}' || c == '!' || c == ',' ||
            c == '$' || c == '~' || c == '+' || std::isdigit(static_cast<unsigned char>(c))) {
            ++i;
            continue;
        }
        if (c == '/') {
            ++i;
            if (!skip_delim('/')) return false;
            continue;
        }
        if (c == '\\' && i + 1 < n) {
            char d = p[i + 1];
            i += 2;
            if (!skip_delim(d)) return false;
            continue;
        }
        if (c == 'w' || c == 'W' || c == 'e') return true;
        if (c == 's' || c == 'y') {
            if (i + 1 >= n) return false;
            char d = p[i + 1];
            i += 2;
            if (!skip_delim(d) || !skip_delim(d)) return false;
            while (i < n && p[i] != ';' && p[i] != '\n' && p[i] != '}') {
                if (c == 's' && (p[i] == 'w' || p[i] == 'e')) return true;
                ++i;
            }
            continue;
        }
        if (c == 'a' || c == 'i' || c == 'c' || c == 'r' || c == 'R' || c == 'b' || c == 't' || c == 'T' ||
            c == ':' || c == 'q' || c == 'Q' || c == 'l' || c == 'L') {
            while (i < n && p[i] != '\n' && (p[i] != ';' || c == 'a' || c == 'i' || c == 'c')) ++i;
            continue;
        }
        ++i;
    }
    return false;
}

bool sed_r29(const Cmd& c) {
    return c.has("-i") || c.has("--in-place") || c.has("-f") || c.has("--file") || sed_writes(sed_program(c));
}

// awk program that writes or executes: system(, "| getline", or >/>>/| in a print outside
// parentheses, strings and regexes. "$1 > 5" or "/a|b/" are reads.
bool awk_program_unsafe(SV p) {
    std::string q;  // program without strings, regexes or comments
    char prev = 0;
    for (std::size_t i = 0; i < p.size(); ++i) {
        char c = p[i];
        if (c == '"' || (c == '/' && (prev == 0 || SV("(,~!{};&|?:=\n").find(prev) != SV::npos))) {
            char end = c;
            for (++i; i < p.size() && p[i] != end; ++i) {
                if (p[i] == '\\') ++i;
            }
            q += 'S';
            prev = 'S';
            continue;
        }
        if (c == '#') {
            while (i < p.size() && p[i] != '\n') ++i;
            q += '\n';
            prev = '\n';
            continue;
        }
        q += c;
        if (c != ' ' && c != '\t') prev = c;
    }
    auto word_at = [&](std::size_t k, SV w) {
        if (q.compare(k, w.size(), w) != 0) return false;
        bool left = k == 0 || !(std::isalnum(static_cast<unsigned char>(q[k - 1])) || q[k - 1] == '_');
        std::size_t e = k + w.size();
        bool right = e >= q.size() || !(std::isalnum(static_cast<unsigned char>(q[e])) || q[e] == '_');
        return left && right;
    };
    auto next_char = [&](std::size_t k) {
        while (k < q.size() && (q[k] == ' ' || q[k] == '\t')) ++k;
        return k < q.size() ? q[k] : '\0';
    };
    for (std::size_t k = 0; k < q.size(); ++k) {
        if (word_at(k, "system") && next_char(k + 6) == '(') return true;
        if (q[k] == '|') {
            std::size_t j = k + 1;
            while (j < q.size() && (q[j] == ' ' || q[j] == '\t')) ++j;
            if (q.compare(j, 7, "getline") == 0) return true;
        }
        bool pf = word_at(k, "printf");
        if (pf || word_at(k, "print")) {
            int level = 0;
            for (std::size_t j = k + (pf ? 6 : 5); j < q.size(); ++j) {
                char c = q[j];
                if (c == '(') ++level;
                if (c == ')' && --level < 0) break;
                if (level == 0 && (c == ';' || c == '\n' || c == '}')) break;
                if (level == 0 && (c == '>' || c == '|')) return true;
            }
        }
    }
    return false;
}

// awk -f, --file or -i/--include of a library: code that cannot be seen.
bool awk_file(const Cmd& c) {
    if (c.has("-f") || c.has("--file")) return true;
    for (SV o : {"-i", "--include"}) {
        const Word* v = c.value(o);
        if (v && v->text.rfind("inplace", 0) != 0) return true;
    }
    return false;
}

bool awk_inplace(const Cmd& c) {
    for (SV o : {"-i", "--include"}) {
        const Word* v = c.value(o);
        if (v && v->text.rfind("inplace", 0) == 0) return true;
    }
    return false;
}

bool awk_r29(const Cmd& c) {
    if (awk_inplace(c)) return true;
    if (awk_file(c)) return false;
    std::string prog;
    for (const auto& [k, v] : c.values) {
        if (k == "-e" || k == "--source") prog += v.text + "\n";
    }
    if (prog.empty()) prog = c.op(0);
    return awk_program_unsafe(prog);
}

bool sysctl_writes(const Cmd& c) {
    if (c.has("-w") || c.has("--write") || c.has("-p") || c.has("--load") || c.has("--system")) return true;
    return std::any_of(c.operands.begin(), c.operands.end(),
                       [](const Word& w) { return w.text.find('=') != std::string::npos; });
}

bool sys_path(SV p) { return starts_with(p, "/proc/sys/") || starts_with(p, "/sys/") || p == "/proc/sys" || p == "/sys"; }

bool always(const Cmd&) { return true; }

const std::vector<SV> kPkgManagers = {"apt", "apt-get", "apt-mark", "aptitude", "dnf", "yum", "microdnf",
                                      "pacman", "zypper", "apk", "snap", "flatpak", "dpkg", "rpm", "pip", "pip3",
                                      "pipx", "npm", "cargo"};

}  // namespace

// ------------------------------------------------------------ Cmd

bool long_match(std::string_view given, std::string_view full) {
    if (given == full) return true;
    return given.size() >= 4 && starts_with(given, "--") && starts_with(full, given);
}

bool Cmd::has(std::string_view f) const {
    return std::any_of(flags.begin(), flags.end(), [&](const std::string& g) { return long_match(g, f); });
}

bool Cmd::has_prefix(std::string_view p) const {
    return std::any_of(flags.begin(), flags.end(), [&](const std::string& f) {
        return starts_with(f, p) || (f.size() >= 4 && starts_with(f, "--") && starts_with(p, f));
    });
}

const Word* Cmd::value(std::string_view opt) const {
    for (const auto& [k, v] : values) {
        if (long_match(k, opt)) return &v;
    }
    return nullptr;
}

std::string Cmd::op(std::size_t i) const { return i < operands.size() ? operands[i].text : std::string(); }

std::string base_name(std::string_view path) {
    std::size_t k = path.rfind('/');
    return std::string(k == std::string_view::npos ? path : path.substr(k + 1));
}

Cmd make_cmd(const std::vector<Word>& argv, bool admin) {
    Cmd c;
    c.admin = admin;
    if (argv.empty()) return c;
    c.name = base_name(argv[0].text);
    c.args.assign(argv.begin() + 1, argv.end());
    const OptSpec* spec = spec_for(c.name);
    SV short_arg = spec ? spec->short_arg : SV();
    bool done = false;
    for (std::size_t k = 0; k < c.args.size(); ++k) {
        const Word& w = c.args[k];
        const std::string& t = w.text;
        if (done || t.size() < 2 || t[0] != '-') {
            c.operands.push_back(w);
            c.paths.push_back(normalize_path(t, w.tilde));
            continue;
        }
        if (t == "--") {
            done = true;
            continue;
        }
        if (t[1] == '-') {
            std::size_t eq = t.find('=');
            std::string name = t.substr(0, eq);
            c.flags.push_back(name);
            if (eq != std::string::npos) {
                c.values.emplace_back(name, plain_word(t.substr(eq + 1)));
            } else if (spec && k + 1 < c.args.size() &&
                       std::any_of(spec->long_arg.begin(), spec->long_arg.end(),
                                   [&](SV full) { return long_match(name, full); })) {
                c.values.emplace_back(name, c.args[++k]);
            }
            continue;
        }
        for (std::size_t j = 1; j < t.size(); ++j) {
            std::string f = std::string("-") + t[j];
            c.flags.push_back(f);
            if (short_arg.find(t[j]) != SV::npos) {
                if (j + 1 < t.size()) {
                    c.values.emplace_back(f, plain_word(t.substr(j + 1)));
                } else if (k + 1 < c.args.size()) {
                    c.values.emplace_back(f, c.args[++k]);
                }
                break;
            }
        }
    }
    return c;
}

// ------------------------------------------------------------ rule table
//
// Policy ids (not from the rule catalog):
//   PARSE  cannot be parsed (unclosed quotes or parentheses, unterminated heredoc, nesting > 8)
//   SIZE   over 8 KiB                CTRL  control characters
//   DYN    command decided at run time ($CMD, $(…) in command position)
//   WRITE  writes to a file          ENV   VAR=x cmd with a variable outside the safe list
//   CASE   case construct            WRAP  unrecognized wrapper option
// eval, source/., at and options whose value is code use R14c; the fork bomb, R11.
// R7c (deleting a file in F) extends the original rule catalog.

const std::vector<Rule>& rule_table() {
    using R = Risk;
    static const std::vector<Rule> t = {
        {"PARSE", {}, R::Precaucion, nullptr, false, "no se ha podido analizar el comando", "the command could not be parsed"},
        {"SIZE", {}, R::Precaucion, nullptr, false, "el comando es demasiado largo para analizarlo", "the command is too long to check"},
        {"CTRL", {}, R::Precaucion, nullptr, false, "contiene caracteres de control", "contains control characters"},
        {"DYN", {}, R::Precaucion, nullptr, false, "el comando se decide al ejecutarse", "the command is decided at run time"},
        {"WRITE", {}, R::Precaucion, nullptr, false, "escribe en un fichero", "writes to a file"},
        {"ENV", {}, R::Precaucion, nullptr, false, "cambia variables de entorno que alteran lo que se ejecuta",
         "changes environment variables that alter what runs"},
        {"CASE", {}, R::Precaucion, nullptr, false, "estructura case no analizada del todo", "case statement not fully checked"},
        {"WRAP", {}, R::Precaucion, nullptr, false, "opción no reconocida de un envoltorio", "unrecognised wrapper option"},

        {"R1", {"rm"}, R::Peligro, [](const Cmd& c) { return rec_r(c) && any_path(c, in_C); }, false,
         "borra de forma recursiva un directorio del sistema, tu carpeta personal o todo el directorio actual",
         "recursively deletes a system directory, your home folder or the whole current directory"},
        {"R2", {"rm"}, R::Peligro,
         [](const Cmd& c) { return rec_r(c) && std::any_of(c.operands.begin(), c.operands.end(), var_slash); }, false,
         "si la variable está vacía, borra desde /", "if the variable is empty, it deletes from /"},
        {"R3", {"rm"}, R::Peligro, [](const Cmd& c) { return c.has("--no-preserve-root"); }, false,
         "desactiva la protección de /", "disables the protection of /"},
        {"R7c", {"rm", "unlink"}, R::Peligro, [](const Cmd& c) { return any_path(c, in_F); }, false,
         "borra un fichero crítico del sistema", "deletes a critical system file"},
        {"R4", {"rm"}, R::Precaucion, always, true, "borra ficheros", "deletes files"},

        {"R5", {"dd", "dcfldd"}, R::Peligro,
         [](const Cmd& c) { auto tg = dd_targets(c); return std::any_of(tg.begin(), tg.end(), [](const std::string& p) { return in_D(p); }); },
         false, "sobrescribe un disco entero", "overwrites a whole disk"},
        {"R5b", {"dd", "dcfldd"}, R::Precaucion, always, true, "copia de bajo nivel", "low-level copy"},

        {"R6", {"cp", "mv", "install"}, R::Peligro, [](const Cmd& c) { return any_dest(c, in_D); }, false,
         "escribe directamente en un disco", "writes directly to a disk"},
        {"R6", {"tee"}, R::Peligro, [](const Cmd& c) { return any_path(c, in_D); }, false,
         "escribe directamente en un disco", "writes directly to a disk"},
        {"R7", {"tee"}, R::Peligro, [](const Cmd& c) { return !tee_append(c) && any_path(c, in_F); }, false,
         "sobrescribe un fichero crítico del sistema", "overwrites a critical system file"},
        {"R7", {"cp", "mv", "install"}, R::Peligro, [](const Cmd& c) { return any_dest(c, in_F); }, false,
         "sobrescribe un fichero crítico del sistema", "overwrites a critical system file"},
        {"R7", {"ln"}, R::Peligro, [](const Cmd& c) { return (c.has("-f") || c.has("--force")) && any_dest(c, in_F); },
         false, "sobrescribe un fichero crítico del sistema", "overwrites a critical system file"},
        {"R7", {"dd", "dcfldd"}, R::Peligro,
         [](const Cmd& c) { auto tg = dd_targets(c); return std::any_of(tg.begin(), tg.end(), [](const std::string& p) { return in_F(p); }); },
         false, "sobrescribe un fichero crítico del sistema", "overwrites a critical system file"},
        {"R7b", {"tee"}, R::Precaucion, [](const Cmd& c) { return tee_append(c) && any_path(c, in_F); }, false,
         "modifica un fichero crítico del sistema", "modifies a critical system file"},
        // R7d: in-place editors (sed -i, awk -i inplace; perl/ruby -i in unwrap.cpp).
        // Any operand counts: the script is never a critical path, so this only errs on the safe side.
        {"R7d", {"sed"}, R::Peligro,
         [](const Cmd& c) { return (c.has("-i") || c.has("--in-place")) && any_path(c, in_F); }, false,
         "modifica en sitio un fichero crítico del sistema", "edits a critical system file in place"},
        {"R7d", {"awk", "gawk", "mawk", "nawk"}, R::Peligro,
         [](const Cmd& c) { return awk_inplace(c) && any_path(c, in_F); }, false,
         "modifica en sitio un fichero crítico del sistema", "edits a critical system file in place"},

        {"R8", {"mkfs", "mkfs.*", "mke2fs", "mkswap", "wipefs", "blkdiscard", "shred", "pvremove", "vgremove", "lvremove"},
         R::Peligro, always, false, "destruye datos de forma irreversible", "irreversibly destroys data"},
        {"R8", {"cryptsetup"}, R::Peligro,
         [](const Cmd& c) {
             return std::any_of(c.operands.begin(), c.operands.end(), [](const Word& w) {
                 return one_of(w.text, {"luksFormat", "erase", "luksErase", "luksKillSlot", "luksRemoveKey"});
             });
         },
         false, "destruye datos de forma irreversible", "irreversibly destroys data"},
        {"R8", {"nvme"}, R::Peligro, [](const Cmd& c) { return one_of(c.op(0), {"format", "sanitize"}); }, false,
         "destruye datos de forma irreversible", "irreversibly destroys data"},
        {"R8", {"hdparm"}, R::Peligro, [](const Cmd& c) { return c.has_prefix("--security-erase"); }, false,
         "destruye datos de forma irreversible", "irreversibly destroys data"},
        {"R8", {"mdadm"}, R::Peligro, [](const Cmd& c) { return c.has("--zero-superblock"); }, false,
         "destruye datos de forma irreversible", "irreversibly destroys data"},
        {"R8", {"zpool", "zfs"}, R::Peligro, [](const Cmd& c) { return c.op(0) == "destroy"; }, false,
         "destruye datos de forma irreversible", "irreversibly destroys data"},
        {"R8", {"btrfs"}, R::Peligro,
         [](const Cmd& c) { return c.op(0).size() >= 2 && abbrev_of(c.op(0), "subvolume") && abbrev_of(c.op(1), "delete"); },
         false, "destruye datos de forma irreversible", "irreversibly destroys data"},
        {"R8", {"badblocks"}, R::Peligro, [](const Cmd& c) { return c.has("-w"); }, false,
         "destruye datos de forma irreversible", "irreversibly destroys data"},

        {"R9", {"fdisk", "sfdisk", "gdisk", "sgdisk", "parted"}, R::Peligro, [](const Cmd& c) { return !disk_listing(c); },
         false, "modifica la tabla de particiones", "modifies the partition table"},

        {"R10", {"chmod", "chown", "chgrp"}, R::Peligro,
         [](const Cmd& c) {
             return (rec_upper(c) && any_path(c, in_Cstar)) || any_path(c, in_F) ||
                    std::find(c.paths.begin(), c.paths.end(), "/") != c.paths.end();
         },
         false, "cambia permisos o dueños del sistema entero o de tu carpeta personal",
         "changes permissions or owners of the whole system or your home folder"},
        {"R10b", {"chmod", "chown", "chgrp"}, R::Precaucion, always, true, "cambia permisos", "changes permissions"},

        {"R12", {"kill"}, R::Peligro, kill_all, false, "mata el sistema, todos tus procesos o la sesión gráfica",
         "kills the system, all your processes or the graphical session"},
        {"R12", {"killall5"}, R::Peligro, always, false, "mata el sistema, todos tus procesos o la sesión gráfica",
         "kills the system, all your processes or the graphical session"},
        {"R12", {"killall", "pkill"}, R::Peligro,
         [](const Cmd& c) {
             if (std::any_of(c.args.begin(), c.args.end(), [](const Word& w) { return protected_process(w.text); }))
                 return true;
             if (c.has("-u") || c.has("-U") || c.has("--user") || c.has("--euid") || c.has("--uid")) return true;
             bool regex = c.name == "pkill" || c.has("-r") || c.has("--regexp");
             return regex && std::any_of(c.operands.begin(), c.operands.end(),
                                         [](const Word& w) { return one_of(w.text, {".", ".*", ""}); });
         },
         false, "mata el sistema, todos tus procesos o la sesión gráfica",
         "kills the system, all your processes or the graphical session"},
        {"R12", {"loginctl"}, R::Peligro,
         [](const Cmd& c) { return one_of(c.op(0), {"kill-user", "terminate-user", "terminate-session"}); }, false,
         "mata el sistema, todos tus procesos o la sesión gráfica",
         "kills the system, all your processes or the graphical session"},
        {"R12b", {"kill", "killall", "pkill"}, R::Precaucion, always, true, "termina procesos", "terminates processes"},

        {"R13", {"reboot", "poweroff", "halt"}, R::Peligro, always, false,
         "apaga o reinicia y puede perderse trabajo sin guardar", "shuts down or reboots; unsaved work may be lost"},
        {"R13", {"shutdown"}, R::Peligro, [](const Cmd& c) { return !c.has("-c"); }, false,
         "apaga o reinicia y puede perderse trabajo sin guardar", "shuts down or reboots; unsaved work may be lost"},
        {"R13", {"loginctl"}, R::Peligro, [](const Cmd& c) { return one_of(c.op(0), {"poweroff", "reboot", "halt"}); },
         false, "apaga o reinicia y puede perderse trabajo sin guardar",
         "shuts down or reboots; unsaved work may be lost"},
        {"R13", {"systemctl"}, R::Peligro,
         [](const Cmd& c) { return one_of(c.op(0), {"poweroff", "reboot", "halt", "kexec", "soft-reboot"}); }, false,
         "apaga o reinicia y puede perderse trabajo sin guardar", "shuts down or reboots; unsaved work may be lost"},
        {"R13", {"init", "telinit"}, R::Peligro, [](const Cmd& c) { return one_of(c.op(0), {"0", "6"}); }, false,
         "apaga o reinicia y puede perderse trabajo sin guardar", "shuts down or reboots; unsaved work may be lost"},
        {"R13", {"tee"}, R::Peligro,
         [](const Cmd& c) { return std::find(c.paths.begin(), c.paths.end(), "/proc/sysrq-trigger") != c.paths.end(); },
         false, "apaga o reinicia y puede perderse trabajo sin guardar", "shuts down or reboots; unsaved work may be lost"},

        {"R14", {}, R::Peligro, nullptr, false, "ejecuta código descargado de Internet sin revisarlo",
         "runs code downloaded from the Internet without reviewing it"},
        {"R14c", {}, R::Precaucion, nullptr, false, "ejecuta código que el verificador no puede revisar",
         "runs code the checker cannot review"},
        {"R14c", {"awk", "gawk", "mawk", "nawk"}, R::Precaucion, awk_file, false,
         "ejecuta código que el verificador no puede revisar", "runs code the checker cannot review"},
        {"R11", {}, R::Peligro, nullptr, false, "agota los procesos del sistema", "exhausts the system's processes"},

        {"R15", {"iptables", "ip6tables"}, R::Peligro,
         [](const Cmd& c) {
             if (c.has("-F") || c.has("--flush") || c.has("-X") || c.has("--delete-chain")) return true;
             for (std::size_t i = 0; i + 2 < c.args.size(); ++i) {
                 if ((c.args[i].text == "-P" || c.args[i].text == "--policy") &&
                     one_of(c.args[i + 2].text, {"DROP", "REJECT", "drop", "reject"}))
                     return true;
             }
             return false;
         },
         false, "borra el cortafuegos o puede dejarte sin acceso remoto",
         "wipes the firewall or may cut off your remote access"},
        {"R15", {"nft"}, R::Peligro,
         [](const Cmd& c) {
             std::string all;
             for (const auto& w : c.operands) all += w.text + " ";
             // flush/delete of a table or chain empties rules like iptables -F/-X.
             for (const char* k : {"flush ruleset", "flush table", "flush chain", "delete table", "delete chain"}) {
                 if (all.find(k) != std::string::npos) return true;
             }
             return false;
         },
         false, "borra el cortafuegos o puede dejarte sin acceso remoto",
         "wipes the firewall or may cut off your remote access"},
        {"R15", {"ufw"}, R::Peligro,
         [](const Cmd& c) {
             return std::any_of(c.operands.begin(), c.operands.end(), [](const Word& w) { return w.text == "reset"; });
         },
         false, "borra el cortafuegos o puede dejarte sin acceso remoto",
         "wipes the firewall or may cut off your remote access"},
        // firewall-cmd --load-zone-defaults resets a zone to its defaults and --reset-to-defaults
        // resets the whole of firewalld, like ufw reset.
        {"R15", {"firewall-cmd"}, R::Peligro,
         [](const Cmd& c) { return c.has_prefix("--load-zone-defaults") || c.has_prefix("--reset-to-defaults"); }, false,
         "borra el cortafuegos o puede dejarte sin acceso remoto",
         "wipes the firewall or may cut off your remote access"},
        {"R15", {"ip"}, R::Peligro, ip_cuts_network, false, "borra el cortafuegos o puede dejarte sin acceso remoto",
         "wipes the firewall or may cut off your remote access"},
        {"R15", {"nmcli"}, R::Peligro, nmcli_cuts_network, false,
         "borra el cortafuegos o puede dejarte sin acceso remoto",
         "wipes the firewall or may cut off your remote access"},
        {"R15", {"ifconfig"}, R::Peligro, ifconfig_cuts_network, false,
         "borra el cortafuegos o puede dejarte sin acceso remoto",
         "wipes the firewall or may cut off your remote access"},
        {"R15", {"ifdown"}, R::Peligro, always, false, "borra el cortafuegos o puede dejarte sin acceso remoto",
         "wipes the firewall or may cut off your remote access"},
        {"R15b", {"iptables", "ip6tables", "nft", "ufw", "firewall-cmd"}, R::Precaucion,
         [](const Cmd& c) { return !fw_listing(c); }, true, "cambia el cortafuegos", "changes the firewall"},

        {"R16", {"systemctl"}, R::Peligro,
         [](const Cmd& c) {
             std::string v = c.op(0);
             if (one_of(v, {"stop", "disable", "mask", "kill"})) {
                 for (std::size_t i = 1; i < c.operands.size(); ++i) {
                     if (protected_unit(c.operands[i].text)) return true;
                 }
             }
             if (v == "isolate") return one_of(unit_base(c.op(1)), {"rescue", "emergency"});
             return one_of(v, {"rescue", "emergency"});
         },
         false, "corta el acceso remoto, la red o la sesión", "cuts remote access, the network or the session"},
        {"R16", {"service"}, R::Peligro, [](const Cmd& c) { return protected_unit(c.op(0)) && c.op(1) == "stop"; },
         false, "corta el acceso remoto, la red o la sesión", "cuts remote access, the network or the session"},
        {"R16b", {"systemctl"}, R::Precaucion,
         [](const Cmd& c) {
             std::string v = c.op(0);
             return !v.empty() && std::find(kQueryVerbs.begin(), kQueryVerbs.end(), SV(v)) == kQueryVerbs.end();
         },
         true, "cambia servicios", "changes services"},
        {"R16b", {"service"}, R::Precaucion, [](const Cmd& c) { return one_of(c.op(1), {"stop", "restart"}); }, true,
         "cambia servicios", "changes services"},

        {"R17", {"git"}, R::Peligro, git_danger, false, "pierde cambios o historial de forma irreversible",
         "irreversibly loses changes or history"},
        {"R17b", {"git"}, R::Precaucion, [](const Cmd& c) { return !git_sub(c).empty() && !git_read_only(c); }, true,
         "modifica el repositorio", "modifies the repository"},

        {"R18", {"docker", "podman", "docker-compose"}, R::Peligro, docker_danger, false,
         "borra contenedores, imágenes o volúmenes con datos", "deletes containers, images or volumes with data"},
        {"R18b", {"docker", "podman", "docker-compose"}, R::Precaucion,
         [](const Cmd& c) { return !dk(c, 0).empty() && !docker_read_only(c); }, true, "cambia contenedores",
         "changes containers"},

        {"R19", {"crontab"}, R::Peligro,
         [](const Cmd& c) { return c.has("-r") || std::find(c.paths.begin(), c.paths.end(), "/dev/null") != c.paths.end(); },
         false,
         "borra todas tus tareas programadas sin preguntar", "deletes all your scheduled jobs without asking"},

        {"R20", {"userdel", "deluser", "groupdel", "delgroup"}, R::Peligro, always, false,
         "elimina usuarios o deja cuentas sin protección", "removes users or leaves accounts unprotected"},
        {"R20", {"passwd"}, R::Peligro,
         [](const Cmd& c) {
             if (c.has("-d") || c.has("--delete")) return true;
             if (std::any_of(c.operands.begin(), c.operands.end(), [](const Word& w) { return w.text == "root"; }))
                 return true;
             return c.admin && c.operands.empty() && !c.has("-S") && !c.has("--status");
         },
         false, "elimina usuarios o deja cuentas sin protección", "removes users or leaves accounts unprotected"},
        {"R20", {"usermod"}, R::Peligro,
         [](const Cmd& c) {
             bool lock_or_pass = c.has("-L") || c.has("--lock") || c.has("-p") || c.has("--password");
             return lock_or_pass && c.op(c.operands.size() - 1) == "root";
         },
         false,
         "elimina usuarios o deja cuentas sin protección", "removes users or leaves accounts unprotected"},
        {"R20", {"chage"}, R::Peligro,
         [](const Cmd& c) {
             const Word* e = c.value("-E");
             if (!e) e = c.value("--expiredate");
             return e && e->text == "0" && c.op(c.operands.size() - 1) == "root";
         },
         false, "elimina usuarios o deja cuentas sin protección", "removes users or leaves accounts unprotected"},
        {"R20", {"gpasswd"}, R::Peligro,
         [](const Cmd& c) {
             return (c.value("-d") || c.value("--delete")) &&
                    std::any_of(c.operands.begin(), c.operands.end(),
                                [](const Word& w) { return one_of(w.text, {"sudo", "wheel", "admin"}); });
         },
         false, "elimina usuarios o deja cuentas sin protección", "removes users or leaves accounts unprotected"},
        {"R20", {"chpasswd"}, R::Peligro, chpasswd_root, false, "cambia la contraseña de root",
         "changes root's password"},
        {"R20b", {"passwd", "chpasswd", "useradd", "usermod", "visudo", "chsh", "chage", "gpasswd"}, R::Precaucion, always, true,
         "cambia cuentas", "changes accounts"},

        {"R21", {"mv"}, R::Peligro,
         [](const Cmd& c) {
             auto src = source_paths(c);
             bool crit = std::any_of(src.begin(), src.end(), [](const std::string& p) { return in_Cstar(p) || in_F(p); });
             return crit || dest_path(c) == "/dev/null";
         },
         false, "mueve un directorio del sistema o destruye el fichero",
         "moves a system directory or destroys the file"},
        {"R22", {"truncate"}, R::Peligro, [](const Cmd& c) { return any_path(c, in_F) || any_path(c, in_D); }, false,
         "vacía un fichero crítico o un disco", "empties a critical file or a disk"},
        {"R21b", {"mv", "cp", "ln", "install", "mkdir", "touch", "rmdir", "unlink", "truncate"}, R::Precaucion, always,
         true, "modifica ficheros", "modifies files"},

        {"R23", {"find"}, R::Peligro,
         [](const Cmd& c) {
             FindInfo f = find_info(c);
             if (!f.del) return false;
             bool crit = std::any_of(f.starts.begin(), f.starts.end(), [](const std::string& p) { return in_Cstar(p); });
             return crit || !f.filter;
         },
         false, "borra en masa", "mass deletion"},
        {"R23b", {"find"}, R::Precaucion, [](const Cmd& c) { return find_info(c).del; }, true, "borra ficheros",
         "deletes files"},

        {"R24", {"rsync"}, R::Peligro, [](const Cmd& c) { return c.has_prefix("--delete") || c.has("--del"); }, false,
         "borra en el destino lo que no está en el origen; con los argumentos al revés se pierden los datos",
         "deletes at the destination whatever is not in the source; with the arguments swapped, data is lost"},
        {"R24b", {"rsync", "scp"}, R::Precaucion, always, true, "copia o sobrescribe ficheros",
         "copies or overwrites files"},

        {"R25", kPkgManagers, R::Peligro,
         [](const Cmd& c) {
             Pkg p = pkg_info(c);
             return p.removal && std::any_of(p.pkgs.begin(), p.pkgs.end(), [](const std::string& s) { return protected_pkg(s); });
         },
         false, "puede dejar el sistema sin arrancar o sin poder gestionarse",
         "may leave the system unable to boot or to be managed"},
        {"R25b", {"pacman", "rpm", "dpkg"}, R::Peligro,
         [](const Cmd& c) {
             if (c.name == "dpkg") return c.has_prefix("--force");
             Pkg p = pkg_info(c);
             if (!p.removal) return false;
             if (c.name == "rpm") return c.has("--nodeps");
             auto n = std::count(c.flags.begin(), c.flags.end(), "-d") + std::count(c.flags.begin(), c.flags.end(), "--nodeps");
             return n >= 2;
         },
         false, "ignora dependencias y puede romper el sistema", "ignores dependencies and may break the system"},
        {"R25c", kPkgManagers, R::Precaucion, [](const Cmd& c) { return pkg_info(c).action; }, true,
         "instala o elimina software", "installs or removes software"},

        {"R26", {"mount", "umount", "swapon", "swapoff", "modprobe", "rmmod", "insmod", "chattr", "setfacl"},
         R::Precaucion, always, false, "cambia el estado del sistema", "changes the system state"},
        {"R26", {"sysctl"}, R::Precaucion, sysctl_writes, false, "cambia el estado del sistema",
         "changes the system state"},
        {"R26", {"tee"}, R::Precaucion, [](const Cmd& c) { return any_path(c, sys_path); }, false,
         "cambia el estado del sistema", "changes the system state"},

        {"R27", {"kubectl"}, R::Peligro,
         [](const Cmd& c) {
             if (c.op(0) != "delete") return false;
             std::string r = c.op(1);
             return c.has("--all") || c.has("-A") || c.has("--all-namespaces") ||
                    one_of(r, {"namespace", "namespaces", "ns"}) || starts_with(r, "namespace/") ||
                    starts_with(r, "namespaces/") || starts_with(r, "ns/");
         },
         false, "borra recursos del clúster en masa", "mass-deletes cluster resources"},
        {"R27b", {"kubectl"}, R::Precaucion,
         [](const Cmd& c) { return !c.op(0).empty() && !one_of(c.op(0), {"get", "describe", "logs", "top", "version"}); },
         true, "modifica el clúster", "modifies the cluster"},

        {"R28", {"curl"}, R::Precaucion, curl_r28, false, "descarga a disco o envía datos",
         "downloads to disk or sends data"},
        {"R28", {"wget"}, R::Precaucion, wget_r28, false, "descarga a disco o envía datos",
         "downloads to disk or sends data"},

        {"R29", {"sed"}, R::Precaucion, sed_r29, false, "modifica ficheros o ejecuta comandos",
         "modifies files or runs commands"},
        {"R29", {"yq"}, R::Precaucion, [](const Cmd& c) { return c.has("-i") || c.has("--inplace"); }, false,
         "modifica ficheros o ejecuta comandos", "modifies files or runs commands"},
        {"R29", {"awk", "gawk", "mawk", "nawk"}, R::Precaucion, awk_r29, false, "modifica ficheros o ejecuta comandos",
         "modifies files or runs commands"},
        {"R29", {}, R::Precaucion, nullptr, false, "modifica ficheros o ejecuta comandos",
         "modifies files or runs commands"},

        {"R30", {}, R::Precaucion, nullptr, false, "se ejecuta como administrador", "runs as administrator"},
        {"R31", {}, R::Precaucion, nullptr, false, "el verificador no conoce este comando",
         "the checker does not know this command"},

        // tee to any other file (goes last: only if nothing matched before).
        {"WRITE", {"tee"}, R::Precaucion,
         [](const Cmd& c) {
             return std::any_of(c.paths.begin(), c.paths.end(), [](const std::string& p) { return p != "/dev/null"; });
         },
         true, "escribe en un fichero", "writes to a file"},
    };
    return t;
}

const Rule* find_rule(std::string_view id) {
    for (const Rule& r : rule_table()) {
        if (r.id == id) return &r;
    }
    return nullptr;
}

bool rule_applies(const Rule& r, std::string_view name) {
    for (SV c : r.commands) {
        if (c == name) return true;
        if (c.size() > 1 && c.back() == '*' && name.size() >= c.size() && starts_with(name, c.substr(0, c.size() - 1)))
            return true;
    }
    return false;
}

// ------------------------------------------------------------ read-only whitelist

namespace {

const std::vector<SV> kAlways = {
    "ls", "cat", "head", "tail", "wc", "grep", "egrep", "fgrep", "ag", "eza", "file", "stat", "du",
    "df", "cut", "tr", "column", "nl", "tac", "rev", "diff", "cmp", "comm", "md5sum", "sha1sum", "sha256sum",
    "sha512sum", "b2sum", "base64", "od", "hexdump", "strings", "basename", "dirname", "realpath", "readlink", "pwd",
    "jq", "locate", "zipinfo", "uname", "whoami", "id", "groups", "uptime", "free", "lscpu", "lsblk", "blkid", "lsusb",
    "lspci", "lsmod", "lsof", "ps", "pgrep", "top", "htop", "btop", "printenv", "env", "which", "type", "whereis",
    "netstat", "ping", "dig", "nslookup", "host", "traceroute", "tracepath", "apt-cache", "ffprobe", "identify",
    "mediainfo", "echo", "printf", "seq", "sleep", "true", "false", "test", "[", "[[", "expr", "bc", "cal", "help",
    "yes", "getfacl"};

// Names with a condition (to tell "unknown" from "known but not read-only").
const std::vector<SV> kConditional = {
    "cd", ":", "bat", "batcat", "crontab", "iptables", "ip6tables", "nft", "ufw", "docker-compose", "rg", "fd", "fdfind", "tree", "sort", "uniq", "xxd", "less", "more", "yq", "sed", "awk", "gawk", "mawk", "nawk",
    "find", "tar", "unzip", "7z", "date", "hostname", "hostnamectl", "timedatectl", "command", "dmesg", "journalctl", "systemctl",
    "loginctl", "fdisk", "parted", "smartctl", "ip", "ifconfig", "ss", "nmcli", "curl", "wget", "apt", "apt-mark", "dpkg", "dnf", "rpm",
    "pacman", "zypper", "flatpak", "snap", "git", "docker", "podman", "kubectl", "magick", "exiftool", "man"};

bool contains(const std::vector<SV>& v, SV s) { return std::find(v.begin(), v.end(), s) != v.end(); }

}  // namespace

bool known_command(std::string_view name) {
    if (contains(kAlways, name) || contains(kConditional, name)) return true;
    for (const Rule& r : rule_table()) {
        if (rule_applies(r, name)) return true;
    }
    return false;
}

bool read_only(const Cmd& c) {
    const std::string& n = c.name;
    if (contains(kAlways, n)) return true;
    if (n == "cd") return true;
    if (n == ":")
        return std::none_of(c.args.begin(), c.args.end(), [](const Word& w) { return w.dynamic; });
    if (n == "bat" || n == "batcat") return !c.has("--pager");
    if (n == "crontab") return c.has("-l") && !c.has("-r") && !c.has("-e") && c.operands.empty();
    if (n == "iptables" || n == "ip6tables") {
        if (!(c.has("-L") || c.has("-S"))) return false;
        return std::all_of(c.flags.begin(), c.flags.end(), [](const std::string& f) {
            return one_of(f, {"-L", "-S", "-n", "-v", "-t", "--line-numbers", "-x"});
        });
    }
    if (n == "nft") {
        return c.op(0) == "list" && std::all_of(c.flags.begin(), c.flags.end(), [](const std::string& f) {
                   return one_of(f, {"-a", "-n", "-s", "-j", "-t", "--handle", "--numeric", "--stateless", "--json",
                                     "--terse"});
               });
    }
    if (n == "ufw")
        return c.flags.empty() && c.op(0) == "status" &&
               (c.operands.size() == 1 || (c.operands.size() == 2 && one_of(c.op(1), {"verbose", "numbered"})));
    if (n == "rg") return !c.has("--pre");
    if (n == "fd" || n == "fdfind")
        return !(c.has("-x") || c.has("-X") || c.has("--exec") || c.has("--exec-batch"));
    if (n == "tree") return !c.has("-o");
    if (n == "sort") return !(c.has("-o") || c.has("--output") || c.has("--compress-program"));
    if (n == "uniq") return c.operands.size() < 2;
    if (n == "xxd") return c.operands.size() < 2 && !c.has("-r") && !c.has("-revert");
    if (n == "less" || n == "more") {
        // a leading +command with ! or | runs commands
        for (const Word& w : c.args) {
            if (!w.text.empty() && w.text[0] == '+' && w.text.find_first_of("!|") != std::string::npos) return false;
        }
        return !(c.has("-o") || c.has("-O") || c.has("--log-file") || c.has("--LOG-FILE"));
    }
    if (n == "man") return !(c.has("-P") || c.has("--pager") || c.has("-H") || c.has("--html"));
    if (n == "ss") return !(c.has("-K") || c.has("--kill"));
    if (n == "yq") return !(c.has("-i") || c.has("--inplace"));
    if (n == "sed") return !sed_r29(c);
    if (n == "awk" || n == "gawk" || n == "mawk" || n == "nawk") return !awk_file(c) && !awk_r29(c);
    if (n == "find") {
        FindInfo f = find_info(c);
        return !f.del && !f.exec && !f.fprint;
    }
    if (n == "tar") return tar_read_only(c);
    if (n == "unzip") return c.has("-l");
    if (n == "7z") return c.op(0) == "l";
    if (n == "date") return !(c.has("-s") || c.has("--set"));
    if (n == "hostname")
        return c.operands.empty() && std::all_of(c.flags.begin(), c.flags.end(), [](const std::string& f) {
                   return one_of(f, {"-I", "-i", "-f", "-s", "-d", "-A", "--all-ip-addresses", "--fqdn"});
               });
    if (n == "hostnamectl") return c.operands.empty() || c.op(0) == "status";
    if (n == "timedatectl")
        return c.operands.empty() ||
               one_of(c.op(0), {"status", "show", "show-timesync", "timesync-status", "list-timezones"});
    if (n == "dmesg")
        return !(c.has("-c") || c.has("-C") || c.has("--clear") || c.has("--read-clear") || c.has("-D") ||
                 c.has("--console-off") || c.has("-E") || c.has("--console-on") || c.has("-n") ||
                 c.has("--console-level"));
    if (n == "journalctl")
        return !(c.has_prefix("--vacuum") || c.has("--rotate") || c.has("--flush") || c.has("--relinquish-var") ||
                 c.has("--smart-relinquish-var") || c.has("--sync") || c.has("--setup-keys") ||
                 c.has("--update-catalog"));
    if (n == "systemctl") return contains(kQueryVerbs, c.op(0));
    if (n == "loginctl") return starts_with(c.op(0), "list-") || starts_with(c.op(0), "show-");
    if (n == "fdisk") return c.has("-l") || c.has("--list");
    if (n == "parted") return disk_listing(c);
    if (n == "smartctl") {
        bool info = c.has("-a") || c.has("-i") || c.has("-H") || c.has("--all") || c.has("--info") || c.has("--health");
        bool change = c.has("-t") || c.has("--test") || c.has("-s") || c.has("--smart") || c.has("-o") ||
                      c.has("--offlineauto") || c.has("-S") || c.has("--saveauto") || c.has("-X") || c.has("--abort");
        return info && !change;
    }
    if (n == "ip") return ip_read_only(c);
    if (n == "ifconfig") return ifconfig_read_only(c);
    if (n == "nmcli") return nmcli_read_only(c);
    if (n == "curl") return curl_read_only(c);
    if (n == "wget") return wget_read_only(c);
    if (contains(kPkgManagers, n)) return pkg_read_only(c);
    if (n == "git") return git_read_only(c);
    if (n == "docker" || n == "podman" || n == "docker-compose") return docker_read_only(c);
    if (n == "kubectl") return one_of(c.op(0), {"get", "describe", "logs", "top", "version"});
    if (n == "magick") return c.op(0) == "identify";
    if (n == "exiftool") {
        return std::none_of(c.args.begin(), c.args.end(), [](const Word& w) {
            std::string t = lower(w.text);
            return t.find('=') != std::string::npos || t == "-o" || starts_with(t, "-overwrite_original") ||
                   starts_with(t, "-delete_original") || t == "-w" || t == "-tagsfromfile" || t == "-geotag";
        });
    }
    return false;
}

bool is_downloader(std::string_view name) {
    // "s_client" is set by the engine for "openssl s_client".
    return one_of(name, {"curl", "wget", "fetch", "nc", "ncat", "netcat", "http", "https", "xh", "socat", "s_client"});
}

bool is_shell(std::string_view name) {
    return one_of(name, {"bash", "sh", "dash", "zsh", "ksh", "mksh", "ash", "fish", "tcsh", "csh", "yash"});
}

// ------------------------------------------------------------ options that write or carry code

namespace {

std::vector<const Word*> values_of(const Cmd& c, std::initializer_list<SV> opts) {
    std::vector<const Word*> out;
    for (const auto& [k, v] : c.values) {
        for (SV o : opts) {
            if (long_match(k, o)) {
                out.push_back(&v);
                break;
            }
        }
    }
    return out;
}

struct OptTable {
    SV cmd;
    std::vector<SV> opts;
    bool append;
};

// Options whose value is a file that gets written.
const std::vector<OptTable> kWriteOpts = {
    {"sort", {"-o", "--output"}, false},
    {"curl", {"-o", "--output", "--stderr", "--hsts", "--alt-svc", "-c", "--cookie-jar", "-D", "--dump-header",
              "--trace", "--trace-ascii"}, false},
    {"wget", {"-O", "--output-document", "--save-cookies", "-o", "--output-file"}, false},
    {"wget", {"-a", "--append-output"}, true},
    {"pv", {"-o", "--output"}, false},
    {"tree", {"-o"}, false},
};

// Options whose value is code.
const std::vector<OptTable> kCodeOpts = {
    {"rsync", {"-e", "--rsh"}, false},
    {"tar", {"--to-command", "-I", "--use-compress-program", "-F", "--info-script", "--new-volume-script",
             "--rsh-command"}, false},
    {"man", {"-P", "--pager"}, false},
    {"bat", {"--pager"}, false},
    {"batcat", {"--pager"}, false},
    {"scp", {"-S"}, false},
    {"sftp", {"-S"}, false},
};

bool tar_creates(const Cmd& c) {
    if (c.has("-c") || c.has("-r") || c.has("-u") || c.has("-A") || c.has("--create") || c.has("--append") ||
        c.has("--update") || c.has("--catenate") || c.has("--concatenate"))
        return true;
    return !c.args.empty() && !c.args[0].text.empty() && c.args[0].text[0] != '-' &&
           c.args[0].text.find_first_of("cruA") != std::string::npos;
}

std::string join_from(const std::vector<Word>& a, std::size_t from) {
    std::string s;
    for (std::size_t i = from; i < a.size(); ++i) {
        if (i > from) s += ' ';
        s += a[i].text;
    }
    return s;
}

}  // namespace

std::vector<std::pair<Word, bool>> write_values(const Cmd& c) {
    std::vector<std::pair<Word, bool>> out;
    auto add = [&](const Word& w, bool append) {
        if (w.text != "-") out.emplace_back(w, append);
    };
    for (const OptTable& t : kWriteOpts) {
        if (t.cmd != c.name) continue;
        for (SV o : t.opts) {
            for (const Word* v : values_of(c, {o})) add(*v, t.append);
        }
    }
    if (c.name == "find") {
        for (std::size_t i = 0; i + 1 < c.args.size(); ++i) {
            if (one_of(c.args[i].text, {"-fprint", "-fprint0", "-fprintf", "-fls"})) add(c.args[i + 1], false);
        }
    }
    if (c.name == "tar" && tar_creates(c)) {
        for (const Word* v : values_of(c, {"-f", "--file"})) add(*v, false);
        const std::string& m = c.args.empty() ? std::string() : c.args[0].text;
        if (!m.empty() && m[0] != '-' && m.find('f') != std::string::npos && c.args.size() > 1) add(c.args[1], false);
    }
    if (c.name == "ddrescue") {
        for (std::size_t i = 1; i < c.operands.size(); ++i) add(c.operands[i], false);
    }
    if (c.name == "rsync" && c.operands.size() >= 2 && c.operands.back().text.find(':') == std::string::npos) {
        add(c.operands.back(), false);
    }
    return out;
}

std::string ssh_option_code(std::string_view opt) {
    std::size_t sep = opt.find_first_of("= ");
    if (sep == std::string_view::npos) return {};
    std::string key = lower(opt.substr(0, sep));
    if (key != "proxycommand" && key != "localcommand") return {};
    std::size_t b = opt.find_first_not_of("= ", sep);
    return b == std::string_view::npos ? std::string() : std::string(opt.substr(b));
}

std::vector<std::string> code_values(const Cmd& c) {
    std::vector<std::string> out;
    for (const OptTable& t : kCodeOpts) {
        if (t.cmd != c.name) continue;
        for (SV o : t.opts) {
            for (const Word* v : values_of(c, {o})) out.push_back(v->text);
        }
    }
    if (c.name == "scp" || c.name == "sftp") {
        for (const Word* v : values_of(c, {"-o"})) {
            std::string code = ssh_option_code(v->text);
            if (!code.empty()) out.push_back(code);
        }
    }
    if (c.name == "tar") {
        for (const Word* v : values_of(c, {"--checkpoint-action"})) {
            if (v->text.rfind("exec=", 0) == 0) out.push_back(v->text.substr(5));
        }
    }
    if (c.name == "trap" && !c.operands.empty() && c.op(0) != "-") out.push_back(c.op(0));
    if (c.name == "git") {
        const auto& a = c.args;
        std::size_t si = git_sub_index(c);
        std::string sub = si < a.size() ? a[si].text : std::string();
        for (std::size_t i = si + 1; i < a.size(); ++i) {
            const std::string& t = a[i].text;
            std::size_t eq = t.find('=');
            std::string name = t.substr(0, eq);
            bool code_opt = (sub == "rebase" && (t == "-x" || long_match(name, "--exec"))) ||
                            (sub == "difftool" && (t == "-x" || long_match(name, "--extcmd")));
            if (code_opt) {
                if (eq != std::string::npos) {
                    out.push_back(t.substr(eq + 1));
                } else if (i + 1 < a.size()) {
                    out.push_back(a[++i].text);
                }
            }
            if (sub == "grep" && (starts_with(t, "-O") || long_match(name, "--open-files-in-pager")) &&
                (t.size() > 2 && t[1] != '-' ? true : eq != std::string::npos)) {
                out.push_back(t[1] != '-' ? t.substr(2) : t.substr(eq + 1));
            }
        }
        if (sub == "bisect" && si + 1 < a.size() && a[si + 1].text == "run") out.push_back(join_from(a, si + 2));
        if (sub == "submodule") {
            for (std::size_t i = si + 1; i < a.size(); ++i) {
                if (a[i].text != "foreach") continue;
                std::size_t k = i + 1;
                while (k < a.size() && a[k].text.size() > 1 && a[k].text[0] == '-') ++k;
                out.push_back(join_from(a, k));
                break;
            }
        }
    }
    return out;
}

std::vector<Word> find_start_words(const std::vector<Word>& w) {
    std::size_t k = 1;
    while (k < w.size()) {
        const std::string& t = w[k].text;
        if (t == "-H" || t == "-L" || t == "-P" || starts_with(t, "-O")) {
            ++k;
        } else if (t == "-D") {
            k += 2;
        } else {
            break;
        }
    }
    std::vector<Word> starts;
    while (k < w.size()) {
        const std::string& t = w[k].text;
        if ((!t.empty() && t[0] == '-') || t == "(" || t == "!" || t == ",") break;
        starts.push_back(w[k++]);
    }
    if (starts.empty()) {
        Word dot;
        dot.text = dot.raw = ".";
        starts.push_back(dot);
    }
    return starts;
}

}  // namespace dale::risk
