# Contributing to liboffs

## Development Setup

```bash
git clone https://github.com/Prometheus-SCN/liboffs.git
cd liboffs
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
cmake --build . --target testliboffs -j$(nproc)
./test/testliboffs
```

## Coding Conventions

See [docs/STYLE_GUIDE.md](docs/STYLE_GUIDE.md) for the full style guide. Key rules:

- **2-space indentation**, Egyptian braces.
- Types: `snake_case_t` suffix. Functions: `module_action()`.
- Reference-counted structs have `refcounter_t` as the first member.
- Memory: `get_memory()` / `get_clear_memory()` (abort on OOM).
- No `TODO` / `FIXME` / `HACK` / `XXX` comments in committed code.

## Testing

- Tests are in `test/` (GoogleTest, C++17).
- 842 tests across 180 test suites.
- Run: `cd build && ./test/testliboffs`
- ASAN: `cmake .. -DOFFS_ENABLE_ASAN=ON` + rebuild.
- Valgrind: `cmake .. -DCMAKE_C_FLAGS="-gdwarf-4"` + `valgrind --leak-check=full ./test/testliboffs`.

## Commit Conventions

- Conventional commits: `feat:`, `fix:`, `docs:`, `test:`, `refactor:`, `chore:`.
- Keep commits focused and atomic.
- Do NOT add `Co-Authored-By` lines.

## Pull Requests

1. Fork the repo.
2. Create a feature branch.
3. Make your changes + add tests.
4. Ensure all tests pass.
5. Open a PR with a clear description.
