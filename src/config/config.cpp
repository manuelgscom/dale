#include "config/config.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

namespace dale::config {

namespace {

std::string_view trim(std::string_view s) {
    const auto is_space = [](char c) { return c == ' ' || c == '\t' || c == '\r'; };
    while (!s.empty() && is_space(s.front())) s.remove_prefix(1);
    while (!s.empty() && is_space(s.back())) s.remove_suffix(1);
    return s;
}

// Strips trailing slashes ("/home/x/" -> "/home/x"; "/" -> "").
std::string_view strip_trailing_slashes(std::string_view s) {
    while (!s.empty() && s.back() == '/') s.remove_suffix(1);
    return s;
}

void append_hex(std::string& out, unsigned char c) {
    static const char digits[] = "0123456789abcdef";
    out += "\\x";
    out += digits[c >> 4];
    out += digits[c & 0x0f];
}

// Length of the valid UTF-8 sequence starting at s[i], or 0 if invalid.
// C1 controls (U+0080..U+009F) count as invalid: they get escaped.
std::size_t utf8_len(std::string_view s, std::size_t i) {
    const auto b = [&](std::size_t k) { return static_cast<unsigned char>(s[k]); };
    const unsigned char c = b(i);
    std::size_t n = 0;
    unsigned cp = 0;
    if (c >= 0xc2 && c <= 0xdf) { n = 2; cp = c & 0x1fu; }
    else if (c >= 0xe0 && c <= 0xef) { n = 3; cp = c & 0x0fu; }
    else if (c >= 0xf0 && c <= 0xf4) { n = 4; cp = c & 0x07u; }
    else return 0;
    if (i + n > s.size()) return 0;
    for (std::size_t k = 1; k < n; ++k) {
        if ((b(i + k) & 0xc0) != 0x80) return 0;
        cp = (cp << 6) | (b(i + k) & 0x3fu);
    }
    if (cp < 0xa0) return 0;                                  // C1 or overlong form
    if (n == 3 && (cp < 0x800 || (cp >= 0xd800 && cp <= 0xdfff))) return 0;
    if (n == 4 && (cp < 0x10000 || cp > 0x10ffff)) return 0;
    return n;
}

// Quoted value for a message: no control characters that could reach the terminal.
std::string quote(std::string_view s) {
    std::string out = "\"";
    for (std::size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        if (c == '"' || c == '\\') {
            out += '\\';
            out += static_cast<char>(c);
            ++i;
        } else if (c < 0x20 || c == 0x7f) {
            append_hex(out, c);
            ++i;
        } else if (c < 0x80) {
            out += static_cast<char>(c);
            ++i;
        } else if (const std::size_t n = utf8_len(s, i); n > 0) {
            out.append(s.substr(i, n));
            i += n;
        } else {
            append_hex(out, c);
            ++i;
        }
    }
    out += '"';
    return out;
}

ConfigResult fail(std::string_view filename, int line, const std::string& es, const std::string& en) {
    const std::string prefix = std::string(filename) + ":" + std::to_string(line) + ": ";
    return {std::nullopt, prefix + es, prefix + en};
}

// Integer 1..256: digits only, no sign; leading zeros do not count towards the length.
bool parse_threads(std::string_view v, int& out) {
    if (v.empty()) return false;
    long n = 0;
    for (char c : v) {
        if (c < '0' || c > '9') return false;
        n = n * 10 + (c - '0');
        if (n > 256) return false;  // stop before overflowing
    }
    if (n < 1) return false;
    out = static_cast<int>(n);
    return true;
}

}  // namespace

ConfigResult parse_config(std::string_view text, std::string_view filename, std::string_view home) {
    Config cfg;
    constexpr int kKeys = 5;
    int seen_line[kKeys] = {0, 0, 0, 0, 0};
    static const char* const keys[kKeys] = {"mode", "lang", "model", "threads", "device"};

    // Leading UTF-8 BOM: ignored.
    if (text.substr(0, 3) == "\xEF\xBB\xBF") text.remove_prefix(3);

    int lineno = 0;
    while (!text.empty()) {
        ++lineno;
        const std::size_t nl = text.find('\n');
        std::string_view raw = text.substr(0, nl);
        text.remove_prefix(nl == std::string_view::npos ? text.size() : nl + 1);

        const std::string_view line = trim(raw);
        if (line.empty() || line.front() == '#') continue;

        const std::size_t eq = line.find('=');
        if (eq == std::string_view::npos)
            return fail(filename, lineno, "falta \"=\" (formato: clave = valor)",
                        "missing \"=\" (format: key = value)");

        const std::string_view key = trim(line.substr(0, eq));
        const std::string_view value = trim(line.substr(eq + 1));

        int k = -1;
        for (int i = 0; i < kKeys; ++i)
            if (key == keys[i]) k = i;
        if (k < 0)
            return fail(filename, lineno,
                        "clave desconocida: " + quote(key) + " (válidas: mode, lang, model, threads, device)",
                        "unknown key: " + quote(key) + " (valid: mode, lang, model, threads, device)");

        const std::string name = keys[k];
        if (seen_line[k] != 0)
            return fail(filename, lineno,
                        "clave repetida: " + name + " (ya definida en la línea " + std::to_string(seen_line[k]) + ")",
                        "duplicate key: " + name + " (already set on line " + std::to_string(seen_line[k]) + ")");
        seen_line[k] = lineno;

        if (value.empty())
            return fail(filename, lineno, "valor vacío para " + name, "empty value for " + name);

        const auto invalid = [&](const std::string& valid_es, const std::string& valid_en) {
            return fail(filename, lineno,
                        "valor no válido para " + name + ": " + quote(value) + " (válidos: " + valid_es + ")",
                        "invalid value for " + name + ": " + quote(value) + " (valid: " + valid_en + ")");
        };

        if (name == "mode") {
            if (value == "suggest") cfg.mode = Mode::Suggest;
            else if (value == "confirm") cfg.mode = Mode::Confirm;
            else if (value == "auto") cfg.mode = Mode::Auto;
            else return invalid("suggest, confirm, auto", "suggest, confirm, auto");
        } else if (name == "lang") {
            if (value == "auto") cfg.lang = Lang::Auto;
            else if (value == "es") cfg.lang = Lang::Es;
            else if (value == "en") cfg.lang = Lang::En;
            else return invalid("auto, es, en", "auto, es, en");
        } else if (name == "model") {
            if (value.starts_with("~/")) {
                const std::string_view base = strip_trailing_slashes(home);
                if (base.empty())
                    return fail(filename, lineno, "no se puede expandir \"~/\" en model: HOME no está definida",
                                "cannot expand \"~/\" in model: HOME is not set");
                cfg.model = std::string(base) + std::string(value.substr(1));
            } else {
                cfg.model = std::string(value);
            }
        } else if (name == "device") {
            if (value == "auto") cfg.device = Device::Auto;
            else if (value == "cpu") cfg.device = Device::Cpu;
            else if (value == "gpu") cfg.device = Device::Gpu;
            else return invalid("auto, cpu, gpu", "auto, cpu, gpu");
        } else {
            if (!parse_threads(value, cfg.threads)) return invalid("entero de 1 a 256", "integer from 1 to 256");
        }
    }
    return {cfg, {}, {}};
}

std::string config_path() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg != nullptr && *xdg != '\0') return std::string(strip_trailing_slashes(xdg)) + "/dale/config";
    const char* home = std::getenv("HOME");
    if (home == nullptr || *home == '\0') return {};
    return std::string(strip_trailing_slashes(home)) + "/.config/dale/config";
}

