# poll-dancer Submodule Migration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the fragile `../poll-dancer/build/libpoll_dancer.a` filesystem dependency in liboffs and OFFS with a proper git submodule at `deps/poll-dancer`, built automatically as part of the parent's CMake.

**Architecture:** Add `vijayee/poll-dancer` as a git submodule at `deps/poll-dancer` in liboffs. Build it via `ExternalProject_Add` (same pattern as `deps/libcbor`) and import the resulting `libpoll_dancer.a` as a static IMPORTED target. Expose that target to consumers via the `offs` library. OFFS, which already does `add_subdirectory(deps/liboffs)`, will then link against `poll-dancer` directly. Both repos' CMake becomes version-pinned and reproducible.

**Tech Stack:** C (C11), CMake 3.22+, git submodules, CMake's `ExternalProject_Add`.

---

## File Structure

| File | Responsibility |
|------|----------------|
| `.gitmodules` | Register the new `deps/poll-dancer` submodule |
| `deps/poll-dancer` (new submodule) | Pinned commit of `vijayee/poll-dancer` (commit `4ae092a`) |
| `CMakeLists.txt` | Replace `${POLL_DANCER_ROOT}/build/libpoll_dancer.a` reference with `ExternalProject_Add` + IMPORTED library named `poll-dancer`; remove obsolete include directives |
| `README.md` | Document the new `--recurse-submodules` clone flow |
| `docs/PRODUCTION_BLOCKERS.md` | Remove Blocker #10; renumber |
| `OFFS/CMakeLists.txt` | Replace `${POLL_DANCER_ROOT}/build/libpoll_dancer.a` with `poll-dancer` imported target |

---

### Task 1: Add poll-dancer as a git submodule in liboffs

**Files:**
- Create: `.gitmodules` (entry for `deps/poll-dancer`)
- Create: `deps/poll-dancer` (gitlink to `vijayee/poll-dancer`)

- [ ] **Step 1: Verify the upstream commit exists and is what we expect**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/liboffs
git ls-remote https://github.com/vijayee/poll-dancer.git refs/heads/master
# Expected: a sha. Then verify it matches the local poll-dancer HEAD.
cd /home/victor/Workspace/src/github.com/vijayee/poll-dancer
git rev-parse HEAD
# Expected: 4ae092af9bbac38bb47ab25eb9a6f88dafc5c4ec
```

- [ ] **Step 2: Move the existing sibling poll-dancer out of the way**

The current build expects `../poll-dancer/build/libpoll_dancer.a`. To avoid conflict
with the new `deps/poll-dancer` checkout, rename the sibling for the duration of
this work:

```bash
mv /home/victor/Workspace/src/github.com/vijayee/poll-dancer /home/victor/Workspace/src/github.com/vijayee/poll-dancer.bak
```

(If you prefer to keep the sibling copy, you can skip this step and pass
`-DPOLL_DANCER_ROOT=...` to override later, but moving it makes the migration
unambiguous.)

- [ ] **Step 3: Add the submodule at the pinned commit**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/liboffs
git submodule add https://github.com/vijayee/poll-dancer.git deps/poll-dancer
cd deps/poll-dancer
git checkout 4ae092af9bbac38bb47ab25eb9a6f88dafc5c4ec
cd ..
git add deps/poll-dancer
git status
```

Expected: `deps/poll-dancer` listed as a new file in "Changes to be committed" with
mode `160000` (gitlink).

- [ ] **Step 4: Verify `.gitmodules` was updated**

```bash
cat .gitmodules
```

Expected: contains a new entry similar to:
```
[submodule "deps/poll-dancer"]
	path = deps/poll-dancer
	url = https://github.com/vijayee/poll-dancer.git
```

- [ ] **Step 5: Verify the submodule checkout is the pinned commit**

```bash
cd deps/poll-dancer
git log --oneline -1
# Expected: 4ae092a fix: destroy watcher before platform close in pd_timer_destroy
```

