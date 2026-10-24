#include "cli/cli.hpp"

#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cli/args.hpp"
#include "cli/context_file.hpp"
#include "cli/flow.hpp"
#include "cli/i18n.hpp"
#include "cli/model.hpp"
#include "cli/report.hpp"
#include "cli/ui.hpp"
#include "config/config.hpp"
#include "context/context.hpp"
#include "exec/exec.hpp"
#include "fetch/fetch.hpp"
#include "fetch/model_info.hpp"
#include "infer/cpu.hpp"
#include "infer/engine.hpp"
#include "infer/fake_engine.hpp"
#include "infer/llama_engine.hpp"
#include "infer/prompt.hpp"
#include "risk/risk.hpp"

namespace dale::cli {

namespace {

void write_fd(int fd, std::string_view text) {
    while (!text.empty()) {
        const ssize_t n = ::write(fd, text.data(), text.size());
        if (n < 0) {
            if (errno == EINTR) continue;
            return;
        }
        text.remove_prefix(static_cast<std::size_t>(n));
    }
}

void out(std::string_view text) { write_fd(1, text); }
void err(std::string_view text) { write_fd(2, text); }

// Canonical read of one stdin line. nullopt = EOF with nothing read.
std::optional<std::string> read_line() {
    std::string line;
    for (;;) {
        char c = 0;
        const ssize_t n = ::read(0, &c, 1);
        if (n < 0) {
            if (errno == EINTR) continue;
            return std::nullopt;
        }
        if (n == 0) {
            if (line.empty()) return std::nullopt;
            return line;
        }
        if (c == '\n') return line;
        line += c;
    }
}

// Ctrl-C during inference or at the prompt: newline and death by SIGINT, so the parent
// shell sees it (130). Async-signal-safe functions only.
extern "C" void on_sigint(int) {
    if (::write(1, "\n", 1) < 0) {
    }
    std::signal(SIGINT, SIG_DFL);
    std::raise(SIGINT);
}

bool debug() {
    const char* v = std::getenv("DALE_DEBUG");
    return v != nullptr && std::string_view(v) == "1";
}

std::string file_name(const std::string& path) {
    const auto pos = path.rfind('/');
    return pos == std::string::npos ? path : path.substr(pos + 1);
}

// FakeEngine (tests only) if DALE_FAKE_* is set; otherwise nullptr.
std::unique_ptr<infer::Engine> fake_engine() {
#if DALE_TEST_HOOKS
    return infer::fake_engine_from_env();
#else
    return nullptr;
#endif
}

// Result of finding (and maybe downloading) the model. exit_code >= 0 => exit with it.
struct ModelPick {
    std::string path;
    int exit_code = -1;
};

// Download dialog for the default model: only with stdin and stderr on a TTY
// and without --json. Enter = yes. No other network access.
ModelPick download_default(const std::string& dir, const ModelSpec& spec, bool json, UiLang lang) {
    const std::string mb = std::to_string(size_mb(spec.size));
    if (json || ::isatty(0) != 1 || ::isatty(2) != 1) {
        err(fill_all(tr(lang, Msg::DownloadNoTty), {spec.filename, mb, spec.url}) + "\n");
        return {{}, 1};
    }
    err(fill_all(tr(lang, Msg::DownloadOffer), {spec.filename, mb, spec.url}));
    err(tr(lang, Msg::DownloadPrompt));
    const auto line = read_line();
    const Answer a = line ? parse_answer(*line) : Answer::No;
    if (!line) err("\n");
    if (a != Answer::Yes && a != Answer::Empty) {
        err(std::string(tr(lang, Msg::DownloadDeclined)) + "\n");
        return {{}, 1};
    }
    const fetch::DownloadResult d = fetch::download(spec.url, dir, spec.filename, spec.size, spec.sha256);
    switch (d.status) {
        case fetch::DownloadResult::Status::Ok:
            err(std::string(tr(lang, Msg::DownloadDone)) + risk::escape_control(d.path) + "\n");
            return {d.path, -1};
        case fetch::DownloadResult::Status::Interrupted:
            err("\n");
            return {{}, 130};
        case fetch::DownloadResult::Status::Failed:
            break;
    }
    err("dale: " + (lang == UiLang::Es ? d.error : d.error_en) + "\n");
    return {{}, 1};
}

// --model > model from the config > default file (downloading it if needed).
ModelPick pick_model(const Args& args, const config::Config& cfg, UiLang lang) {
    const std::string explicit_path = args.model ? *args.model : cfg.model;
    const ModelSpec spec = default_model_spec();
    const std::string dir = fetch::models_dir();
    const ModelResolution r = resolve_model(explicit_path, dir, spec);
    switch (r.status) {
        case ModelResolution::Status::Ready:
            return {r.path, -1};
        case ModelResolution::Status::Missing:
            err(std::string(tr(lang, Msg::ModelMissing)) + risk::escape_control(r.path) + "\n");
            return {{}, 1};
        case ModelResolution::Status::NoDir:
            err(std::string(tr(lang, Msg::NoModelsDir)) + "\n");
            return {{}, 1};
        case ModelResolution::Status::NeedsDownload:
            break;
    }
    return download_default(dir, spec, args.json, lang);
}

// Honest message for an engine error (nothing is ever run).
std::string engine_error_message(const infer::EngineResult& r, const std::string& model_path, UiLang lang) {
    const std::string& e = r.error;
    if (e == "invalid_json" || e == "invalid_schema") return std::string(tr(lang, Msg::InvalidResponse));
    if (e == "truncated")
        return std::string(tr(lang, Msg::InvalidResponse)) + std::string(tr(lang, Msg::Incomplete));
    if (e == "timeout") return std::string(tr(lang, Msg::Timeout));
    if (e == "too_long") return fill(tr(lang, Msg::RequestTooLong), std::to_string(infer::kMaxRequestBytes));
    if (e == "load_failed") return std::string(tr(lang, Msg::LoadFailed)) + risk::escape_control(model_path);
    return std::string(tr(lang, Msg::EngineError)) + risk::escape_control(e);
}

}  // namespace

int run(int argc, char** argv) {
    std::vector<std::string> raw;
    for (int i = 1; i < argc; ++i) raw.emplace_back(argv[i]);
    const Args args = parse_args(raw);

    // Before reading the config, the language comes from the environment only.
    const UiLang env_lang = ui_lang_from_env(config::Lang::Auto);
    switch (args.action) {
        case Args::Action::Help:
            out(tr(env_lang, Msg::Help));
            return 0;
        case Args::Action::Version:
            out(fill_all(tr(env_lang, Msg::Version), {DALE_VERSION, fetch::kDefaultModel.version,
                                                      fetch::kDefaultModel.quant,
                                                      std::string_view(fetch::kDefaultModel.sha256).substr(0, 12)}));
            return 0;
        case Args::Action::NoRequest:
            err(tr(env_lang, Msg::Help));
            return 2;
        case Args::Action::BadFlag:
            err(std::string(tr(env_lang, Msg::UnknownFlag)) + args.bad_flag + "\n\n");
            err(tr(env_lang, Msg::Help));
            return 2;
        case Args::Action::MissingValue:
            err(std::string(tr(env_lang, Msg::MissingValue)) + args.bad_flag + "\n\n");
            err(tr(env_lang, Msg::Help));
            return 2;
        case Args::Action::Run:
            break;
    }

    // Normalized request: empty = no request; more than 1000 bytes = usage
    // error.
    const std::string request = infer::normalize_request(args.request);
    if (request.empty()) {
        err(tr(env_lang, Msg::Help));
        return 2;
    }
    if (request.size() > infer::kMaxRequestBytes) {
        err(fill(tr(env_lang, Msg::RequestTooLong), std::to_string(infer::kMaxRequestBytes)) + "\n");
        return 2;
    }

    // Context line: DALE_CONTEXT_FILE or the system one.
    std::string context_line;
    const char* context_file = std::getenv("DALE_CONTEXT_FILE");
    if (context_file != nullptr && *context_file != '\0') {
        const ContextFile cf = read_context_file(context_file);
        if (cf.status != ContextFile::Status::Ok) {
            const Msg m = cf.status == ContextFile::Status::Unreadable ? Msg::ContextUnreadable : Msg::ContextInvalid;
            err(std::string(tr(env_lang, m)) + risk::escape_control(context_file) + "\n");
            return 2;
        }
        context_line = cf.line;
    } else {
        context_line = context::to_line(context::detect_context());
    }

    // --print-prompt: no config, no model, no TTY.
    if (args.print_prompt) {
        out(infer::render_prompt(context_line, request));
        return 0;
    }

    const config::ConfigResult cfg = config::load_config();
    if (!cfg.config) {
        err("dale: " + (env_lang == UiLang::Es ? cfg.error : cfg.error_en) + "\n");
        return 1;
    }
    const UiLang lang = ui_lang_from_env(cfg.config->lang);

    std::signal(SIGINT, on_sigint);

    // With FakeEngine the model is not looked up (end-to-end tests), unless --model or the
    // DALE_TEST_MODEL_* hooks are given.
    std::unique_ptr<infer::Engine> engine = fake_engine();
    std::string model_path;
    if (!engine || args.model || test_model_hooks()) {
        const ModelPick m = pick_model(args, *cfg.config, lang);
        if (m.exit_code >= 0) return m.exit_code;
        model_path = m.path;
    }
    const int threads = cfg.config->threads > 0 ? cfg.config->threads : infer::default_threads();
    if (!engine) engine = infer::make_llama_engine({model_path, threads});

    const infer::EngineResult result = engine->suggest(infer::Request{context_line, request});

    if (args.json) {
        const ReportInput in{file_name(model_path), threads, request, context_line};
        out(json_report(in, result, lang) + "\n");
        if (result.error == "load_failed") {
            err(engine_error_message(result, model_path, lang) + "\n");
            return 1;
        }
        return 0;
    }

    if (!result.suggestion) {
        err(engine_error_message(result, model_path, lang) + "\n");
        if (debug()) {
            err("dale: error_detail: " + risk::escape_control(result.error_detail) + "\n");
            err("dale: raw: " + risk::escape_control(result.raw) + "\n");
        }
        return 1;
    }
    // Refusals do not prompt: stdout being a TTY is enough to render them.
    const bool stdout_tty = ::isatty(1) == 1;
    const bool color = use_color(stdout_tty, std::getenv("NO_COLOR"), std::getenv("TERM"));
    const infer::Suggestion& s = *result.suggestion;
    if (s.kind != infer::Suggestion::Kind::Command) {
        out(render_refusal(s.kind == infer::Suggestion::Kind::Unsure, s.explanation, lang, color));
        return 0;
    }

    FlowOptions opt;
    opt.mode = cfg.config->mode;
    opt.lang = lang;
    opt.interactive = stdout_tty && ::isatty(0) == 1;
    opt.color = opt.interactive && color;

    Io io;
    io.write = out;
    io.read_line = read_line;
    io.classify = risk::classify;
    io.run = exec::run_bash;
    io.edit = exec::edit_in_editor;
    io.copy = exec::copy_to_clipboard;
    return run_flow(s.command, s.explanation, opt, io);
}

}  // namespace dale::cli
