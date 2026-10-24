#include "fetch/fetch.hpp"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <vector>

#include "exec/posix_util.hpp"
#include "fetch/sha256.hpp"

namespace dale::fetch {

namespace {

std::string env_or_empty(const char* name) {
    const char* v = std::getenv(name);
    return v != nullptr ? v : "";
}

bool is_dir(const std::string& path) {
    struct stat st {};
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// mkdir -p with 0755.
bool make_dirs(const std::string& dir) {
    if (dir.empty()) return false;
    for (std::size_t pos = 1; pos <= dir.size(); ++pos) {
        if (pos < dir.size() && dir[pos] != '/') continue;
        const std::string part = dir.substr(0, pos);
        if (mkdir(part.c_str(), 0755) != 0 && errno != EEXIST) return false;
    }
    return is_dir(dir);
}

std::optional<std::uint64_t> statvfs_free(const std::string& dir) {
    struct statvfs st {};
    if (statvfs(dir.c_str(), &st) != 0) return std::nullopt;
    return std::uint64_t{st.f_bavail} * std::uint64_t{st.f_frsize};
}

// Absolute path of the first executable prog in path; empty if not found.
std::string find_in_path(const std::string& path, const std::string& prog) {
    std::size_t start = 0;
    while (start <= path.size()) {
        std::size_t end = path.find(':', start);
        if (end == std::string::npos) end = path.size();
        const std::string dir = path.substr(start, end - start);
        if (!dir.empty()) {
            const std::string full = dir + "/" + prog;
            struct stat st {};
            if (stat(full.c_str(), &st) == 0 && S_ISREG(st.st_mode) &&
                access(full.c_str(), X_OK) == 0)
                return full;
        }
        start = end + 1;
    }
    return {};
}

std::string mib(std::uint64_t bytes) { return std::to_string((bytes + (1 << 20) - 1) >> 20); }

bool fsync_path(const std::string& path, int flags) {
    const int fd = open(path.c_str(), flags | O_CLOEXEC);
    if (fd < 0) return false;
    const bool ok = fsync(fd) == 0;
    close(fd);
    return ok;
}

DownloadResult failed(std::string es, std::string en) {
    DownloadResult r;
    r.status = DownloadResult::Status::Failed;
    r.error = std::move(es);
    r.error_en = std::move(en);
    return r;
}

}  // namespace

std::string models_dir() {
    const std::string xdg = env_or_empty("XDG_DATA_HOME");
    if (!xdg.empty()) return xdg + "/dale/models";
    const std::string home = env_or_empty("HOME");
    if (!home.empty()) return home + "/.local/share/dale/models";
    return {};
}

DownloadResult download(const std::string& url, const std::string& dir, const std::string& filename,
                        std::uint64_t size, const std::string& sha256,
                        const DownloadOptions& options) {
    // 1. Directory.
    if (!make_dirs(dir)) {
        return failed("no se pudo crear el directorio " + dir, "could not create directory " + dir);
    }

    // 2. Free space: size + 10%.
    const std::uint64_t needed = size + size / 10;
    const std::optional<std::uint64_t> avail =
        options.free_space ? options.free_space(dir) : statvfs_free(dir);
    if (avail && *avail < needed) {
        return failed("no hay espacio suficiente en " + dir + ": hacen falta " + mib(needed) +
                          " MiB y hay " + mib(*avail) + " MiB libres",
                      "not enough space in " + dir + ": " + mib(needed) + " MiB needed, " +
                          mib(*avail) + " MiB free");
    }

    // 4. curl, or else wget (before the temp file: if neither exists nothing is created).
    const std::string path_env = options.path_env.empty() ? env_or_empty("PATH") : options.path_env;
    std::string tool = find_in_path(path_env, "curl");
    const bool use_curl = !tool.empty();
    if (!use_curl) tool = find_in_path(path_env, "wget");
    if (tool.empty()) {
        return failed("no se encuentra curl ni wget; descarga el modelo a mano desde " + url +
                          " (SHA-256 " + sha256 + ") y usa --model <fichero>",
                      "neither curl nor wget found; download the model manually from " + url +
                          " (SHA-256 " + sha256 + ") and use --model <file>");
    }
    const char* tool_name = use_curl ? "curl" : "wget";

    // 3. Temporary file in the same directory.
    std::string tmpl = dir + "/." + filename + ".part-XXXXXX";
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    const int fd = mkstemp(buf.data());
    if (fd < 0) {
        return failed("no se pudo crear el fichero temporal en " + dir,
                      "could not create temporary file in " + dir);
    }
    close(fd);  // the tool reopens it by name
    const std::string tmp(buf.data());

    // 5. Fixed argv, no shell. Runs the resolved path (execvp with a '/' does not search PATH).
    const bool tty = isatty(2) != 0;
    std::vector<std::string> argv;
    if (use_curl) {
        argv = {tool, "-fL", "--proto", "=https", "--tlsv1.2", "--retry", "3",
                "--connect-timeout", "20"};
        argv.emplace_back(tty ? "--progress-bar" : "-sS");
        argv.insert(argv.end(), {"-o", tmp, url});
    } else {
        argv = {tool, "--https-only"};
        if (tty) argv.insert(argv.end(), {"-q", "--show-progress"});
        else argv.emplace_back("-q");
        argv.insert(argv.end(), {"-O", tmp, url});
    }

    // 6. The parent ignores SIGINT while waiting; the child keeps the default handler.
    int code = 0;
    {
        std::fflush(nullptr);
        exec::detail::IgnoreInterrupts guard;
        exec::detail::ChildOptions opts;
        const pid_t pid = exec::detail::spawn(argv, opts);
        code = pid < 0 ? -1 : exec::detail::wait_child(pid);
    }
    if (code != 0) {
        unlink(tmp.c_str());
        const std::string name(tool_name);
        if (code == 128 + SIGINT) {
            DownloadResult r;
            r.status = DownloadResult::Status::Interrupted;
            r.error = "descarga interrumpida";
            r.error_en = "download interrupted";
            return r;
        }
        if (code < 0) return failed("no se pudo lanzar " + name, "could not launch " + name);
        if (code > 128) {
            const std::string sig = std::to_string(code - 128);
            return failed("la descarga ha fallado: " + name + " terminó por la señal " + sig,
                          "the download failed: " + name + " was killed by signal " + sig);
        }
        const std::string c = std::to_string(code);
        return failed("la descarga ha fallado: " + name + " salió con código " + c,
                      "the download failed: " + name + " exited with code " + c);
    }

    // 7. Exact size and SHA-256.
    struct stat st {};
    const bool size_ok = stat(tmp.c_str(), &st) == 0 && S_ISREG(st.st_mode) &&
                         static_cast<std::uint64_t>(st.st_size) == size;
    if (!size_ok || sha256_file(tmp) != sha256) {
        unlink(tmp.c_str());
        return failed("el fichero descargado no es íntegro", "the downloaded file is corrupted");
    }

    // 8. fsync, rename and fsync of the directory. mkstemp creates with 0600; the model is not secret.
    const std::string final_path = dir + "/" + filename;
    if (chmod(tmp.c_str(), 0644) != 0 || !fsync_path(tmp, O_RDWR) || rename(tmp.c_str(), final_path.c_str()) != 0) {
        unlink(tmp.c_str());
        return failed("no se pudo guardar el modelo en " + final_path,
                      "could not save the model to " + final_path);
    }
    fsync_path(dir, O_RDONLY | O_DIRECTORY);  // the file is in place; a failure here is not rolled back

    DownloadResult r;
    r.status = DownloadResult::Status::Ok;
    r.path = final_path;
    return r;
}

}  // namespace dale::fetch
