#include "doctest.h"

#include "context/context.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using dale::context::ContextSources;
using dale::context::detect_context;
using dale::context::SystemContext;
using dale::context::to_line;

// Temporary directory removed when it goes out of scope.
struct TempDir {
    std::string path;
    TempDir() {
        const char* base = std::getenv("TMPDIR");
        std::string tmpl = std::string(base && *base ? base : "/tmp") + "/dale-context-XXXXXX";
        std::vector<char> buf(tmpl.begin(), tmpl.end());
        buf.push_back('\0');
        REQUIRE(::mkdtemp(buf.data()) != nullptr);
        path = buf.data();
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    std::string write(const std::string& name, const std::string& content, mode_t mode = 0644) const {
        const std::string file = path + "/" + name;
        {
            std::ofstream out(file);
            out << content;
        }
        ::chmod(file.c_str(), mode);
        return file;
    }
};

// distro from an os-release; empty PATH.
std::string distro_of(const std::string& content) {
    TempDir dir;
    ContextSources src;
    src.os_release_paths = {dir.write("os-release", content)};
    return detect_context(src).distro;
}

// pm from an os-release and the binaries present in a fake PATH.
std::string pm_of(const std::string& os_release, const std::vector<std::string>& bins) {
    TempDir dir;
    TempDir bin;
    for (const auto& b : bins) bin.write(b, "", 0755);
    ContextSources src;
    src.os_release_paths = {dir.write("os-release", os_release)};
    src.path_env = bin.path;
    return detect_context(src).pm;
}

}  // namespace

