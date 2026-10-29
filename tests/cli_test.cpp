#include "doctest.h"

#include <algorithm>
#include <deque>
#include <optional>
#include <string>
#include <vector>

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

#include "cli/args.hpp"
#include "cli/context_file.hpp"
#include "cli/flow.hpp"
#include "cli/i18n.hpp"
#include "cli/model.hpp"
#include "cli/report.hpp"
#include "cli/ui.hpp"

using namespace dale::cli;
using dale::config::Lang;
using dale::config::Mode;
using dale::risk::Assessment;
using dale::risk::Finding;
using dale::risk::Risk;

namespace {

// Level based on a word in the command, so as not to depend on the real checker.
Assessment fake_classify(std::string_view cmd) {
    Assessment a;
    if (cmd.find("PELIGRO") != std::string_view::npos) {
        a.level = Risk::Peligro;
        a.findings.push_back({Risk::Peligro, "R", "PELIGRO", "borra cosas", "deletes stuff"});
    } else if (cmd.find("PRECAUCION") != std::string_view::npos) {
        a.level = Risk::Precaucion;
        a.findings.push_back({Risk::Precaucion, "R", "PRECAUCION", "cambia cosas", "changes stuff"});
    }
    return a;
}

struct FakeWorld {
    std::deque<std::optional<std::string>> input;
    std::deque<std::optional<std::string>> edits;
    std::string screen;
    std::vector<std::string> ran, copied;
    int run_code = 7;

    Io io() {
        Io io;
        io.write = [this](std::string_view t) { screen += t; };
        io.read_line = [this]() -> std::optional<std::string> {
            if (input.empty()) return std::nullopt;
            auto v = input.front();
            input.pop_front();
            return v;
        };
        io.classify = fake_classify;
        io.run = [this](const std::string& c) {
            ran.push_back(c);
            return run_code;
        };
        io.edit = [this](const std::string&) -> std::optional<std::string> {
            if (edits.empty()) return std::nullopt;
            auto v = edits.front();
            edits.pop_front();
            return v;
        };
        io.copy = [this](const std::string& c) {
            copied.push_back(c);
            return true;
        };
        return io;
    }

    int go(const std::string& cmd, Mode mode = Mode::Confirm, UiLang lang = UiLang::Es, bool interactive = true) {
        FlowOptions opt;
        opt.mode = mode;
        opt.lang = lang;
        opt.interactive = interactive;
        return run_flow(cmd, "Explica.", opt, io());
    }
};

bool has(const std::string& s, const std::string& sub) { return s.find(sub) != std::string::npos; }

}  // namespace