ConfigResult load_config() {
    const std::string path = config_path();
    if (path.empty()) return {Config{}, {}, {}};

    const char* home_env = std::getenv("HOME");
    const std::string_view home = home_env != nullptr ? home_env : "";

    // Display name: $HOME/... -> ~/...
    std::string shown = path;
    const std::string_view base = strip_trailing_slashes(home);
    if (!base.empty() && path.size() > base.size() + 1 && path.compare(0, base.size(), base) == 0 &&
        path[base.size()] == '/')
        shown = "~" + path.substr(base.size());

    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::file_status st = fs::status(path, ec);
    if (st.type() == fs::file_type::not_found) return {Config{}, {}, {}};
    if (ec) {
        const std::string why = ec.message();
        return {std::nullopt, shown + ": no se puede leer: " + why, shown + ": cannot read: " + why};
    }
    if (fs::is_directory(st))
        return {std::nullopt, shown + ": no se puede leer: es un directorio", shown + ": cannot read: is a directory"};

    errno = 0;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        const std::string why = errno != 0 ? std::strerror(errno) : "error de lectura";
        const std::string why_en = errno != 0 ? std::strerror(errno) : "read error";
        return {std::nullopt, shown + ": no se puede leer: " + why, shown + ": cannot read: " + why_en};
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    if (in.bad())
        return {std::nullopt, shown + ": no se puede leer: error de lectura", shown + ": cannot read: read error"};

    return parse_config(buf.str(), shown, home);
}

}  // namespace dale::config