- [ ] **Step 6: Commit**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/liboffs
git add .gitmodules deps/poll-dancer
git commit -m "build: add poll-dancer as deps submodule pinned to 4ae092a"
```

---

### Task 2: Build poll-dancer via ExternalProject_Add in liboffs

**Files:**
- Modify: `CMakeLists.txt` (replace the `${POLL_DANCER_ROOT}` direct-path setup with an
  `ExternalProject_Add` + IMPORTED library)

- [ ] **Step 1: Read the current CMakeLists.txt to confirm the exact lines to replace**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/liboffs
sed -n '1,75p' CMakeLists.txt
```

Expected: confirms lines 6, 63, 64, 67 reference `${POLL_DANCER_ROOT}` and that
`include(ExternalProject)` is already used (line 22 for libcbor).

- [ ] **Step 2: Replace the POLL_DANCER_ROOT line with the new setup**

Edit `CMakeLists.txt`. Replace the existing line 6:

```cmake
set(POLL_DANCER_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../poll-dancer CACHE PATH "Path to poll-dancer library")
```

with:

```cmake
# poll-dancer — built as an external project, same pattern as libcbor above.
# Pinned via the deps/poll-dancer git submodule (commit recorded in .gitmodules).
set(POLL_DANCER_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/deps/poll-dancer CACHE PATH "Path to poll-dancer source")
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

- [ ] **Step 3: Remove the now-redundant include and link lines**

Edit `CMakeLists.txt`. Delete lines 63 and 64 (the two `target_include_directories(offs
PRIVATE ${POLL_DANCER_ROOT}/...)` lines) and replace line 67 (`target_link_libraries(offs
PRIVATE ${POLL_DANCER_ROOT}/build/libpoll_dancer.a)`) with:

```cmake
target_link_libraries(offs PRIVATE poll-dancer)
```

Final state of the relevant block in `CMakeLists.txt` should be:

```cmake
target_include_directories(offs PRIVATE ${xxHash_SOURCE_DIR})
target_include_directories(offs PRIVATE ${hashmap_SOURCE_DIR}/include)
target_include_directories(offs PUBLIC ${LIBCBOR_BUILD_PATH}/include)
target_include_directories(offs PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/deps/cJSON)
target_link_libraries(offs PRIVATE poll-dancer)
target_link_libraries(offs PRIVATE http-parser)
target_link_libraries(offs PRIVATE cjson)
target_link_libraries(offs PRIVATE crypt)
```

- [ ] **Step 4: Verify the CMake configure step succeeds**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/liboffs
rm -rf build-test
mkdir build-test && cd build-test
cmake .. -DCMAKE_BUILD_TYPE=Release 2>&1 | tail -30
```

Expected: CMake configures without error. Look for lines mentioning `poll-dancer`:
```
-- Configuring done
-- Generating done
-- Build files have been written to: .../build-test
```

- [ ] **Step 5: Build and verify the new poll-dancer artifact is produced**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/liboffs/build-test
make -j$(nproc) 2>&1 | tail -20
ls -la deps/poll-dancer/libpoll_dancer.a
```

Expected: `libpoll_dancer.a` exists in `build-test/deps/poll-dancer/`. The `make` for
`offs` should succeed.

- [ ] **Step 6: Run the integration tests to confirm no regression**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/liboffs/build-test
./test/test_offs_client_integration 2>&1 | tail -10
```

Expected:
```
[==========] 5 tests from 1 test suite ran.
[  PASSED  ] 5 tests.
```

- [ ] **Step 7: Run the existing testliboffs to confirm no regression**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/liboffs/build-test
./test/testliboffs --gtest_filter='TestOffsClient*' 2>&1 | tail -5
```

Expected: most tests pass. (The pre-existing `BlockPutGetRoundTrip` failure
unrelated to this change is acceptable — see spec.)

- [ ] **Step 8: Clean up the test build dir**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/liboffs
rm -rf build-test
```

- [ ] **Step 9: Commit the CMakeLists change**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/liboffs
git add CMakeLists.txt
git commit -m "build: build poll-dancer via ExternalProject_Add

