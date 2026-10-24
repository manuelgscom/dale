#pragma once
// Model download with the system curl/wget and SHA-256 check.
// dale's only network request, and only when the user accepts it.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace dale::fetch {

// $XDG_DATA_HOME/dale/models if set and non-empty; otherwise $HOME/.local/share/dale/models.
// Empty if neither is available.
std::string models_dir();

struct DownloadResult {
    enum class Status {
        Ok,
        Interrupted,  // Ctrl-C during the download: the CLI exits with 130
        Failed,
    };
    Status status = Status::Failed;
    std::string path;     // final path if Ok
    std::string error;    // Spanish
    std::string error_en; // English
};

struct DownloadOptions {
    // Free bytes on dir's filesystem. Defaults to statvfs; tests override it.
    // nullopt = unknown (does not block the download).
    std::function<std::optional<std::uint64_t>(const std::string& dir)> free_space;
    // PATH in which to look for curl/wget. Empty = the environment's PATH.
    std::string path_env;
};

// Creates dir (0755, with parents), downloads url to a temporary file in dir, checks size and
// SHA-256 and renames it to dir/filename. On any failure neither the temp nor the final file remains.
DownloadResult download(const std::string& url, const std::string& dir, const std::string& filename,
                        std::uint64_t size, const std::string& sha256,
                        const DownloadOptions& options = {});

}  // namespace dale::fetch
