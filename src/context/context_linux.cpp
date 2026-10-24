#include "context/context.hpp"

#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>
#include <string_view>

namespace dale::context {

namespace {

// Fixed list of detected tools. Includes the package managers.
constexpr std::string_view kTools[] = {
    "7z",      "apk",         "apt",          "bat",     "batcat",   "convert",    "curl",
    "dig",     "dnf",         "doas",         "docker",  "exiftool", "eza",        "fd",
    "fdfind",  "ffmpeg",      "firewall-cmd", "flatpak", "fzf",      "gcc",        "getfacl",
    "git",     "hostnamectl", "htop",         "ip",      "iptables", "journalctl", "jq",
    "kubectl", "loginctl",    "lsof",         "magick",  "make",     "nc",         "ncdu",
    "netstat", "nft",         "nmap",         "nmcli",   "pacman",   "pdftotext",  "podman",
    "python3", "rg",          "rsync",        "setfacl", "snap",     "ss",         "ssh",
    "sudo",    "systemctl",   "timedatectl",  "tree",    "ufw",      "unrar",      "unzip",
    "wget",    "xz",          "yq",           "yum",     "zip",      "zstd",       "zypper",
};

constexpr std::string_view kPmFallback[] = {"apt", "dnf", "yum", "pacman", "zypper", "apk"};

std::string trim(std::string_view s) {
    const auto first = s.find_first_not_of(" \t\r");
    if (first == std::string_view::npos) return {};
    const auto last = s.find_last_not_of(" \t\r");
    return std::string(s.substr(first, last - first + 1));
}

// Lowercase and only [a-z0-9._+-]: strips quotes and anything that would break the line.
// Spaces are kept only in ID_LIKE, which is a list.
std::string clean(std::string_view s, bool keep_spaces) {
    std::string out;
    for (char c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-' ||
            c == '+' || (keep_spaces && c == ' '))
            out += c;
    }
    return out;
}

struct OsRelease {
    bool found = false;
    std::string id, version_id, id_like;
};

OsRelease read_os_release(const std::vector<std::string>& paths) {
    OsRelease r;
    for (const auto& path : paths) {
        std::ifstream in(path);
        if (!in) continue;
        r.found = true;
        std::string line;
        while (std::getline(in, line)) {
            const auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            const std::string key = trim(std::string_view(line).substr(0, eq));
            const std::string_view value = std::string_view(line).substr(eq + 1);
            if (key == "ID") r.id = clean(value, false);
            else if (key == "VERSION_ID") r.version_id = clean(value, false);
            else if (key == "ID_LIKE") r.id_like = clean(value, true);
        }
        break;
    }
    return r;
}

std::string pm_for_id(const std::string& id) {
    if (id == "debian" || id == "ubuntu") return "apt";
    if (id == "fedora" || id == "rhel" || id == "centos") return "dnf";
    if (id == "arch") return "pacman";
    if (id == "suse" || id.rfind("opensuse", 0) == 0) return "zypper";
    if (id == "alpine") return "apk";
    return {};
}

// Walks PATH once and returns the listed tools it finds.
std::set<std::string> find_tools(const std::string& path_env) {
    std::set<std::string> found;
    std::string_view rest = path_env;
    while (true) {
        const auto colon = rest.find(':');
        const std::string_view dir = rest.substr(0, colon);
        // Empty and relative entries are ignored (they would mean the current directory).
        if (!dir.empty() && dir.front() == '/') {
            std::string prefix(dir);
            if (prefix.back() != '/') prefix += '/';
            for (auto tool : kTools) {
                std::string name(tool);
                if (found.count(name) != 0) continue;
                if (::access((prefix + name).c_str(), X_OK) == 0) found.insert(std::move(name));
            }
        }
        if (colon == std::string_view::npos) break;
        rest.remove_prefix(colon + 1);
    }
    return found;
}

}  // namespace

SystemContext detect_context() {
    ContextSources sources;
    if (const char* p = std::getenv("PATH")) sources.path_env = p;
    return detect_context(sources);
}

SystemContext detect_context(const ContextSources& sources) {
    SystemContext ctx;
    ctx.os = "linux";

    const OsRelease rel = read_os_release(sources.os_release_paths);
    if (!rel.found || rel.id.empty()) ctx.distro = "unknown";
    else if (rel.version_id.empty()) ctx.distro = rel.id;
    else ctx.distro = rel.id + "-" + rel.version_id;

    const std::set<std::string> found = find_tools(sources.path_env);
    ctx.tools.assign(found.begin(), found.end());  // std::set already gives alphabetical order

    // ID first, then each word of ID_LIKE; the first one whose binary is in PATH wins.
    std::vector<std::string> ids;
    if (!rel.id.empty()) ids.push_back(rel.id);
    std::istringstream like(rel.id_like);
    for (std::string w; like >> w;) ids.push_back(w);
    for (const auto& id : ids) {
        const std::string pm = pm_for_id(id);
        if (!pm.empty() && found.count(pm) != 0) {
            ctx.pm = pm;
            break;
        }
    }
    if (ctx.pm.empty()) {
        for (auto pm : kPmFallback) {
            if (found.count(std::string(pm)) != 0) {
                ctx.pm = std::string(pm);
                break;
            }
        }
    }
    if (ctx.pm.empty()) ctx.pm = "none";
    return ctx;
}

}  // namespace dale::context