Replaces the fragile ../poll-dancer/build/libpoll_dancer.a filesystem
reference with an ExternalProject_Add that builds poll-dancer in-tree
as part of liboffs's CMake. poll-dancer is now pinned via the
deps/poll-dancer git submodule. The 'poll-dancer' IMPORTED target
replaces the direct .a file reference, exposing its include dirs
via INTERFACE_INCLUDE_DIRECTORIES so consumers (including OFFS via
add_subdirectory(deps/liboffs)) get the headers automatically.

Resolves PRODUCTION_BLOCKERS #10 (fragile build paths)."
```

---

### Task 3: Update liboffs README to document the new clone flow

**Files:**
- Modify: `README.md` (the "Building" section)

- [ ] **Step 1: Read the current Building section**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/liboffs
sed -n '/## Building/,/## Tools/p' README.md
```

- [ ] **Step 2: Add the clone line above the build commands**

Edit `README.md`. In the `## Building` section, prepend:

```bash
git clone --recurse-submodules https://github.com/Prometheus-SCN/liboffs.git
```

Expected final state of the section:

```markdown
## Building

```bash
git clone --recurse-submodules https://github.com/Prometheus-SCN/liboffs.git
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j$(nproc)
```
```

- [ ] **Step 3: Commit**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/liboffs
git add README.md
git commit -m "docs: document --recurse-submodules for poll-dancer dependency"
```

---

### Task 4: Remove PRODUCTION_BLOCKERS #10

**Files:**
- Modify: `docs/PRODUCTION_BLOCKERS.md`

- [ ] **Step 1: Find the blocker section**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/liboffs
grep -n "10 | Fragile\|### 10\. Fragile\|### 11\.\|### 12\." docs/PRODUCTION_BLOCKERS.md
```

Expected: shows the locations of the #10, #11, and #12 rows in both the table and
section headers.

- [ ] **Step 2: Remove the #10 row from the summary table**

Edit `docs/PRODUCTION_BLOCKERS.md`. In the Critical (Production Blockers) table,
delete the row:

```
| 10 | Fragile build paths | `libpoll_dancer.a` at relative path |
```

- [ ] **Step 3: Renumber the #11 and #12 rows**

Edit `docs/PRODUCTION_BLOCKERS.md`. In the same table, change:

```
| 11 | No configuration validation | Invalid config values pass silently |
| 12 | Bootstrap relies on static peer lists | No DNS seed, DHT, or mDNS discovery |
```

to:

```
| 10 | No configuration validation | Invalid config values pass silently |
| 11 | Bootstrap relies on static peer lists | No DNS seed, DHT, or mDNS discovery |
```

- [ ] **Step 4: Remove the ### 10. Fragile Build Paths section**

Edit `docs/PRODUCTION_BLOCKERS.md`. Delete the section:

```markdown
### 10. Fragile Build Paths

`CMakeLists.txt` references `${CMAKE_CURRENT_SOURCE_DIR}/../poll-dancer/build/libpoll_dancer.a`
as a relative path. This breaks if the build directory is outside the source tree
or poll-dancer is installed elsewhere.
```

- [ ] **Step 5: Renumber the subsequent section headers**

Edit `docs/PRODUCTION_BLOCKERS.md`. Change `### 11. No Configuration Validation` to
`### 10. No Configuration Validation`, and `### 12. Static Peer Bootstrap Only` to
`### 11. Static Peer Bootstrap Only`.

- [ ] **Step 6: Verify the renumbering is consistent**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/liboffs
grep -nE "^\| [0-9]+ \|" docs/PRODUCTION_BLOCKERS.md | head -15
grep -nE "^### [0-9]+\." docs/PRODUCTION_BLOCKERS.md | head -15
```

Expected: 11 rows in the table, 11 section headers, both numbered 1-11 with no gaps.

- [ ] **Step 7: Commit**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/liboffs
git add docs/PRODUCTION_BLOCKERS.md
git commit -m "docs: remove resolved PRODUCTION_BLOCKERS #10 (fragile build paths)"
```

---

### Task 5: Update OFFS CMakeLists.txt to use the imported poll-dancer target

**Files:**
- Modify: `OFFS/CMakeLists.txt` (replace `${POLL_DANCER_ROOT}/build/libpoll_dancer.a`
  references and the redundant `target_include_directories` lines with
  `target_link_libraries(... poll-dancer)`)

