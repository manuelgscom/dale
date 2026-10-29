#pragma once
// User configuration: "key = value" file, custom parser.

#include <optional>
#include <string>
#include <string_view>

namespace dale::config {

enum class Mode { Suggest, Confirm, Auto };
enum class Lang { Auto, Es, En };
// auto = the first-run choice (GPU only with a dedicated GPU and enough VRAM); cpu and gpu force it.
enum class Device { Auto, Cpu, Gpu };

struct Config {
    Mode mode = Mode::Confirm;
    Lang lang = Lang::Auto;
    std::string model;  // empty = default model; "~/" already expanded
    int threads = 0;    // 0 = automatic
    Device device = Device::Auto;
};

// Empty config => error in the format "file:line: reason", in both languages
// (the UI language may depend on the broken file itself; the CLI picks one).
struct ConfigResult {
    std::optional<Config> config;
    std::string error;     // Spanish
    std::string error_en;  // English
};

// filename is only used in error messages; home, to expand "~/" in model.
ConfigResult parse_config(std::string_view text, std::string_view filename, std::string_view home = {});

// $XDG_CONFIG_HOME/dale/config if set and non-empty; otherwise $HOME/.config/dale/config.
std::string config_path();

// Missing file => defaults. dale never creates the file.
ConfigResult load_config();

}  // namespace dale::config