TEST_SUITE("cli") {

TEST_CASE("parse_args") {
    CHECK(parse_args({}).action == Args::Action::NoRequest);
    CHECK(parse_args({"--help"}).action == Args::Action::Help);
    CHECK(parse_args({"-h", "x"}).action == Args::Action::Help);
    CHECK(parse_args({"--version"}).action == Args::Action::Version);
    const Args bad = parse_args({"lista", "--raro"});
    CHECK(bad.action == Args::Action::BadFlag);
    CHECK(bad.bad_flag == "--raro");
    const Args ok = parse_args({"lista", "los", "ficheros"});
    CHECK(ok.action == Args::Action::Run);
    CHECK(ok.request == "lista los ficheros");
    CHECK(parse_args({"--", "-x", "borra"}).request == "-x borra");
    CHECK(parse_args({""}).action == Args::Action::NoRequest);
}

TEST_CASE("parse_args: flags") {
    const Args a = parse_args({"--json", "--model", "m.gguf", "busca", "--print-prompt", "ficheros"});
    CHECK(a.action == Args::Action::Run);
    CHECK(a.json);
    CHECK(a.print_prompt);
    REQUIRE(a.model);
    CHECK(*a.model == "m.gguf");
    CHECK(a.request == "busca ficheros");

    const Args eq = parse_args({"--model=/x/y z.gguf", "hola"});
    REQUIRE(eq.model);
    CHECK(*eq.model == "/x/y z.gguf");
    CHECK(eq.request == "hola");
    CHECK_FALSE(eq.json);
    CHECK_FALSE(eq.print_prompt);

    // --model always takes the next argument, even if it starts with "-".
    const Args dash = parse_args({"--model", "--json", "hola"});
    REQUIRE(dash.model);
    CHECK(*dash.model == "--json");
    CHECK_FALSE(dash.json);
    CHECK(parse_args({"--model", "a", "--model", "b", "x"}).model == "b");

    for (const auto& v : std::vector<std::vector<std::string>>{{"hola", "--model"}, {"--model=", "hola"},
                                                               {"--model", "", "hola"}, {"--help", "--model"}}) {
        const Args m = parse_args(v);
        CHECK(m.action == Args::Action::MissingValue);
        CHECK(m.bad_flag == "--model");
    }
    CHECK(parse_args({"--model", "--raro"}).action == Args::Action::NoRequest);  // "--raro" is the value
    CHECK(parse_args({"--modelo", "x"}).action == Args::Action::BadFlag);
    CHECK(parse_args({"--json"}).action == Args::Action::NoRequest);
    CHECK(parse_args({"--", "--json", "x"}).request == "--json x");
    CHECK_FALSE(parse_args({"--", "--json", "x"}).json);
    // An unknown flag takes precedence over the missing value.
    CHECK(parse_args({"--raro", "--model"}).action == Args::Action::BadFlag);
}

TEST_CASE("parse_args: --setup") {
    const Args alone = parse_args({"--setup"});
    CHECK(alone.action == Args::Action::Setup);
    CHECK(alone.setup);
    CHECK(alone.request.empty());
    CHECK_FALSE(parse_args({"hola"}).setup);

    const Args json = parse_args({"--json", "--setup"});
    CHECK(json.action == Args::Action::Setup);
    CHECK(json.json);

    // With a request it runs it after choosing again.
    const Args run = parse_args({"--setup", "lista", "ficheros"});
    CHECK(run.action == Args::Action::Run);
    CHECK(run.setup);
    CHECK(run.request == "lista ficheros");
    CHECK(parse_args({"lista", "--setup"}).action == Args::Action::Run);

    CHECK(parse_args({"--setup", "--help"}).action == Args::Action::Help);
    CHECK(parse_args({"--version", "--setup"}).action == Args::Action::Version);
    CHECK(parse_args({"--setup", "--raro"}).action == Args::Action::BadFlag);
    CHECK(parse_args({"--setup", "--model"}).action == Args::Action::MissingValue);
    CHECK(parse_args({"--setups"}).action == Args::Action::BadFlag);
    const Args after = parse_args({"--", "--setup"});
    CHECK(after.action == Args::Action::Run);
    CHECK_FALSE(after.setup);
    CHECK(after.request == "--setup");
}

TEST_CASE("línea de contexto de DALE_CONTEXT_FILE") {
    CHECK(is_context_line("os=linux distro=ubuntu-24.04 pm=apt tools=curl,git"));
    CHECK(is_context_line("os=linux distro=unknown pm=none tools=none"));
    CHECK(is_context_line("os=x distro=y pm=z tools=\xC3\xB1"));
    for (const char* bad : {"", "os=linux", "os=linux distro=ubuntu pm=apt", "os=linux distro=ubuntu pm=apt tools=",
                            "os= distro=a pm=b tools=c", " os=a distro=b pm=c tools=d", "os=a distro=b pm=c tools=d ",
                            "os=a  distro=b pm=c tools=d", "os=a\tdistro=b pm=c tools=d", "os=a distro=b pm=c tools=d\r",
                            "distro=b os=a pm=c tools=d", "os=a distro=b pm=c tools=d extra=e", "OS=a distro=b pm=c tools=d",
                            "os=a distro=b\x1b pm=c tools=d"}) {
        CAPTURE(bad);
        CHECK_FALSE(is_context_line(bad));
    }

    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / ("dale-cli-test-" + std::to_string(::getpid()));
    fs::create_directories(dir);
    const std::string file = (dir / "ctx.txt").string();
    std::ofstream(file) << "os=linux distro=arch pm=pacman tools=git\nresto\n";
    const ContextFile ok = read_context_file(file);
    CHECK(ok.status == ContextFile::Status::Ok);
    CHECK(ok.line == "os=linux distro=arch pm=pacman tools=git");
    std::ofstream(file) << "os=linux distro=arch pm=pacman tools=git";  // no trailing "\n"
    CHECK(read_context_file(file).status == ContextFile::Status::Ok);
    std::ofstream(file) << "hola\n";
    CHECK(read_context_file(file).status == ContextFile::Status::Invalid);
    CHECK(read_context_file((dir / "no-existe").string()).status == ContextFile::Status::Unreadable);
    fs::remove_all(dir);
}

TEST_CASE("resolución del modelo") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / ("dale-model-test-" + std::to_string(::getpid()));
    fs::create_directories(dir);
    const ModelSpec spec{"m.gguf", "https://example.invalid/m.gguf", 3, "x"};
    using S = ModelResolution::Status;

    // Explicit: exists or not; never downloads.
    const std::string other = (dir / "otro.gguf").string();
    CHECK(resolve_model(other, dir.string(), spec).status == S::Missing);
    std::ofstream(other) << "cualquier tamaño";
    const ModelResolution e = resolve_model(other, dir.string(), spec);
    CHECK(e.status == S::Ready);
    CHECK(e.path == other);

    // Default: missing, wrong size or correct.
    const std::string def = (dir / "m.gguf").string();
    ModelResolution d = resolve_model("", dir.string(), spec);
    CHECK(d.status == S::NeedsDownload);
    CHECK(d.path == def);
    std::ofstream(def) << "abcd";
    CHECK(resolve_model("", dir.string(), spec).status == S::NeedsDownload);
    std::ofstream(def) << "abc";
    CHECK(resolve_model("", dir.string(), spec).status == S::Ready);
    CHECK(resolve_model("", "", spec).status == S::NoDir);
    fs::remove_all(dir);

    // Decimal megabytes, as Hugging Face shows them; at least 1 for a non-empty file.
    CHECK(size_mb(0) == 0);
    CHECK(size_mb(3) == 1);
    CHECK(size_mb(1499999) == 1);
    CHECK(size_mb(1500000) == 2);
    CHECK(size_mb(529297952ULL) == 529);
    CHECK(size_mb(2708805056ULL) == 2709);
    CHECK(size_mb(5629109760ULL) == 5629);
}