- [ ] **Step 1: Locate all references to POLL_DANCER_ROOT in OFFS CMake**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/OFFS
grep -n "POLL_DANCER" CMakeLists.txt
```

Expected: shows lines defining POLL_DANCER_ROOT, including the include paths and
the link line.

- [ ] **Step 2: Update the link line in OFFS**

Edit `OFFS/CMakeLists.txt`. Replace the line:

```cmake
target_link_libraries(offsd PRIVATE ${POLL_DANCER_ROOT}/build/libpoll_dancer.a)
```

with:

```cmake
target_link_libraries(offsd PRIVATE poll-dancer)
```

- [ ] **Step 3: Remove the now-redundant include directives in OFFS**

Edit `OFFS/CMakeLists.txt`. Delete the two `target_include_directories(... ${POLL_DANCER_ROOT}/include)` and `target_include_directories(... ${POLL_DANCER_ROOT}/src)` lines wherever they appear in the OFFS-only targets (offsd and offs-metrics — NOT inside any imported liboffs target).

For each occurrence in OFFS targets, delete the line. The `poll-dancer` IMPORTED
target's `INTERFACE_INCLUDE_DIRECTORIES` will provide these to its consumers.

- [ ] **Step 4: Verify the OFFS cmake configure step succeeds**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/OFFS
rm -rf build-test
mkdir build-test && cd build-test
cmake .. -DCMAKE_BUILD_TYPE=Release 2>&1 | tail -15
```

Expected: CMake configures without error.

- [ ] **Step 5: Note: the pre-existing OFFS build error**

If the build hits the `OFFS_MAX_BUFFERED_BODY_SIZE` undeclared error (caused by a
stale `deps/liboffs` submodule in OFFS, not by this change), stop here and report
it as a separate issue. Do **not** try to fix it as part of this plan — the spec
explicitly excludes it.

- [ ] **Step 6: Clean up the test build dir**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/OFFS
rm -rf build-test
```

- [ ] **Step 7: Commit the OFFS CMakeLists change**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/OFFS
git add CMakeLists.txt
git commit -m "build: link against poll-dancer via the IMPORTED target

The poll-dancer library is now built and exported by liboffs (as a
side effect of the poll-dancer submodule migration in liboffs).
OFFS picks it up via add_subdirectory(deps/liboffs) and links
against the 'poll-dancer' IMPORTED target instead of pointing at
deps/poll-dancer/build/libpoll_dancer.a directly. The IMPORTED
target's INTERFACE_INCLUDE_DIRECTORIES provide the headers
automatically."
```

---

### Task 6: Final verification

- [ ] **Step 1: Run the liboffs test suite**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/liboffs
rm -rf build-final && mkdir build-final && cd build-final
cmake .. -DCMAKE_BUILD_TYPE=Release 2>&1 | tail -3
make -j$(nproc) 2>&1 | tail -3
./test/test_offs_client_integration 2>&1 | tail -3
```

Expected: all 5 integration tests pass, no build errors.

- [ ] **Step 2: Run the poll-dancer tests to confirm the new build path works**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/liboffs/deps/poll-dancer/build
ctest 2>&1 | tail -5
```

Expected: 100% pass (31 tests).

- [ ] **Step 3: Document the migration in a summary commit message (no source change)**

The four commits from Tasks 1-5 are already the implementation. No additional
commit needed. If any of the verifications in this task uncovered an issue, address
it as a follow-up commit.

---

## Self-Review Notes

- **Spec coverage**: All four approach items in the spec (submodule, ExternalProject,
  README, PRODUCTION_BLOCKERS) are covered by Tasks 1-4. The OFFS update is Task 5.
- **Type consistency**: `poll-dancer` (lowercase, hyphen) is the target name in both
  repos. `POLL_DANCER_ROOT` is the cache variable preserved for advanced users.
- **No placeholders**: All steps have actual commands and code blocks.
- **Risks called out in the spec** are not separately addressed because the migration
  is small enough to be one logical change; if the sibling `../poll-dancer` directory
  is preserved (`.bak`), users can recover by removing the submodule and the new
  CMakeLists.txt entries.
