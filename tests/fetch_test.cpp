#include "doctest.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "fetch/fetch.hpp"
#include "fetch/sha256.hpp"

namespace fs = std::filesystem;
using dale::fetch::download;
using dale::fetch::DownloadOptions;
using dale::fetch::DownloadResult;
using dale::fetch::models_dir;
using dale::fetch::sha256_hex;
using Status = DownloadResult::Status;

namespace {

constexpr const char* kUrl = "https://example.invalid/model.gguf";
const fs::path kFakeBin = fs::path(DALE_TESTS_DIR) / "fetch_bin";

struct TempDir {
    fs::path path;
    TempDir() {
        std::string tmpl = (fs::temp_directory_path() / "dale-fetch-test-XXXXXX").string();
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

std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void write_file(const fs::path& p, const std::string& data) {
    std::ofstream out(p, std::ios::binary);
    out << data;
}

std::vector<std::string> read_lines(const fs::path& p) {
    std::ifstream in(p);
    std::vector<std::string> lines;
    for (std::string line; std::getline(in, line);) lines.push_back(line);
    return lines;
}

bool contains(const std::vector<std::string>& v, const std::string& s) {
    for (const auto& x : v) {
        if (x == s) return true;
    }
    return false;
}

bool has_part_files(const fs::path& dir) {
    if (!fs::exists(dir)) return false;
    for (const auto& e : fs::directory_iterator(dir)) {
        if (e.path().filename().string().find(".part-") != std::string::npos) return true;
    }
    return false;
}

bool clean_message(const std::string& m) {
    return !m.empty() && m.back() != '.' && m.back() != '\n';
}

// Scenario: source file, temporary PATH with the requested fake tools and the FAKE_*
// variables the scripts read. Never any network: path_env does not include the real PATH.
struct Fixture {
    TempDir tmp;
    fs::path bin = tmp.path / "bin";
    fs::path dir = tmp.path / "models";
    fs::path src = tmp.path / "source.gguf";
    fs::path log = tmp.path / "log.txt";
    std::string content = "contenido del modelo falso\n" + std::string(5000, 'x');
    EnvGuard fake_src{"FAKE_SRC"};
    EnvGuard fake_log{"FAKE_LOG"};
    EnvGuard fake_fail{"FAKE_FAIL"};
    EnvGuard fake_signal{"FAKE_SIGNAL"};
    DownloadOptions options;

    explicit Fixture(std::initializer_list<const char*> tools) {
        fs::create_directory(bin);
        // Copied and made executable: does not depend on the mode of the repo files.
        for (const char* t : tools) {
            fs::copy_file(kFakeBin / t, bin / t);
            fs::permissions(bin / t, fs::perms::owner_all);
        }
        write_file(src, content);
        fake_src.set(src.string());
        fake_log.set(log.string());
        fake_fail.unset();
        fake_signal.unset();
        options.path_env = bin.string();
    }

    DownloadResult run(std::uint64_t size, const std::string& sha) const {
        return download(kUrl, dir.string(), "model.gguf", size, sha, options);
    }
    DownloadResult run() const { return run(content.size(), sha256_hex(content)); }
    fs::path final_path() const { return dir / "model.gguf"; }
};

}  // namespace

TEST_SUITE("fetch") {

TEST_CASE("models_dir: XDG_DATA_HOME y HOME") {
    EnvGuard xdg("XDG_DATA_HOME");
    EnvGuard home("HOME");
    xdg.set("/datos");
    home.set("/casa");
    CHECK(models_dir() == "/datos/dale/models");
    xdg.set("");
    CHECK(models_dir() == "/casa/.local/share/dale/models");
    xdg.unset();
    CHECK(models_dir() == "/casa/.local/share/dale/models");
    home.unset();
    CHECK(models_dir().empty());
}

TEST_CASE("download: camino correcto con curl") {
    Fixture f({"curl", "wget"});
    const auto r = f.run();
    REQUIRE(r.status == Status::Ok);
    CHECK(r.path == f.final_path().string());
    CHECK(read_file(f.final_path()) == f.content);
    CHECK((fs::status(f.final_path()).permissions() & fs::perms::all) == fs::perms(0644));
    CHECK_FALSE(has_part_files(f.dir));
    const auto args = read_lines(f.log);
    REQUIRE(!args.empty());
    CHECK(args.front() == "curl");
    CHECK(args.back() == kUrl);
    for (const char* a : {"-fL", "--proto", "=https", "--tlsv1.2", "--retry", "3",
                          "--connect-timeout", "20", "-o"}) {
        CAPTURE(a);
        CHECK(contains(args, a));
    }
    CHECK((contains(args, "--progress-bar") || contains(args, "-sS")));
}

TEST_CASE("download: sin curl se usa wget") {
    Fixture f({"wget"});
    const auto r = f.run();
    REQUIRE(r.status == Status::Ok);
    CHECK(read_file(f.final_path()) == f.content);
    CHECK_FALSE(has_part_files(f.dir));
    const auto args = read_lines(f.log);
    REQUIRE(!args.empty());
    CHECK(args.front() == "wget");
    CHECK(contains(args, "--https-only"));
    CHECK(contains(args, "-q"));
    CHECK(contains(args, "-O"));
    CHECK(args.back() == kUrl);
}

TEST_CASE("download: hash erróneo no deja ni final ni temporal") {
    Fixture f({"curl"});
    const auto r = f.run(f.content.size(), sha256_hex("otra cosa"));
    CHECK(r.status == Status::Failed);
    CHECK(r.error == "el fichero descargado no es íntegro");
    CHECK(r.error_en == "the downloaded file is corrupted");
    CHECK_FALSE(fs::exists(f.final_path()));
    CHECK_FALSE(has_part_files(f.dir));
}

TEST_CASE("download: tamaño erróneo") {
    Fixture f({"curl"});
    const auto r = f.run(f.content.size() + 1, sha256_hex(f.content));
    CHECK(r.status == Status::Failed);
    CHECK(r.error == "el fichero descargado no es íntegro");
    CHECK_FALSE(fs::exists(f.final_path()));
    CHECK_FALSE(has_part_files(f.dir));
}

TEST_CASE("download: sin curl ni wget, mensaje con URL y SHA-256") {
    Fixture f({});
    const std::string sha = sha256_hex(f.content);
    const auto r = f.run(f.content.size(), sha);
    CHECK(r.status == Status::Failed);
    CHECK(r.error.find(kUrl) != std::string::npos);
    CHECK(r.error.find(sha) != std::string::npos);
    CHECK(r.error.find("--model") != std::string::npos);
    CHECK(r.error_en.find(kUrl) != std::string::npos);
    CHECK(r.error_en.find(sha) != std::string::npos);
    CHECK(r.error_en.find("--model") != std::string::npos);
    CHECK(clean_message(r.error));
    CHECK(clean_message(r.error_en));
    CHECK_FALSE(fs::exists(f.log));
    CHECK_FALSE(has_part_files(f.dir));
}

TEST_CASE("download: error de la herramienta") {
    for (const char* tool : {"curl", "wget"}) {
        CAPTURE(tool);
        Fixture f({tool});
        f.fake_fail.set("1");
        const auto r = f.run();
        CHECK(r.status == Status::Failed);
        CHECK(r.error.find(tool) != std::string::npos);
        CHECK(r.error.find("22") != std::string::npos);
        CHECK(r.error_en.find(tool) != std::string::npos);
        CHECK(clean_message(r.error));
        CHECK(clean_message(r.error_en));
        CHECK_FALSE(fs::exists(f.final_path()));
        CHECK_FALSE(has_part_files(f.dir));
    }
}

TEST_CASE("download: Ctrl-C en la herramienta es Interrupted; otra señal es Failed") {
    {
        Fixture f({"curl"});
        f.fake_signal.set("INT");
        const auto r = f.run();
        CHECK(r.status == Status::Interrupted);
        CHECK_FALSE(fs::exists(f.final_path()));
        CHECK_FALSE(has_part_files(f.dir));
    }
    {
        Fixture f({"curl"});
        f.fake_signal.set("TERM");
        const auto r = f.run();
        CHECK(r.status == Status::Failed);
        CHECK(r.error.find("15") != std::string::npos);
        CHECK_FALSE(has_part_files(f.dir));
    }
}

TEST_CASE("download: falta de espacio") {
    Fixture f({"curl"});
    std::string seen_dir;
    // Needs the size plus 10 %: with 10000 bytes, 10999 is not enough.
    f.options.free_space = [&](const std::string& d) -> std::optional<std::uint64_t> {
        seen_dir = d;
        return 10999;
    };
    const auto r = f.run(10000, sha256_hex(f.content));
    CHECK(r.status == Status::Failed);
    CHECK(seen_dir == f.dir.string());
    CHECK(r.error.find("espacio") != std::string::npos);
    CHECK(r.error_en.find("space") != std::string::npos);
    CHECK(clean_message(r.error));
    CHECK(clean_message(r.error_en));
    CHECK_FALSE(fs::exists(f.log));  // curl was never even launched
    CHECK_FALSE(has_part_files(f.dir));

    // Exactly what is needed, or unknown free space: goes ahead.
    f.options.free_space = [&](const std::string&) -> std::optional<std::uint64_t> {
        return f.content.size() + f.content.size() / 10;
    };
    CHECK(f.run().status == Status::Ok);
    fs::remove(f.final_path());
    f.options.free_space = [](const std::string&) -> std::optional<std::uint64_t> {
        return std::nullopt;
    };
    CHECK(f.run().status == Status::Ok);
}

TEST_CASE("download: crea el directorio con los padres") {
    Fixture f({"curl"});
    const fs::path deep = f.tmp.path / "a" / "b" / "c";
    const auto r = download(kUrl, deep.string(), "m.gguf", f.content.size(),
                            sha256_hex(f.content), f.options);
    REQUIRE(r.status == Status::Ok);
    CHECK(fs::is_directory(deep));
    CHECK(read_file(deep / "m.gguf") == f.content);
    struct stat st {};
    REQUIRE(stat(deep.c_str(), &st) == 0);
    CHECK((st.st_mode & 0700) == 0700);
    CHECK((st.st_mode & 0022) == 0);  // 0755 (or less due to the umask), never writable by others
}

TEST_CASE("download: no se puede crear el directorio") {
    Fixture f({"curl"});
    write_file(f.tmp.path / "fichero", "no soy un directorio");
    const auto r = download(kUrl, (f.tmp.path / "fichero" / "sub").string(), "m.gguf",
                            f.content.size(), sha256_hex(f.content), f.options);
    CHECK(r.status == Status::Failed);
    CHECK(clean_message(r.error));
    CHECK(clean_message(r.error_en));
    CHECK_FALSE(fs::exists(f.log));
}

}  // TEST_SUITE
