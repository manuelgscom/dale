#include "doctest.h"

#include "infer/cpu.hpp"
#include "infer/json_tracker.hpp"
#include "infer/llama_engine.hpp"
#include "infer/prompt.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <sched.h>
#include <unistd.h>

using namespace dale::infer;
namespace fs = std::filesystem;

namespace {

using State = JsonTracker::State;

// Feeds the pieces to the tracker and returns the accepted text.
std::string feed_all(JsonTracker& t, const std::vector<std::string_view>& pieces) {
    std::string out;
    for (std::string_view p : pieces) out.append(p.substr(0, t.feed(p)));
    return out;
}

const std::string kContext =
    "os=linux distro=ubuntu-24.04 pm=apt tools=curl,docker,fdfind,git,jq,python3,rg,rsync,ssh,systemctl";

const char* test_model() {
    const char* m = std::getenv("DALE_TEST_MODEL");
    return (m != nullptr && *m != '\0') ? m : nullptr;
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
        path = fs::temp_directory_path() / ("dale_engine_test_" + std::to_string(getpid()));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

// The cache .state files in dir.
std::vector<fs::path> state_files(const fs::path& dir) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (e.path().extension() == ".state") out.push_back(e.path());
    }
    return out;
}

std::vector<std::string> raws(Engine& engine, const std::vector<std::string>& requests) {
    std::vector<std::string> out;
    for (const std::string& q : requests) {
        EngineResult r = engine.suggest({kContext, q});
        CHECK(r.error != "decode_failed");
        out.push_back(r.raw);
    }
    return out;
}

}  // namespace