TEST_CASE("json_report") {
    using dale::infer::EngineResult;
    using dale::infer::Suggestion;
    using Json = nlohmann::json;
    const ReportInput in{"m.gguf", 4, "borra tmp", "os=linux distro=x pm=apt tools=none", ""};
    const std::vector<std::string> keys = {"contract_version", "dale_version", "model",  "threads",
                                           "device",           "gpu",
                                           "request",          "context",      "raw",    "response",
                                           "error",            "error_detail", "risk",         "timings_ms", "tokens"};

    EngineResult r;
    r.raw = "{...}";
    r.suggestion = Suggestion{Suggestion::Kind::Command, "rm -rf ~", "Borra."};
    r.timings = {12.4, 100.5, 200.49, 313.6, 150, 20};
    const std::string line = json_report(in, r, UiLang::En);
    CHECK(line.find('\n') == std::string::npos);
    const Json j = Json::parse(line);
    std::vector<std::string> got;
    for (const auto& [k, v] : j.items()) got.push_back(k);
    std::vector<std::string> sorted_keys = keys;
    std::sort(sorted_keys.begin(), sorted_keys.end());
    CHECK(got == sorted_keys);
    CHECK(line.rfind("{\"contract_version\":2,\"dale_version\":", 0) == 0);  // schema order
    CHECK(j["contract_version"] == 2);
    CHECK(j["dale_version"] == DALE_VERSION);
    CHECK(j["model"] == "m.gguf");
    CHECK(j["threads"] == 4);
    CHECK(j["device"] == "cpu");
    CHECK(j["gpu"].is_null());
    CHECK(j["request"] == "borra tmp");
    CHECK(j["context"] == "os=linux distro=x pm=apt tools=none");
    CHECK(j["raw"] == "{...}");
    CHECK(j["response"] == Json{{"type", "command"}, {"command", "rm -rf ~"}, {"explanation", "Borra."}});
    CHECK(j["error"].is_null());
    CHECK(j["error_detail"].is_null());
    CHECK(j["risk"]["level"] == "peligro");
    REQUIRE(j["risk"]["rules"].is_array());
    CHECK_FALSE(j["risk"]["rules"].empty());
    CHECK(j["risk"]["rules"].size() == j["risk"]["reasons"].size());
    CHECK(j["timings_ms"] == Json{{"load", 12}, {"prefill", 101}, {"generate", 200}, {"total", 314}});
    CHECK(j["tokens"] == Json{{"prompt", 150}, {"generated", 20}});

    // Safe: empty lists.
    r.suggestion = Suggestion{Suggestion::Kind::Command, "ls", "Lista."};
    const Json safe = Json::parse(json_report(in, r, UiLang::Es));
    CHECK(safe["risk"] == Json{{"level", "seguro"}, {"rules", Json::array()}, {"reasons", Json::array()}});

    // refuse: risk is null.
    r.suggestion = Suggestion{Suggestion::Kind::Refuse, "", "No."};
    const Json refuse = Json::parse(json_report(in, r, UiLang::Es));
    CHECK(refuse["response"]["type"] == "refuse");
    CHECK(refuse["risk"].is_null());

    // Error with invalid UTF-8 in raw: does not throw.
    EngineResult bad;
    bad.error = "truncated";
    bad.error_detail = "200 tokens";
    bad.raw = "{\"type\":\"comm\xC3";
    const Json err = Json::parse(json_report(in, bad, UiLang::Es));
    CHECK(err["response"].is_null());
    CHECK(err["risk"].is_null());
    CHECK(err["error"] == "truncated");
    CHECK(err["error_detail"] == "200 tokens");
    CHECK(err["raw"] == "{\"type\":\"comm\xEF\xBF\xBD");
}

