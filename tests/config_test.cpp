#include "doctest.h"

#include "config/config.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include <unistd.h>

using namespace dale::config;

namespace {

const char* const kFile = "~/.config/dale/config";

ConfigResult parse(std::string_view text, std::string_view home = "/home/ana") {
    return parse_config(text, kFile, home);
}

// Checks that it fails on the given line and returns the result to inspect the messages.
ConfigResult parse_err(std::string_view text, int line) {
    ConfigResult r = parse(text);
    CHECK_FALSE(r.config.has_value());
    const std::string prefix = std::string(kFile) + ":" + std::to_string(line) + ": ";
    CHECK(r.error.rfind(prefix, 0) == 0);
    CHECK(r.error_en.rfind(prefix, 0) == 0);
    return r;
}

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
    std::filesystem::path path;
    TempDir() {
        path = std::filesystem::temp_directory_path() /
               ("dale_config_test_" + std::to_string(getpid()) + "_" + std::to_string(counter()++));
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    static int& counter() {
        static int n = 0;
        return n;
    }
};

void write_file(const std::filesystem::path& p, const std::string& text) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary);
    out << text;
}

}  // namespace

TEST_SUITE("config") {

TEST_CASE("texto vacío y solo comentarios: valores por defecto") {
    for (const char* text : {"", "\n\n", "# comentario\n", "   # con espacios\n\t# con tab\n", "\r\n# x\r\n"}) {
        CAPTURE(text);
        const ConfigResult r = parse(text);
        REQUIRE(r.config.has_value());
        CHECK(r.config->mode == Mode::Confirm);
        CHECK(r.config->lang == Lang::Auto);
        CHECK(r.config->model.empty());
        CHECK(r.config->threads == 0);
        CHECK(r.error.empty());
        CHECK(r.error_en.empty());
    }
}

TEST_CASE("mode: valores válidos") {
    CHECK(parse("mode = suggest").config->mode == Mode::Suggest);
    CHECK(parse("mode = confirm").config->mode == Mode::Confirm);
    CHECK(parse("mode = auto").config->mode == Mode::Auto);
}

TEST_CASE("mode: valor no válido, mensaje exacto en los dos idiomas") {
    const ConfigResult r = parse("# dale\n\nmode = autom\n");
    CHECK_FALSE(r.config.has_value());
    CHECK(r.error == "~/.config/dale/config:3: valor no válido para mode: \"autom\" (válidos: suggest, confirm, auto)");
    CHECK(r.error_en == "~/.config/dale/config:3: invalid value for mode: \"autom\" (valid: suggest, confirm, auto)");
}

TEST_CASE("mode: otros inválidos") {
    for (const char* text : {"mode = Auto", "mode = AUTO", "mode = auto auto", "mode = sugest", "mode = 1"}) {
        CAPTURE(text);
        parse_err(text, 1);
    }
}

TEST_CASE("lang: válidos e inválidos") {
    CHECK(parse("lang = auto").config->lang == Lang::Auto);
    CHECK(parse("lang = es").config->lang == Lang::Es);
    CHECK(parse("lang = en").config->lang == Lang::En);
    for (const char* text : {"lang = ES", "lang = fr", "lang = es_ES", "lang = english"}) {
        CAPTURE(text);
        parse_err(text, 1);
    }
    const ConfigResult r = parse("lang = fr");
    CHECK(r.error == "~/.config/dale/config:1: valor no válido para lang: \"fr\" (válidos: auto, es, en)");
    CHECK(r.error_en == "~/.config/dale/config:1: invalid value for lang: \"fr\" (valid: auto, es, en)");
}

TEST_CASE("model: rutas, ~/ y # dentro del valor") {
    CHECK(parse("model = /opt/m.gguf").config->model == "/opt/m.gguf");
    CHECK(parse("model = rel/m.gguf").config->model == "rel/m.gguf");
    CHECK(parse("model = ~/models/m.gguf").config->model == "/home/ana/models/m.gguf");
    CHECK(parse_config("model = ~/m.gguf", kFile, "/home/ana/").config->model == "/home/ana/m.gguf");
    CHECK(parse("model = /a/b#c.gguf").config->model == "/a/b#c.gguf");
    CHECK(parse("model = /a/m.gguf # no es comentario").config->model == "/a/m.gguf # no es comentario");
    CHECK(parse("model = /a/x=y.gguf").config->model == "/a/x=y.gguf");
    CHECK(parse("model = ~user/m.gguf").config->model == "~user/m.gguf");  // only "~/" is expanded
    CHECK(parse("model = /a/~/b").config->model == "/a/~/b");
}

TEST_CASE("model: ~/ sin HOME es un error") {
    const ConfigResult r = parse_config("model = ~/m.gguf", kFile, "");
    CHECK_FALSE(r.config.has_value());
    CHECK(r.error == "~/.config/dale/config:1: no se puede expandir \"~/\" en model: HOME no está definida");
    CHECK(r.error_en == "~/.config/dale/config:1: cannot expand \"~/\" in model: HOME is not set");
}

TEST_CASE("threads: válidos") {
    CHECK(parse("threads = 1").config->threads == 1);
    CHECK(parse("threads = 8").config->threads == 8);
    CHECK(parse("threads = 256").config->threads == 256);
    CHECK(parse("threads = 004").config->threads == 4);
}

TEST_CASE("threads: inválidos") {
    for (const char* text : {"threads = 0", "threads = 257", "threads = -1", "threads = +4", "threads = abc",
                             "threads = 4 4", "threads = 4x", "threads = 99999999999999999999", "threads = 1.5",
                             "threads = 0x10"}) {
        CAPTURE(text);
        parse_err(text, 1);
    }
    const ConfigResult r = parse("mode = auto\nthreads = 257\n");
    CHECK(r.error == "~/.config/dale/config:2: valor no válido para threads: \"257\" (válidos: entero de 1 a 256)");
    CHECK(r.error_en == "~/.config/dale/config:2: invalid value for threads: \"257\" (valid: integer from 1 to 256)");
}

TEST_CASE("threads: espacio final se recorta, interno no") {
    CHECK(parse("threads = 4 ").config->threads == 4);
    CHECK(parse("threads = 4\t").config->threads == 4);
    parse_err("threads = 4 2", 1);
}

TEST_CASE("todas las claves juntas, con espacios, tabuladores, CRLF y comentarios") {
    const ConfigResult r = parse("# config de dale\r\n"
                                 "\r\n"
                                 "   # comentario con espacios\r\n"
                                 "\t# comentario con tab\r\n"
                                 "  mode\t=\tauto  \r\n"
                                 "lang=en\r\n"
                                 "\t model =  ~/m/q.gguf \t\r\n"
                                 "threads   =   12\r\n"
                                 "   \t  \r\n");
    REQUIRE(r.config.has_value());
    CHECK(r.config->mode == Mode::Auto);
    CHECK(r.config->lang == Lang::En);
    CHECK(r.config->model == "/home/ana/m/q.gguf");
    CHECK(r.config->threads == 12);
}

TEST_CASE("BOM UTF-8 al principio se ignora") {
    const ConfigResult r = parse("\xEF\xBB\xBFmode = suggest\n");
    REQUIRE(r.config.has_value());
    CHECK(r.config->mode == Mode::Suggest);
}

TEST_CASE("clave desconocida") {
    const ConfigResult r = parse_err("mode = auto\n\nmodo = auto\n", 3);
    CHECK(r.error == "~/.config/dale/config:3: clave desconocida: \"modo\" (válidas: mode, lang, model, threads)");
    CHECK(r.error_en == "~/.config/dale/config:3: unknown key: \"modo\" (valid: mode, lang, model, threads)");
    parse_err("MODE = auto", 1);
    parse_err("= auto", 1);
    parse_err("mo de = auto", 1);
}

TEST_CASE("clave repetida") {
    const ConfigResult r = parse_err("mode = auto\nlang = es\nmode = auto\n", 3);
    CHECK(r.error == "~/.config/dale/config:3: clave repetida: mode (ya definida en la línea 1)");
    CHECK(r.error_en == "~/.config/dale/config:3: duplicate key: mode (already set on line 1)");
    parse_err("threads = 2\n# x\nthreads = 3", 3);
}

TEST_CASE("línea sin =") {
    const ConfigResult r = parse_err("mode = auto\nlang es\n", 2);
    CHECK(r.error == "~/.config/dale/config:2: falta \"=\" (formato: clave = valor)");
    CHECK(r.error_en == "~/.config/dale/config:2: missing \"=\" (format: key = value)");
    parse_err("model", 1);
    parse_err("\n\n\n\tthreads 4\r\n", 4);
}

TEST_CASE("valor vacío") {
    const ConfigResult r = parse_err("lang = es\nmodel =   \n", 2);
    CHECK(r.error == "~/.config/dale/config:2: valor vacío para model");
    CHECK(r.error_en == "~/.config/dale/config:2: empty value for model");
    parse_err("mode =", 1);
    parse_err("lang=\r\n", 1);
    parse_err("threads = \t", 1);
}

TEST_CASE("el primer error manda") {
    const ConfigResult r = parse("mode = mal\nlang = mal\n");
    CHECK(r.error.find(":1: ") != std::string::npos);
    CHECK(r.error_en.find(":1: ") != std::string::npos);
}

TEST_CASE("seguridad: el valor citado no lleva caracteres de control") {
    const ConfigResult r = parse("mode = \x1b[31mrojo\x07\x7f\"\\\n");
    CHECK_FALSE(r.config.has_value());
    CHECK(r.error == "~/.config/dale/config:1: valor no válido para mode: \"\\x1b[31mrojo\\x07\\x7f\\\"\\\\\" "
                     "(válidos: suggest, confirm, auto)");
    for (const std::string* msg : {&r.error, &r.error_en}) {
        for (char c : *msg) {
            const auto u = static_cast<unsigned char>(c);
            CHECK((u >= 0x20 && u != 0x7f));
        }
    }
    // Unknown key with an escape sequence.
    const ConfigResult k = parse("\x1b]0;titulo\x07 = x\n");
    CHECK(k.error.find('\x1b') == std::string::npos);
    CHECK(k.error_en.find('\x1b') == std::string::npos);
    CHECK(k.error.find("\\x1b]0;titulo\\x07") != std::string::npos);
    // 8-bit CSI (U+009B in UTF-8) and stray invalid bytes are escaped; normal UTF-8 is kept.
    const ConfigResult c = parse("lang = \xC2\x9B" "31m\xFF\xC3 añ\n");
    CHECK(c.error.find("\"\\xc2\\x9b31m\\xff\\xc3 añ\"") != std::string::npos);
}

TEST_CASE("config_path: XDG_CONFIG_HOME definida, vacía y sin definir") {
    EnvGuard xdg("XDG_CONFIG_HOME");
    EnvGuard home("HOME");
    home.set("/home/ana");

    xdg.set("/tmp/xdg");
    CHECK(config_path() == "/tmp/xdg/dale/config");
    xdg.set("/tmp/xdg/");
    CHECK(config_path() == "/tmp/xdg/dale/config");

    xdg.set("");
    CHECK(config_path() == "/home/ana/.config/dale/config");

    xdg.unset();
    CHECK(config_path() == "/home/ana/.config/dale/config");

    home.unset();
    CHECK(config_path().empty());
    home.set("");
    CHECK(config_path().empty());
    xdg.set("/tmp/xdg");
    CHECK(config_path() == "/tmp/xdg/dale/config");  // XDG does not need HOME
}

TEST_CASE("load_config: HOME temporal") {
    EnvGuard xdg("XDG_CONFIG_HOME");
    EnvGuard home("HOME");
    TempDir tmp;
    xdg.unset();
    home.set(tmp.path.string());
    const auto file = tmp.path / ".config" / "dale" / "config";

    SUBCASE("fichero ausente: valores por defecto y no se crea") {
        const ConfigResult r = load_config();
        REQUIRE(r.config.has_value());
        CHECK(r.config->mode == Mode::Confirm);
        CHECK(r.config->lang == Lang::Auto);
        CHECK(r.config->model.empty());
        CHECK(r.config->threads == 0);
        CHECK_FALSE(std::filesystem::exists(file));
        CHECK_FALSE(std::filesystem::exists(tmp.path / ".config"));
    }
    SUBCASE("fichero válido, ~/ se expande con HOME") {
        write_file(file, "mode = suggest\nlang = es\nmodel = ~/m.gguf\nthreads = 3\n");
        const ConfigResult r = load_config();
        REQUIRE(r.config.has_value());
        CHECK(r.config->mode == Mode::Suggest);
        CHECK(r.config->lang == Lang::Es);
        CHECK(r.config->model == (tmp.path / "m.gguf").string());
        CHECK(r.config->threads == 3);
    }
    SUBCASE("fichero inválido: la ruta se muestra con ~/") {
        write_file(file, "mode = auto\n\nmode = suggest\n");
        const ConfigResult r = load_config();
        CHECK_FALSE(r.config.has_value());
        CHECK(r.error.rfind("~/.config/dale/config:3: ", 0) == 0);
        CHECK(r.error_en.rfind("~/.config/dale/config:3: ", 0) == 0);
    }
    SUBCASE("la ruta es un directorio: error de lectura") {
        std::filesystem::create_directories(file);
        const ConfigResult r = load_config();
        CHECK_FALSE(r.config.has_value());
        CHECK(r.error.rfind("~/.config/dale/config: no se puede leer", 0) == 0);
        CHECK(r.error_en.rfind("~/.config/dale/config: cannot read", 0) == 0);
    }
}

TEST_CASE("load_config: XDG_CONFIG_HOME temporal") {
    EnvGuard xdg("XDG_CONFIG_HOME");
    EnvGuard home("HOME");
    TempDir tmp;
    home.set("/nonexistent-home-dale");
    xdg.set((tmp.path / "xdg").string());
    const auto file = tmp.path / "xdg" / "dale" / "config";

    SUBCASE("ausente") {
        const ConfigResult r = load_config();
        REQUIRE(r.config.has_value());
        CHECK(r.config->mode == Mode::Confirm);
        CHECK_FALSE(std::filesystem::exists(file));
    }
    SUBCASE("válido") {
        write_file(file, "lang = en\r\n");
        const ConfigResult r = load_config();
        REQUIRE(r.config.has_value());
        CHECK(r.config->lang == Lang::En);
    }
    SUBCASE("inválido: fuera de HOME se muestra la ruta completa") {
        write_file(file, "threads = 0\n");
        const ConfigResult r = load_config();
        CHECK_FALSE(r.config.has_value());
        const std::string prefix = file.string() + ":1: ";
        CHECK(r.error.rfind(prefix, 0) == 0);
        CHECK(r.error_en.rfind(prefix, 0) == 0);
        CHECK(r.error.find("config:1:") != std::string::npos);
    }
}

}  // TEST_SUITE("config")
