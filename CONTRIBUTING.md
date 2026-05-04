# Contributing to auralog-cpp

This repo is the **C++ SDK** only. For issues with the Auralog service itself, head to [auralog.ai](https://auralog.ai) or [docs.auralog.ai](https://docs.auralog.ai).

## Development Setup

Requirements:

- C++17 compiler
- CMake 3.20+
- libcurl

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

On machines without CMake:

```bash
scripts/test-local.sh
```

## Commit Messages

We follow [Conventional Commits](https://www.conventionalcommits.org/).

## License

By contributing, you agree that your contributions will be licensed under the [MIT License](./LICENSE).

