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
| **LLVM** | development libraries, 22 or newer |
| **GoogleTest** | found automatically, or downloaded by CMake |
| **A C toolchain** | `clang`, `cc` or `gcc` — `mincc build` invokes one to link |

`ccache` is used when it is installed, and makes a rebuild after a small edit
close to free.

LLVM's *build configuration* is part of its ABI: this compiler is built with RTTI
on, exceptions off and assertions off, and the configure step refuses an LLVM that
disagrees rather than linking against it and hoping. A release package has that
configuration; a local build with `LLVM_ENABLE_ASSERTIONS=ON` does not.

## Installing LLVM

import Tabs from '@theme/Tabs';
import TabItem from '@theme/TabItem';

<Tabs>
<TabItem value="linux" label="Linux">

Install the development package for your distribution:

```sh
# Debian / Ubuntu
sudo apt install llvm-22-dev

# Fedora
sudo dnf install llvm-devel

# Arch
sudo pacman -S llvm
```

If the package lives somewhere CMake does not look by default (e.g. a versioned
path like `/usr/lib/llvm-22`):

```sh
MINC_LLVM_ROOT=/usr/lib/llvm-22 cmake --preset dev
```

</TabItem>
<TabItem value="macos" label="macOS">

Install via Homebrew:

```sh
brew install llvm
```

Homebrew installs to a versioned prefix. Point CMake at it:

```sh
MINC_LLVM_ROOT=$(brew --prefix llvm) cmake --preset dev
```

</TabItem>
<TabItem value="windows" label="Windows">

The project builds with **MSYS2/MinGW-w64** on Windows. The Chocolatey LLVM
package does not include development files (`LLVMConfig.cmake`), so it cannot
be used to build against.

Install MSYS2 from [msys2.org](https://www.msys2.org), then from an MSYS2
MinGW64 shell:

```sh
pacman -S mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja \
          mingw-w64-x86_64-clang mingw-w64-x86_64-llvm
```

From a MinGW64 shell:

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

</TabItem>
</Tabs>

## Build

Once LLVM is installed, the build steps are the same everywhere:

```sh
git clone https://github.com/CarlosDlw/mincplus.git
cd mincplus

cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

Or, using the `Makefile` shortcut:

```sh
make quick      # build, test and format-check — the inner loop
make gates      # everything CI runs, before a commit
```

`make quick` uses one preset, so it never pays for the sanitizer build or the
static analysis. `make gates` runs `ci`, `sanitize`, `format-check` and `tidy` in
the order CI runs them.

Four presets are defined:

| Preset | What it is |
| --- | --- |
| `dev` | `Debug`, no sanitizers — the everyday build |
| `release` | optimized, for measuring and shipping |
| `ci` | `dev` plus `-Werror` — the warnings CI fails on |
| `sanitize` | AddressSanitizer and UndefinedBehaviorSanitizer |

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
