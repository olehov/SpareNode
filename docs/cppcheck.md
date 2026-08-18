# Cppcheck static analysis

SpareNode uses Cppcheck as an additional first-party static analyzer. It
complements compiler warnings and clang-tidy with independent correctness,
performance, portability, and maintainability checks.

## Supported version

The minimum supported upstream version is Cppcheck 2.21.0. CI builds the
official source at commit `e73bf44c3e49686b7495fab352d03a6c6075516b` and
verifies that the executable reports `Cppcheck 2.21.0` before analysis starts.
This version supports the project's explicit `--std=c++23` analysis mode.

## Run locally

Use a package manager when it provides Cppcheck 2.21.0 or newer. Ubuntu 24.04's
standard package is older, so reproduce CI there by building the pinned source:

```sh
set -e
mkdir -p build/tools
git clone --branch 2.21.0 --depth 1 \
  https://github.com/cppcheck-opensource/cppcheck.git build/tools/cppcheck-source
test "$(git -C build/tools/cppcheck-source rev-parse HEAD)" = \
  "e73bf44c3e49686b7495fab352d03a6c6075516b"
cmake -S build/tools/cppcheck-source -B build/tools/cppcheck-build \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_GUI=OFF -DBUILD_TESTING=OFF
cmake --build build/tools/cppcheck-build --target cppcheck --parallel
build/tools/cppcheck-build/bin/cppcheck --version
```

The reported version must be `Cppcheck 2.21.0`. Select this executable during
project configuration with the CMake cache variable shown below.

Configure a dedicated analysis build:

```sh
cmake -S . -B build/cppcheck \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_BUILD_TYPE=Debug \
  -DSPARENODE_ENABLE_CPPCHECK=ON \
  -DSPARENODE_CPPCHECK_EXECUTABLE="$PWD/build/tools/cppcheck-build/bin/cppcheck"
cmake --build build/cppcheck --parallel
```

Cppcheck runs as part of compilation only for the repository-owned
`sparenode_core`, `sparenode`, and `sparenode_tests` targets. Catch2, generated
files, and other third-party targets do not receive the integration. Use a
fresh dedicated build directory so every translation unit is analyzed.

To select a non-default executable, configure with:

```sh
-DSPARENODE_CPPCHECK_EXECUTABLE=/absolute/path/to/cppcheck
```

## Diagnostic policy

The build enables Cppcheck's warning, performance, portability, and style
categories. Configured findings return a non-zero exit code and fail the build.
Inconclusive diagnostics are not enabled because they require a separate
false-positive review before becoming blocking.

Fix a valid finding in the code. Suppress only a confirmed false positive or a
deliberate API decision, using the narrowest inline form immediately before the
reported statement:

```cpp
// Explain why the diagnostic does not represent a defect here.
// cppcheck-suppress diagnosticId
statement();
```

Do not add wildcard, file-wide, or category-wide suppressions merely to make CI
pass. Missing system-header diagnostics are suppressed because Cppcheck does
not model the host standard library; compiler builds remain responsible for
validating actual includes and platform declarations.

## Updating Cppcheck

When upgrading the analyzer:

1. update the minimum version in `cmake/SpareNodeCppcheck.cmake`;
2. update the pinned package and reported version in `.github/workflows/ci.yml`;
3. update the language mode when supported;
4. run a clean local analysis and review every new diagnostic;
5. update this guide with any intentional policy change.
