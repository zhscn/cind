# cind

cind is a C++ editor core with incremental C/C++ syntax analysis, structural editing,
Emacs-style commands and shared terminal and graphical frontends. The graphical frontend uses
SDL3 for the platform window and input, and Skia for self-drawn UI rendering.

## Requirements

The project requires CMake 3.28 or newer, Ninja or another CMake-supported build tool, and a C++23
compiler.

On Fedora, install the development dependencies with:

```sh
sudo dnf install \
  cmake ninja-build gcc gcc-c++ make autoconf automake libtool gperf gettext-devel texinfo \
  pkgconf-pkg-config ripgrep gc-devel libffi-devel libunistring-devel gmp-devel \
  libtool-ltdl-devel libxcrypt-devel utf8proc-devel libuv-devel SDL3-devel \
  libepoxy-devel freetype-devel fontconfig-devel \
  libicu-devel libjpeg-turbo-devel libpng-devel harfbuzz-devel
```

Skia and its WebKit-maintained CMake build files are included in `third_party/skia`. Building the
GUI does not require a separate Skia checkout, GN, depot_tools, or an external source directory.
CMake builds a project-private Guile runtime and Fibers dependency, and downloads doctest when
tests are enabled. Their initial builds require network access. Ares Scheme sources are included in
`third_party/guile-ares-rs`.

C++ semantic completion uses `clangd` from `PATH`. Local word and path completion remain available
when no language server is installed.

On macOS with Homebrew, install the native dependencies with:

```sh
brew install cmake ninja pkgconf autoconf automake libtool gperf gettext texinfo \
  bdw-gc libffi libunistring gmp utf8proc libuv sdl3 freetype fontconfig \
  icu4c@78 jpeg-turbo libpng harfbuzz
```

## Build the GUI

Configure and build with standard CMake commands:

```sh
cmake -S . -B build-gui -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCIND_BUILD_GUI=ON
cmake --build build-gui -j
```

Homebrew installs ICU as a keg-only dependency. On macOS, add its prefix while configuring:

```sh
cmake -S . -B build-gui -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCIND_BUILD_GUI=ON \
  -DCMAKE_PREFIX_PATH="$(brew --prefix icu4c@78)"
```

The first GUI build compiles the vendored Skia library and can take longer than subsequent builds.

Open a file in the graphical editor:

```sh
./build-gui/src/cind-gui path/to/file.cpp
```

An optional `+LINE` argument selects the initial line. The GUI asks Fontconfig for the generic
`monospace` family by default. Font family, size and smoothing can be overridden:

```sh
./build-gui/src/cind-gui \
  --font-family Iosevka \
  --font-size 16 \
  --font-smoothing smooth \
  +120 path/to/file.cpp
```

Use `--inspect` to expose the GUI inspector for debugging self-drawn UI state. See
[GUI inspector](docs/gui-inspector.md) for its commands and socket protocol.

## Configuration

The graphical and terminal editors load `cind/init.scm` from `XDG_CONFIG_HOME`, or from
`~/.config/cind/init.scm` when `XDG_CONFIG_HOME` is unset. The file runs in an isolated Guile
module with `host`, `(cind host)`, `(cind command)`, `(cind input)`, and `(cind minibuffer)`
available:

```scheme
(define-command! host "user.hello"
  (lambda (context invocation)
    (set-message! host "hello from Scheme")
    (command-completed))
  #f)

(bind-key! host 'editor.default "C-c h" 'user.hello)
```

Definitions made by the file are installed as one transaction. A condition aborts its registry
changes and is reported in the editor message area and scripting inspector state.

File mode and project discovery policy are also Scheme data. `define-file-mode-rule!` associates
suffixes or exact filenames with a major mode, while `define-project-provider!` declares ancestor
markers. Later rules have precedence and participate in the same extension transaction.

The `C-h` prefix opens editor self-description commands. `C-h k` describes the command resolved by
a key sequence, `C-h x` describes a command, `C-h b` lists active bindings, `C-h m` describes the
current mode policy, and `C-h f`/`C-h v` describe Scheme functions and variables. Descriptions use a
read-only `*Help*` buffer in both graphical and terminal frontends.

`M-:` evaluates a Scheme expression in the application's persistent user module. The command
palette also exposes `scheme.eval-region` and `scheme.eval-buffer`; definitions made by any of the
three commands remain available to later evaluations and to `describe-function` /
`describe-variable`. Multiline output is shown in the reusable `*Scheme Evaluation*` buffer.
Scheme source files use `scheme-mode`, with `C-c C-e`, `C-c C-r`, and `C-c C-b` bound to expression,
region, and buffer evaluation.

`M-x scheme.ares-start` starts an Ares nREPL endpoint on localhost and writes `.nrepl-port` in the
active project root. `scheme.ares-status` reports its address, and `scheme.ares-stop` closes the
listener and active scheduler. The endpoint uses the same application user module as `M-:`, so
definitions are visible through both evaluation paths. Closing the editor stops the endpoint and
removes its port file.

## Build without the GUI

The default build produces the terminal editor and command-line analysis tools without compiling
Skia or SDL support:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Open a file in the terminal editor with:

```sh
./build/src/indent-core edit path/to/file.cpp
```

Run `./build/src/indent-core` without arguments to list the available syntax, indentation, fixture
and benchmark commands.

## Tests

Build and run the configured test suite with:

```sh
cmake --build build-gui -j
ctest --test-dir build-gui --output-on-failure
```

Use the non-GUI `build` directory instead to run only the frontend-independent and terminal test
suite. Replace `Release` with `Debug` in the configure command when debug symbols and assertions are
preferred.

## Design documentation

- [GUI architecture](docs/gui-architecture.md)
- [GUI inspector](docs/gui-inspector.md)
- [Command and interaction architecture](docs/command-loop.md)
- [Guile scripting architecture](docs/scripting.md)
- [Asynchronous runtime](docs/async-runtime.md)
- [LSP sessions and semantic completion](docs/lsp.md)
- [Diagnostics](docs/diagnostics.md)
- [Workspace edits](docs/workspace-edits.md)
- [Projects](docs/projects.md)
- [Workbenches](docs/workbenches.md)
- [Location lists](docs/location-lists.md)
