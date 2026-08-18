# Clang-Tidy static analysis

SpareNode uses Clang-Tidy 18 as the reference static analyzer. The analysis is
enabled only for first-party application, library, and test targets; generated
files and third-party dependencies are outside its scope.

## Run locally

Use a dedicated build directory so enabling Clang-Tidy does not affect a normal
development build:

```bash
cmake -S . -B build/analysis \
    -DCMAKE_CXX_COMPILER=clang++-18 \
    -DCMAKE_BUILD_TYPE=Debug \
    -DSPARENODE_ENABLE_CLANG_TIDY=ON
cmake --build build/analysis --parallel
ctest --test-dir build/analysis --output-on-failure
```

The CMake integration prefers `clang-tidy-18` and falls back to `clang-tidy`
when the versioned executable is unavailable. CI installs Clang-Tidy 18
explicitly and treats every configured diagnostic as an error.

After changing `.clang-tidy`, clean the analysis build before rebuilding. CMake
does not automatically recompile every translation unit when only the analyzer
configuration changes:

```bash
cmake --build build/analysis --target clean
cmake --build build/analysis --parallel
```

## Readability and complexity policy

The project enables the broad `bugprone-*` family, including
`bugprone-easily-swappable-parameters`, and selected high-signal readability
checks. Explicit options keep the policy stable across analyzer upgrades.

| Check | Limit or convention | Rationale |
| --- | --- | --- |
| Function lines | 80 | Keeps implementation units small enough to review as one operation. |
| Function statements | 80 | Flags large implementations without penalising cohesive Catch2 helpers whose macros expand into several statements. |
| Function branches | 10 | Encourages branching and retry policies to be expressed through focused helpers. |
| Function nesting | 4 levels | Prevents deeply nested control flow. |
| Function parameters | 5 | Encourages related dependencies and state to be grouped into a meaningful context object. |
| Local variables | 20 | Detects functions carrying too much state while accommodating test setup code. |
| Swappable parameter run | 2 | Reports adjacent parameters that can be accidentally exchanged at a call site. |
| Implicit conversions | Modelled | Treats implicitly convertible parameter types as potentially swappable. |
| Mixed qualifiers | Not modelled | Avoids low-signal findings based only on different cv-qualifiers. |
| Parameters used together | Suppressed | Avoids warnings when the implementation demonstrates that parameters form a coupled operation. |
| Types | `CamelCase` | Applies to classes, structures, enumerations, aliases, and template parameters. |
| Functions and data | `lower_case` | Applies to functions, methods, variables, parameters, namespaces, and enumeration constants. |
| Private members | `lower_case_` | The trailing underscore distinguishes object state from local variables and parameters. |

The limits are review triggers rather than targets. Code should remain smaller
when a natural decomposition improves ownership, error reporting, or tests.

## Suppression policy

Fix a diagnostic when the change improves production code. A suppression is
acceptable only for an intentional construct or a demonstrated false positive.
It must use the narrow check name, for example
`NOLINT(bugprone-easily-swappable-parameters)`, and an adjacent comment must
explain why the construct is safe.

Do not use wildcard, category-wide, file-wide, or directory-wide suppressions.
Do not suppress findings in generated or third-party code; keep those targets
outside the first-party analysis integration instead.

## Updating the policy

When upgrading Clang-Tidy or enabling another check:

1. Run a clean analysis build over every first-party target.
2. Classify each finding as a defect, a worthwhile refactor, or a false positive.
3. Fix defects and useful refactors; add only narrow, documented suppressions.
4. Record new options and their rationale in this guide.
5. Run formatting, Clang-Tidy, Cppcheck, Doxygen, all supported builds, and tests.

The CI `Static analysis` job runs the same CMake integration, and its result is
part of the required aggregate `Quality gate`.
