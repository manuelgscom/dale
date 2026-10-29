#pragma once
// First-run choice of device and model, written by dale (the config file is only read).
// File: $XDG_STATE_HOME/dale/setup, or $HOME/.local/state/dale/setup. "key = value" lines.
// Deleting it (or `dale --setup`) repeats the detection.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "config/config.hpp"

namespace dale::config {

struct Setup {
    Device device = Device::Cpu;  // Cpu or Gpu
    std::string model;            // file name in the models directory
    std::string gpu;              // GPU description (device = gpu)
    std::uint64_t vram_free_mib = 0;  // free VRAM at detection, for reference
};

// nullopt if a line is malformed, a key is unknown or device/model are missing.
std::optional<Setup> parse_setup(std::string_view text);
std::string format_setup(const Setup& setup);

// Empty if neither XDG_STATE_HOME nor HOME is set.
std::string setup_path();

// Missing, unreadable or invalid file => nullopt (dale detects again).
std::optional<Setup> load_setup();

// Atomic write (temporary file + rename), creating the directories. false on failure.
bool save_setup(const Setup& setup);

// GPU run markers: <setup dir>/gpu-run.<pid>-<start time> exists while a dale process runs a
// request on the GPU of the saved setup. A marker left by a process that no longer exists (pid
// gone, or reused by a process with another start time) means it died on the GPU (driver abort,
// crash): the next run moves the setup to the CPU. Ctrl-C and the
// termination signals remove the marker (cli.cpp), so they never count as a GPU failure.
bool begin_gpu_run();             // creates this process's marker; false if it cannot
void end_gpu_run();               // removes it
void end_gpu_run_from_signal();   // the same, async-signal-safe (only unlink)
bool gpu_run_died();              // removes the markers of dead processes; true if there was one

}  // namespace dale::config
