# dale

[![Release](https://img.shields.io/github/v/release/manuelgscom/dale?logo=github&color=blue)](https://github.com/manuelgscom/dale/releases) [![License: MIT](https://img.shields.io/badge/license-MIT-blue)](LICENSE) ![Languages: English · Spanish](https://img.shields.io/badge/languages-English%20%C2%B7%20Spanish-informational) [![llama.cpp](https://img.shields.io/badge/built%20with-llama.cpp-informational)](https://github.com/ggml-org/llama.cpp) ![Runs on: CPU · GPU (Vulkan)](https://img.shields.io/badge/runs%20on-CPU%20%C2%B7%20GPU%20%28Vulkan%29-informational) [![Hugging Face](https://img.shields.io/badge/Hugging%20Face-dale%20models-FFD21E?logo=huggingface)](https://huggingface.co/collections/manuelgscom/dale-6addf69f0c5ce62251553885)

**Natural language to Linux commands. 100% offline. One native binary.**

```bash
$ dale "find the files over 200 MB in this folder"

  ❯ find . -type f -size +200M -exec ls -lh {} +
    Recursively finds files larger than 200 MB in the current
    directory and lists them with human-readable sizes.

  ● safe  ·  Run it? [Y/n/e(dit)/c(opy)]
```

> **You say dale. It asks if you're sure.**

`dale` turns a request in **English or Spanish** into one bash command. It explains the command, rates its risk and runs it only if you confirm. Everything happens on your machine: a language model fine-tuned for this single task, run by a native C++ binary with [llama.cpp](https://github.com/ggml-org/llama.cpp) built in. A small model on the CPU by default; a larger, more accurate one on the GPU (through Vulkan) when you have one. No connection, no accounts, no API keys, no telemetry.

https://github.com/user-attachments/assets/c674c03c-7c1c-42f1-aa2e-abc9944c0ea4

*`dale` live: ask in plain English; it proposes the command, explains it, rates the risk and only runs it once you confirm. Real, unedited recording on a desktop CPU (about 1 s per request; 1.7 s on the laptop in [The models](#the-models)).*

## Features

- **English and Spanish.** The explanation comes in the language of the request.
- **Aware of your system.** It takes into account your distribution, package manager and the tools in your `PATH`. For example, it proposes `fd` when it is installed and `find` when it is not.
- **Risk decided by code, not by the model.** A deterministic verifier written in C++ rates every command as **safe**, **caution** or **danger**. Anything it cannot analyse counts as caution at least.
- **Dangerous commands need a typed word.** Mass deletion, disk writes, piping downloaded code into a shell, flushing the firewall, `git push --force` and the like show a prominent warning. They run only after you type `run` (`ejecutar` in Spanish), in every mode.
- **Honest when unsure.** When a request is not a shell task, `dale` says so. When it is ambiguous, `dale` asks for details instead of guessing.
- **Edit or copy** the command before running it.
- **CPU or GPU, one binary.** On the first run `dale` detects your GPU and offers a larger model if it fits; otherwise it stays on the CPU. See [First run](#4-first-run).
- **Three modes:**
  - `suggest` shows the command and never runs it.
  - `confirm` (the default) asks before running it.
  - `auto` runs safe commands directly and asks for everything else.

https://github.com/user-attachments/assets/2287c8ba-1873-479d-bae3-c15fbc173c51

*Safety is decided by code, not by the model: caution with its reasons, danger with a confirmation word, and an honest "no" for non-commands or ambiguous requests. Real, unedited recording; nothing was run.*

## Install

The quickest way is the prebuilt binary from the [latest release](https://github.com/manuelgscom/dale/releases/latest). It is one file of about 51 MB for both CPU and GPU, and there is nothing else to install: no Python, no root access and, for the CPU, no GPU drivers. To compile it yourself instead, see [Build from source](#build-from-source).

### 1. Check the requirements

- Linux on x86_64 with a CPU that supports AVX2.
- glibc 2.29 or later (Ubuntu 20.04, Debian 11, Fedora 30 or newer).
- `curl` or `wget`, for the first model download.
- On Windows, use it through WSL 2.
- **Optional, for the GPU:** a dedicated GPU with at least 4 GiB of free VRAM (NVIDIA, AMD or Intel), its driver, and the Vulkan loader `libvulkan.so.1` (`libvulkan1` on Debian and Ubuntu, `vulkan-loader` on Fedora, `vulkan-icd-loader` on Arch). Almost every desktop already has the loader, since Mesa, browsers, Steam and VS Code bring it. It may be missing on a server with only the NVIDIA driver; then `dale` uses the CPU and tells you what to install.

```bash
grep -q avx2 /proc/cpuinfo && echo "AVX2: OK" || echo "AVX2: not supported"
ldd --version | head -n 1     # the last number is the glibc version
ldconfig -p | grep -q libvulkan.so.1 && echo "Vulkan loader: OK" || echo "Vulkan loader: missing (CPU only)"
```

The binary does not link the Vulkan library: it starts on any machine, and opens the loader only when it is going to use the GPU.

### 2. Download, verify and install

```bash
curl -LO https://github.com/manuelgscom/dale/releases/latest/download/dale-linux-x86_64
curl -LO https://github.com/manuelgscom/dale/releases/latest/download/SHA256SUMS
sha256sum -c --ignore-missing SHA256SUMS     # must print: dale-linux-x86_64: OK
chmod +x dale-linux-x86_64
mkdir -p ~/.local/bin
mv dale-linux-x86_64 ~/.local/bin/dale
```

This installs `dale` for your user only, in `~/.local/bin`.

### 3. Check that your shell finds it

```bash
dale --version
```

If you get `dale: command not found`, open a new terminal. Most distributions add `~/.local/bin` to your `PATH` at login, but only once the folder exists. If a new terminal does not help, add it yourself:

```bash
echo 'export PATH="$HOME/.local/bin:$PATH"' >> ~/.bashrc
source ~/.bashrc
```

### 4. First run

```bash
dale "list the files bigger than 200 MB"
```

The first time, `dale` looks for a GPU and picks a model:

```
first run ─► dedicated GPU, free VRAM?
               ├─ 8 GiB or more ─► choose: 1) dale-4b (Enter)  2) dale-9b  n) CPU
               ├─ 4 GiB or more ─► offer dale-4b                [Y/n]
               └─ less, or no GPU ─► dale-0.8b on the CPU
```

It always asks before downloading a model from Hugging Face (529 MB, 2.7 GB or 5.6 GB; see [The models](#the-models)). It checks its SHA-256 and stores it in `~/.local/share/dale/models/` (or `$XDG_DATA_HOME/dale/models/`). After that it makes no network requests at all.

- The choice is saved in `~/.local/state/dale/setup` (or `$XDG_STATE_HOME/dale/setup`). `dale --setup` detects the GPU and asks again.
- On the GPU, the first answers take a few seconds longer while the shaders are compiled. This happens once, and again on the first request after the computer starts.
- Integrated GPUs are never chosen automatically; `device = gpu` in the [configuration](#configuration) forces the GPU.
- If the GPU fails to load the model or to decode, `dale` answers that request on the CPU. After a GPU error or crash it uses the CPU from then on (with `device = auto`); `dale --setup` tries the GPU again.
- Without a terminal, or with `--json`, `dale` neither asks nor downloads.

If you already have a GGUF file, use `--model PATH` or the `model` key in the [configuration](#configuration) instead.

### Update and uninstall

- **Update:** repeat step 2. The models you already downloaded are kept, and one is downloaded again only if the new version uses a different one. When you update from 1.0.0, the first run detects your GPU as described in [step 4](#4-first-run).
- **Uninstall:**

  ```bash
  rm ~/.local/bin/dale
  rm -rf ~/.local/share/dale ~/.config/dale ~/.local/state/dale ~/.cache/dale   # models, configuration, first-run choice and cache
  ```

## Usage

```bash
dale <request…>
dale --model PATH <request…>   # use this GGUF file instead of the downloaded model
dale --setup                   # detect the GPU and choose the model again
dale --help
dale --version                 # also shows the choice saved on the first run
```

Answers at the prompt: `y` runs the command, `n` cancels, `e` opens it for editing and `c` copies it to the clipboard. In tmux, copying to the clipboard needs `set-clipboard on`.

### Configuration

`~/.config/dale/config` takes `key = value` lines, and every key is optional:

```ini
# suggest | confirm | auto
mode = confirm
# auto | en | es (language of the interface)
lang = auto
# a GGUF file instead of the downloaded model
# model = ~/models/dale-0.8b-1.0.0-Q4_K_M.gguf
# inference threads (1-256; by default, chosen by dale)
# threads = 4
# auto | cpu | gpu (auto follows the first-run choice)
device = auto
```

`device = cpu` keeps every request on the CPU. `device = gpu` uses the dedicated GPU with the most free VRAM (an integrated one if there is none) and the largest model that fits it.

## How it works

```
request ─► system context ─► local model ─► risk verifier ─► mode ─► show / run
           (os, distro,      (proposes one   (deterministic           (bash -c)
            package manager,  bash command,   rules in C++,
            installed tools)  refuses or      independent of
                              says unsure)    the model)
```

1. **Context collector.** It reads the OS, the distribution, the package manager and the optional tools found in `PATH`. This information never leaves the machine.
2. **Inference engine.** llama.cpp is linked into the binary through `llama.h`, with no server, daemon or port. It runs on the CPU or, through Vulkan, on the GPU. It decodes greedily, caps the output in tokens and stops as soon as the JSON object is closed.
3. **Risk verifier.** A shell lexer splits the command into simple commands, and a table of C++ rules assigns each one a level. The highest level wins. This is the most critical piece of the project and the one with the most tests.
4. **Executor.** It runs the approved command with `bash -c`. Without a TTY it runs nothing.
5. **Model download.** It uses the system's `curl` or `wget`, verifies the SHA-256 and renames the file atomically.

The model answers with one JSON object: `{"type":"command|refuse|unsure","command":"...","explanation":"..."}`. Anything else is rejected as invalid, and nothing is run.

## The models

The same task, contract and prompt in three sizes, all quantized to Q4_K_M. Scores are on the public test set [`manuelgscom/dale-bash-test-v2`](https://huggingface.co/datasets/manuelgscom/dale-bash-test-v2) (471 cases); the GPU ones were measured through `dale` itself, with Vulkan.

| Model | Used by `dale` | Download | VRAM in use | Test v2 | Median time per request |
|---|---|---|---|---|---|
| [`dale-0.8b`](https://huggingface.co/manuelgscom/dale-0.8b-GGUF) | CPU (default) | 529 MB | — | **79.4 %** (374/471) | 1.7 s, mid-range 2020 laptop, 4 cores / 8 threads |
| [`dale-4b`](https://huggingface.co/manuelgscom/dale-4b-GGUF) | GPU, ≥ 4 GiB free | 2.7 GB | ≈ 3.2 GiB | **86.6 %** (408/471) | 1.2 s, 16 GB desktop GPU |
| [`dale-9b`](https://huggingface.co/manuelgscom/dale-9b-GGUF) | GPU, ≥ 8 GiB free, if you choose it | 5.6 GB | ≈ 5.4 GiB | **89.6 %** (422/471) | 1.7 s, 16 GB desktop GPU |

They are fine-tunes of Qwen3.5-0.8B, Qwen3.5-4B and Qwen3.5-9B (the last one with a merged LoRA). For comparison, Qwen3.5-9B and Gemma 4 E4B-it without fine-tuning get 62.8 %. Times are warm requests; on the GPU, see the note on the first answers in [step 4](#4-first-run).

There is also [`dale-2b`](https://huggingface.co/manuelgscom/dale-2b-GGUF) (81.7 %), which the CLI does not use, for running with llama.cpp directly. The model cards, in the [collection](https://huggingface.co/collections/manuelgscom/dale-6addf69f0c5ce62251553885), describe the evaluation, the prompt contract and how to use each model with llama.cpp.

## Build from source

Use this if you want to read or change the code. For everyday use, the [prebuilt binary](#install) is simpler. llama.cpp is compiled along with `dale`, so there are no other libraries to install. The default build is CPU-only; the GPU is optional, see below.

### 1. Install the build tools

You need git, CMake 3.28 or later and GCC 13 or later.

| Distribution | Command |
|---|---|
| Ubuntu 24.04 or later, Debian 13 or later | `sudo apt install build-essential cmake git` |
| Fedora | `sudo dnf install gcc-c++ make cmake git` |
| Arch Linux | `sudo pacman -S --needed base-devel cmake git` |

Ubuntu 22.04 and Debian 12 ship an older CMake and GCC. There, use the prebuilt binary.

### 2. Get the code

```bash
git clone --recursive https://github.com/manuelgscom/dale.git
cd dale
```

`--recursive` also fetches llama.cpp (v0.6.0), which is a git submodule. If you cloned without it, run `git submodule update --init`.

### 3. Build and install

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DDALE_TEST_HOOKS=OFF
cmake --build build -j"$(nproc)" --target dale
install -Dm755 build/dale ~/.local/bin/dale
dale --version
```

The binary is optimised for the CPU of the machine where you build it.

To include the GPU, add `-DDALE_VULKAN=ON` to the first command. It needs the Vulkan headers, SPIRV-Headers and the `glslc` shader compiler, from the [Vulkan SDK](https://vulkan.lunarg.com/sdk/home) or your distribution (on Debian and Ubuntu: `sudo apt install libvulkan-dev spirv-headers glslc`). The result still starts without a Vulkan loader, like the release binary.

From here on, everything works as in [Install](#install): if `dale` is not found, see [step 3](#3-check-that-your-shell-finds-it); the first run downloads the model.

### Development build and tests

```bash
cmake -B build-dev
cmake --build build-dev -j"$(nproc)"
ctest --test-dir build-dev
```

The development build keeps the test hooks that the tests need, so do not use it as your everyday binary.

- `ctest` runs the unit tests (doctest) and the end-to-end tests in a pseudo-terminal. A fake engine stands in for the model, so these tests need no model file.
- The command lists in `tests/risk/` (`seguro.txt`, `precaucion.txt`, `peligro.txt`) fix the expected risk level of each command.
- The tests that load a real model are kept apart:

  ```bash
  DALE_TEST_MODEL=/path/to/dale-0.8b-1.0.0-Q4_K_M.gguf ctest --test-dir build-dev -L model
  ```

### Repository layout

```
CMakeLists.txt
src/
  main.cpp
  cli/        options, modes, prompts, output, configuration flow, i18n (en/es)
  config/     ~/.config/dale/config parser, saved first-run choice
  context/    system context collector
  infer/      prompt, llama.cpp engine, JSON parser, GPU detection
  vulkan/     forwards Vulkan calls to libvulkan.so.1, opened at run time
  risk/       shell lexer and risk rules (plus the dale_risk helper)
  exec/       bash -c executor, clipboard
  fetch/      model download and SHA-256
tests/        doctest unit tests, risk lists, golden prompt files, e2e tests in a pty
third_party/  llama.cpp (submodule), doctest
```

## Limitations

- bash on Linux only. No zsh, fish, PowerShell, cmd or macOS for now.
- The model can be wrong, in about 1 of 5 cases on the test set with `dale-0.8b` and 1 of 10 with `dale-9b`, and a wrong command can look plausible. **Always read the command before running it.**
- The GPU is used only through Vulkan (no CUDA or ROCm builds), and only one GPU at a time.
- `dale` is not a security tool. Its safety comes from the deterministic verifier and the confirmations, not from the model.

## License

The code is under the [MIT](LICENSE) license. The models are under Apache-2.0, the same license as their base models. The release binary also contains llama.cpp/ggml and nlohmann/json (MIT), code from the Khronos Vulkan and SPIR-V headers (Apache-2.0 and MIT) and the GCC runtime (under the GCC Runtime Library Exception); their notices are in [THIRD_PARTY_NOTICES](THIRD_PARTY_NOTICES), along with doctest (MIT), which only the tests use.

---

Author: [Manuel García Sanz](https://manuelgs.com)
