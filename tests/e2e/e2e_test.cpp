// dale e2e tests with a pseudo-terminal. The commands run are harmless and only act
// inside each test's temporary directory.

#include "doctest.h"

#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "pty_session.hpp"

namespace fs = std::filesystem;
using dale::e2e::ExitStatus;
using dale::e2e::PtySession;

namespace {

// SHA-256 of "abc" (NIST vector).
const std::string kShaAbc = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
const std::string kShaEmpty = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
const std::string kContext = "os=linux distro=ubuntu-24.04 pm=apt tools=curl,git,jq";

// Own temporary directory with HOME, config, bin/ and a controlled environment.
struct Sandbox {
    std::string dir;
    std::map<std::string, std::string> vars;

    Sandbox() {
        std::string tmpl = (fs::temp_directory_path() / "dale-e2e-XXXXXX").string();
        REQUIRE(mkdtemp(tmpl.data()) != nullptr);
        dir = tmpl;
        fs::create_directories(dir + "/bin");
        fs::create_directories(dir + "/.config/dale");
        vars = {
            {"HOME", dir},
            {"XDG_CONFIG_HOME", dir + "/.config"},
            {"TMPDIR", dir},
            {"NO_COLOR", "1"},
            {"LANG", "es_ES.UTF-8"},
            {"TERM", "xterm"},
            {"PATH", dir + "/bin:/usr/bin:/bin"},
        };
    }
    ~Sandbox() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    Sandbox(const Sandbox&) = delete;
    Sandbox& operator=(const Sandbox&) = delete;

    std::string path(const std::string& name) const { return dir + "/" + name; }
    bool exists(const std::string& name) const { return fs::exists(path(name)); }

    void write_file(const std::string& name, const std::string& text) const {
        std::ofstream(path(name), std::ios::binary) << text;
    }
    void config(const std::string& text) const { write_file(".config/dale/config", text); }

    // Fake program in <tmp>/bin.
    void fake_bin(const std::string& name, const std::string& script) const {
        write_file("bin/" + name, script);
        fs::permissions(path("bin/" + name), fs::perms::owner_all);
    }

    // Fake curl (tests/e2e/fake_curl.sh) in <tmp>/bin: copies src.bin and logs to curl.log.
    void fake_curl() {
        fs::copy_file(std::string(DALE_E2E_DIR) + "/fake_curl.sh", path("bin/curl"));
        fs::permissions(path("bin/curl"), fs::perms::owner_all);
        vars["FAKE_CURL_SRC"] = path("src.bin");
        vars["FAKE_CURL_LOG"] = path("curl.log");
    }

    // DALE_TEST_MODEL_* hooks: a 3-byte "model" whose correct content is "abc".
    void fake_model(const std::string& sha256 = kShaAbc) {
        fake_curl();
        write_file("src.bin", "abc");
        vars["DALE_TEST_MODEL_URL"] = "https://example.invalid/modelo.gguf";
        vars["DALE_TEST_MODEL_SIZE"] = "3";
        vars["DALE_TEST_MODEL_SHA256"] = sha256;
    }

    std::string models_dir() const { return path(".local/share/dale/models"); }

    // Files (hidden ones too, like the temporaries) in the models directory.
    std::vector<std::string> model_files() const {
        std::vector<std::string> out;
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(models_dir(), ec)) out.push_back(e.path().string());
        return out;
    }

    std::unique_ptr<PtySession> launch_args(const std::vector<std::string>& args) const {
        std::vector<std::string> argv{DALE_BIN};
        argv.insert(argv.end(), args.begin(), args.end());
        return std::make_unique<PtySession>(argv, env());
    }

    std::vector<std::string> env() const {
        std::vector<std::string> out;
        for (const auto& [k, v] : vars) out.push_back(k + "=" + v);
        return out;
    }

    // Fake engine with a command.
    void fake(const std::string& command, const std::string& explanation = "Explicación de prueba.") {
        vars["DALE_FAKE_COMMAND"] = command;
        vars["DALE_FAKE_EXPLANATION"] = explanation;
    }

    std::unique_ptr<PtySession> launch(const std::string& request = "haz algo") const {
        return std::make_unique<PtySession>(std::vector<std::string>{DALE_BIN, request}, env());
    }

    dale::e2e::PipeResult pipes(const std::vector<std::string>& args, const std::string& input = "") const {
        std::vector<std::string> argv{DALE_BIN};
        argv.insert(argv.end(), args.begin(), args.end());
        return dale::e2e::run_with_pipes(argv, env(), input);
    }
};

std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

std::size_t count(const std::string& haystack, const std::string& needle) {
    std::size_t n = 0;
    for (auto pos = haystack.find(needle); pos != std::string::npos; pos = haystack.find(needle, pos + 1)) ++n;
    return n;
}

