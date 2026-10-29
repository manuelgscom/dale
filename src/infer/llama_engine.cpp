#include "infer/llama_engine.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fetch/sha256.hpp"
#include "infer/json_tracker.hpp"
#include "infer/parser.hpp"
#include "infer/prompt.hpp"
#include "infer/vulkan_loader.hpp"
#include "llama.h"

namespace dale::infer {

namespace {

constexpr std::uint32_t kCtx = 2048;
constexpr std::uint32_t kUbatch = 512;
constexpr int kMaxGenTokens = 200;
constexpr auto kTimeout = std::chrono::seconds(15);  // counted from the end of loading

// The prompt prefix (the system part) runs up to and including the last user turn header.
constexpr std::string_view kUserTurn = "<|im_start|>user\n";

using Clock = std::chrono::steady_clock;

double ms_since(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

// llama_log_set also installs the callback in ggml (llama-impl.cpp), so it covers both.
// Non-null user_data => DALE_DEBUG=1: logs go to stderr.
void log_callback(ggml_log_level, const char* text, void* debug) {
    if (debug != nullptr) std::fputs(text, stderr);
}

bool debug_enabled() {
    const char* v = std::getenv("DALE_DEBUG");
    return v != nullptr && std::strcmp(v, "1") == 0;
}

int g_debug_tag = 0;  // any non-null address turns the logs on

// Prefix cache directory: $XDG_CACHE_HOME/dale, or $HOME/.cache/dale.
// Created with 0755. Empty if neither variable is set or it cannot be created.
std::string prefix_cache_dir() {
    std::string base;
    if (const char* x = std::getenv("XDG_CACHE_HOME"); x != nullptr && *x != '\0') {
        base = x;
    } else if (const char* h = std::getenv("HOME"); h != nullptr && *h != '\0') {
        base = std::string(h) + "/.cache";
    } else {
        return {};
    }
    const std::string dir = base + "/dale";
    for (const std::string& d : {base, dir}) {
        if (::mkdir(d.c_str(), 0755) != 0 && errno != EEXIST) return {};
    }
    return dir;
}

// Fast checksum (FNV-1a over 64-bit words) to detect a corrupt cache file.
std::uint64_t checksum(const std::vector<unsigned char>& data) {
    constexpr std::uint64_t kPrime = 1099511628211ull;
    std::uint64_t h = 14695981039346656037ull;
    std::size_t i = 0;
    for (; i + 8 <= data.size(); i += 8) {
        std::uint64_t w = 0;
        std::memcpy(&w, data.data() + i, 8);
        h = (h ^ w) * kPrime;
    }
    for (; i < data.size(); ++i) h = (h ^ data[i]) * kPrime;
    return h;
}

class LlamaEngine : public Engine {
public:
    explicit LlamaEngine(LlamaOptions options) : options_(std::move(options)) {}

    ~LlamaEngine() override {
        if (sampler_ != nullptr) llama_sampler_free(sampler_);
        if (ctx_ != nullptr) llama_free(ctx_);
        if (model_ != nullptr) llama_model_free(model_);
        if (backend_) llama_backend_free();
    }

    EngineResult suggest(const Request& request) override {
        const auto t_start = Clock::now();
        const std::string prompt = render_prompt(request.context_line, request.text);
        const std::size_t cut = prompt.rfind(kUserTurn);
        const std::string prefix =
            cut == std::string::npos ? std::string() : prompt.substr(0, cut + kUserTurn.size());

        EngineResult r;
        if (!load(r)) {
            r.timings.total_ms = ms_since(t_start);
            return r;
        }
        const auto t_loaded = Clock::now();
        generate(prompt, prefix, t_loaded, r);
        if (r.error.empty()) {
            ParseResult p = parse_response(r.raw);
            r.suggestion = std::move(p.suggestion);
            r.error = std::move(p.error);
            r.error_detail = std::move(p.detail);
        }
        r.timings.total_ms = ms_since(t_start);
        return r;
    }

private:
    // Lazy load: backend, model, context and sampler. Attempted only once.
    bool load(EngineResult& r) {
        if (ctx_ != nullptr) return true;
        if (!load_error_.empty()) {
            r.error = "load_failed";
            r.error_detail = load_error_;
            return false;
        }
        const auto t0 = Clock::now();
        set_llama_logging();
        // CPU only: ggml must not start Vulkan (without a Vulkan loader it would crash). If an
        // earlier GPU detection already registered it, this changes nothing.
        if (options_.gpu_device.empty()) disable_vulkan_backend();
        llama_backend_init();
        backend_ = true;

        llama_model_params mp = llama_model_default_params();
        // Explicit device list: only the chosen GPU (never split with an iGPU), or none at all.
        ggml_backend_dev_t devices[2] = {nullptr, nullptr};
        mp.devices = devices;
        mp.n_gpu_layers = 0;
        if (!options_.gpu_device.empty()) {
            devices[0] = ggml_backend_dev_by_name(options_.gpu_device.c_str());
            mp.n_gpu_layers = 999;  // every layer
            device_id_ = devices[0] == nullptr ? "" : ggml_backend_dev_description(devices[0]);
        }
        mp.load_mode = LLAMA_LOAD_MODE_MMAP;
        if (!options_.gpu_device.empty() && devices[0] == nullptr) {
            load_error_ = "no se encuentra la GPU " + options_.gpu_device;
        } else {
            model_ = llama_model_load_from_file(options_.model_path.c_str(), mp);
        }
        if (model_ == nullptr) {
            if (load_error_.empty()) load_error_ = "no se ha podido cargar el modelo: " + options_.model_path;
        } else {
            llama_context_params cp = llama_context_default_params();
            cp.n_ctx = kCtx;
            cp.n_batch = kCtx;  // the prompt fits in a single llama_decode call
            // n_ubatch=2048 reserves a ~2 GB compute buffer and does not speed up ~200-token prompts;
            // with 512 a longer prompt is split into micro-batches within the same llama_decode.
            cp.n_ubatch = kUbatch;
            cp.n_threads = options_.threads;
            cp.n_threads_batch = options_.threads;
            ctx_ = llama_init_from_model(model_, cp);
            if (ctx_ == nullptr) load_error_ = "no se ha podido crear el contexto: " + options_.model_path;
        }
        r.timings.load_ms = ms_since(t0);
        if (ctx_ == nullptr) {
            r.error = "load_failed";
            r.error_detail = load_error_;
            return false;
        }
        vocab_ = llama_model_get_vocab(model_);
        sampler_ = llama_sampler_chain_init(llama_sampler_chain_default_params());
        llama_sampler_chain_add(sampler_, llama_sampler_init_greedy());

        // Model identity for the cache key: name, size and mtime.
        struct stat st {};
        if (::stat(options_.model_path.c_str(), &st) == 0) {
            const std::size_t slash = options_.model_path.rfind('/');
            model_id_ = "model=" + options_.model_path.substr(slash == std::string::npos ? 0 : slash + 1) +
                        "\nsize=" + std::to_string(st.st_size) + "\nmtime=" + std::to_string(st.st_mtim.tv_sec) +
                        "." + std::to_string(st.st_mtim.tv_nsec) + "\n";
        }
        return true;
    }

    std::vector<llama_token> tokenize(const std::string& text) const {
        const bool add_bos = llama_vocab_get_add_bos(vocab_);
        const auto len = static_cast<std::int32_t>(text.size());
        std::vector<llama_token> tokens(text.size() + 2);
        std::int32_t n = llama_tokenize(vocab_, text.data(), len, tokens.data(),
                                        static_cast<std::int32_t>(tokens.size()), add_bos, true);
        if (n < 0) {
            tokens.resize(static_cast<std::size_t>(-n));
            n = llama_tokenize(vocab_, text.data(), len, tokens.data(),
                               static_cast<std::int32_t>(tokens.size()), add_bos, true);
        }
        tokens.resize(n < 0 ? 0 : static_cast<std::size_t>(n));
        return tokens;
    }

    std::string piece(llama_token token) const {
        char buf[256];
        const std::int32_t n = llama_token_to_piece(vocab_, token, buf, sizeof(buf), 0, false);
        if (n >= 0) return std::string(buf, static_cast<std::size_t>(n));
        std::string big(static_cast<std::size_t>(-n), '\0');
        const std::int32_t m = llama_token_to_piece(vocab_, token, big.data(), -n, 0, false);
        big.resize(m < 0 ? 0 : static_cast<std::size_t>(m));
        return big;
    }

    // Decodes tokens[from, to) right after what is already in memory.
    bool decode(std::vector<llama_token>& tokens, std::size_t from, std::size_t to) {
        return llama_decode(ctx_, llama_batch_get_one(tokens.data() + from,
                                                      static_cast<std::int32_t>(to - from))) == 0;
    }

    void clear_memory() { llama_memory_clear(llama_get_memory(ctx_), true); }

    // Path of the prefix state. The key covers everything that changes the state, including the
    // prefix text: changing the prompt invalidates the cache by itself. Empty => no cache.
    std::string prefix_cache_path(const std::string& prefix) const {
        if (model_id_.empty()) return {};
        const std::string dir = prefix_cache_dir();
        if (dir.empty()) return {};
        const std::string key = "contract=" + std::to_string(kContractVersion) + "\n" + model_id_ +
                                "n_ctx=" + std::to_string(kCtx) + "\nversion=" DALE_VERSION "\nggml=" +
                                ggml_version() + "-" + ggml_commit() + "\ndevice=" +
                                (device_id_.empty() ? "cpu" : device_id_) + "\n" + prefix;
        return dir + "/prefix-" + fetch::sha256_hex(key).substr(0, 16) + ".state";
    }

    // Custom file format: header, prefix tokens and the llama_state_seq_get_data state.
    // llama_state_seq_load_file is not used because it aborts the process (GGML_ASSERT) on a
    // truncated file; here size and checksum are verified before handing anything to llama.cpp.
    struct CacheHeader {
        char magic[8];
        std::uint64_t n_tokens;
        std::uint64_t n_state;
        std::uint64_t checksum;  // of the tokens and the state
    };
    static constexpr char kCacheMagic[8] = {'D', 'A', 'L', 'E', 'P', 'F', 'X', '1'};

    // Loads the saved state and checks it holds exactly the prefix tokens.
    // If the file exists but is invalid, deletes it. On failure, leaves memory cleared.
    bool load_prefix(const std::string& path, const std::vector<llama_token>& tokens, std::size_t n_prefix) {
        std::FILE* f = std::fopen(path.c_str(), "rb");
        if (f == nullptr) return false;
        CacheHeader h{};
        std::vector<unsigned char> data;
        bool ok = std::fread(&h, sizeof(h), 1, f) == 1 && std::memcmp(h.magic, kCacheMagic, 8) == 0 &&
                  h.n_tokens == n_prefix && h.n_state > 0 && h.n_state < (std::uint64_t{1} << 32);
        if (ok) {
            data.resize(n_prefix * sizeof(llama_token) + h.n_state);
            ok = std::fread(data.data(), 1, data.size(), f) == data.size() && std::fgetc(f) == EOF &&
                 checksum(data) == h.checksum &&
                 std::memcmp(data.data(), tokens.data(), n_prefix * sizeof(llama_token)) == 0;
        }
        std::fclose(f);
        ok = ok &&
             llama_state_seq_set_data(ctx_, data.data() + n_prefix * sizeof(llama_token), h.n_state, 0) > 0 &&
             llama_memory_seq_pos_max(llama_get_memory(ctx_), 0) == static_cast<llama_pos>(n_prefix) - 1;
        if (!ok) {
            std::remove(path.c_str());
            clear_memory();
        }
        return ok;
    }

    // Saves the prefix state to a temporary file in the same directory and renames it (atomic).
    // A failure only means there is no cache.
    void save_prefix(const std::string& path, const std::vector<llama_token>& tokens, std::size_t n_prefix) {
        const std::size_t n_state = llama_state_seq_get_size(ctx_, 0);
        if (n_state == 0) return;
        std::vector<unsigned char> data(n_prefix * sizeof(llama_token) + n_state);
        std::memcpy(data.data(), tokens.data(), n_prefix * sizeof(llama_token));
        if (llama_state_seq_get_data(ctx_, data.data() + n_prefix * sizeof(llama_token), n_state, 0) != n_state) {
            return;
        }
        CacheHeader h{};
        std::memcpy(h.magic, kCacheMagic, 8);
        h.n_tokens = n_prefix;
        h.n_state = n_state;
        h.checksum = checksum(data);

        std::string tmp = path.substr(0, path.rfind('/') + 1) + ".prefix.state-XXXXXX";
        const int fd = ::mkstemp(tmp.data());
        if (fd < 0) return;
        std::FILE* f = ::fdopen(fd, "wb");
        if (f == nullptr) {
            ::close(fd);
            std::remove(tmp.c_str());
            return;
        }
        const bool written = std::fwrite(&h, sizeof(h), 1, f) == 1 &&
                             std::fwrite(data.data(), 1, data.size(), f) == data.size();
        if (std::fclose(f) != 0 || !written || std::rename(tmp.c_str(), path.c_str()) != 0) {
            std::remove(tmp.c_str());
            return;
        }
        const std::size_t slash = path.rfind('/');
        prune_prefix_cache(path.substr(0, slash), path.substr(slash + 1), std::time(nullptr));
    }

    // Prefill. If the prompt starts with the prefix tokens, it is decoded in two parts
    // (prefix and rest), cached or not, so the output is the same with and without the cache.
    bool prefill(std::vector<llama_token>& tokens, const std::string& prefix) {
        clear_memory();
        std::size_t n_prefix = 0;
        if (!prefix.empty()) {
            const std::vector<llama_token> p = tokenize(prefix);
            if (!p.empty() && p.size() < tokens.size() && std::equal(p.begin(), p.end(), tokens.begin())) {
                n_prefix = p.size();
            }
        }
        if (n_prefix == 0) return decode(tokens, 0, tokens.size());

        const std::string path = prefix_cache_path(prefix);
        if (!path.empty() && load_prefix(path, tokens, n_prefix)) {
            if (decode(tokens, n_prefix, tokens.size())) return true;
            std::remove(path.c_str());  // loaded state unusable: decode everything
            clear_memory();
        }
        if (!decode(tokens, 0, n_prefix)) return false;
        if (!path.empty()) save_prefix(path, tokens, n_prefix);
        return decode(tokens, n_prefix, tokens.size());
    }

    // Prefill and greedy decoding. Stores in r the raw text, the timings and, if it stops before
    // the parser, the error.
    void generate(const std::string& prompt, const std::string& prefix, Clock::time_point t_loaded,
                  EngineResult& r) {
        llama_sampler_reset(sampler_);

        const auto t_prefill = Clock::now();
        std::vector<llama_token> tokens = tokenize(prompt);
        r.timings.prompt_tokens = static_cast<int>(tokens.size());
        if (tokens.empty()) {
            r.error = "invalid_json";
            r.error_detail = "prompt vacío o no tokenizable";
            return;
        }
        if (tokens.size() + kMaxGenTokens > kCtx) {
            r.error = "too_long";
            r.error_detail = "el prompt tiene " + std::to_string(tokens.size()) + " tokens";
            return;
        }
        if (!prefill(tokens, prefix)) {
            r.error = "decode_failed";
            r.error_detail = "llama_decode falló en el prefill";
            return;
        }
        r.timings.prefill_ms = ms_since(t_prefill);

        const auto t_gen = Clock::now();
        JsonTracker tracker;
        for (;;) {
            llama_token token = llama_sampler_sample(sampler_, ctx_, -1);
            if (llama_vocab_is_eog(vocab_, token)) break;
            ++r.timings.gen_tokens;
            const std::string text = piece(token);
            r.raw.append(text, 0, tracker.feed(text));
            if (tracker.state() == JsonTracker::State::Done) break;
            if (tracker.state() == JsonTracker::State::Invalid) {
                r.error = "invalid_json";
                r.error_detail = "la respuesta no empieza por '{'";
                break;
            }
            if (r.timings.gen_tokens >= kMaxGenTokens) {
                r.error = "truncated";
                r.error_detail = "máximo de " + std::to_string(kMaxGenTokens) + " tokens";
                break;
            }
            if (Clock::now() - t_loaded > kTimeout) {
                r.error = "timeout";
                r.error_detail = "más de " + std::to_string(kTimeout.count()) + " s";
                break;
            }
            if (llama_decode(ctx_, llama_batch_get_one(&token, 1)) != 0) {
                r.error = "decode_failed";
                r.error_detail = "llama_decode falló en la generación";
                break;
            }
        }
        r.timings.generate_ms = ms_since(t_gen);
    }

    LlamaOptions options_;
    bool backend_ = false;
    std::string load_error_;
    std::string model_id_;  // empty => no prefix cache
    std::string device_id_;  // GPU description; empty => CPU
    llama_model* model_ = nullptr;
    llama_context* ctx_ = nullptr;
    const llama_vocab* vocab_ = nullptr;
    llama_sampler* sampler_ = nullptr;
};

}  // namespace

void prune_prefix_cache(const std::string& dir, const std::string& keep_name, std::time_t now) {
    constexpr std::time_t kStaleTmp = 60 * 60;
    DIR* d = ::opendir(dir.c_str());
    if (d == nullptr) return;
    while (const dirent* e = ::readdir(d)) {
        const std::string_view name = e->d_name;
        const bool state = name.starts_with("prefix-") && name.ends_with(".state") && name != keep_name;
        const bool tmp = name.starts_with(".prefix.state-");
        if (!state && !tmp) continue;
        const std::string path = dir + "/" + std::string(name);
        struct stat st{};
        if (::lstat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
        if (tmp && now - st.st_mtime <= kStaleTmp) continue;
        ::unlink(path.c_str());
    }
    ::closedir(d);
}

void set_llama_logging() { llama_log_set(log_callback, debug_enabled() ? &g_debug_tag : nullptr); }

std::unique_ptr<Engine> make_llama_engine(const LlamaOptions& options) {
    return std::make_unique<LlamaEngine>(options);
}

}  // namespace dale::infer
