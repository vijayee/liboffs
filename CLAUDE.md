# Liboffs Project

Always read and follow the coding conventions in [STYLE_GUIDE.md](./docs/STYLE_GUIDE.md) when writing or modifying C code in this project. Use harmony when available
to keep track of plans and phases in the form of epics and tickets. Close the tickets when done.
Use the de-wonk skill when completing an implementation and always check your tests for memory leaks after completing an implementation.
## Git Commit Conventions

- **Do NOT add "Co-Authored-By" lines to commit messages.** All commits should have only the author's information.
- Use clear, descriptive commit messages following conventional commit format (e.g., "feat:", "fix:", "docs:", "test:", "refactor:", "chore:")
- Keep commits focused and atomic - one logical change per commit

## Key Patterns
- Reference-counted structs have `refcounter_t refcounter` as the first member
- Types use `_t` suffix, functions follow `type_action()` naming
- Create functions use `get_clear_memory()` and call `refcounter_init()` last
- Destroy functions check count==0 before freeing
- Directories in `src/` are organized by semantic purpose (Memory/, RefCount/, Crypto/, Shamir/, EAuth/)
