# Doxygen documentation

SpareNode uses Doxygen comments for project-owned C++ code under `include`,
`src`, and `apps`. Documentation generation treats warnings as errors, so a
parameter name that no longer matches its declaration or a missing contract is
reported before merge.

## Comment style

Use `///` comments with explicit Doxygen commands:

```cpp
/// @brief Waits until a socket reaches the requested readiness state.
///
/// @param[in] context Stable resources used by the wait.
/// @param[in] request Readiness interest and associated operation.
/// @param[in] stop_token Token that can cancel the blocking wait.
/// @return The readiness state, cancellation, or a structured network error.
/// @note The referenced resources must outlive the call.
[[nodiscard]] Result<SocketWaitStatus, NetworkError>
wait_for_socket(SocketWaitContext context, SocketWaitRequest request,
                const std::stop_token &stop_token);
```

Document every parameter with `@param[in]`, `@param[out]`, or
`@param[in,out]`. Use `@tparam` for every template parameter and `@return` for
non-void functions. Add `@note`, `@warning`, `@pre`, and `@post` only when they
describe a meaningful ownership, lifetime, concurrency, or behavioural
contract. Functions that intentionally propagate exceptions must use `@throws`.

Comments should explain why a declaration exists and how to use it safely. Do
not merely restate its name or implementation.

## Generate locally

Doxygen and CMake 3.25 or newer are required. Configure a dedicated build and
build the documentation target:

```sh
cmake -S . -B build/docs -DBUILD_TESTING=OFF -DSPARENODE_BUILD_DOCUMENTATION=ON
cmake --build build/docs --target sparenode_docs
```

Open `build/docs/doxygen/html/index.html` in a browser. Generated output stays
inside the ignored build directory and must not be committed.

The same target runs in continuous integration. Any Doxygen warning from
project-owned production code fails the documentation job.
