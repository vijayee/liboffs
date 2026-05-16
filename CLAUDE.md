# Liboffs Project

Always read and follow the coding conventions in [STYLE_GUIDE.md](./docs/STYLE_GUIDE.md) when writing or modifying C code in this project. Use harmony when available
to keep track of plans and phases in the form of epics and tickets. Close the tickets when done.
Use the de-wonk skill when completing an implementation and always check your tests for memory leaks after completing an implementation.
## No TODOs in Completed Work

Never leave a TODO/FIXME/HACK/XXX comment in code and mark a task as completed. Every TODO represents unfinished work. When completing a task:
- Implement the code the TODO describes, or
- If it requires design decisions beyond scope, escalate to the user rather than leaving it
- A task is not done until every TODO in the files it touched is resolved
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
