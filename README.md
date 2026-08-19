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
- Doxygen 1.9.8 or newer (only when generating API documentation)
- Cppcheck 2.21.0 or newer (only when running Cppcheck analysis)

Supported toolchains:

- Windows: MSVC
- Linux: GCC or Clang

## Build

### Windows (MSVC)

Run these commands from a Visual Studio Developer PowerShell:

```powershell
cmake -S . -B build/msvc -A x64
cmake --build build/msvc --config Debug --parallel
Copy-Item .env.example .env
# Set SPARENODE_SHARED_ROOT in .env, then run:
.\build\msvc\bin\Debug\sparenode.exe
```

Replace `Debug` with `Release` for an optimised build.

### Linux (GCC)

```bash
cmake -S . -B build/gcc -DCMAKE_CXX_COMPILER=g++ -DCMAKE_BUILD_TYPE=Debug
cmake --build build/gcc --parallel
cp .env.example .env
# Set SPARENODE_SHARED_ROOT in .env, then run:
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

## Networking

The cross-platform TCP listener exposes move-only, RAII-managed listener and
connection objects with structured errors. Binding requires an explicit numeric
IPv4 or IPv6 interface address. See the [TCP listener documentation](docs/tcp-listener.md)
for binding and accept behaviour, and [TCP connection I/O](docs/tcp-connection.md)
for bounded receive/send operations, cancellation, and ownership rules.

## Shared directory

SpareNode requires exactly one existing directory through the
`SPARENODE_SHARED_ROOT` key in a local `.env` file. The path is validated and
stored canonically before the server starts. See the
[shared-root documentation](docs/shared-root.md) for the configuration contract
and security boundary.

## Code quality

The repository contains project-wide `.clang-format` and `.clang-tidy`
configurations. CI treats formatting violations and configured static-analysis
findings as errors.

Check formatting locally without modifying files:

```bash
git ls-files -z -- '*.c' '*.cc' '*.cpp' '*.cxx' '*.h' '*.hpp' \
    | xargs -0 clang-format-18 --dry-run --Werror
```

Run static analysis through the build system:

```bash
cmake -S . -B build/analysis \
    -DCMAKE_CXX_COMPILER=clang++-18 \
    -DCMAKE_BUILD_TYPE=Debug \
    -DSPARENODE_ENABLE_CLANG_TIDY=ON
cmake --build build/analysis --parallel
```

Generated and third-party targets are not assigned the clang-tidy integration;
analysis is enabled only for first-party SpareNode targets.

See the [Clang-Tidy guide](docs/clang-tidy.md) for the readability and complexity
limits, naming conventions, suppression policy, and analyzer upgrade procedure.

Run the complementary Cppcheck analysis in a dedicated build:

```bash
cmake -S . -B build/cppcheck \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_BUILD_TYPE=Debug \
    -DSPARENODE_ENABLE_CPPCHECK=ON
cmake --build build/cppcheck --parallel
```

See the [Cppcheck guide](docs/cppcheck.md) for the diagnostic and suppression
policy, supported version, analyzer limitations, and upgrade procedure.

## API documentation

Production C++ interfaces and internal networking helpers use Doxygen contracts
that describe parameters, results, ownership, lifetime, cancellation, and
concurrency where relevant. See the [Doxygen documentation guide](docs/doxygen.md)
for the required comment style and local generation commands.

## Continuous integration

The GitHub Actions CI workflow runs for pushes and pull requests targeting
`main`, and can also be started manually. It provides these checks:

- Windows MSVC build and CTest execution
- Linux GCC build and CTest execution
- Linux Clang build and CTest execution
- clang-format validation
- clang-tidy static analysis
- Cppcheck static analysis
- Doxygen generation with warnings treated as errors
- an aggregate `Quality gate` suitable for branch protection

The workflow has read-only repository permissions and does not publish or
deploy artifacts.

## Automated pull request review

CodeRabbit provides advisory review feedback for new pull requests and updates
its review after subsequent pushes. Repository-specific guidance covers C++23,
resource ownership, filesystem and network security, concurrency, portability,
tests, CMake, and GitHub Actions.

Automated review does not replace human review or the required CI `Quality gate`.
See [Automated pull request review](docs/automated-code-review.md) for the App
installation, branch-protection, configuration, and verification steps.

## Releases

Releases are created from semantic version tags in the form
`vMAJOR.MINOR.PATCH`. The release workflow builds and tests tagged source on
Windows and Linux, verifies the embedded version, creates archives, and only
then publishes a GitHub Release.

Create a release after the target commit has passed CI on `main`:

```bash
git switch main
git pull --ff-only
git tag -a v0.1.0 -m "SpareNode v0.1.0"
git push origin v0.1.0
```

Each release contains:

- `SpareNode-<version>-windows-x64.zip`
- `SpareNode-<version>-linux-x64.tar.gz`

Both archives contain the executable, `README.md`, `LICENSE`, and a `VERSION`
file. Passing `-DSPARENODE_VERSION=MAJOR.MINOR.PATCH` to CMake reproduces the
version embedded in a tagged release build.

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
