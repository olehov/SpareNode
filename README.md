# SpareNode

SpareNode is a lightweight, cross-platform file server for securely sharing
selected folders over a local network. It is built with modern C++ and targets
Windows and Linux.

## Project status

SpareNode is in early development. The current repository provides the initial
C++23 project foundation and a minimal `sparenode` executable.

## Repository layout

```text
SpareNode/
|-- apps/       # Executable entry points
|-- src/        # Implementation files
|-- include/    # Public headers
|-- tests/      # Automated tests
|-- web/        # Web client assets
|-- cmake/      # Reusable CMake modules
|-- docs/       # Project documentation
`-- CMakeLists.txt
```

## Requirements

- A C++23-compatible compiler
- CMake 3.25 or newer

Supported toolchains:

- Windows: MSVC
- Linux: GCC or Clang

## Build

### Windows (MSVC)

Run these commands from a Visual Studio Developer PowerShell:

```powershell
cmake -S . -B build/msvc -A x64
cmake --build build/msvc --config Debug --parallel
.\build\msvc\bin\Debug\sparenode.exe
```

Replace `Debug` with `Release` for an optimised build.

### Linux (GCC)

```bash
cmake -S . -B build/gcc -DCMAKE_CXX_COMPILER=g++ -DCMAKE_BUILD_TYPE=Debug
cmake --build build/gcc --parallel
./build/gcc/bin/Debug/sparenode
```

### Linux (Clang)

```bash
cmake -S . -B build/clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Debug
cmake --build build/clang --parallel
./build/clang/bin/Debug/sparenode
```

For a release build on Linux, configure with `-DCMAKE_BUILD_TYPE=Release`.
All generators place executables in `bin/<configuration>` and libraries in
`lib/<configuration>` inside the selected build directory.

## Test

SpareNode uses Catch2 for tests and CTest for test discovery and execution.
Testing is enabled by default through CMake's standard `BUILD_TESTING` option.

Windows (MSVC):

```powershell
cmake --build build/msvc --config Debug --parallel
ctest --test-dir build/msvc -C Debug --output-on-failure
```

Linux (GCC or Clang):

```bash
cmake --build build/gcc --parallel
ctest --test-dir build/gcc --output-on-failure

cmake --build build/clang --parallel
ctest --test-dir build/clang --output-on-failure
```

To configure an application-only build without downloading or building the
test dependency, pass `-DBUILD_TESTING=OFF` when configuring CMake.

The advanced `SPARENODE_ENABLE_FAILURE_PROBE` option registers a deliberately
failing test. It is disabled by default and exists only to verify that CTest
returns a failing status when a test fails.

## Clean

Remove compiled files while keeping the CMake configuration:

Windows (MSVC):

```powershell
cmake --build build/msvc --config Debug --target clean
```

Linux (GCC or Clang):

```bash
cmake --build build/gcc --target clean
cmake --build build/clang --target clean
```

For a completely clean reconfiguration, remove the selected build directory:

```powershell
# Windows
Remove-Item -Recurse -Force build/msvc
```

```bash
# Linux
rm -rf build/gcc build/clang
```

## License

SpareNode is available under the [MIT License](LICENSE).
