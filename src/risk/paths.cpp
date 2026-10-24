#include "risk/paths.hpp"

#include <vector>

namespace dale::risk {
namespace {

bool starts_with(std::string_view s, std::string_view p) { return s.substr(0, p.size()) == p; }

const char* const kCritDirs[] = {"/",          "~",           "/bin",        "/boot",       "/dev",     "/etc",
                                 "/home",      "/lib",        "/lib32",      "/lib64",      "/opt",     "/proc",
                                 "/root",      "/run",        "/sbin",       "/srv",        "/sys",     "/usr",
                                 "/var",       "/mnt",        "/media",      "/usr/bin",    "/usr/lib", "/usr/local",
                                 "/usr/share", "/var/lib",    "/usr/sbin",   "/usr/lib64",  "/usr/libexec",
                                 "/usr/include", "~/.ssh",    "~/.gnupg"};

// ".", ".." or paths made only of ".." (already normalized).
bool is_dots(std::string_view p) {
    if (p == ".") return true;
    if (p.empty()) return false;
    std::size_t k = 0;
    while (k < p.size()) {
        if (p.substr(k, 2) != "..") return false;
        k += 2;
        if (k == p.size()) return true;
        if (p[k] != '/') return false;
        ++k;
    }
    return false;
}

bool crit_dir(std::string_view p, bool dots) {
    for (const char* d : kCritDirs) {
        if (p == d) return true;
    }
    // /home/<name>: another user's home directory (one level)
    if (p.size() > 6 && starts_with(p, "/home/") && p.find('/', 6) == std::string_view::npos) return true;
    return dots && is_dots(p);
}

bool has_glob(std::string_view s) { return s.find_first_of("*?[") != std::string_view::npos; }

bool in_c(std::string_view p, bool dots) {
    if (dots && (p == "*" || p == ".*")) return true;
    // Glob in the last component of a critical directory (/*, /**, /[a-z]*, ~/.*, /etc/*.conf)
    std::size_t slash = p.rfind('/');
    if (slash != std::string_view::npos) {
        std::string_view last = p.substr(slash + 1), base = p.substr(0, slash);
        if (base.empty()) base = "/";
        if (last == "*" && crit_dir(base, dots)) return true;            // ../*
        if (has_glob(last) && crit_dir(base, false)) return true;        // /**, ~/.*, /etc/*.conf
    }
    return crit_dir(p, dots);
}

bool name_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

// Replaces a leading $VAR, ${VAR} or ${VAR<operator>…} with "to".
bool replace_var(std::string& s, std::string_view var, std::string_view to) {
    std::string plain = "$" + std::string(var), brace = "${" + std::string(var);
    if (starts_with(s, brace) && s.size() > brace.size() && !name_char(s[brace.size()])) {
        std::size_t close = s.find('}', brace.size());
        if (close == std::string::npos) return false;
        s = std::string(to) + s.substr(close + 1);
        return true;
    }
    if (starts_with(s, plain) && (s.size() == plain.size() || !name_char(s[plain.size()]))) {
        s = std::string(to) + s.substr(plain.size());
        return true;
    }
    return false;
}

}  // namespace

std::string normalize_path(std::string_view text, bool tilde) {
    std::string s(text);
    if (s.empty()) return s;
    if (s[0] == '~' && !tilde) {
        s = "./" + s;  // quoted ~: a file with that name
    } else if (!replace_var(s, "HOME", "~")) {
        replace_var(s, "PWD", ".");
    }
    if (s.empty()) return s;
    if (s[0] == '~') {
        if (s.size() > 1 && s[1] != '/') {
            // ~root is /root; ~+ is the current directory; any other ~user is treated as ~.
            std::size_t slash = s.find('/');
            std::string user = s.substr(1, slash == std::string::npos ? std::string::npos : slash - 1);
            std::string rest = slash == std::string::npos ? std::string() : s.substr(slash);
            s = (user == "root" ? "/root" : user == "+" ? "." : "~") + rest;
        }
    }

    enum class Base { Rel, Abs, Home } base = Base::Rel;
    std::size_t k = 0;
    if (s[0] == '/') {
        base = Base::Abs;
    } else if (s[0] == '~' && (s.size() == 1 || s[1] == '/')) {
        base = Base::Home;
        k = 1;
    }
    std::vector<std::string> parts;
    while (k < s.size()) {
        std::size_t e = s.find('/', k);
        if (e == std::string::npos) e = s.size();
        std::string part = s.substr(k, e - k);
        k = e + 1;
        if (part.empty() || part == ".") continue;
        if (part == "..") {
            if (!parts.empty() && parts.back() != "..") {
                parts.pop_back();
            } else if (base == Base::Home) {
                base = Base::Abs;  // the parent of ~ is /home
                parts = {"home"};
            } else if (base == Base::Rel) {
                parts.push_back(part);
            }
            continue;
        }
        parts.push_back(part);
    }
    std::string out;
    if (base == Base::Abs) out = "/";
    if (base == Base::Home) out = "~";
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (!out.empty() && out.back() != '/') out += '/';
        out += parts[i];
    }
    if (out.empty()) out = ".";
    if (base == Base::Rel && out[0] == '~') out = "./" + out;  // never confused with ~
    return out;
}

bool in_C(std::string_view norm) { return in_c(norm, true); }
bool in_Cstar(std::string_view norm) { return in_c(norm, false); }

bool in_F(std::string_view p) {
    static const char* const kFiles[] = {
        "/etc/passwd",        "/etc/shadow",          "/etc/group",         "/etc/gshadow",
        "/etc/sudoers",       "/etc/fstab",           "/etc/hosts",         "/etc/resolv.conf",
        "/etc/ld.so.preload", "/etc/ld.so.conf",      "/etc/ssh/sshd_config", "/etc/profile",
        "/etc/bash.bashrc",   "/etc/environment",     "/etc/crontab",       "~/.bashrc",
        "~/.profile",         "~/.bash_profile",      "~/.bash_login",      "~/.zshrc",
        "~/.zprofile",        "~/.ssh/authorized_keys", "/root/.bashrc",    "/root/.profile",
        "/root/.ssh/authorized_keys"};
    for (const char* f : kFiles) {
        if (p == f) return true;
    }
    for (std::string_view pre : {"/etc/sudoers.d/", "/boot/", "/etc/pam.d/"}) {
        if (p.size() > pre.size() && starts_with(p, pre)) return true;
    }
    return false;
}

bool in_D(std::string_view p) {
    // Any /dev/* except the harmless pseudo-devices.
    if (!starts_with(p, "/dev/") || p.size() == 5) return false;
    std::string_view n = p.substr(5);
    std::string_view first = n.substr(0, n.find('/'));  // /dev/null/x is not a disk either
    for (const char* ok : {"null", "zero", "full", "random", "urandom", "tty", "console", "stdin", "stdout", "stderr",
                           "fd", "pts", "shm"}) {
        if (first == ok) return false;
    }
    return true;
}

}  // namespace dale::risk
