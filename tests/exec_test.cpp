#include "doctest.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "exec/base64.hpp"
#include "exec/exec.hpp"

namespace fs = std::filesystem;
using dale::exec::base64_encode;
using dale::exec::copy_to_clipboard;
using dale::exec::edit_in_editor;
using dale::exec::run_bash;

namespace {

// Own temporary directory; removed on exit.
struct TempDir {
    fs::path path;
    TempDir() {
        std::string tmpl = (fs::temp_directory_path() / "dale-exec-test-XXXXXX").string();
        std::vector<char> buf(tmpl.begin(), tmpl.end());
        buf.push_back('\0');
        REQUIRE(mkdtemp(buf.data()) != nullptr);
        path = buf.data();
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

// Saves an environment variable and restores it on exit.
struct EnvGuard {
    std::string name;
    std::optional<std::string> old;
    explicit EnvGuard(std::string n) : name(std::move(n)) {
        if (const char* v = std::getenv(name.c_str())) old = v;
    }
    ~EnvGuard() {
        if (old) setenv(name.c_str(), old->c_str(), 1);
        else unsetenv(name.c_str());
    }
    void set(const std::string& v) const { setenv(name.c_str(), v.c_str(), 1); }
    void unset() const { unsetenv(name.c_str()); }
    EnvGuard(const EnvGuard&) = delete;
    EnvGuard& operator=(const EnvGuard&) = delete;
};

// Changes the current directory and restores it on exit.
struct CwdGuard {
    fs::path old = fs::current_path();
    explicit CwdGuard(const fs::path& p) { fs::current_path(p); }
    ~CwdGuard() {
        std::error_code ec;
        fs::current_path(old, ec);
    }
    CwdGuard(const CwdGuard&) = delete;
    CwdGuard& operator=(const CwdGuard&) = delete;
};

std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void write_script(const fs::path& p, const std::string& body) {
    {
        std::ofstream out(p, std::ios::binary);
        out << "#!/bin/bash\n" << body;
    }
    fs::permissions(p, fs::perms::owner_all);
}

// Single quotes for bash.
std::string sq(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    return out + "'";
}

bool has_dale_temp(const fs::path& dir) {
    for (const auto& e : fs::directory_iterator(dir)) {
        const std::string name = e.path().filename().string();
        if (name.rfind("dale-", 0) == 0 && name.size() > 3 &&
            name.compare(name.size() - 3, 3, ".sh") == 0)
            return true;
    }
    return false;
}

// Runs f with fd 1 redirected to a file and returns what was written.
template <typename F>
std::string capture_stdout(const fs::path& file, F f) {
    std::fflush(stdout);
    const int saved = dup(1);
    const int fd = open(file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    dup2(fd, 1);
    close(fd);
    f();
    std::fflush(stdout);
    dup2(saved, 1);
    close(saved);
    return read_file(file);
}

}  // namespace

TEST_SUITE("exec") {

TEST_CASE("base64: vectores del RFC 4648") {
    CHECK(base64_encode("") == "");
    CHECK(base64_encode("f") == "Zg==");
    CHECK(base64_encode("fo") == "Zm8=");
    CHECK(base64_encode("foo") == "Zm9v");
    CHECK(base64_encode("foob") == "Zm9vYg==");
    CHECK(base64_encode("fooba") == "Zm9vYmE=");
    CHECK(base64_encode("foobar") == "Zm9vYmFy");
}

TEST_CASE("base64: bytes binarios") {
    CHECK(base64_encode(std::string(1, '\0')) == "AA==");
    CHECK(base64_encode(std::string(3, '\0')) == "AAAA");
    CHECK(base64_encode("\xff") == "/w==");
    CHECK(base64_encode("\xff\xff\xff") == "////");
    CHECK(base64_encode("\xfb\xff") == "+/8=");
}

TEST_CASE("run_bash: códigos de salida") {
    CHECK(run_bash("exit 0") == 0);
    CHECK(run_bash("exit 3") == 3);
    CHECK(run_bash("exit 255") == 255);
    CHECK(run_bash("true") == 0);
    CHECK(run_bash("false") == 1);
}

TEST_CASE("run_bash: muerte por señal") {
    CHECK(run_bash("kill -TERM $$") == 143);
    CHECK(run_bash("kill -KILL $$") == 137);
}

TEST_CASE("run_bash: comillas, $, directorio actual y entorno") {
    TempDir dir;
    EnvGuard var("DALE_EXEC_TEST_VAR");
    var.set("valor con espacios");
    {
        CwdGuard cwd(dir.path);
        CHECK(run_bash("printf '%s' \"$DALE_EXEC_TEST_VAR\" > env.txt") == 0);
        CHECK(run_bash("pwd -P > cwd.txt") == 0);
        CHECK(run_bash("printf '%s|%s' 'a \"b\" $HOME' \"x'y\" > quotes.txt") == 0);
        CHECK(run_bash("test \"$(printf '%s' '$1')\" = '$1'") == 0);
    }
    CHECK(read_file(dir.path / "env.txt") == "valor con espacios");
    CHECK(read_file(dir.path / "cwd.txt") == fs::canonical(dir.path).string() + "\n");
    CHECK(read_file(dir.path / "quotes.txt") == "a \"b\" $HOME|x'y");
}

TEST_CASE("run_bash: el hijo recibe SIGINT con la acción por defecto") {
    struct sigaction ign {}, old{};
    ign.sa_handler = SIG_IGN;
    sigemptyset(&ign.sa_mask);
    sigaction(SIGINT, &ign, &old);
    const int code = run_bash("kill -INT $$");
    struct sigaction after {};
    sigaction(SIGINT, nullptr, &after);
    sigaction(SIGINT, &old, nullptr);
    CHECK(code == 130);
    CHECK(after.sa_handler == SIG_IGN);  // dale restores the previous action
}

TEST_CASE("run_bash: sin bash en PATH devuelve 127") {
    TempDir empty;
    EnvGuard path("PATH");
    path.set(empty.path.string());
    CHECK(run_bash("exit 0") == 127);
}

TEST_CASE("edit_in_editor: editores falsos") {
    TempDir dir;
    EnvGuard visual("VISUAL");
    EnvGuard editor("EDITOR");
    EnvGuard tmpdir("TMPDIR");
    const fs::path work = dir.path / "tmp";
    fs::create_directory(work);
    tmpdir.set(work.string());
    visual.unset();

    const fs::path d = dir.path;
    const std::string D = sq(d.string());

    SUBCASE("reescribe el fichero; permisos 0600 y temporal borrado") {
        const fs::path script = d / "ed-rewrite";
        write_script(script, "stat -c %a \"$1\" > " + D + "/perm\n"
                             "cat \"$1\" > " + D + "/orig\n"
                             "printf '%s\\n' \"$1\" > " + D + "/path\n"
                             "printf 'ls -la /tmp\\n\\r\\n' > \"$1\"\n");
        editor.set(script.string());
        const auto r = edit_in_editor("echo 'hola' $HOME");
        REQUIRE(r.has_value());
        CHECK(*r == "ls -la /tmp");
        CHECK(read_file(d / "perm") == "600\n");
        CHECK(read_file(d / "orig") == "echo 'hola' $HOME\n");
        const std::string p = read_file(d / "path");
        CHECK(p.rfind(work.string() + "/dale-", 0) == 0);
        CHECK(p.size() > 4);
        CHECK(p.compare(p.size() - 4, 4, ".sh\n") == 0);
        CHECK_FALSE(has_dale_temp(work));
    }

    SUBCASE("puede quedar vacío") {
        const fs::path script = d / "ed-empty";
        write_script(script, ": > \"$1\"\n");
        editor.set(script.string());
        const auto r = edit_in_editor("ls");
        REQUIRE(r.has_value());
        CHECK(r->empty());
        CHECK_FALSE(has_dale_temp(work));
    }

    SUBCASE("sale con 1: cancelado") {
        const fs::path script = d / "ed-fail";
        write_script(script, "printf 'otro\\n' > \"$1\"\nexit 1\n");
        editor.set(script.string());
        CHECK_FALSE(edit_in_editor("ls").has_value());
        CHECK_FALSE(has_dale_temp(work));
    }

    SUBCASE("muere por una señal: cancelado") {
        const fs::path script = d / "ed-killed";
        write_script(script, "kill -TERM $$\n");
        editor.set(script.string());
        CHECK_FALSE(edit_in_editor("ls").has_value());
        CHECK_FALSE(has_dale_temp(work));
    }

    SUBCASE("editor con argumentos") {
        const fs::path script = d / "ed-args";
        write_script(script, "printf '%s\\n' \"$#\" \"$1\" > " + D + "/args\n"
                             "printf 'pwd\\n' > \"$2\"\n");
        editor.set(script.string() + " --flag");
        const auto r = edit_in_editor("ls");
        REQUIRE(r.has_value());
        CHECK(*r == "pwd");
        CHECK(read_file(d / "args") == "2\n--flag\n");
    }

    SUBCASE("VISUAL tiene prioridad sobre EDITOR") {
        const fs::path sv = d / "ed-visual";
        const fs::path se = d / "ed-editor";
        write_script(sv, "printf 'visual\\n' > \"$1\"\n");
        write_script(se, "printf 'editor\\n' > \"$1\"\n");
        visual.set(sv.string());
        editor.set(se.string());
        CHECK(edit_in_editor("ls") == std::optional<std::string>("visual"));
        visual.set("");
        CHECK(edit_in_editor("ls") == std::optional<std::string>("editor"));
    }
}

TEST_CASE("copy_to_clipboard: programas falsos y OSC 52") {
    TempDir dir;
    EnvGuard path("PATH");
    EnvGuard wayland("WAYLAND_DISPLAY");
    EnvGuard display("DISPLAY");
    wayland.unset();
    display.unset();

    const fs::path bin = dir.path / "bin";
    fs::create_directory(bin);
    const std::string D = sq(dir.path.string());
    // Bash builtins only: PATH points only to the fake programs.
    auto fake = [&](const std::string& name, int code) {
        write_script(bin / name, "printf '%s\\n' \"$*\" > " + D + "/" + name + ".args\n"
                                 "IFS= read -r -d '' data\n"
                                 "printf '%s' \"$data\" > " + D + "/" + name + ".out\n"
                                 "exit " + std::to_string(code) + "\n");
    };
    path.set(bin.string());
    const std::string text = "echo 'hola $USER' | grep \"x\"\nlínea 2\n";

    SUBCASE("WAYLAND_DISPLAY: wl-copy") {
        fake("wl-copy", 0);
        fake("xclip", 0);
        wayland.set("wayland-0");
        display.set(":0");
        CHECK(copy_to_clipboard(text));
        CHECK(read_file(dir.path / "wl-copy.out") == text);
        CHECK_FALSE(fs::exists(dir.path / "xclip.out"));
    }

    SUBCASE("solo DISPLAY: xclip") {
        fake("wl-copy", 0);
        fake("xclip", 0);
        display.set(":0");
        CHECK(copy_to_clipboard(text));
        CHECK(read_file(dir.path / "xclip.out") == text);
        CHECK(read_file(dir.path / "xclip.args") == "-selection clipboard\n");
        CHECK_FALSE(fs::exists(dir.path / "wl-copy.out"));
    }

    SUBCASE("wl-copy falla: xclip") {
        fake("wl-copy", 1);
        fake("xclip", 0);
        wayland.set("wayland-0");
        display.set(":0");
        CHECK(copy_to_clipboard(text));
        CHECK(read_file(dir.path / "xclip.out") == text);
    }

    SUBCASE("xclip falla: xsel") {
        fake("xclip", 1);
        fake("xsel", 0);
        display.set(":0");
        CHECK(copy_to_clipboard(text));
        CHECK(read_file(dir.path / "xsel.out") == text);
        CHECK(read_file(dir.path / "xsel.args") == "--clipboard --input\n");
    }

    SUBCASE("sale sin leer stdin: no mata a dale") {
        write_script(bin / "xclip", "exit 1\n");
        fake("xsel", 0);
        display.set(":0");
        CHECK(copy_to_clipboard(std::string(1 << 18, 'a')));
        CHECK(read_file(dir.path / "xsel.out").size() == (1u << 18));
    }

    SUBCASE("sin variables: OSC 52") {
        fake("wl-copy", 0);
        fake("xclip", 0);
        bool ok = false;
        const std::string out =
            capture_stdout(dir.path / "osc.out", [&] { ok = copy_to_clipboard(text); });
        CHECK(ok);
        CHECK(out == "\x1b]52;c;" + base64_encode(text) + "\x07");
        CHECK_FALSE(fs::exists(dir.path / "wl-copy.out"));
        CHECK_FALSE(fs::exists(dir.path / "xclip.out"));
    }

    SUBCASE("variables sin herramientas: OSC 52") {
        wayland.set("wayland-0");
        display.set(":0");
        bool ok = false;
        const std::string out =
            capture_stdout(dir.path / "osc.out", [&] { ok = copy_to_clipboard("ls"); });
        CHECK(ok);
        CHECK(out == "\x1b]52;c;bHM=\x07");
    }
}

}  // TEST_SUITE("exec")
