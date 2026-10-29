#include "doctest.h"

#include "config/state.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>

using namespace dale::config;
namespace fs = std::filesystem;

namespace {

// Saves an environment variable and restores it when going out of scope.
class EnvGuard {
public:
    explicit EnvGuard(const char* name) : name_(name) {
        if (const char* v = std::getenv(name)) old_ = v;
    }
    ~EnvGuard() {
        if (old_) setenv(name_, old_->c_str(), 1);
        else unsetenv(name_);
    }
    void set(const std::string& v) { setenv(name_, v.c_str(), 1); }
    void unset() { unsetenv(name_); }
    EnvGuard(const EnvGuard&) = delete;
    EnvGuard& operator=(const EnvGuard&) = delete;

private:
    const char* name_;
    std::optional<std::string> old_;
};

// Own temporary directory, removed on exit.
struct TempDir {
    fs::path path;
    TempDir() {
        std::string tmpl = (fs::temp_directory_path() / "dale_state_test_XXXXXX").string();
        REQUIRE(mkdtemp(tmpl.data()) != nullptr);
        path = tmpl;
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

Setup gpu_setup() {
    Setup s;
    s.device = Device::Gpu;
    s.model = "dale-9b-1.0.0-Q4_K_M.gguf";
    s.gpu = "NVIDIA GeForce RTX 5070 Ti";
    s.vram_free_mib = 12000;
    return s;
}

Setup cpu_setup() {
    Setup s;
    s.device = Device::Cpu;
    s.model = "dale-0.8b-1.0.0-Q4_K_M.gguf";
    return s;
}

bool same(const Setup& a, const Setup& b) {
    return a.device == b.device && a.model == b.model && a.gpu == b.gpu && a.vram_free_mib == b.vram_free_mib;
}

}  // namespace

TEST_SUITE("state") {

TEST_CASE("format_setup y parse_setup: ida y vuelta") {
    const Setup g = gpu_setup();
    const std::string text = format_setup(g);
    CHECK(text.rfind("# ", 0) == 0);
    CHECK(text.find("device = gpu\n") != std::string::npos);
    CHECK(text.find("model = dale-9b-1.0.0-Q4_K_M.gguf\n") != std::string::npos);
    CHECK(text.find("gpu = NVIDIA GeForce RTX 5070 Ti\n") != std::string::npos);
    CHECK(text.find("vram_free_mib = 12000\n") != std::string::npos);
    const auto back = parse_setup(text);
    REQUIRE(back);
    CHECK(same(*back, g));

    // CPU: only device and model are written.
    const Setup cpu = cpu_setup();
    const std::string cpu_text = format_setup(cpu);
    CHECK(cpu_text.find("gpu =") == std::string::npos);
    CHECK(cpu_text.find("vram_free_mib") == std::string::npos);
    const auto cpu_back = parse_setup(cpu_text);
    REQUIRE(cpu_back);
    CHECK(same(*cpu_back, cpu));
}

TEST_CASE("parse_setup: comentarios, líneas vacías, espacios y CRLF") {
    const auto s = parse_setup("# hola\r\n\r\n  device=gpu \r\n\tmodel =\tdale-4b-1.0.0-Q4_K_M.gguf\r\n"
                               "gpu = AMD Radeon RX 7800 XT\r\nvram_free_mib = 6000");
    REQUIRE(s);
    CHECK(s->device == Device::Gpu);
    CHECK(s->model == "dale-4b-1.0.0-Q4_K_M.gguf");
    CHECK(s->gpu == "AMD Radeon RX 7800 XT");
    CHECK(s->vram_free_mib == 6000);
}

TEST_CASE("parse_setup: líneas, claves y valores no válidos") {
    const std::string ok = "device = cpu\nmodel = dale-0.8b-1.0.0-Q4_K_M.gguf\n";
    REQUIRE(parse_setup(ok));
    for (const char* text : {
             "",
             "# solo comentario\n",
             "model = dale-0.8b-1.0.0-Q4_K_M.gguf\n",                    // no device
             "device = cpu\n",                                           // no model
             "device = cpu\nmodel =\n",                                  // empty model
             "device = tpu\nmodel = m.gguf\n",                           // unknown device
             "device = GPU\nmodel = m.gguf\n",                           // case matters
             "device = cpu\nmodel = m.gguf\nlinea sin igual\n",          // no '='
             "device = cpu\nmodel = m.gguf\ncolor = azul\n",             // unknown key
             "device = gpu\nmodel = m.gguf\nvram_free_mib = 12a\n",      // not a number
             "device = gpu\nmodel = m.gguf\nvram_free_mib = -1\n",       // negative
             "device = gpu\nmodel = m.gguf\nvram_free_mib =\n",          // empty number
             "device = gpu\nmodel = m.gguf\nvram_free_mib = 1234567890123\n",  // more than 12 digits
             "device = cpu\nmodel = ../m.gguf\n",                        // '/' in the model
             "device = cpu\nmodel = /home/ana/m.gguf\n",
         }) {
        CAPTURE(text);
        CHECK_FALSE(parse_setup(text).has_value());
    }
    const auto twelve = parse_setup("device = gpu\nmodel = m.gguf\nvram_free_mib = 999999999999\n");
    REQUIRE(twelve);
    CHECK(twelve->vram_free_mib == 999999999999ull);
}

TEST_CASE("setup_path: XDG_STATE_HOME, HOME y ninguna") {
    EnvGuard xdg("XDG_STATE_HOME");
    EnvGuard home("HOME");
    home.set("/home/ana");
    xdg.set("/tmp/estado");
    CHECK(setup_path() == "/tmp/estado/dale/setup");
    xdg.set("");
    CHECK(setup_path() == "/home/ana/.local/state/dale/setup");
    xdg.unset();
    CHECK(setup_path() == "/home/ana/.local/state/dale/setup");
    home.unset();
    CHECK(setup_path().empty());
    CHECK_FALSE(load_setup().has_value());
    CHECK_FALSE(save_setup(gpu_setup()));
}

TEST_CASE("save_setup y load_setup en un XDG_STATE_HOME temporal") {
    EnvGuard xdg("XDG_STATE_HOME");
    EnvGuard home("HOME");
    TempDir tmp;
    home.set("/nonexistent-home-dale");
    const fs::path state = tmp.path / "a" / "b";
    xdg.set(state.string());
    const fs::path file = state / "dale" / "setup";

    CHECK_FALSE(load_setup().has_value());
    CHECK_FALSE(fs::exists(state));

    // Creates the missing directories; 0644; no temporary left behind.
    const Setup g = gpu_setup();
    REQUIRE(save_setup(g));
    REQUIRE(fs::is_regular_file(file));
    CHECK(read_file(file) == format_setup(g));
    struct stat st{};
    REQUIRE(::stat(file.c_str(), &st) == 0);
    CHECK((st.st_mode & 07777) == 0644);
    int entries = 0;
    for (const auto& e : fs::directory_iterator(state / "dale")) {
        CAPTURE(e.path().string());
        CHECK(e.path().filename() == "setup");
        ++entries;
    }
    CHECK(entries == 1);
    const auto loaded = load_setup();
    REQUIRE(loaded);
    CHECK(same(*loaded, g));

    // Overwrites the previous one.
    const Setup cpu = cpu_setup();
    REQUIRE(save_setup(cpu));
    const auto again = load_setup();
    REQUIRE(again);
    CHECK(same(*again, cpu));

    // An invalid file loads as nothing.
    std::ofstream(file, std::ios::binary | std::ios::trunc) << "device = quizá\nmodel = m.gguf\n";
    CHECK_FALSE(load_setup().has_value());

    // Without XDG_STATE_HOME, under HOME.
    xdg.unset();
    home.set(tmp.path.string());
    REQUIRE(save_setup(cpu));
    CHECK(fs::is_regular_file(tmp.path / ".local" / "state" / "dale" / "setup"));
}

TEST_CASE("save_setup: directorio imposible de crear, false") {
    EnvGuard xdg("XDG_STATE_HOME");
    TempDir tmp;
    std::ofstream(tmp.path / "fichero") << "x";
    xdg.set((tmp.path / "fichero").string());  // a file where the directory should go
    CHECK_FALSE(save_setup(gpu_setup()));
}

}  // TEST_SUITE("state")