TEST_CASE("idioma de la interfaz") {
    CHECK(ui_lang(Lang::Es, nullptr, nullptr, "en_US.UTF-8") == UiLang::Es);
    CHECK(ui_lang(Lang::En, "es_ES.UTF-8", nullptr, nullptr) == UiLang::En);
    CHECK(ui_lang(Lang::Auto, "", "es_ES", "en_US") == UiLang::Es);
    CHECK(ui_lang(Lang::Auto, "C", "es_ES", nullptr) == UiLang::En);
    CHECK(ui_lang(Lang::Auto, nullptr, nullptr, "es_AR.UTF-8") == UiLang::Es);
    CHECK(ui_lang(Lang::Auto, nullptr, nullptr, nullptr) == UiLang::En);
}

TEST_CASE("textos y fill") {
    CHECK(tr(UiLang::Es, Msg::DangerWord) == "ejecutar");
    CHECK(tr(UiLang::En, Msg::DangerWord) == "run");
    CHECK(fill("y {} motivos más", "4") == "y 4 motivos más");
    CHECK(fill_all("{} y {} ({})", {"a{}", "b", "c"}) == "a{} y b (c)");
    CHECK(fill_all("{} y {}", {"a"}) == "a y ");
    CHECK(fill_all("sin huecos {", {"a"}) == "sin huecos {");
}

TEST_CASE("use_color") {
    CHECK(use_color(true, nullptr, "xterm"));
    CHECK(use_color(true, "", "xterm"));
    CHECK_FALSE(use_color(true, "1", "xterm"));
    CHECK_FALSE(use_color(true, nullptr, "dumb"));
    CHECK_FALSE(use_color(false, nullptr, "xterm"));
}

