# poll-dancer: from filesystem dependency to git submodule

## Problem

`CMakeLists.txt` references `${CMAKE_CURRENT_SOURCE_DIR}/../poll-dancer/build/libpoll_dancer.a`
as a relative filesystem path. The user is required to:

1. Clone `vijayee/poll-dancer` into the parent directory of `liboffs` (or `OFFS`).
2. Manually `cd ../poll-dancer && cmake -B build && make` before building liboffs.
3. Have a compatible poll-dancer version — there is no version tracking, so any change
   to poll-dancer is silently picked up on the next `make`.

The fragility is documented as Blocker #10 in `docs/PRODUCTION_BLOCKERS.md`:

> `CMakeLists.txt` references `${CMAKE_CURRENT_SOURCE_DIR}/../poll-dancer/build/libpoll_dancer.a`
> as a relative path. This breaks if the build directory is outside the source tree
> or poll-dancer is installed elsewhere.

OFFS has the same dependency pattern. It already has `deps/poll-dancer` as a git
submodule, but its CMake still uses the same fragile path. liboffs does not have
poll-dancer as a submodule at all.

## Goal

Replace the `../poll-dancer` filesystem dependency with a proper git submodule at
`deps/poll-dancer`, built automatically as part of the parent's CMake. This makes
the build reproducible and version-pinned across both repos.

## Approach

### 1. Add `deps/poll-dancer` as a git submodule

In liboffs (currently no submodule at all):
```
git submodule add https://github.com/vijayee/poll-dancer.git deps/poll-dancer
```

In OFFS (already a submodule, no action needed beyond updating the pinned commit if
necessary): keep the existing `deps/poll-dancer` layout. OFFS's submodule pointer is
already at `4ae092a` (the `pd_timer_destroy` fix) as of the most recent commit.

Pin both repos to commit `4ae092a` (the `pd_timer_destroy` watcher-before-close fix
just landed) so the build is reproducible.

### 2. Build poll-dancer via `ExternalProject_Add`

Use the same pattern that liboffs already uses for `deps/libcbor`. In
`CMakeLists.txt`:

```cmake
include(ExternalProject)
set(POLL_DANCER_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/deps/poll-dancer)
set(POLL_DANCER_BUILD_PATH ${CMAKE_BINARY_DIR}/deps/poll-dancer)
set(POLL_DANCER_CONFIGURE cd ${POLL_DANCER_ROOT} && cmake -B${POLL_DANCER_BUILD_PATH} -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF)
set(POLL_DANCER_MAKE cd ${POLL_DANCER_BUILD_PATH} && make poll_dancer)
ExternalProject_Add(poll-dancer
  SOURCE_DIR ${POLL_DANCER_ROOT}
  CONFIGURE_COMMAND ${POLL_DANCER_CONFIGURE}
  BUILD_COMMAND ${POLL_DANCER_MAKE}
  BUILD_BYPRODUCTS ${POLL_DANCER_BUILD_PATH}/libpoll_dancer.a
  BINARY_DIR ${POLL_DANCER_BUILD_PATH}
)
add_library(poll-dancer STATIC IMPORTED)
set_target_properties(poll-dancer PROPERTIES
  IMPORTED_LOCATION ${POLL_DANCER_BUILD_PATH}/libpoll_dancer.a
  INTERFACE_INCLUDE_DIRECTORIES "${POLL_DANCER_ROOT}/include;${POLL_DANCER_ROOT}/src")
add_dependencies(poll-dancer poll-dancer-project)
```

`BUILD_TESTING=OFF` keeps poll-dancer's gtest-based test sources out of the parent's
build graph (poll-dancer's `CMakeLists.txt` `find_package(GTest)` would otherwise try
to pull gtest into a context where it is not provided).

Replace the existing include/link lines with:
```cmake
target_link_libraries(offs PRIVATE poll-dancer)
```

OFFS applies the same pattern to its `offsd` and `offs-metrics` targets.

### 3. Submodule initialization

Document the new clone flow in `README.md`:
```
git clone --recurse-submodules https://github.com/Prometheus-SCN/liboffs.git
```
(For people who cloned without `--recurse-submodules`, the existing
`git submodule update --init --recursive` will pick it up.)

### 4. Update PRODUCTION_BLOCKERS

Remove Blocker #10 ("Fragile build paths") from `docs/PRODUCTION_BLOCKERS.md` since
this design resolves it. Renumber the subsequent blockers (#11 → #10, #12 → #11).

## What stays the same

- poll-dancer's own repo (`vijayee/poll-dancer`) is unchanged. No fork, no private
  mirror.
- The `POLL_DANCER_ROOT` CMake cache variable is preserved for advanced users who
  want a custom location (e.g. system-installed poll-dancer).
- The pre-existing `OFFS_MAX_BUFFERED_BODY_SIZE` build error in OFFS (caused by a
  stale `deps/liboffs` submodule) is unrelated to this change and is not addressed.

## Why ExternalProject_Add (not add_subdirectory)

| Option | Pros | Cons |
|--------|------|------|
| `ExternalProject_Add` (chosen) | Reuses existing libcbor pattern. poll-dancer's tests and gtest dependency stay isolated. | poll-dancer builds in its own tree, not in-tree. |
| `add_subdirectory(deps/poll-dancer)` | Reuses poll-dancer's CMakeLists unchanged. | poll-dancer's `CMakeLists.txt` defines `gtest` and `gtest_main` targets at top level. liboffs already provides `gtest` via `add_subdirectory(deps/googletest)`. The two would collide. Avoiding the collision would require teaching poll-dancer to suppress its own gtest target when included as a subproject — a much larger change. |
| Just check `libpoll_dancer.a` exists at configure time | Smallest diff. | User still has to manually build poll-dancer first. Does not fix the fragility. |

## Risks

- **Clone size**: poll-dancer is ~5MB and builds in ~1s. Negligible.
- **Existing developer workflow**: anyone who currently has a sibling `../poll-dancer`
  directory will now get an unexpected `deps/poll-dancer` checkout. They can either
  delete their sibling copy or override `POLL_DANCER_ROOT` via
  `-DPOLL_DANCER_ROOT=/path/to/their/sibling`.
- **CI**: any CI script that pre-builds poll-dancer to `../poll-dancer/build` will
  stop being necessary. The new build does it inline. This is a net simplification.

## Scope

Two commits, one per repo:

1. `liboffs`: add submodule + `CMakeLists.txt` change + `README.md` +
   `PRODUCTION_BLOCKERS.md` update.
2. `OFFS`: `CMakeLists.txt` change (submodule already exists, may need pin update).
