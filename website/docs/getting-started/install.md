---
sidebar_position: 1
---

# Installation

The compiler is `mincc`. It is built from source today; there are no binary
releases yet.

## What you need

| | |
| --- | --- |
| **CMake** | 3.28 or newer |
| **Ninja** | any version |
| **A C++20 compiler** | Clang, GCC or MSVC |
| **LLVM** | development libraries (`llvm-dev`, `llvm` via Homebrew, or the official Windows installer) |
| **GoogleTest** | found automatically, or downloaded by CMake |
| **A C toolchain** | `clang`, `cc` or `gcc` — `mincc build` invokes one to link |

`ccache` is used when it is installed, and makes a rebuild after a small edit
close to free.

## Build

```sh
git clone <the repository>
cd mincplus

cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

Four presets are defined; the names are the whole interface:

| Preset | What it is |
| --- | --- |
| `dev` | `Debug`, no sanitizers — the everyday build |
| `release` | optimized, for measuring and shipping |
| `ci` | `dev` plus `-Werror` — the warnings CI fails on |
| `sanitize` | AddressSanitizer and UndefinedBehaviorSanitizer |

The `Makefile` is a shortcut over those presets and nothing else:

```sh
make quick      # build, test and format-check — the inner loop
make gates      # everything CI runs, before a commit
```

`make quick` uses one preset, so it never pays for the sanitizer build or the
static analysis. `make gates` runs `ci`, `sanitize`, `format-check` and `tidy` in
the order CI runs them.

## Check it works

```console
$ ./build/dev/src/driver/mincc --version
mincc 0.1.0

$ ./build/dev/src/driver/mincc -vV
mincc 0.1.0
binary:         mincc
host:           x86_64-unknown-linux-gnu
default target: x86_64-unknown-linux-gnu
LLVM:           22.1.8
```

The exact version, triple and LLVM version depend on your checkout and your
machine. `-vV` is the block a bug report needs; `--version` is just the version.

The **default target is the host**: `mincc build hello.mx` links and runs on the
machine the compiler was built for, and `--target` is how a cross build is
spelled.

## Platform support

The compiler builds and runs on **Linux, macOS and Windows**, with Clang, GCC or
MSVC. Host support and the code `mincc` emits are separate axes:

| Axis | Support |
| --- | --- |
| Hosts (where `mincc` runs) | Linux, macOS, Windows |
| Toolchains | Clang, GCC, MSVC (C++20) |
| Emitted code | any LLVM triple; System V AMD64 and Windows x64 are the two the backend is exercised against |

Because the emitted code is LLVM's, adding a target is a triple and a data
layout rather than a code path written by hand — and the front end does not
change when one is added.

## Building this site

The documentation is a [Docusaurus](https://docusaurus.io) site in `website/`:

```sh
cd website
npm install
npm start          # a dev server with hot reload
npm run build      # static output in website/build
```
