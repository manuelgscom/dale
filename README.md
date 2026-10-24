# dale

**Natural language to Linux commands. 100% offline. One native binary.**

```bash
$ dale "find the files over 200 MB in this folder"

  ❯ find . -type f -size +200M -exec ls -lh {} +
    Recursively finds files larger than 200 MB in the current
    directory and lists them with human-readable sizes.

  ● safe  ·  Run it? [Y/n/e(dit)/c(opy)]
```

> **You say dale. It asks if you're sure.**

`dale` turns a request in **English or Spanish** into one bash command. It explains the command, rates its risk and runs it only if you confirm. Everything happens on your machine: a small language model fine-tuned for this single task, run on the CPU by a native C++ binary with [llama.cpp](https://github.com/ggml-org/llama.cpp) built in. No connection, no accounts, no API keys, no telemetry.

https://github.com/user-attachments/assets/c674c03c-7c1c-42f1-aa2e-abc9944c0ea4

*`dale` live: ask in plain English; it proposes the command, explains it, rates the risk and only runs it once you confirm. Real, unedited recording on a desktop CPU (about 1 s per request; 1.7 s on the laptop in [The model](#the-model)).*

## Features

- **English and Spanish.** The explanation comes in the language of the request.
- **Aware of your system.** It takes into account your distribution, package manager and the tools in your `PATH`. For example, it proposes `fd` when it is installed and `find` when it is not.
- **Risk decided by code, not by the model.** A deterministic verifier written in C++ rates every command as **safe**, **caution** or **danger**. Anything it cannot analyse counts as caution at least.
- **Dangerous commands need a typed word.** Mass deletion, disk writes, piping downloaded code into a shell, flushing the firewall, `git push --force` and the like show a prominent warning. They run only after you type `run` (`ejecutar` in Spanish), in every mode.
- **Honest when unsure.** When a request is not a shell task, `dale` says so. When it is ambiguous, `dale` asks for details instead of guessing.
- **Edit or copy** the command before running it.
- **Three modes:**
  - `suggest` shows the command and never runs it.
  - `confirm` (the default) asks before running it.
  - `auto` runs safe commands directly and asks for everything else.

https://github.com/user-attachments/assets/2287c8ba-1873-479d-bae3-c15fbc173c51

*Safety is decided by code, not by the model: caution with its reasons, danger with a confirmation word, and an honest "no" for non-commands or ambiguous requests. Real, unedited recording; nothing was run.*

## Install

The quickest way is the prebuilt binary from the [latest release](https://github.com/manuelgscom/dale/releases/latest). There is nothing else to install: no Python, no GPU drivers, no root access. To compile it yourself instead, see [Build from source](#build-from-source).

### 1. Check the requirements

- Linux on x86_64 with a CPU that supports AVX2.
- glibc 2.29 or later (Ubuntu 20.04, Debian 11, Fedora 30 or newer).
- `curl` or `wget`, for the first model download.
- No GPU needed. On Windows, use it through WSL 2.

```bash
grep -q avx2 /proc/cpuinfo && echo "AVX2: OK" || echo "AVX2: not supported"
ldd --version | head -n 1     # the last number is the glibc version
```

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

The first time, `dale` asks before downloading the model (529 MB) from Hugging Face. It checks its SHA-256 and stores it in `~/.local/share/dale/models/` (or `$XDG_DATA_HOME/dale/models/`). After that it makes no network requests at all. If you already have the GGUF file, use `--model PATH` or the `model` key in the [configuration](#configuration) instead.

### Update and uninstall

- **Update:** repeat step 2. The model is downloaded again only if the new version uses a different one.
- **Uninstall:**

  ```bash
  rm ~/.local/bin/dale
  rm -rf ~/.local/share/dale ~/.config/dale     # downloaded model and configuration
  ```

## Usage

```bash
dale <request…>
dale --model PATH <request…>   # use this GGUF file instead of the downloaded model
dale --help
dale --version
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
model = ~/models/dale-0.8b-1.0.0-Q4_K_M.gguf
# inference threads (1-256)
threads = 4
```

## How it works

```
request ─► system context ─► local model ─► risk verifier ─► mode ─► show / run
           (os, distro,      (proposes one   (deterministic           (bash -c)
            package manager,  bash command,   rules in C++,
            installed tools)  refuses or      independent of
                              says unsure)    the model)
```

1. **Context collector.** It reads the OS, the distribution, the package manager and the optional tools found in `PATH`. This information never leaves the machine.
2. **Inference engine.** llama.cpp is linked into the binary through `llama.h`, with no server, daemon or port. It decodes greedily, caps the output in tokens and stops as soon as the JSON object is closed.
3. **Risk verifier.** A shell lexer splits the command into simple commands, and a table of C++ rules assigns each one a level. The highest level wins. This is the most critical piece of the project and the one with the most tests.
4. **Executor.** It runs the approved command with `bash -c`. Without a TTY it runs nothing.
5. **Model download.** It uses the system's `curl` or `wget`, verifies the SHA-256 and renames the file atomically.

The model answers with one JSON object: `{"type":"command|refuse|unsure","command":"...","explanation":"..."}`. Anything else is rejected as invalid, and nothing is run.

## The model

[`manuelgscom/dale-0.8b-GGUF`](https://huggingface.co/manuelgscom/dale-0.8b-GGUF) is a full fine-tune of Qwen3.5-0.8B, quantized to Q4_K_M (529 MB). On the public test set [`manuelgscom/dale-bash-test-v2`](https://huggingface.co/datasets/manuelgscom/dale-bash-test-v2), with 471 cases, it gets **79.4 %** right. Qwen3.5-9B and Gemma 4 E4B-it without fine-tuning get 62.8 %. A request takes about 1.7 s (median) on a mid-range 2020 laptop CPU with 4 cores and 8 threads, with no GPU.

The [model card](https://huggingface.co/manuelgscom/dale-0.8b-GGUF) describes the evaluation, the prompt contract and how to use the model directly with llama.cpp.

## Build from source

Use this if you want to read or change the code. For everyday use, the [prebuilt binary](#install) is simpler. llama.cpp is compiled along with `dale`, so there are no other libraries to install.

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

The binary is optimised for the CPU of the machine where you build it. From here on, everything works as in [Install](#install): if `dale` is not found, see [step 3](#3-check-that-your-shell-finds-it); the first run downloads the model.

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
  config/     ~/.config/dale/config parser
  context/    system context collector
  infer/      prompt, llama.cpp engine, JSON parser
  risk/       shell lexer and risk rules (plus the dale_risk helper)
  exec/       bash -c executor, clipboard
  fetch/      model download and SHA-256
tests/        doctest unit tests, risk lists, golden prompt files, e2e tests in a pty
third_party/  llama.cpp (submodule), doctest
```

## Limitations

- bash on Linux only. No zsh, fish, PowerShell, cmd or macOS for now.
- The model can be wrong, in about 1 of 5 cases on the test set, and a wrong command can look plausible. **Always read the command before running it.**
- `dale` is not a security tool. Its safety comes from the deterministic verifier and the confirmations, not from the model.

## License

The code is under the [MIT](LICENSE) license. The model is under Apache-2.0, the same license as its base model. The release binary also contains llama.cpp/ggml and nlohmann/json (MIT) and the GCC runtime (under the GCC Runtime Library Exception); their notices are in [THIRD_PARTY_NOTICES](THIRD_PARTY_NOTICES), along with doctest (MIT), which only the tests use.

---

Author: [Manuel García Sanz](https://manuelgs.com)
