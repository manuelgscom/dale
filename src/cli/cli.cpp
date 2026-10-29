#include "cli/cli.hpp"

#include <unistd.h>

#include <cerrno>
#include <cstdint>
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
#include "config/state.hpp"
#include "context/context.hpp"
#include "exec/exec.hpp"
#include "fetch/fetch.hpp"
#include "fetch/model_info.hpp"
#include "infer/cpu.hpp"
#include "infer/engine.hpp"
#include "infer/fake_engine.hpp"
#include "infer/hardware.hpp"
#include "infer/llama_engine.hpp"
#include "infer/prompt.hpp"
#include "infer/vulkan_loader.hpp"
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
    config::end_gpu_run_from_signal();
    std::signal(SIGINT, SIG_DFL);
    std::raise(SIGINT);
}

// SIGTERM/SIGHUP (closed terminal): same death, without leaving a GPU run marker behind.
extern "C" void on_termination(int sig) {
    config::end_gpu_run_from_signal();
    std::signal(sig, SIG_DFL);
    std::raise(sig);
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
std::unique_ptr<infer::Engine> fake_engine(bool gpu = false) {
#if DALE_TEST_HOOKS
    return infer::fake_engine_from_env(gpu);
#else
    (void)gpu;
    return nullptr;
#endif
}

// DALE_FAKE_GPU (tests only): fake GPUs instead of the detection, even without a Vulkan loader.
bool fake_gpu_hook() {
#if DALE_TEST_HOOKS
    return std::getenv("DALE_FAKE_GPU") != nullptr;
#else
    return false;
#endif
}

// Questions only with stdin and stderr on a TTY and without --json.
bool can_ask(bool json) { return !json && ::isatty(0) == 1 && ::isatty(2) == 1; }

// [S/n] question on stderr: Enter = yes; EOF or anything else = no.
bool ask_yes(Msg prompt, UiLang lang) {
    err(tr(lang, prompt));
    const auto line = read_line();
    if (!line) err("\n");
    const Answer a = line ? parse_answer(*line) : Answer::No;
    return a == Answer::Yes || a == Answer::Empty;
}

// Result of finding (and maybe downloading) the model. exit_code >= 0 => exit with it.
struct ModelPick {
    std::string path;
    int exit_code = -1;
    bool failed_download = false;  // the download was attempted and failed (not Ctrl-C)
};

ModelPick download(const std::string& dir, const ModelSpec& spec, UiLang lang) {
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
    return {{}, 1, true};
}

// Download dialog for a model of dale. No other network access.
ModelPick offer_download(const std::string& dir, const ModelSpec& spec, bool json, UiLang lang) {
    const std::string mb = std::to_string(size_mb(spec.size));
    if (!can_ask(json)) {
        err(fill_all(tr(lang, Msg::DownloadNoTty), {spec.filename, mb, spec.url}) + "\n");
        return {{}, 1};
    }
    err(fill_all(tr(lang, Msg::DownloadOffer), {spec.filename, mb, spec.url}));
    if (!ask_yes(Msg::DownloadPrompt, lang)) {
        err(std::string(tr(lang, Msg::DownloadDeclined)) + "\n");
        return {{}, 1};
    }
    return download(dir, spec, lang);
}

// explicit_path (--model or model from the config) > the file of info (downloading it if needed).
ModelPick pick_model(const std::string& explicit_path, const fetch::ModelInfo& info, bool json, UiLang lang) {
    const ModelSpec spec = model_spec(info);
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
    return offer_download(dir, spec, json, lang);
}

// Path of a downloaded model of dale; empty if it is not there.
std::string downloaded(const fetch::ModelInfo& info) {
    const ModelResolution r = resolve_model({}, fetch::models_dir(), model_spec(info));
    return r.status == ModelResolution::Status::Ready ? r.path : std::string();
}

// Where and with what this request runs.
struct Plan {
    std::string model_path;
    std::string gpu_device;  // ggml device name; empty = CPU
    std::string gpu;         // GPU description
    bool managed = false;    // follows the saved setup: a GPU failure moves it to the CPU
    int exit_code = -1;
};

const config::Setup kCpuSetup{config::Device::Cpu, fetch::kDefaultModel.filename, {}, 0};

Plan from_pick(const ModelPick& m) {
    Plan p;
    p.model_path = m.path;
    p.exit_code = m.exit_code;
    return p;
}

// dale-0.8b (or the explicit model) on the CPU.
Plan cpu_plan(const std::string& explicit_path, bool json, UiLang lang) {
    return from_pick(pick_model(explicit_path, fetch::kDefaultModel, json, lang));
}

Plan gpu_plan(const infer::Gpu& g, const ModelPick& m) {
    Plan p = from_pick(m);
    p.gpu_device = g.name;
    p.gpu = g.description;
    return p;
}

// GPUs ggml sees, or none without a Vulkan loader.
std::vector<infer::Gpu> available_gpus() {
    if (!fake_gpu_hook() && !infer::vulkan_loader_available()) return {};
    return infer::detect_gpus();
}

std::uint64_t mib(std::uint64_t bytes) { return bytes / (1024 * 1024); }

// [1/2/n] question between dale-4b and dale-9b: Enter or 1 = dale-4b, 2 = dale-9b, n = none
// (nullptr). EOF or three invalid answers = none.
const fetch::ModelInfo* ask_gpu_model(UiLang lang) {
    for (int i = 0; i < 3; ++i) {
        err(tr(lang, Msg::GpuChoicePrompt));
        const auto line = read_line();
        if (!line) {
            err("\n");
            return nullptr;
        }
        std::string digit;
        for (char ch : *line)
            if (ch != ' ' && ch != '\t' && ch != '\r') digit += ch;
        const Answer a = parse_answer(*line);
        if (a == Answer::Empty || digit == "1") return &fetch::kDale4B_1_0_0_Q4_K_M;
        if (digit == "2") return &fetch::kDale9B_1_0_0_Q4_K_M;
        if (a == Answer::No) return nullptr;
    }
    return nullptr;
}

// First run or --setup: detects the GPU, offers the GPU model and saves the choice. With
// ≥ kMinFree9B free the user picks dale-4b or dale-9b; with less, dale-4b or nothing.
// A GPU model already downloaded is used without a question (dale-9b first, if it fits).
// Without a terminal it neither asks nor downloads, and only saves "CPU" when there is no GPU
// to offer.
Plan first_run(bool json, UiLang lang) {
    config::gpu_run_died();  // a new choice starts clean: old markers do not count against it
    const std::vector<infer::Gpu> gpus = available_gpus();
    const infer::Choice c = infer::choose(gpus, false);
    if (!c.gpu) {
#if DALE_VULKAN
        if (!fake_gpu_hook() && !infer::vulkan_loader_available() && infer::gpu_device_node_present())
            err(std::string(tr(lang, Msg::NoVulkanLoader)) + "\n");
#endif
        config::save_setup(kCpuSetup);
        return cpu_plan({}, json, lang);
    }
    const infer::Gpu& g = gpus[c.gpu_index];
    const bool ask = can_ask(json);
    // The GPU choice, saved (with a terminal) along with the warm-up notice.
    const auto use_gpu = [&](const fetch::ModelInfo& model, const ModelPick& m) {
        Plan p = gpu_plan(g, m);
        if (ask) {
            config::save_setup({config::Device::Gpu, model.filename, g.description, mib(g.free_bytes)});
            err(tr(lang, Msg::GpuWarmup));
            p.managed = true;
        }
        return p;
    };
    const bool fits_9b = c.model == &fetch::kDale9B_1_0_0_Q4_K_M;
    for (const fetch::ModelInfo* model : {&fetch::kDale9B_1_0_0_Q4_K_M, &fetch::kDale4B_1_0_0_Q4_K_M}) {
        if (model == &fetch::kDale9B_1_0_0_Q4_K_M && !fits_9b) continue;
        if (const std::string path = downloaded(*model); !path.empty()) return use_gpu(*model, {path});
    }
    if (!ask) return cpu_plan({}, json, lang);

    const ModelSpec spec4 = model_spec(fetch::kDale4B_1_0_0_Q4_K_M);
    const fetch::ModelInfo* chosen = nullptr;
    if (fits_9b) {
        const ModelSpec spec9 = model_spec(fetch::kDale9B_1_0_0_Q4_K_M);
        err(fill_all(tr(lang, Msg::GpuChoice),
                     {risk::escape_control(g.description), std::to_string(mib(g.free_bytes)), spec4.filename,
                      std::to_string(size_mb(spec4.size)), spec9.filename, std::to_string(size_mb(spec9.size))}));
        chosen = ask_gpu_model(lang);
    } else {
        err(fill_all(tr(lang, Msg::GpuOffer), {risk::escape_control(g.description), std::to_string(mib(g.free_bytes)),
                                               spec4.filename, std::to_string(size_mb(spec4.size)), spec4.url}));
        if (ask_yes(Msg::GpuPrompt, lang)) chosen = &fetch::kDale4B_1_0_0_Q4_K_M;
    }
    if (chosen == nullptr) {
        config::save_setup(kCpuSetup);
        err(std::string(tr(lang, Msg::GpuDeclined)) + "\n");
        return cpu_plan({}, json, lang);
    }
    const ModelPick m = download(fetch::models_dir(), model_spec(*chosen), lang);
    if (m.failed_download) {
        err(std::string(tr(lang, Msg::GpuDownloadFailed)) + "\n");
        return cpu_plan({}, json, lang);
    }
    if (m.exit_code >= 0) return from_pick(m);
    return use_gpu(*chosen, m);
}

// Device and model for this request: config device > saved setup > first-run detection.
// (On the CPU the engine keeps ggml from starting Vulkan.)
Plan plan_run(const std::string& explicit_path, const config::Config& cfg, bool setup_again, bool json,
              UiLang lang) {
    if (cfg.device == config::Device::Cpu) return cpu_plan(explicit_path, json, lang);
    if (cfg.device == config::Device::Gpu) {
        const std::vector<infer::Gpu> gpus = available_gpus();
        const infer::Choice c = infer::choose(gpus, true);
        if (!c.gpu) {
            err(std::string(tr(lang, Msg::NoGpuFound)) + "\n");
            return cpu_plan(explicit_path, json, lang);
        }
        return gpu_plan(gpus[c.gpu_index], pick_model(explicit_path, *c.model, json, lang));
    }

    const std::optional<config::Setup> saved = setup_again ? std::nullopt : config::load_setup();
    if (!saved) {
        if (explicit_path.empty()) return first_run(json, lang);
        // Explicit model and nothing chosen yet: the device follows the detection; nothing saved.
        const std::vector<infer::Gpu> gpus = available_gpus();
        const infer::Choice c = infer::choose(gpus, false);
        if (!c.gpu) return cpu_plan(explicit_path, json, lang);
        return gpu_plan(gpus[c.gpu_index], pick_model(explicit_path, *c.model, json, lang));
    }
    if (saved->device == config::Device::Cpu) {
        const fetch::ModelInfo* info = fetch::find_model(saved->model);
        return from_pick(pick_model(explicit_path, info != nullptr ? *info : fetch::kDefaultModel, json, lang));
    }
    if (config::gpu_run_died()) {
        err(std::string(tr(lang, Msg::GpuCrashed)) + "\n");
        config::save_setup(kCpuSetup);
        return cpu_plan(explicit_path, json, lang);
    }
    const fetch::ModelInfo* info = fetch::find_model(saved->model);
    if (info == nullptr) return first_run(json, lang);
    const std::vector<infer::Gpu> gpus = available_gpus();
    for (const infer::Gpu& g : gpus) {
        if (g.description != saved->gpu) continue;
        Plan p = gpu_plan(g, pick_model(explicit_path, *info, json, lang));
        p.managed = true;
        return p;
    }
    err(fill(tr(lang, Msg::GpuMissing), risk::escape_control(saved->gpu)) + "\n");
    return cpu_plan(explicit_path, json, lang);
}

std::unique_ptr<infer::Engine> make_engine(const Plan& plan, int threads) {
    if (std::unique_ptr<infer::Engine> fake = fake_engine(!plan.gpu_device.empty())) return fake;
    return infer::make_llama_engine({plan.model_path, threads, plan.gpu_device});
}

// Runs the request on the planned device. If the GPU fails to load or decode, the same request
// runs on the CPU (with dale-0.8b if it is downloaded) and a managed setup moves to the CPU.
// A managed GPU run is wrapped in a run marker (config::begin_gpu_run): if the process dies on
// the GPU, the next run finds it and moves to the CPU.
infer::EngineResult suggest(Plan& plan, int threads, const infer::Request& request, UiLang lang) {
    if (plan.managed && !plan.gpu_device.empty()) config::begin_gpu_run();
    infer::EngineResult result = make_engine(plan, threads)->suggest(request);
    config::end_gpu_run();
    if (plan.gpu_device.empty()) return result;
    if (result.error == "load_failed" || result.error == "decode_failed") {
        err(std::string(tr(lang, Msg::GpuFailed)) + risk::escape_control(result.error_detail) + "\n");
        if (plan.managed) {
            config::save_setup(kCpuSetup);
            if (const std::string path = downloaded(fetch::kDefaultModel); !path.empty()) plan.model_path = path;
        }
        plan.gpu_device.clear();
        plan.gpu.clear();
        return make_engine(plan, threads)->suggest(request);
    }
    return result;
}

// "dale will use …" line for --setup and --version.
std::string setup_line(const config::Setup& s, UiLang lang) {
    if (s.device == config::Device::Gpu)
        return fill_all(tr(lang, Msg::SetupGpu), {risk::escape_control(s.gpu), risk::escape_control(s.model)});
    return fill(tr(lang, Msg::SetupCpu), risk::escape_control(s.model));
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
            if (const std::optional<config::Setup> s = config::load_setup()) out(setup_line(*s, env_lang));
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
        case Args::Action::Setup: {
            const config::ConfigResult cfg = config::load_config();
            if (!cfg.config) {
                err("dale: " + (env_lang == UiLang::Es ? cfg.error : cfg.error_en) + "\n");
                return 1;
            }
            const UiLang lang = ui_lang_from_env(cfg.config->lang);
            std::signal(SIGINT, on_sigint);
            const Plan plan = first_run(args.json, lang);
            if (plan.exit_code >= 0) return plan.exit_code;
            if (const std::optional<config::Setup> s = config::load_setup()) err(setup_line(*s, lang));
            return 0;
        }
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
    std::signal(SIGTERM, on_termination);
    std::signal(SIGHUP, on_termination);

    // With FakeEngine neither the device nor the model is looked up (end-to-end tests), unless
    // --model or the DALE_TEST_MODEL_* hooks are given.
    Plan plan;
    if (!fake_engine() || args.model || test_model_hooks()) {
        plan = plan_run(args.model ? *args.model : cfg.config->model, *cfg.config, args.setup, args.json, lang);
        if (plan.exit_code >= 0) return plan.exit_code;
    }
    const int threads = cfg.config->threads > 0 ? cfg.config->threads : infer::default_threads();
    const infer::EngineResult result = suggest(plan, threads, infer::Request{context_line, request}, lang);
    const std::string& model_path = plan.model_path;

    if (args.json) {
        const ReportInput in{file_name(model_path), threads, request, context_line, plan.gpu};
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
