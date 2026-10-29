#pragma once
// UI strings in Spanish and English.

#include <initializer_list>
#include <string>
#include <string_view>

#include "config/config.hpp"

namespace dale::cli {

enum class UiLang { Es, En };

enum class Msg {
    Help,
    Version,           // {} dale, {} model version, {} quantization, {} short sha256
    UnknownFlag,       // + flag
    MissingValue,      // + flag
    RequestTooLong,    // {} = max bytes
    ContextUnreadable, // + path
    ContextInvalid,    // + path
    EngineError,       // + unknown error code
    InvalidResponse,
    Incomplete,        // appended to InvalidResponse
    Timeout,
    LoadFailed,        // + path
    ModelMissing,      // + path
    NoModelsDir,
    DownloadOffer,     // {} file, {} MB, {} URL
    DownloadPrompt,
    DownloadDeclined,
    DownloadNoTty,     // {} file, {} MB, {} URL
    DownloadDone,      // + path
    GpuOffer,          // {} GPU, {} free MB, {} file, {} MB, {} URL
    GpuPrompt,
    GpuChoice,         // {} GPU, {} free MB, {} 4B file, {} 4B MB, {} 9B file, {} 9B MB
    GpuChoicePrompt,
    GpuWarmup,
    GpuDeclined,
    GpuDownloadFailed,
    GpuFailed,         // + detail
    GpuCrashed,
    GpuMissing,        // {} GPU
    NoGpuFound,
    NoVulkanLoader,
    SetupCpu,          // {} model file
    SetupGpu,          // {} GPU, {} model file
    Refuse,            // + explanation
    Unsure,            // + explanation
    LevelSafe,
    LevelCaution,
    DangerBanner,
    Irreversible,
    PromptSafe,
    PromptCaution,
    PromptDanger,
    DangerWord,
    MoreReason,        // "and 1 more reason"
    MoreReasons,       // "and {} more reasons"
    Cancelled,
    Copied,
    CopyFailed,
    EditedExplanation,
    Count_
};

std::string_view tr(UiLang lang, Msg msg);

// Replaces the first "{}" in pattern with value.
std::string fill(std::string_view pattern, std::string_view value);

// Replaces each "{}" in pattern, in order, with the matching value (extra ones get "").
std::string fill_all(std::string_view pattern, std::initializer_list<std::string_view> values);

// lang from the config unless Auto; otherwise the first non-empty of LC_ALL, LC_MESSAGES
// and LANG (nullptr = unset): "es…" => Spanish; anything else => English.
UiLang ui_lang(config::Lang lang, const char* lc_all, const char* lc_messages, const char* lang_env);

// Same, reading the real environment.
UiLang ui_lang_from_env(config::Lang lang);

}  // namespace dale::cli