TEST_SUITE("context") {

TEST_CASE("distro desde os-release") {
    CHECK(distro_of("NAME=\"Ubuntu\"\nID=ubuntu\nID_LIKE=debian\nVERSION_ID=\"24.04\"\n") ==
          "ubuntu-24.04");
    CHECK(distro_of("ID=debian\nVERSION_ID=\"12\"\n") == "debian-12");
    CHECK(distro_of("NAME=\"Fedora Linux\"\nID=fedora\nVERSION_ID=40\n") == "fedora-40");
    CHECK(distro_of("NAME=\"Arch Linux\"\nID=arch\nBUILD_ID=rolling\n") == "arch");
    CHECK(distro_of("ID=\"opensuse-tumbleweed\"\nID_LIKE=\"opensuse suse\"\nVERSION_ID=\"20241001\"\n") ==
          "opensuse-tumbleweed-20241001");
    CHECK(distro_of("ID=alpine\nVERSION_ID=3.20.3\n") == "alpine-3.20.3");
    CHECK(distro_of("ID=linuxmint\nID_LIKE=\"ubuntu debian\"\nVERSION_ID=\"22\"\n") == "linuxmint-22");
    CHECK(distro_of("# comentario\nID='Ubuntu'\nVERSION_ID='24.04'\n") == "ubuntu-24.04");
    CHECK(distro_of("NAME=sin-id\n") == "unknown");
}

TEST_CASE("os-release ausente o en la segunda ruta") {
    TempDir dir;
    ContextSources src;
    src.os_release_paths = {dir.path + "/no-existe"};
    const SystemContext ctx = detect_context(src);
    CHECK(ctx.os == "linux");
    CHECK(ctx.distro == "unknown");
    CHECK(ctx.pm == "none");
    CHECK(ctx.tools.empty());

    src.os_release_paths = {dir.path + "/no-existe", dir.write("segundo", "ID=debian\nVERSION_ID=12\n")};
    CHECK(detect_context(src).distro == "debian-12");

    // If both exist, the first one wins.
    src.os_release_paths = {dir.write("primero", "ID=fedora\nVERSION_ID=40\n"), dir.path + "/segundo"};
    CHECK(detect_context(src).distro == "fedora-40");
}

TEST_CASE("pm según ID e ID_LIKE, confirmado en PATH") {
    const std::vector<std::string> all = {"apt", "dnf", "yum", "pacman", "zypper", "apk"};
    CHECK(pm_of("ID=ubuntu\n", all) == "apt");
    CHECK(pm_of("ID=debian\n", all) == "apt");
    CHECK(pm_of("ID=fedora\n", all) == "dnf");
    CHECK(pm_of("ID=\"rhel\"\n", all) == "dnf");
    CHECK(pm_of("ID=arch\n", all) == "pacman");
    CHECK(pm_of("ID=\"opensuse-tumbleweed\"\nID_LIKE=\"opensuse suse\"\n", all) == "zypper");
    CHECK(pm_of("ID=sles\nID_LIKE=\"suse\"\n", all) == "zypper");
    CHECK(pm_of("ID=alpine\n", all) == "apk");
    CHECK(pm_of("ID=linuxmint\nID_LIKE=ubuntu\n", all) == "apt");
    CHECK(pm_of("ID=rocky\nID_LIKE=\"rhel centos fedora\"\n", all) == "dnf");

    // Expected binary missing: the first one present from the fallback list.
    CHECK(pm_of("ID=centos\n", {"yum", "apk"}) == "yum");
    CHECK(pm_of("ID=ubuntu\n", {"apk", "pacman"}) == "pacman");
    // Unknown ID.
    CHECK(pm_of("ID=gentoo\n", {"zypper", "apk"}) == "zypper");
    // None.
    CHECK(pm_of("ID=ubuntu\n", {"git"}) == "none");
    CHECK(pm_of("ID=ubuntu\n", {}) == "none");
}

TEST_CASE("tools desde un PATH falso") {
    TempDir a;
    TempDir b;
    TempDir os;
    a.write("rg", "", 0755);
    a.write("jq", "", 0755);
    a.write("git", "", 0644);   // not executable
    a.write("grep", "", 0755);  // not on the list
    b.write("fd", "", 0755);
    b.write("jq", "", 0755);  // duplicate
    b.write("apt", "", 0755);
    b.write("7z", "", 0755);
    b.write("timedatectl", "", 0755);
    a.write("getfacl", "", 0755);
    a.write("apt-mark", "", 0755);  // ships with apt: not listed

    ContextSources src;
    src.os_release_paths = {os.write("os-release", "ID=ubuntu\nVERSION_ID=24.04\n")};
    src.path_env = ":" + a.path + "::" + b.path + "/:";
    const SystemContext ctx = detect_context(src);
    CHECK(ctx.tools == std::vector<std::string>{"7z", "apt", "fd", "getfacl", "jq", "rg", "timedatectl"});
    CHECK(ctx.pm == "apt");
    CHECK(to_line(ctx) == "os=linux distro=ubuntu-24.04 pm=apt tools=7z,apt,fd,getfacl,jq,rg,timedatectl");
}

TEST_CASE("to_line") {
    CHECK(to_line({"linux", "ubuntu-24.04", "apt", {"fd", "jq", "rg"}}) ==
          "os=linux distro=ubuntu-24.04 pm=apt tools=fd,jq,rg");
    CHECK(to_line({"linux", "arch", "pacman", {"git"}}) == "os=linux distro=arch pm=pacman tools=git");
    CHECK(to_line({"linux", "unknown", "none", {}}) == "os=linux distro=unknown pm=none tools=none");
}

TEST_CASE("sistema real: coste por debajo de 5 ms") {
    std::vector<double> ms;
    for (int i = 0; i < 50; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        const SystemContext ctx = detect_context();
        const auto t1 = std::chrono::steady_clock::now();
        CHECK(ctx.os == "linux");
        ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    std::sort(ms.begin(), ms.end());
    const double median = (ms[24] + ms[25]) / 2;
    MESSAGE("mediana detect_context(): " << median << " ms; " << to_line(detect_context()));
    CHECK(median < 5.0);
}

}  // TEST_SUITE