TEST_CASE("pantallas") {
    CHECK(render_suggestion("ls", "Lista.\nTodo.", false) == "\n  ❯ ls\n    Lista.\n    Todo.\n\n");
    CHECK(render_suggestion("a\nb", "", false) == "\n  ❯ a\n    b\n\n");
    CHECK(render_suggestion("ls\x1b[2J", "", false) == "\n  ❯ ls\\x1b[2J\n\n");
    CHECK(render_suggestion("a\nb", "Lista.", true) ==
          "\n  ❯ \x1b[1ma\x1b[0m\n    \x1b[1mb\x1b[0m\n    \x1b[2mLista.\x1b[0m\n\n");

    Assessment safe;
    CHECK(render_risk(safe, UiLang::Es, false, true) == "  ● seguro  ·  ¿Ejecutar? [S/n/e(ditar)/c(opiar)] ");
    CHECK(render_risk(safe, UiLang::En, false, false) == "  ● safe\n");
    CHECK(render_risk(safe, UiLang::Es, true, false) == "  \x1b[32m● seguro\x1b[0m\n");

    Assessment caution;
    caution.level = Risk::Precaucion;
    caution.findings = {{Risk::Precaucion, "R1", "sudo", "se ejecuta como administrador", "runs as root"},
                        {Risk::Seguro, "X", "ls", "nada", "nothing"},
                        {Risk::Precaucion, "R2", "apt install", "instala o elimina software", "installs software"}};
    CHECK(render_risk(caution, UiLang::Es, false, true) ==
          "  ▲ precaución\n"
          "    · sudo: se ejecuta como administrador\n"
          "    · apt install: instala o elimina software\n"
          "  ¿Ejecutar? [s/N/e(ditar)/c(opiar)] ");

    Assessment danger;
    danger.level = Risk::Peligro;
    for (int i = 0; i < 5; ++i) danger.findings.push_back({Risk::Peligro, "R", "rm -rf ~", "borra", "deletes"});
    const std::string d = render_risk(danger, UiLang::Es, false, true);
    CHECK(d.rfind("  ███ PELIGRO ███  Esta acción puede ser irreversible.\n    · rm -rf ~: borra\n", 0) == 0);
    CHECK(has(d, "    y 2 motivos más\n"));
    CHECK(has(d, "  Para ejecutarlo escribe «ejecutar». e = editar, c = copiar, Intro = cancelar: "));
    danger.findings.pop_back();
    CHECK(has(render_risk(danger, UiLang::En, false, false), "    and 1 more reason\n"));
    CHECK(has(render_risk(danger, UiLang::Es, true, false), "  \x1b[1;31m███ PELIGRO ███\x1b[0m  Esta acción"));
    CHECK(has(render_risk(caution, UiLang::Es, true, false), "  \x1b[33m▲ precaución\x1b[0m\n"));

    CHECK(render_status(true, "Copiado", false) == "  ✓ Copiado\n");
    CHECK(render_status(false, "Cancelado", false) == "  ✗ Cancelado\n");
    CHECK(render_status(true, "Copiado", true) == "  \x1b[32m✓\x1b[0m Copiado\n");
    CHECK(render_status(false, "Cancelado", true) == "  \x1b[2m✗\x1b[0m Cancelado\n");

    CHECK(render_separator(false) == "  ────────────────────────────────────────\n");
    CHECK(render_separator(true) == "  \x1b[2m────────────────────────────────────────\x1b[0m\n");

    CHECK(render_refusal(true, "falta contexto.\nDime más.", UiLang::Es, false) ==
          "\n  ✗ No estoy seguro: falta contexto.\n    Dime más.\n\n");
    CHECK(render_refusal(false, "es una pregunta.", UiLang::En, true) ==
          "\n  \x1b[2m✗\x1b[0m This can't be solved with a command: es una pregunta.\n\n");
}

TEST_CASE("respuestas") {
    for (const char* yes : {"s", "S", "si", "SI", "sí", "SÍ", "Sí", "y", "Y", "yes", "YES", "  s  "})
        CHECK(parse_answer(yes) == Answer::Yes);
    CHECK(parse_answer("n") == Answer::No);
    CHECK(parse_answer("No") == Answer::No);
    CHECK(parse_answer("e") == Answer::Edit);
    CHECK(parse_answer("c") == Answer::Copy);
    CHECK(parse_answer("") == Answer::Empty);
    CHECK(parse_answer(" ") == Answer::Empty);
    CHECK(parse_answer("vale") == Answer::Invalid);

    CHECK(parse_danger_answer(" ejecutar ", "ejecutar") == Answer::Yes);
    CHECK(parse_danger_answer("Ejecutar", "ejecutar") == Answer::No);
    CHECK(parse_danger_answer("s", "ejecutar") == Answer::No);
    CHECK(parse_danger_answer("", "ejecutar") == Answer::No);
    CHECK(parse_danger_answer("e", "ejecutar") == Answer::Edit);
    CHECK(parse_danger_answer("c", "ejecutar") == Answer::Copy);
}