TEST_SUITE("infer") {

TEST_CASE("rastreador: objeto simple en un trozo y en varios") {
    JsonTracker a;
    CHECK(feed_all(a, {"  \n{\"a\":1}"}) == "  \n{\"a\":1}");
    CHECK(a.state() == State::Done);

    JsonTracker b;
    CHECK(feed_all(b, {"{\"ty", "pe\":\"com", "mand\"", "}"}) == "{\"type\":\"command\"}");
    CHECK(b.state() == State::Done);
}

TEST_CASE("rastreador: llaves dentro de cadenas no cuentan") {
    JsonTracker t;
    feed_all(t, {"{\"command\":\"echo }{ }}\""});
    CHECK(t.state() == State::Open);
    feed_all(t, {"}"});
    CHECK(t.state() == State::Done);
}

TEST_CASE("rastreador: escapes \\\" y \\\\") {
    JsonTracker a;
    feed_all(a, {R"({"c":"say \"}\" ok"})"});
    CHECK(a.state() == State::Done);

    // \\ closes the escape: the next quote ends the string and the '}' counts.
    JsonTracker b;
    CHECK(feed_all(b, {R"({"c":"dir\\"})"}) == R"({"c":"dir\\"})");
    CHECK(b.state() == State::Done);

    // Escape split across two pieces.
    JsonTracker c;
    feed_all(c, {R"({"c":"a\)", R"("}")"});
    CHECK(c.state() == State::Open);
    feed_all(c, {"}"});
    CHECK(c.state() == State::Done);
}

TEST_CASE("rastreador: objetos anidados y texto tras el cierre") {
    JsonTracker t;
    CHECK(feed_all(t, {"{\"a\":{\"b\":{}},", "\"c\":2}\n<|im_end|>", "más"}) ==
          "{\"a\":{\"b\":{}},\"c\":2}");
    CHECK(t.state() == State::Done);
}

TEST_CASE("rastreador: texto que no empieza por {") {
    JsonTracker a;
    feed_all(a, {"  "});
    CHECK(a.state() == State::Waiting);
    feed_all(a, {"<think>{"});
    CHECK(a.state() == State::Invalid);

    JsonTracker b;
    feed_all(b, {"Sure! {\"a\":1}"});
    CHECK(b.state() == State::Invalid);
}

TEST_CASE("default_threads: entre 1 y los CPU de la máscara") {
    cpu_set_t mask;
    CPU_ZERO(&mask);
    REQUIRE(sched_getaffinity(0, sizeof(mask), &mask) == 0);
    const int n = default_threads();
    CHECK(n >= 1);
    CHECK(n <= CPU_COUNT(&mask));
}

TEST_CASE("modelo inexistente: load_failed con la ruta") {
    const std::string path = "/nonexistent/dale/model.gguf";
    auto engine = make_llama_engine({path, 1});
    for (int i = 0; i < 2; ++i) {  // the second attempt returns the same error without reloading
        EngineResult r = engine->suggest({"os=linux", "list files"});
        CHECK_FALSE(r.suggestion.has_value());
        CHECK(r.error == "load_failed");
        CHECK(r.error_detail.find(path) != std::string::npos);
    }
}

TEST_CASE("prune_prefix_cache: deja el estado actual y los temporales recientes") {
    TempDir tmp;
    const fs::path& dir = tmp.path;
    const std::time_t now = std::time(nullptr);
    // Files with their age in seconds.
    const std::vector<std::pair<std::string, std::time_t>> files = {
        {"prefix-actual.state", 7200},       // the one just saved: kept
        {"prefix-otro.state", 0},            // another model or version: removed
        {"prefix-viejo.state", 7200},        // removed
        {".prefix.state-viejo1", 7200},      // abandoned temporary: removed
        {".prefix.state-nuevo1", 60},        // temporary of another dale saving right now: kept
        {"prefix-actual.state.bak", 7200},   // not a cache file: kept
        {"otro.state", 7200},                // kept
    };
    for (const auto& [name, age] : files) {
        std::ofstream(dir / name) << "x";
        const auto mtime = fs::file_time_type::clock::now() - std::chrono::seconds(age);
        fs::last_write_time(dir / name, mtime);
    }
    fs::create_directory(dir / "prefix-dir.state");  // not a file: kept

    prune_prefix_cache(dir.string(), "prefix-actual.state", now);

    std::vector<std::string> left;
    for (const auto& e : fs::directory_iterator(dir)) left.push_back(e.path().filename().string());
    std::sort(left.begin(), left.end());
    CHECK(left == std::vector<std::string>{".prefix.state-nuevo1", "otro.state", "prefix-actual.state",
                                           "prefix-actual.state.bak", "prefix-dir.state"});
}

TEST_CASE("prune_prefix_cache: directorio inexistente no falla") {
    TempDir tmp;
    prune_prefix_cache((tmp.path / "no-existe").string(), "prefix-x.state", std::time(nullptr));
    CHECK(fs::is_empty(tmp.path));
}

}  // TEST_SUITE

TEST_SUITE("model") {

TEST_CASE("genera con el modelo real") {
    const char* model = test_model();
    if (model == nullptr) {
        MESSAGE("DALE_TEST_MODEL no definida: se salta");
        return;
    }
    TempDir tmp;
    EnvGuard xdg("XDG_CACHE_HOME");
    xdg.set(tmp.path.string());
    auto engine = make_llama_engine({model, default_threads()});
    const Request request{kContext, "list the files in the current directory"};

    EngineResult r = engine->suggest(request);
    MESSAGE("raw: " << r.raw);
    MESSAGE("error: " << r.error << " " << r.error_detail);
    MESSAGE("tokens prompt/gen: " << r.timings.prompt_tokens << "/" << r.timings.gen_tokens
                                  << ", ms load/prefill/gen/total: " << r.timings.load_ms << "/"
                                  << r.timings.prefill_ms << "/" << r.timings.generate_ms << "/"
                                  << r.timings.total_ms);
    CHECK(r.error != "load_failed");
    CHECK(r.error != "truncated");
    CHECK(r.error != "timeout");
    REQUIRE_FALSE(r.raw.empty());
    CHECK(r.raw.front() == '{');
    CHECK(r.raw.back() == '}');
    CHECK(r.timings.load_ms > 0);
    CHECK(r.timings.prefill_ms > 0);
    CHECK(r.timings.generate_ms > 0);
    CHECK(r.timings.total_ms > 0);
    CHECK(r.timings.prompt_tokens > 0);
    CHECK(r.timings.gen_tokens > 0);

    // Second request on the same engine: no reload and, being greedy, the same output
    // (checks that the context memory is cleared between requests).
    EngineResult again = engine->suggest(request);
    CHECK(again.timings.load_ms == 0);
    CHECK(again.raw == r.raw);
    CHECK(again.timings.prompt_tokens == r.timings.prompt_tokens);
}

TEST_CASE("caché del prefijo: la misma salida con y sin caché, también si está corrupta") {
    const char* model = test_model();
    if (model == nullptr) {
        MESSAGE("DALE_TEST_MODEL no definida: se salta");
        return;
    }
    const std::vector<std::string> requests = {
        "lista los ficheros de más de 200 MB en mi carpeta personal",
        "show the processes using the most CPU",
        "escríbeme un poema sobre el otoño",
        "fix it",
    };
    TempDir tmp;
    EnvGuard xdg("XDG_CACHE_HOME");

    // No cache: XDG_CACHE_HOME under a regular file, so the directory cannot be created.
    std::ofstream(tmp.path / "file") << "x";
    xdg.set((tmp.path / "file" / "cache").string());
    std::vector<std::string> expected;
    {
        auto engine = make_llama_engine({model, default_threads()});
        expected = raws(*engine, requests);
    }
    for (const std::string& raw : expected) CHECK(raw.rfind('{', 0) == 0);

    xdg.set((tmp.path / "cache").string());
    const fs::path dir = tmp.path / "cache" / "dale";
    {
        // First engine: cache miss on the first request, then saves it.
        auto engine = make_llama_engine({model, default_threads()});
        CHECK(raws(*engine, requests) == expected);
    }
    const std::vector<fs::path> files = state_files(dir);
    REQUIRE(files.size() == 1);
    const fs::path state = files[0];
    const auto size = fs::file_size(state);
    MESSAGE("estado del prefijo: " << state.filename().string() << ", " << size << " bytes");
    CHECK((fs::status(dir).permissions() & fs::perms::all) ==
          (fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
           fs::perms::others_read | fs::perms::others_exec));
    {
        // New engine: loads the saved state.
        auto engine = make_llama_engine({model, default_threads()});
        CHECK(raws(*engine, requests) == expected);
    }

    // Truncated file and garbage file: same output, and it is saved correctly again. Saving
    // removes the state of another model or version.
    const std::vector<std::string> corruptions = {"truncado", "basura"};
    for (const std::string& kind : corruptions) {
        CAPTURE(kind);
        if (kind == "truncado") {
            fs::resize_file(state, size / 2);
        } else {
            std::ofstream out(state, std::ios::binary | std::ios::trunc);
            for (std::uintmax_t i = 0; i < size; ++i) out.put(static_cast<char>((i * 131) & 0xff));
        }
        std::ofstream(dir / "prefix-0000000000000000.state") << "x";
        auto engine = make_llama_engine({model, default_threads()});
        CHECK(raws(*engine, {requests[0]}) == std::vector<std::string>{expected[0]});
        CHECK(state_files(dir).size() == 1);
        CHECK(fs::file_size(state) == size);
    }
}

TEST_CASE("prompt demasiado largo: too_long") {
    const char* model = test_model();
    if (model == nullptr) {
        MESSAGE("DALE_TEST_MODEL no definida: se salta");
        return;
    }
    TempDir tmp;
    EnvGuard xdg("XDG_CACHE_HOME");
    xdg.set(tmp.path.string());
    auto engine = make_llama_engine({model, default_threads()});
    // 1000 bytes of emoji (the CLI limit) and a context line with many tools.
    std::string request;
    while (request.size() + 4 <= kMaxRequestBytes) request += "\U0001F600";
    std::string context = "os=linux distro=ubuntu-24.04 pm=apt tools=";
    for (int i = 0; i < 400; ++i) context += "tool" + std::to_string(i) + ",";
    context.pop_back();
    EngineResult r = engine->suggest({context, request});
    CHECK(r.error == "too_long");
    CHECK(r.timings.prompt_tokens > 2048 - 200);
    MESSAGE("tokens del prompt: " << r.timings.prompt_tokens);
}

}  // TEST_SUITE
