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

## License

SpareNode is available under the [MIT License](LICENSE).