TEST_CASE("flujo: confirm") {
    SUBCASE("seguro + Intro ejecuta") {
        FakeWorld w;
        w.input = {""};
        CHECK(w.go("ls") == 7);
        CHECK(w.ran == std::vector<std::string>{"ls"});
        CHECK(has(w.screen, "  ● seguro  ·  ¿Ejecutar? [S/n/e(ditar)/c(opiar)] " + render_separator(false)));
    }
    SUBCASE("precaución + Intro cancela") {
        FakeWorld w;
        w.input = {""};
        CHECK(w.go("PRECAUCION") == 0);
        CHECK(w.ran.empty());
        CHECK(has(w.screen, "  ✗ Cancelado\n"));
        CHECK_FALSE(has(w.screen, "─"));
    }
    SUBCASE("peligro: s cancela y la palabra ejecuta") {
        FakeWorld w;
        w.input = {"s"};
        CHECK(w.go("PELIGRO") == 0);
        CHECK(w.ran.empty());
        FakeWorld w2;
        w2.input = {"ejecutar"};
        CHECK(w2.go("PELIGRO") == 7);
        CHECK(w2.ran.size() == 1);
    }
    SUBCASE("inglés: run") {
        FakeWorld w;
        w.input = {"run"};
        CHECK(w.go("PELIGRO", Mode::Confirm, UiLang::En) == 7);
    }
    SUBCASE("tres respuestas inválidas cancelan") {
        FakeWorld w;
        w.input = {"x", "y?", "zz", "s"};
        CHECK(w.go("ls") == 0);
        CHECK(w.ran.empty());
        CHECK(w.input.size() == 1);
    }
    SUBCASE("dos inválidas y luego sí") {
        FakeWorld w;
        w.input = {"x", "zz", "s"};
        CHECK(w.go("PRECAUCION") == 7);
    }
    SUBCASE("EOF cancela") {
        FakeWorld w;
        CHECK(w.go("ls") == 0);
        CHECK(w.ran.empty());
        CHECK(has(w.screen, "Cancelado"));
    }
    SUBCASE("copiar") {
        FakeWorld w;
        w.input = {"c"};
        CHECK(w.go("PELIGRO") == 0);
        CHECK(w.copied == std::vector<std::string>{"PELIGRO"});
        CHECK(has(w.screen, "  ✓ Copiado al portapapeles\n"));
        CHECK(w.ran.empty());
    }
}

TEST_CASE("flujo: editar") {
    SUBCASE("editado peligroso pide la palabra") {
        FakeWorld w;
        w.input = {"e", ""};
        w.edits = {std::string("PELIGRO")};
        CHECK(w.go("ls") == 0);
        CHECK(w.ran.empty());
        CHECK(has(w.screen, "(comando editado)"));
        CHECK(has(w.screen, "███ PELIGRO ███"));
    }
    SUBCASE("vacío o cancelado cancela") {
        FakeWorld w;
        w.input = {"e"};
        w.edits = {std::string("  \n")};
        CHECK(w.go("ls") == 0);
        CHECK(w.ran.empty());
        FakeWorld w2;
        w2.input = {"e"};
        CHECK(w2.go("ls") == 0);
        CHECK(w2.ran.empty());
    }
    SUBCASE("auto: editado seguro se ejecuta sin preguntar") {
        FakeWorld w;
        w.input = {"e"};
        w.edits = {std::string("ls -l")};
        CHECK(w.go("PRECAUCION", Mode::Auto) == 7);
        CHECK(w.ran == std::vector<std::string>{"ls -l"});
    }
}

TEST_CASE("flujo: modos") {
    SUBCASE("auto + seguro ejecuta sin leer") {
        FakeWorld w;
        CHECK(w.go("ls", Mode::Auto) == 7);
        CHECK_FALSE(has(w.screen, "¿Ejecutar?"));
        CHECK(has(w.screen, "  ● seguro\n" + render_separator(false)));
    }
    SUBCASE("auto + peligro pide la palabra") {
        FakeWorld w;
        w.input = {"s"};
        CHECK(w.go("PELIGRO", Mode::Auto) == 0);
        CHECK(w.ran.empty());
    }
    SUBCASE("suggest no pregunta ni ejecuta") {
        for (const char* c : {"ls", "PRECAUCION", "PELIGRO"}) {
            FakeWorld w;
            w.input = {"s", "ejecutar"};
            CHECK(w.go(c, Mode::Suggest) == 0);
            CHECK(w.ran.empty());
            CHECK(w.input.size() == 2);
            CHECK_FALSE(has(w.screen, "¿Ejecutar?"));
            CHECK_FALSE(has(w.screen, "escribe"));
        }
    }
    SUBCASE("sin TTY no ejecuta ni en auto") {
        FakeWorld w;
        w.input = {"s"};
        CHECK(w.go("ls", Mode::Auto, UiLang::Es, false) == 0);
        CHECK(w.ran.empty());
        CHECK(has(w.screen, "● seguro\n"));
        CHECK_FALSE(has(w.screen, "─"));
    }
}

}  // TEST_SUITE