bool exited_with(const ExitStatus& s, int code) { return s.exited && s.code == code; }

// Reference Base64, independent of dale's implementation.
std::string b64(const std::string& in) {
    static const char* abc = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    std::size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        const unsigned v = (static_cast<unsigned char>(in[i]) << 16) |
                           (static_cast<unsigned char>(in[i + 1]) << 8) | static_cast<unsigned char>(in[i + 2]);
        out += abc[(v >> 18) & 63];
        out += abc[(v >> 12) & 63];
        out += abc[(v >> 6) & 63];
        out += abc[v & 63];
    }
    const std::size_t rest = in.size() - i;
    if (rest > 0) {
        unsigned v = static_cast<unsigned>(static_cast<unsigned char>(in[i])) << 16;
        if (rest == 2) v |= static_cast<unsigned>(static_cast<unsigned char>(in[i + 1])) << 8;
        out += abc[(v >> 18) & 63];
        out += abc[(v >> 12) & 63];
        out += rest == 2 ? abc[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

const char* kPromptSafe = "[S/n/e(ditar)/c(opiar)]";
const char* kPromptCaution = "[s/N/e(ditar)/c(opiar)]";
const char* kPromptDanger = "escribe «ejecutar»";

}  // namespace

TEST_SUITE("e2e") {

TEST_CASE("1. confirm + seguro + s: se ejecuta") {
    Sandbox sb;
    sb.fake("echo DALE_OK_1");
    auto s = sb.launch();
    REQUIRE(s->wait_for(kPromptSafe));
    CHECK(contains(s->output(), "● seguro  ·  ¿Ejecutar? [S/n/e(ditar)/c(opiar)]"));
    s->write("s\n");
    const auto st = s->wait_exit();
    CHECK(exited_with(st, 0));
    CHECK(contains(s->output(), "────────────────────────────────────────"));
    CHECK(contains(s->output(), "\nDALE_OK_1"));
}

TEST_CASE("2. confirm + seguro + Intro: se ejecuta") {
    Sandbox sb;
    sb.fake("echo DALE_OK_2");
    auto s = sb.launch();
    REQUIRE(s->wait_for(kPromptSafe));
    s->write("\n");
    CHECK(exited_with(s->wait_exit(), 0));
    CHECK(contains(s->output(), "\nDALE_OK_2"));
}

TEST_CASE("3. confirm + precaución + n o Intro: no se ejecuta") {
    for (const char* answer : {"n\n", "\n"}) {
        CAPTURE(answer);
        Sandbox sb;
        sb.fake("touch " + sb.path("marca"));
        auto s = sb.launch();
        REQUIRE(s->wait_for(kPromptCaution));
        CHECK(contains(s->output(), "▲ precaución"));
        s->write(answer);
        CHECK(exited_with(s->wait_exit(), 0));
        CHECK(contains(s->output(), "Cancelado"));
        CHECK_FALSE(sb.exists("marca"));
    }
}

TEST_CASE("4. confirm + peligro + s: no se ejecuta") {
    Sandbox sb;
    sb.write_file("victima.txt", "datos\n");
    sb.fake("shred -u " + sb.path("victima.txt"));
    auto s = sb.launch();
    REQUIRE(s->wait_for(kPromptDanger));
    CHECK(contains(s->output(), "███ PELIGRO ███"));
    CHECK(contains(s->output(), "Esta acción puede ser irreversible."));
    s->write("s\n");
    CHECK(exited_with(s->wait_exit(), 0));
    CHECK(contains(s->output(), "Cancelado"));
    CHECK(sb.exists("victima.txt"));
}

TEST_CASE("5. confirm + peligro + ejecutar: se ejecuta") {
    Sandbox sb;
    sb.write_file("victima.txt", "datos\n");
    sb.fake("shred -u " + sb.path("victima.txt"));
    auto s = sb.launch();
    REQUIRE(s->wait_for(kPromptDanger));
    s->write("  ejecutar  \n");
    CHECK(exited_with(s->wait_exit(), 0));
    CHECK_FALSE(sb.exists("victima.txt"));
}

TEST_CASE("6. auto: seguro se ejecuta sin preguntar; precaución pregunta") {
    {
        Sandbox sb;
        sb.config("mode = auto\n");
        sb.fake("echo DALE_OK_6");
        auto s = sb.launch();
        CHECK(exited_with(s->wait_exit(), 0));
        CHECK(contains(s->output(), "\nDALE_OK_6"));
        CHECK_FALSE(contains(s->output(), "¿Ejecutar?"));
    }
    {
        Sandbox sb;
        sb.config("mode = auto\n");
        sb.fake("touch " + sb.path("marca"));
        auto s = sb.launch();
        REQUIRE(s->wait_for(kPromptCaution));
        CHECK_FALSE(sb.exists("marca"));
        s->write("s\n");
        CHECK(exited_with(s->wait_exit(), 0));
        CHECK(sb.exists("marca"));
    }
}

TEST_CASE("7. suggest: nunca pregunta ni ejecuta") {
    Sandbox sb;
    sb.config("mode = suggest\n");
    sb.write_file("victima.txt", "datos\n");
    const std::vector<std::string> commands = {"echo DALE_OK_7", "touch " + sb.path("marca"),
                                               "shred -u " + sb.path("victima.txt")};
    for (const auto& cmd : commands) {
        CAPTURE(cmd);
        sb.fake(cmd);
        auto s = sb.launch();
        CHECK(exited_with(s->wait_exit(), 0));
        CHECK(contains(s->output(), cmd));
        CHECK_FALSE(contains(s->output(), "¿Ejecutar?"));
        CHECK_FALSE(contains(s->output(), kPromptDanger));
        CHECK_FALSE(contains(s->output(), "\nDALE_OK_7"));
    }
    CHECK_FALSE(sb.exists("marca"));
    CHECK(sb.exists("victima.txt"));
}

TEST_CASE("7b. suggest + peligro muestra el aviso") {
    Sandbox sb;
    sb.config("mode = suggest\n");
    sb.write_file("victima.txt", "datos\n");
    sb.fake("shred -u " + sb.path("victima.txt"));
    auto s = sb.launch();
    CHECK(exited_with(s->wait_exit(), 0));
    CHECK(contains(s->output(), "███ PELIGRO ███"));
    CHECK(sb.exists("victima.txt"));
}

TEST_CASE("8. sin TTY nunca ejecuta") {
    Sandbox sb;
    sb.config("mode = auto\n");
    sb.fake("touch " + sb.path("marca"), "Crea la marca.");
    const auto r = sb.pipes({"crea la marca"}, "s\nejecutar\n");
    CHECK(exited_with(r.status, 0));
    CHECK(contains(r.out, "touch " + sb.path("marca")));
    CHECK(contains(r.out, "Crea la marca."));
    CHECK(contains(r.out, "▲ precaución"));
    CHECK_FALSE(contains(r.out, "\x1b["));
    CHECK_FALSE(sb.exists("marca"));

    // Not even safe commands in auto mode.
    sb.fake("echo DALE_OK_8");
    const auto r2 = sb.pipes({"di ok"});
    CHECK(exited_with(r2.status, 0));
    CHECK_FALSE(contains(r2.out, "\nDALE_OK_8"));
}

TEST_CASE("9. editar vuelve a clasificar") {
    Sandbox sb;
    sb.write_file("victima.txt", "datos\n");
    sb.fake("echo hola");
    sb.vars["EDITOR"] = std::string(DALE_E2E_DIR) + "/fake_editor.sh";
    sb.vars["FAKE_EDIT_TO"] = "shred -u " + sb.path("victima.txt");
    auto s = sb.launch();
    REQUIRE(s->wait_for(kPromptSafe));
    s->write("e\n");
    REQUIRE(s->wait_for(kPromptDanger));
    CHECK(contains(s->output(), "███ PELIGRO ███"));
    CHECK(contains(s->output(), "(comando editado)"));
    CHECK(contains(s->output(), "shred -u " + sb.path("victima.txt")));
    s->write("\n");
    CHECK(exited_with(s->wait_exit(), 0));
    CHECK(contains(s->output(), "Cancelado"));
    CHECK(sb.exists("victima.txt"));
}

TEST_CASE("9b. editar y dejarlo vacío cancela") {
    Sandbox sb;
    sb.fake("touch " + sb.path("marca"));
    sb.vars["EDITOR"] = std::string(DALE_E2E_DIR) + "/fake_editor.sh";
    sb.vars["FAKE_EDIT_TO"] = "   ";
    auto s = sb.launch();
    REQUIRE(s->wait_for(kPromptCaution));
    s->write("e\n");
    CHECK(exited_with(s->wait_exit(), 0));
    CHECK(contains(s->output(), "Cancelado"));
    CHECK_FALSE(sb.exists("marca"));
}

TEST_CASE("10. copiar con wl-copy falso") {
    Sandbox sb;
    const std::string cmd = "touch " + sb.path("marca");
    sb.fake(cmd);
    sb.vars["WAYLAND_DISPLAY"] = "x";
    sb.fake_bin("wl-copy", "#!/bin/sh\ncat > '" + sb.path("clip.txt") + "'\n");
    auto s = sb.launch();
    REQUIRE(s->wait_for(kPromptCaution));
    s->write("c\n");
    CHECK(exited_with(s->wait_exit(), 0));
    CHECK(contains(s->output(), "Copiado al portapapeles"));
    CHECK(contains(read_file(sb.path("clip.txt")), cmd));
    CHECK_FALSE(sb.exists("marca"));
}

TEST_CASE("10b. copiar sin pantalla gráfica usa OSC 52") {
    Sandbox sb;
    const std::string cmd = "touch " + sb.path("marca");
    sb.fake(cmd);
    auto s = sb.launch();
    REQUIRE(s->wait_for(kPromptCaution));
    s->write("c\n");
    CHECK(exited_with(s->wait_exit(), 0));
    CHECK(contains(s->output(), "\x1b]52;c;" + b64(cmd)));
    CHECK(contains(s->output(), "Copiado al portapapeles"));
    CHECK_FALSE(sb.exists("marca"));
}

TEST_CASE("11. Ctrl-C en el prompt mata a dale por SIGINT") {
    Sandbox sb;
    sb.fake("touch " + sb.path("marca"));
    auto s = sb.launch();
    REQUIRE(s->wait_for(kPromptCaution));
    s->write("\x03");
    const auto st = s->wait_exit();
    CHECK(st.signaled);
    CHECK(st.signal == SIGINT);
    CHECK_FALSE(sb.exists("marca"));
}

TEST_CASE("11b. Ctrl-C durante el comando devuelve 130 enseguida") {
    Sandbox sb;
    sb.fake("sleep 5");
    auto s = sb.launch();
    REQUIRE(s->wait_for("¿Ejecutar?"));
    s->write("s\n");
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    const auto start = std::chrono::steady_clock::now();
    s->write("\x03");
    const auto st = s->wait_exit();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    CHECK(exited_with(st, 130));
    CHECK(elapsed < std::chrono::seconds(2));
}

TEST_CASE("12. refuse y unsure se muestran sin pregunta") {
    Sandbox sb;
    sb.vars["DALE_FAKE_TYPE"] = "refuse";
    sb.vars["DALE_FAKE_EXPLANATION"] = "Eso no es una tarea de terminal.";
    {
        auto s = sb.launch();
        CHECK(exited_with(s->wait_exit(), 0));
        CHECK(contains(s->output(), "Esto no se resuelve con un comando: Eso no es una tarea de terminal."));
        CHECK_FALSE(contains(s->output(), "¿Ejecutar?"));
    }
    sb.vars["DALE_FAKE_TYPE"] = "unsure";
    sb.vars["DALE_FAKE_EXPLANATION"] = "Falta saber qué carpeta.";
    {
        auto s = sb.launch();
        CHECK(exited_with(s->wait_exit(), 0));
        CHECK(contains(s->output(), "No estoy seguro: Falta saber qué carpeta."));
        CHECK_FALSE(contains(s->output(), "¿Ejecutar?"));
    }
}

TEST_CASE("13. configuración inválida: mensaje con config:N: y código 1") {
    Sandbox sb;
    sb.config("# comentario\nmode = autom\n");
    sb.fake("echo hola");
    const auto r = sb.pipes({"di hola"});
    CHECK(exited_with(r.status, 1));
    CHECK(contains(r.err, "config:2:"));
}

TEST_CASE("14. inglés: prompt [Y/n/e(dit)/c(opy)] y palabra run") {
    {
        Sandbox sb;
        sb.config("lang = en\n");
        sb.fake("echo DALE_OK_14");
        auto s = sb.launch();
        REQUIRE(s->wait_for("[Y/n/e(dit)/c(opy)]"));
        CHECK(contains(s->output(), "● safe"));
        s->write("n\n");
        CHECK(exited_with(s->wait_exit(), 0));
        CHECK(contains(s->output(), "Cancelled"));
    }
    {
        Sandbox sb;
        sb.config("lang = en\n");
        sb.write_file("victima.txt", "datos\n");
        sb.fake("shred -u " + sb.path("victima.txt"));
        auto s = sb.launch();
        REQUIRE(s->wait_for("type \"run\""));
        CHECK(contains(s->output(), "███ DANGER ███"));
        s->write("ejecutar\n");
        CHECK(exited_with(s->wait_exit(), 0));
        CHECK(sb.exists("victima.txt"));
    }
    {
        Sandbox sb;
        sb.vars["LANG"] = "en_US.UTF-8";
        sb.write_file("victima.txt", "datos\n");
        sb.fake("shred -u " + sb.path("victima.txt"));
        auto s = sb.launch();
        REQUIRE(s->wait_for("type \"run\""));
        s->write("run\n");
        CHECK(exited_with(s->wait_exit(), 0));
        CHECK_FALSE(sb.exists("victima.txt"));
    }
}

TEST_CASE("15. colores solo sin NO_COLOR") {
    Sandbox sb;
    sb.config("mode = suggest\n");
    sb.fake("touch " + sb.path("marca"));
    {
        auto s = sb.launch();
        CHECK(exited_with(s->wait_exit(), 0));
        CHECK_FALSE(contains(s->output(), "\x1b["));
    }
    sb.vars.erase("NO_COLOR");
    {
        auto s = sb.launch();
        CHECK(exited_with(s->wait_exit(), 0));
        CHECK(contains(s->output(), "\x1b["));
    }
    sb.vars["TERM"] = "dumb";
    {
        auto s = sb.launch();
        CHECK(exited_with(s->wait_exit(), 0));
        CHECK_FALSE(contains(s->output(), "\x1b["));
    }
}

TEST_CASE("--version, sin petición y opción desconocida") {
    Sandbox sb;
    const auto v = sb.pipes({"--version"});
    CHECK(exited_with(v.status, 0));
    CHECK(v.out == "dale 1.0.0 (modelo 1.0.0, Q4_K_M, sha256 8738d2535680)\n");
    sb.vars["LANG"] = "en_US.UTF-8";
    const auto v_en = sb.pipes({"--version"});
    CHECK(exited_with(v_en.status, 0));
    CHECK(v_en.out == "dale 1.0.0 (model 1.0.0, Q4_K_M, sha256 8738d2535680)\n");
    sb.vars["LANG"] = "es_ES.UTF-8";

    const auto none = sb.pipes({});
    CHECK(exited_with(none.status, 2));
    CHECK(contains(none.err, "Uso: dale"));

    const auto bad = sb.pipes({"--nope", "hola"});
    CHECK(exited_with(bad.status, 2));
    CHECK(contains(bad.err, "--nope"));

    const auto help = sb.pipes({"--help"});
    CHECK(exited_with(help.status, 0));
    CHECK(contains(help.out, "Uso: dale"));
}

TEST_CASE("sin modelo ni TTY y error del motor: código 1") {
    Sandbox sb;
    sb.fake_curl();
    const auto r = sb.pipes({"di hola"});
    CHECK(exited_with(r.status, 1));
    CHECK(contains(r.err, "Ejecuta dale en un terminal"));
    CHECK(contains(r.err, "--model"));
    CHECK_FALSE(sb.exists("curl.log"));

    sb.vars["DALE_FAKE_TYPE"] = "command";
    sb.vars["DALE_FAKE_ERROR"] = "fallo simulado";
    const auto e = sb.pipes({"di hola"});
    CHECK(exited_with(e.status, 1));
    CHECK(contains(e.err, "fallo simulado"));
}

TEST_CASE("respuesta inválida 3 veces cancela") {
    Sandbox sb;
    sb.fake("touch " + sb.path("marca"));
    auto s = sb.launch();
    REQUIRE(s->wait_for(kPromptCaution));
    s->write("quizá\n");
    s->write("tal vez\n");
    s->write("ok\n");
    CHECK(exited_with(s->wait_exit(), 0));
    CHECK(contains(s->output(), "Cancelado"));
    CHECK(count(s->output(), kPromptCaution) == 3);
    CHECK_FALSE(sb.exists("marca"));
}

TEST_CASE("respuestas sí en mayúsculas y con tilde") {
    for (const char* answer : {"SÍ\n", "sí\n", "Yes\n", "Y\n"}) {
        CAPTURE(answer);
        Sandbox sb;
        sb.fake("touch " + sb.path("marca"));
        auto s = sb.launch();
        REQUIRE(s->wait_for(kPromptCaution));
        s->write(answer);
        CHECK(exited_with(s->wait_exit(), 0));
        CHECK(sb.exists("marca"));
    }
}

TEST_CASE("EOF en el prompt cancela") {
    Sandbox sb;
    sb.fake("touch " + sb.path("marca"));
    auto s = sb.launch();
    REQUIRE(s->wait_for(kPromptCaution));
    s->write("\x04");
    CHECK(exited_with(s->wait_exit(), 0));
    CHECK(contains(s->output(), "Cancelado"));
    CHECK_FALSE(sb.exists("marca"));
}

}  // TEST_SUITE

// Flags, DALE_CONTEXT_FILE, --json, honest errors and download, with FakeEngine and a fake
// curl. Never any network.
TEST_SUITE("e2e") {

TEST_CASE("DALE_FAKE_RAW con JSON roto: respuesta no válida y código 1") {
    Sandbox sb;
    sb.vars["DALE_FAKE_RAW"] = "<think>hmm</think> {\"type\":\"command\"";
    const auto r = sb.pipes({"di hola"});
    CHECK(exited_with(r.status, 1));
    CHECK(contains(r.err, "no he podido generar una respuesta válida"));
    CHECK_FALSE(contains(r.err, "raw:"));
    CHECK(r.out.empty());

    sb.vars["DALE_DEBUG"] = "1";
    sb.vars["DALE_FAKE_RAW"] = "{\"type\":\"command\x1b[31m\"";
    const auto d = sb.pipes({"di hola"});
    CHECK(exited_with(d.status, 1));
    CHECK(contains(d.err, "error_detail: "));
    CHECK(contains(d.err, "raw: {\"type\":\"command\\x1b[31m\""));
    CHECK_FALSE(contains(d.err, "\x1b["));
}

TEST_CASE("DALE_FAKE_RAW válido sigue el flujo normal") {
    Sandbox sb;
    sb.vars["DALE_FAKE_RAW"] = R"({"type":"command","command":"echo DALE_RAW_OK","explanation":"Dice ok."})";
    auto s = sb.launch();
    REQUIRE(s->wait_for(kPromptSafe));
    CHECK(contains(s->output(), "Dice ok."));
    s->write("s\n");
    CHECK(exited_with(s->wait_exit(), 0));
    CHECK(contains(s->output(), "\nDALE_RAW_OK"));

    sb.vars["DALE_FAKE_RAW"] = R"({"type":"unsure","command":"","explanation":"¿Qué carpeta?"})";
    const auto u = sb.pipes({"borra eso"});
    CHECK(exited_with(u.status, 0));
    CHECK(contains(u.out, "No estoy seguro: ¿Qué carpeta?"));
}

TEST_CASE("errores del motor con mensaje honesto") {
    Sandbox sb;
    sb.vars["DALE_FAKE_TYPE"] = "command";
    const std::vector<std::pair<std::string, std::string>> cases = {
        {"invalid_schema", "no he podido generar una respuesta válida"},
        {"truncated", "no he podido generar una respuesta válida (respuesta incompleta)"},
        {"timeout", "ha tardado demasiado; prueba a reformular la petición"},
        {"too_long", "petición demasiado larga"},
    };
    for (const auto& [code, text] : cases) {
        CAPTURE(code);
        sb.vars["DALE_FAKE_ERROR"] = code;
        const auto r = sb.pipes({"di hola"});
        CHECK(exited_with(r.status, 1));
        CHECK(contains(r.err, text));
        CHECK(r.out.empty());
    }
    sb.config("lang = en\n");
    sb.vars["DALE_FAKE_ERROR"] = "timeout";
    const auto en = sb.pipes({"say hi"});
    CHECK(contains(en.err, "it took too long; try rephrasing the request"));
}

TEST_CASE("--print-prompt con DALE_CONTEXT_FILE") {
    Sandbox sb;
    sb.config("mode = roto\n");  // does not read the config
    sb.write_file("ctx.txt", kContext + "\nsegunda línea ignorada\n");
    sb.vars["DALE_CONTEXT_FILE"] = sb.path("ctx.txt");
    const auto r = sb.pipes({"--print-prompt", "lista", "los ficheros"});
    CHECK(exited_with(r.status, 0));
    CHECK(r.err.empty());
    CHECK(r.out.rfind("<|im_start|>system\n", 0) == 0);
    const std::string tail = "<think>\n\n</think>\n\n";
    REQUIRE(r.out.size() >= tail.size());
    CHECK(r.out.compare(r.out.size() - tail.size(), tail.size(), tail) == 0);
    CHECK(contains(r.out, kContext + "\nlista los ficheros<|im_end|>"));
    CHECK_FALSE(contains(r.out, "segunda"));
}

TEST_CASE("DALE_CONTEXT_FILE ilegible o inválido: código 2") {
    Sandbox sb;
    sb.fake("echo hola");
    sb.vars["DALE_CONTEXT_FILE"] = sb.path("no-existe.txt");
    const auto missing = sb.pipes({"di hola"});
    CHECK(exited_with(missing.status, 2));
    CHECK(contains(missing.err, "no se puede leer DALE_CONTEXT_FILE"));

    sb.vars["DALE_CONTEXT_FILE"] = sb.path("ctx.txt");
    for (const std::string bad : {"", "os=linux distro=ubuntu pm=apt", "os=linux distro=ubuntu pm=apt tools=",
                                  "os=linux  distro=ubuntu pm=apt tools=git", "distro=x os=linux pm=apt tools=git",
                                  "os=linux distro=ubuntu pm=apt tools=git\r", "os=linux distro=a b pm=apt tools=git"}) {
        CAPTURE(bad);
        sb.write_file("ctx.txt", bad + "\n");
        const auto r = sb.pipes({"--print-prompt", "di hola"});
        CHECK(exited_with(r.status, 2));
        CHECK(contains(r.err, "no tiene el formato"));
        CHECK(r.out.empty());
    }
}

TEST_CASE("--json con FakeEngine") {
    Sandbox sb;
    sb.write_file("ctx.txt", kContext);
    sb.vars["DALE_CONTEXT_FILE"] = sb.path("ctx.txt");
    sb.config("mode = auto\n");
    sb.fake("touch " + sb.path("marca"), "Crea \"la\" marca.");
    // On a TTY it does not ask or run either.
    auto s = sb.launch_args({"--json", "crea", "la marca"});
    CHECK(exited_with(s->wait_exit(), 0));
    CHECK_FALSE(sb.exists("marca"));
    CHECK_FALSE(contains(s->output(), "¿Ejecutar?"));

    const auto r = sb.pipes({"--json", "  crea   la marca  "});
    CHECK(exited_with(r.status, 0));
    CHECK(r.err.empty());
    CHECK(count(r.out, "\n") == 1);
    CHECK(r.out.rfind("{\"contract_version\":2,\"dale_version\":\"", 0) == 0);
    CHECK(r.out.substr(r.out.size() - 2) == "}\n");
    for (const char* key : {"\"model\":", "\"threads\":", "\"request\":", "\"context\":", "\"raw\":",
                            "\"response\":", "\"error\":null", "\"risk\":", "\"timings_ms\":{\"load\":",
                            "\"tokens\":{\"prompt\":"}) {
        CAPTURE(key);
        CHECK(contains(r.out, key));
    }
    CHECK(contains(r.out, "\"context\":\"" + kContext + "\""));
    CHECK(contains(r.out, "\"explanation\":\"Crea \\\"la\\\" marca.\""));
    CHECK(contains(r.out, "\"risk\":{\"level\":\"precaucion\",\"rules\":[\""));
    CHECK_FALSE(sb.exists("marca"));

    // Model error: response and risk null, exit code 0.
    sb.vars["DALE_FAKE_RAW"] = "no es JSON";
    const auto e = sb.pipes({"--json", "di hola"});
    CHECK(exited_with(e.status, 0));
    CHECK(contains(e.out, "\"raw\":\"no es JSON\",\"response\":null,\"error\":\"invalid_json\",\"error_detail\":"));

    // refuse: no risk.
    sb.vars["DALE_FAKE_RAW"] = R"({"type":"refuse","command":"","explanation":"No."})";
    const auto f = sb.pipes({"--json", "escribe un poema"});
    CHECK(exited_with(f.status, 0));
    CHECK(contains(f.out, "\"response\":{\"type\":\"refuse\",\"command\":\"\",\"explanation\":\"No.\"}"));
    CHECK(contains(f.out, "\"error\":null,\"error_detail\":null,\"risk\":null"));
}

TEST_CASE("petición demasiado larga o vacía: código 2") {
    Sandbox sb;
    sb.fake("echo hola");
    const auto r = sb.pipes({std::string(1001, 'a')});
    CHECK(exited_with(r.status, 2));
    CHECK(contains(r.err, "petición demasiado larga"));
    CHECK(r.out.empty());

    const auto ok = sb.pipes({std::string(1000, 'a')});
    CHECK(exited_with(ok.status, 0));

    const auto blank = sb.pipes({"  ", "\n"});
    CHECK(exited_with(blank.status, 2));
    CHECK(contains(blank.err, "Uso: dale"));
}

TEST_CASE("--model inexistente o sin valor") {
    Sandbox sb;
    sb.fake_curl();
    const auto r = sb.pipes({"--model", sb.path("no-existe.gguf"), "di hola"});
    CHECK(exited_with(r.status, 1));
    CHECK(contains(r.err, "no existe el fichero del modelo: " + sb.path("no-existe.gguf")));
    CHECK_FALSE(sb.exists("curl.log"));
    CHECK(sb.model_files().empty());

    // Same with model from the config, and on a TTY: never downloads.
    sb.config("model = " + sb.path("tampoco.gguf") + "\n");
    auto s = sb.launch();
    CHECK(exited_with(s->wait_exit(), 1));
    CHECK(contains(s->output(), "no existe el fichero del modelo"));
    CHECK_FALSE(contains(s->output(), "[S/n]"));
    CHECK_FALSE(sb.exists("curl.log"));

    const auto none = sb.pipes({"di hola", "--model"});
    CHECK(exited_with(none.status, 2));
    CHECK(contains(none.err, "falta el valor de la opción: --model"));
    const auto empty = sb.pipes({"--model=", "di hola"});
    CHECK(exited_with(empty.status, 2));
}

TEST_CASE("--model con un fichero que no es un modelo") {
    Sandbox sb;
    sb.write_file("roto.gguf", "esto no es un GGUF");
    const auto r = sb.pipes({"--model=" + sb.path("roto.gguf"), "di hola"});
    CHECK(exited_with(r.status, 1));
    CHECK(contains(r.err, "no se ha podido cargar el modelo: " + sb.path("roto.gguf")));

    const auto j = sb.pipes({"--json", "--model", sb.path("roto.gguf"), "di hola"});
    CHECK(exited_with(j.status, 1));
    CHECK(contains(j.out, "\"model\":\"roto.gguf\""));
    CHECK(contains(j.out, "\"error\":\"load_failed\""));
}

TEST_CASE("descarga aceptada con s o Intro") {
    for (const char* answer : {"s\n", "\n"}) {
        CAPTURE(answer);
        Sandbox sb;
        sb.config("mode = suggest\n");
        sb.fake("echo DALE_DL_OK");
        sb.fake_model();
        auto s = sb.launch();
        REQUIRE(s->wait_for("¿Descargarlo ahora? [S/n]"));
        CHECK(contains(s->output(), "https://example.invalid/modelo.gguf"));
        CHECK(contains(s->output(), "1 MB"));
        CHECK_FALSE(sb.exists("curl.log"));
        s->write(answer);
        CHECK(exited_with(s->wait_exit(), 0));
        CHECK(contains(s->output(), "Modelo descargado y verificado"));
        CHECK(contains(s->output(), "echo DALE_DL_OK"));
        CHECK(contains(read_file(sb.path("curl.log")), "https://example.invalid/modelo.gguf"));
        const auto files = sb.model_files();
        REQUIRE(files.size() == 1);
        CHECK(read_file(files[0]) == "abc");

        // Already downloaded: does not ask or download again.
        fs::remove(sb.path("curl.log"));
        auto again = sb.launch();
        CHECK(exited_with(again->wait_exit(), 0));
        CHECK_FALSE(contains(again->output(), "[S/n]"));
        CHECK_FALSE(sb.exists("curl.log"));
    }
}

TEST_CASE("descarga rechazada, sin TTY o con --json: no descarga") {
    {
        Sandbox sb;
        sb.fake("echo hola");
        sb.fake_model();
        auto s = sb.launch();
        REQUIRE(s->wait_for("[S/n]"));
        s->write("n\n");
        CHECK(exited_with(s->wait_exit(), 1));
        CHECK(contains(s->output(), "--model RUTA"));
        CHECK_FALSE(sb.exists("curl.log"));
        CHECK(sb.model_files().empty());
    }
    {
        Sandbox sb;
        sb.fake("echo hola");
        sb.fake_model();
        const auto r = sb.pipes({"di hola"}, "s\n");
        CHECK(exited_with(r.status, 1));
        CHECK(contains(r.err, "Ejecuta dale en un terminal"));
        CHECK(contains(r.err, "https://example.invalid/modelo.gguf"));
        CHECK_FALSE(sb.exists("curl.log"));
        CHECK(sb.model_files().empty());
    }
    {
        Sandbox sb;
        sb.fake("echo hola");
        sb.fake_model();
        auto s = sb.launch_args({"--json", "di hola"});
        CHECK(exited_with(s->wait_exit(), 1));
        CHECK_FALSE(contains(s->output(), "[S/n]"));
        CHECK_FALSE(sb.exists("curl.log"));
        CHECK(sb.model_files().empty());
    }
    {
        // A default file with a different size counts as missing; with the correct size
        // it is used without downloading.
        Sandbox sb;
        sb.fake("echo hola");
        sb.fake_model();
        const auto first = sb.pipes({"di hola"});
        const std::string prefix = "falta el modelo ";
        const auto from = first.err.find(prefix);
        REQUIRE(from != std::string::npos);
        const auto name_at = from + prefix.size();
        const std::string name = first.err.substr(name_at, first.err.find(" (", name_at) - name_at);
        CHECK(name.ends_with(".gguf"));
        fs::create_directories(sb.models_dir());
        sb.write_file(".local/share/dale/models/" + name, "abcd");
        const auto r = sb.pipes({"di hola"});
        CHECK(exited_with(r.status, 1));
        CHECK(contains(r.err, "falta el modelo"));
        sb.write_file(".local/share/dale/models/" + name, "xyz");
        const auto ok = sb.pipes({"di hola"});
        CHECK(exited_with(ok.status, 0));
        CHECK(contains(ok.out, "echo hola"));
        CHECK_FALSE(sb.exists("curl.log"));
    }
}

TEST_CASE("descarga con hash erróneo no deja ningún fichero") {
    Sandbox sb;
    sb.fake("echo hola");
    sb.fake_model(kShaEmpty);
    auto s = sb.launch();
    REQUIRE(s->wait_for("[S/n]"));
    s->write("s\n");
    CHECK(exited_with(s->wait_exit(), 1));
    CHECK(sb.exists("curl.log"));
    CHECK(sb.model_files().empty());
    CHECK_FALSE(contains(s->output(), "echo hola"));
}

}  // TEST_SUITE
