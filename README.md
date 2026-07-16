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
  cmake ninja-build gcc-c++ pkgconf-pkg-config ripgrep \
  utf8proc-devel libuv-devel SDL3-devel libepoxy-devel freetype-devel fontconfig-devel \
  libicu-devel libjpeg-turbo-devel libpng-devel harfbuzz-devel
```

Skia and its WebKit-maintained CMake build files are included in `third_party/skia`. Building the
GUI does not require a separate Skia checkout, GN, depot_tools, or an external source directory.
CMake downloads doctest when tests are enabled, so the initial configure requires network access.

## Build the GUI

Configure and build with standard CMake commands:

```sh
cmake -S . -B build-gui -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCIND_BUILD_GUI=ON
cmake --build build-gui -j
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
- [Asynchronous runtime](docs/async-runtime.md)
- [Projects](docs/projects.md)
