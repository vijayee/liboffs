# Move App-Level Code from liboffs to OFFS Repo

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move all app-level code (service management, updater binary, packaging, release scripts, version definition) from liboffs to the OFFS repo. liboffs retains only updater plumbing (`src/Update/`).

**Architecture:** liboffs is a library — it provides `src/Update/` (check, download, stage, actor) as updater plumbing. OFFS is the application that consumes liboffs as a submodule and builds the actual binaries (offsd, offs-cli, offs-updater), packages them, and distributes them. The files being moved have no dependency on liboffs internals — `updater_main.c` is a standalone binary (only links pthread), and `src/Service/` is a self-contained platform abstraction.

**Tech Stack:** C11, CMake, bash scripts, systemd/launchd/WiX packaging

---

## Task 1: Move Service module to OFFS

**Files:**
- Create: `src/Service/service.h` in OFFS
- Create: `src/Service/service_linux.c` in OFFS (with macOS guard fix)
- Create: `src/Service/service_windows.c` in OFFS
- Delete: `src/Service/service.h` from liboffs
- Delete: `src/Service/service_linux.c` from liboffs
- Delete: `src/Service/service_windows.c` from liboffs

- [ ] **Step 1: Create Service directory in OFFS and copy files**

```bash
mkdir -p /home/victor/Workspace/src/github.com/vijayee/OFFS/src/Service
cp /home/victor/Workspace/src/github.com/vijayee/liboffs/src/Service/service.h \
   /home/victor/Workspace/src/github.com/vijayee/OFFS/src/Service/service.h
cp /home/victor/Workspace/src/github.com/vijayee/liboffs/src/Service/service_windows.c \
   /home/victor/Workspace/src/github.com/vijayee/OFFS/src/Service/service_windows.c
```

- [ ] **Step 2: Write service_linux.c to OFFS with macOS guard fix applied**

The working tree change adds `!defined(__APPLE__)` guards. Write the final version:

```c
//
// Created by victor on 5/28/25.
//

#include "service.h"
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32) && !defined(__APPLE__)

static int service_linux_stop(void) {
  int result = system("systemctl stop offs-daemon");
  return (result == 0) ? service_result_ok : service_result_error;
}

static int service_linux_start(void) {
  int result = system("systemctl start offs-daemon");
  return (result == 0) ? service_result_ok : service_result_error;
}

static int service_linux_is_running(void) {
  int result = system("systemctl is-active --quiet offs-daemon");
  return (result == 0) ? 1 : 0;
}

static int service_linux_install(const char* install_dir) {
  (void)install_dir;
  int result = system("systemctl daemon-reload && systemctl enable offs-daemon");
  return (result == 0) ? service_result_ok : service_result_error;
}

static int service_linux_uninstall(void) {
  system("systemctl stop offs-daemon");
  system("systemctl disable offs-daemon");
  int result = system("systemctl daemon-reload");
  return (result == 0) ? service_result_ok : service_result_error;
}

static service_ops_t linux_service_ops = {
  service_linux_stop,
  service_linux_start,
  service_linux_is_running,
  service_linux_install,
  service_linux_uninstall,
  "offs-daemon"
};

const service_ops_t* service_get_ops(void) {
  return &linux_service_ops;
}

#endif // !_WIN32 && !__APPLE__
```

- [ ] **Step 3: Remove Service files from liboffs**

```bash
rm /home/victor/Workspace/src/github.com/vijayee/liboffs/src/Service/service.h
rm /home/victor/Workspace/src/github.com/vijayee/liboffs/src/Service/service_linux.c
rm /home/victor/Workspace/src/github.com/vijayee/liboffs/src/Service/service_windows.c
rmdir /home/victor/Workspace/src/github.com/vijayee/liboffs/src/Service/
```

- [ ] **Step 4: Commit liboffs Service removal**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/liboffs
git add src/Service/
git commit -m "refactor: move Service module to OFFS repo"
```

- [ ] **Step 5: Commit OFFS Service addition**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/OFFS
git add src/Service/
git commit -m "feat: add Service module from liboffs"
```

---

## Task 2: Move updater_main.c to OFFS

**Files:**
- Create: `src/Updater/updater_main.c` in OFFS (with macOS support)
- Delete: `src/Updater/updater_main.c` from liboffs
- Delete: `src/Updater/` directory from liboffs (will be empty)
- Modify: `CMakeLists.txt` in liboffs — remove `add_executable(offs-updater ...)` lines
- Modify: `CMakeLists.txt` in OFFS — add `offs-updater` executable target

- [ ] **Step 1: Write updater_main.c to OFFS with macOS support**

Create `/home/victor/Workspace/src/github.com/vijayee/OFFS/src/Updater/updater_main.c` with the working tree macOS changes applied:

```c
//
// Created by victor on 5/28/25.
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#define SLEEP_MS(ms) Sleep(ms)
#else
#define SLEEP_MS(ms) usleep((ms) * 1000)
#endif

#define STARTUP_TIMEOUT_SEC 30
#define PID_FILE "/var/run/offs-updater.pid"

static FILE* g_log = NULL;

static void _log_open(void) {
  g_log = fopen("/var/log/offs-updater.log", "a");
}

static void _log_write(const char* msg) {
  if (g_log == NULL) _log_open();
  if (g_log == NULL) return;
  time_t now = time(NULL);
  char* ts = ctime(&now);
  char* nl = strchr(ts, '\n');
  if (nl) *nl = '\0';
  fprintf(g_log, "[%s] %s\n", ts, msg);
  fflush(g_log);
}

static void _log_close(void) {
  if (g_log != NULL) { fclose(g_log); g_log = NULL; }
}

static int _copy_file(const char* src, const char* dst) {
  FILE* fsrc = fopen(src, "rb");
  if (fsrc == NULL) return -1;
  FILE* fdst = fopen(dst, "wb");
  if (fdst == NULL) { fclose(fsrc); return -1; }

  char buf[8192];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), fsrc)) > 0) {
    fwrite(buf, 1, n, fdst);
  }
  fclose(fsrc);
  fclose(fdst);

#ifndef _WIN32
  struct stat st;
  if (stat(src, &st) == 0) {
    chmod(dst, st.st_mode);
  }
#endif
  return 0;
}

static int _service_stop(void) {
  _log_write("Stopping daemon service");
#ifdef _WIN32
  return system("sc stop offs-daemon > nul 2>&1");
#elif __APPLE__
  return system("launchctl bootout system "
                "/Library/LaunchDaemons/com.offs.daemon.plist 2>/dev/null");
#else
  return system("systemctl stop offs-daemon 2>/dev/null");
#endif
}

static int _service_start(void) {
  _log_write("Starting daemon service");
#ifdef _WIN32
  return system("sc start offs-daemon > nul 2>&1");
#elif __APPLE__
  return system("launchctl bootstrap system "
                "/Library/LaunchDaemons/com.offs.daemon.plist 2>/dev/null");
#else
  return system("systemctl daemon-reload 2>/dev/null && systemctl start offs-daemon 2>/dev/null");
#endif
}

static int _wait_for_daemon_stop(void) {
  _log_write("Waiting for daemon to exit");
  for (int i = 0; i < 100; i++) {
    SLEEP_MS(300);
#ifdef _WIN32
    int running = system("sc query offs-daemon 2>nul | findstr /C:\"RUNNING\" > nul");
#elif __APPLE__
    int running = system("launchctl list com.offs.daemon > /dev/null 2>&1");
#else
    int running = system("systemctl is-active --quiet offs-daemon");
#endif
    if (running != 0) {
      _log_write("Daemon stopped");
      return 0;
    }
  }
  _log_write("Timeout waiting for daemon to stop");
  return -1;
}

static int _wait_for_daemon_start(void) {
  _log_write("Waiting for daemon to start");
  for (int i = 0; i < STARTUP_TIMEOUT_SEC * 2; i++) {
    SLEEP_MS(500);
#ifdef _WIN32
    int running = system("sc query offs-daemon 2>nul | findstr /C:\"RUNNING\" > nul");
#elif __APPLE__
    int running = system("launchctl list com.offs.daemon > /dev/null 2>&1");
#else
    int running = system("systemctl is-active --quiet offs-daemon");
#endif
    if (running == 0) {
      _log_write("Daemon started successfully");
      return 0;
    }
  }
  _log_write("ERROR: Daemon failed to start within timeout");
  return -1;
}

static int _restore_backup(const char* backup_dir, const char* install_dir) {
  char backup_prev[1024];
  char cmd[2048];

  snprintf(backup_prev, sizeof(backup_prev), "%s/previous", backup_dir);

  _log_write("Restoring from backup");
#ifdef _WIN32
  snprintf(cmd, sizeof(cmd), "xcopy /E /I /Y \"%s\" \"%s\" > nul",
           backup_prev, install_dir);
#else
  snprintf(cmd, sizeof(cmd), "cp -r \"%s\"/* \"%s\" 2>/dev/null || true",
           backup_prev, install_dir);
#endif
  int result = system(cmd);
  if (result != 0) {
    _log_write("WARNING: backup restore may be incomplete");
  }
  return 0;
}

typedef enum { FINISH_NORMAL, FINISH_SELF_REPLACE } finish_mode_t;

int main(int argc, char** argv) {
  finish_mode_t mode = FINISH_NORMAL;
  const char* staging_dir = NULL;
  const char* install_dir = NULL;
  const char* backup_dir = NULL;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--finish") == 0) {
      mode = FINISH_SELF_REPLACE;
    } else if (staging_dir == NULL) {
      staging_dir = argv[i];
    } else if (install_dir == NULL) {
      install_dir = argv[i];
    } else if (backup_dir == NULL) {
      backup_dir = argv[i];
    }
  }

  if (staging_dir == NULL || install_dir == NULL || backup_dir == NULL) {
    fprintf(stderr, "Usage: offs-updater [--finish] <staging> <install> <backup>\n");
    return 1;
  }

  FILE* pidf = fopen(PID_FILE, "w");
  if (pidf != NULL) {
    fprintf(pidf, "%d\n", getpid());
    fclose(pidf);
  }

  _log_open();
  _log_write(mode == FINISH_SELF_REPLACE ? "Updater starting (finish mode)" : "Updater starting");

  if (mode == FINISH_SELF_REPLACE) {
    char src[1024], dst[1024];
    snprintf(src, sizeof(src), "%s/offs-updater", staging_dir);
    snprintf(dst, sizeof(dst), "%s/offs-updater", install_dir);
    _copy_file(src, dst);

    const char* files[] = {"offs-daemon", "offs-cli", NULL};
    for (int i = 0; files[i] != NULL; i++) {
      snprintf(src, sizeof(src), "%s/%s", staging_dir, files[i]);
      snprintf(dst, sizeof(dst), "%s/%s", install_dir, files[i]);
      _copy_file(src, dst);
    }
  } else {
    _service_stop();
    _wait_for_daemon_stop();

    const char* files[] = {"offs-daemon", "offs-cli", NULL};
    char src[1024], dst[1024];
    for (int i = 0; files[i] != NULL; i++) {
      snprintf(src, sizeof(src), "%s/%s", staging_dir, files[i]);
      snprintf(dst, sizeof(dst), "%s/%s", install_dir, files[i]);
      _copy_file(src, dst);
    }

    char temp_updater[1024];
    snprintf(temp_updater, sizeof(temp_updater), "/tmp/offs-updater-temp");
    snprintf(src, sizeof(src), "%s/offs-updater", staging_dir);
    _copy_file(src, temp_updater);
    chmod(temp_updater, 0755);

    _log_write("Self-replacing via temp copy");
    execl(temp_updater, "offs-updater", "--finish",
          staging_dir, install_dir, backup_dir, NULL);

    _log_write("ERROR: exec of temp updater failed, attempting direct service restart");
  }

  _service_start();

  int daemon_ok = _wait_for_daemon_start();

  if (daemon_ok != 0) {
    _service_stop();
    _restore_backup(backup_dir, install_dir);
    _service_start();
    _wait_for_daemon_start();
    _log_write("Rollback complete");
  } else {
    _log_write("Update complete");
  }

  unlink(PID_FILE);
  _log_close();
  return daemon_ok == 0 ? 0 : 1;
}
```

- [ ] **Step 2: Remove updater_main.c and Updater directory from liboffs**

```bash
rm /home/victor/Workspace/src/github.com/vijayee/liboffs/src/Updater/updater_main.c
rmdir /home/victor/Workspace/src/github.com/vijayee/liboffs/src/Updater/
```

- [ ] **Step 3: Remove offs-updater target from liboffs CMakeLists.txt**

Remove these lines from `/home/victor/Workspace/src/github.com/vijayee/liboffs/CMakeLists.txt` (lines 137-139):

```cmake
# Standalone updater binary (no dependency on liboffs)
add_executable(offs-updater src/Updater/updater_main.c)
target_link_libraries(offs-updater PRIVATE pthread)
```

- [ ] **Step 4: Add offs-updater target to OFFS CMakeLists.txt**

In `/home/victor/Workspace/src/github.com/vijayee/OFFS/CMakeLists.txt`, after the `offs_cli` block (before `include(CTest)`), add:

```cmake
# Standalone updater binary
add_executable(offs-updater src/Updater/updater_main.c)
target_link_libraries(offs-updater PRIVATE pthread)
```

- [ ] **Step 5: Commit liboffs changes**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/liboffs
git add src/Updater/ CMakeLists.txt
git commit -m "refactor: move updater binary to OFFS repo"
```

- [ ] **Step 6: Commit OFFS changes**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/OFFS
git add src/Updater/ CMakeLists.txt
git commit -m "feat: add offs-updater binary from liboffs"
```

---

## Task 3: Remove OFFS_VERSION from liboffs CMakeLists.txt

**Files:**
- Modify: `CMakeLists.txt` in liboffs — remove version variables and compile definition

- [ ] **Step 1: Remove OFFS_VERSION block from liboffs CMakeLists.txt**

Remove lines 6-9 and line 61 from `/home/victor/Workspace/src/github.com/vijayee/liboffs/CMakeLists.txt`:

Remove these lines (the version variables):
```cmake
set(OFFS_VERSION_MAJOR 0)
set(OFFS_VERSION_MINOR 1)
set(OFFS_VERSION_PATCH 0)
set(OFFS_VERSION "${OFFS_VERSION_MAJOR}.${OFFS_VERSION_MINOR}.${OFFS_VERSION_PATCH}")
```

And remove this line (the compile definition):
```cmake
target_compile_definitions(offs PRIVATE OFFS_VERSION="${OFFS_VERSION}")
```

- [ ] **Step 2: Add OFFS_VERSION to OFFS CMakeLists.txt**

In `/home/victor/Workspace/src/github.com/vijayee/OFFS/CMakeLists.txt`, add after `set(CMAKE_CXX_STANDARD 17)`:

```cmake
set(OFFS_VERSION_MAJOR 0)
set(OFFS_VERSION_MINOR 1)
set(OFFS_VERSION_PATCH 0)
set(OFFS_VERSION "${OFFS_VERSION_MAJOR}.${OFFS_VERSION_MINOR}.${OFFS_VERSION_PATCH}")
```

And add compile definitions to the `offsd` and `offs_cli` targets. For `offsd`, add after `target_include_directories(offsd PRIVATE ...)`:
```cmake
target_compile_definitions(offsd PRIVATE OFFS_VERSION="${OFFS_VERSION}")
```

For `offs_cli`, add after the `target_include_directories` line:
```cmake
target_compile_definitions(offs_cli PRIVATE OFFS_VERSION="${OFFS_VERSION}")
```

- [ ] **Step 3: Commit liboffs change**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/liboffs
git add CMakeLists.txt
git commit -m "refactor: move OFFS_VERSION definition to OFFS repo"
```

- [ ] **Step 4: Commit OFFS change**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/OFFS
git add CMakeLists.txt
git commit -m "feat: add OFFS_VERSION compile definition"
```

---

## Task 4: Move packaging directory to OFFS

**Files:**
- Create: `packaging/` tree in OFFS (copy from liboffs)
- Delete: `packaging/` tree from liboffs

- [ ] **Step 1: Copy packaging directory to OFFS**

```bash
cp -r /home/victor/Workspace/src/github.com/vijayee/liboffs/packaging \
      /home/victor/Workspace/src/github.com/vijayee/OFFS/packaging
```

- [ ] **Step 2: Remove packaging from liboffs**

```bash
rm -rf /home/victor/Workspace/src/github.com/vijayee/liboffs/packaging
```

- [ ] **Step 3: Commit liboffs removal**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/liboffs
git add packaging/
git commit -m "refactor: move packaging files to OFFS repo"
```

- [ ] **Step 4: Commit OFFS addition**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/OFFS
git add packaging/
git commit -m "feat: add platform packaging files"
```

---

## Task 5: Move release scripts to OFFS

**Files:**
- Create: `scripts/` in OFFS (copy from liboffs, with path fixes)
- Delete: `scripts/` from liboffs

- [ ] **Step 1: Write release.sh to OFFS with path fixes**

The script references `$PROJECT_DIR/../OFFS/build` for offsd/offs-cli. When in the OFFS repo, `$PROJECT_DIR` IS the OFFS repo, so the build dir is just `$PROJECT_DIR/build`. Copy `scripts/release.sh` and fix paths.

Create `/home/victor/Workspace/src/github.com/vijayee/OFFS/scripts/release.sh`:

```bash
#!/bin/bash
set -euo pipefail

PLATFORM="${1:-}"
TAG="${2:-}"

if [ -z "$PLATFORM" ] || [ -z "$TAG" ]; then
  echo "Usage: $0 <linux-x64|macos-x64|windows-x64> <tag>"
  echo ""
  echo "Run on each platform to build and package the release."
  echo "  linux-x64   - Build .tar.gz, .deb, .rpm on Linux"
  echo "  macos-x64   - Build .tar.gz, .pkg on macOS"
  echo "  windows-x64 - Build .zip, .msi on Windows"
  exit 1
fi

echo "=== OFFS Release Builder ==="
echo "Platform: $PLATFORM"
echo "Tag: $TAG"

PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$PROJECT_DIR/build-release"
BUNDLE="offs-${PLATFORM}"
ARTIFACTS_DIR="$PROJECT_DIR/artifacts"

# Clean and build
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR" "$ARTIFACTS_DIR"

cd "$BUILD_DIR"
cmake .. -DCMAKE_BUILD_TYPE=Release

# Detect core count
if command -v nproc &>/dev/null; then
  CORES=$(nproc)
elif command -v sysctl &>/dev/null; then
  CORES=$(sysctl -n hw.ncpu)
else
  CORES=4
fi

cmake --build . -j"$CORES"

# Create bundle directory
rm -rf "$BUNDLE"
mkdir -p "$BUNDLE"

case "$PLATFORM" in
  linux-x64)
    # Copy binaries from OFFS build
    if [ -f "$BUILD_DIR/offs-updater" ]; then
      cp "$BUILD_DIR/offs-updater" "$BUNDLE/offs-updater"
    fi
    if [ -f "$BUILD_DIR/offsd" ]; then
      cp "$BUILD_DIR/offsd" "$BUNDLE/offs-daemon"
    fi
    if [ -f "$BUILD_DIR/offs_cli" ]; then
      cp "$BUILD_DIR/offs_cli" "$BUNDLE/offs-cli"
    fi

    # Version file
    echo "$TAG" > "$BUNDLE/VERSION"

    # Checksums
    cd "$BUNDLE" && sha256sum * > checksums.sha256 && cd ..

    # Create tarball
    tar -czf "$ARTIFACTS_DIR/${BUNDLE}.tar.gz" "$BUNDLE"

    # Build .deb
    echo "Building .deb package..."
    DEB_DIR="$BUILD_DIR/deb/offs_${TAG#v}_amd64"
    mkdir -p "$DEB_DIR/DEBIAN" "$DEB_DIR/usr/bin" "$DEB_DIR/usr/lib/systemd/system"
    cp "$PROJECT_DIR/packaging/linux/debian/control" "$DEB_DIR/DEBIAN/"
    sed -i "s/Version: .*/Version: ${TAG#v}/" "$DEB_DIR/DEBIAN/control"
    cp "$PROJECT_DIR/packaging/linux/debian/postinst" "$DEB_DIR/DEBIAN/"
    cp "$PROJECT_DIR/packaging/linux/debian/prerm" "$DEB_DIR/DEBIAN/"
    cp "$PROJECT_DIR/packaging/linux/debian/postrm" "$DEB_DIR/DEBIAN/"
    cp "$BUNDLE/offs-daemon" "$DEB_DIR/usr/bin/"
    cp "$BUNDLE/offs-cli" "$DEB_DIR/usr/bin/"
    cp "$BUNDLE/offs-updater" "$DEB_DIR/usr/bin/"
    cp "$PROJECT_DIR/packaging/linux/debian/offs-daemon.service" "$DEB_DIR/usr/lib/systemd/system/"
    chmod 755 "$DEB_DIR/DEBIAN/postinst" "$DEB_DIR/DEBIAN/prerm" "$DEB_DIR/DEBIAN/postrm"
    dpkg-deb --build "$DEB_DIR" "$ARTIFACTS_DIR/offs_${TAG#v}_amd64.deb"
    echo "  -> $ARTIFACTS_DIR/offs_${TAG#v}_amd64.deb"

    # Build .rpm
    echo "Building .rpm package..."
    RPMBUILD_DIR="$BUILD_DIR/rpmbuild"
    mkdir -p "$RPMBUILD_DIR/SOURCES"
    cp "$ARTIFACTS_DIR/${BUNDLE}.tar.gz" "$RPMBUILD_DIR/SOURCES/offs-${TAG#v}.tar.gz"
    rpmbuild -ba "$PROJECT_DIR/packaging/linux/rpm/offs.spec" \
      --define "_topdir $RPMBUILD_DIR" \
      --define "version ${TAG#v}" \
      -D"_sourcedir $RPMBUILD_DIR/SOURCES" 2>/dev/null || \
      echo "  WARNING: rpmbuild not available, skipping .rpm"
    ;;

  macos-x64)
    if [ -f "$BUILD_DIR/offs-updater" ]; then
      cp "$BUILD_DIR/offs-updater" "$BUNDLE/offs-updater"
    fi
    if [ -f "$BUILD_DIR/offsd" ]; then
      cp "$BUILD_DIR/offsd" "$BUNDLE/offs-daemon"
    fi
    if [ -f "$BUILD_DIR/offs_cli" ]; then
      cp "$BUILD_DIR/offs_cli" "$BUNDLE/offs-cli"
    fi
    echo "$TAG" > "$BUNDLE/VERSION"
    cd "$BUNDLE" && shasum -a 256 * > checksums.sha256 && cd ..
    tar -czf "$ARTIFACTS_DIR/${BUNDLE}.tar.gz" "$BUNDLE"

    # Build .pkg
    if command -v pkgbuild &>/dev/null; then
      echo "Building .pkg..."
      mkdir -p "$BUILD_DIR/pkg/root/usr/local/bin"
      cp "$BUNDLE/offs-daemon" "$BUILD_DIR/pkg/root/usr/local/bin/"
      cp "$BUNDLE/offs-cli" "$BUILD_DIR/pkg/root/usr/local/bin/"
      cp "$BUNDLE/offs-updater" "$BUILD_DIR/pkg/root/usr/local/bin/"
      pkgbuild --root "$BUILD_DIR/pkg/root" \
        --scripts "$PROJECT_DIR/packaging/macos" \
        --identifier com.offs.daemon \
        --version "${TAG#v}" \
        "$ARTIFACTS_DIR/offs-${TAG#v}.pkg"
      echo "  -> $ARTIFACTS_DIR/offs-${TAG#v}.pkg"
    else
      echo "WARNING: pkgbuild not available, skipping .pkg"
    fi
    ;;

  windows-x64)
    if [ -f "$BUILD_DIR/offs-updater.exe" ]; then
      cp "$BUILD_DIR/offs-updater.exe" "$BUNDLE/offs-updater.exe"
    fi
    if [ -f "$BUILD_DIR/offsd.exe" ]; then
      cp "$BUILD_DIR/offsd.exe" "$BUNDLE/offs-daemon.exe"
    fi
    if [ -f "$BUILD_DIR/offs_cli.exe" ]; then
      cp "$BUILD_DIR/offs_cli.exe" "$BUNDLE/offs-cli.exe"
    fi
    echo "$TAG" > "$BUNDLE/VERSION"
    cd "$BUNDLE" && sha256sum * > checksums.sha256 && cd ..
    zip -r "$ARTIFACTS_DIR/${BUNDLE}.zip" "$BUNDLE"

    # Build .msi
    if command -v candle &>/dev/null && command -v light &>/dev/null; then
      echo "Building .msi..."
      cd "$PROJECT_DIR/packaging/windows"
      candle offs.wxs -dVersion="${TAG#v}" -o "$BUILD_DIR/"
      light "$BUILD_DIR/offs.wixobj" -o "$ARTIFACTS_DIR/offs-${TAG#v}.msi"
      echo "  -> $ARTIFACTS_DIR/offs-${TAG#v}.msi"
    else
      echo "WARNING: WiX Toolset not available, skipping .msi"
    fi
    ;;
esac

echo ""
echo "=== Build complete ==="
echo "Artifacts in: $ARTIFACTS_DIR"
ls -la "$ARTIFACTS_DIR/"
```

Key change: `$PROJECT_DIR/../OFFS/build` → `$BUILD_DIR` (since `$PROJECT_DIR` IS the OFFS repo now).

- [ ] **Step 2: Copy collect-release.sh to OFFS (no changes needed)**

```bash
cp /home/victor/Workspace/src/github.com/vijayee/liboffs/scripts/collect-release.sh \
   /home/victor/Workspace/src/github.com/vijayee/OFFS/scripts/collect-release.sh
```

- [ ] **Step 3: Make scripts executable in OFFS**

```bash
chmod +x /home/victor/Workspace/src/github.com/vijayee/OFFS/scripts/release.sh
chmod +x /home/victor/Workspace/src/github.com/vijayee/OFFS/scripts/collect-release.sh
```

- [ ] **Step 4: Remove scripts from liboffs**

```bash
rm -rf /home/victor/Workspace/src/github.com/vijayee/liboffs/scripts
```

- [ ] **Step 5: Commit liboffs removal**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/liboffs
git add scripts/
git commit -m "refactor: move release scripts to OFFS repo"
```

- [ ] **Step 6: Commit OFFS addition**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/OFFS
git add scripts/
git commit -m "feat: add release and collection scripts"
```

---

## Task 6: Verify final state

- [ ] **Step 1: Verify liboffs has no app-level code**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/liboffs
# These should all fail (directories removed):
ls src/Service/ 2>&1 | grep -q "No such file"
ls src/Updater/ 2>&1 | grep -q "No such file"
ls packaging/ 2>&1 | grep -q "No such file"
ls scripts/ 2>&1 | grep -q "No such file"
# CMakeLists.txt should NOT contain offs-updater or OFFS_VERSION:
grep -q "offs-updater" CMakeLists.txt && echo "FAIL: offs-updater still in CMakeLists.txt" || echo "OK: no offs-updater"
grep -q "OFFS_VERSION" CMakeLists.txt && echo "FAIL: OFFS_VERSION still in CMakeLists.txt" || echo "OK: no OFFS_VERSION"
# Update plumbing should still exist:
ls src/Update/update_check.c src/Update/update_download.c src/Update/update_stage.c src/Update/update_actor.c
```

- [ ] **Step 2: Verify OFFS has all moved files**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/OFFS
ls src/Service/service.h src/Service/service_linux.c src/Service/service_windows.c
ls src/Updater/updater_main.c
ls packaging/linux/debian/ packaging/linux/rpm/ packaging/macos/ packaging/windows/
ls scripts/release.sh scripts/collect-release.sh
grep -q "offs-updater" CMakeLists.txt && echo "OK: offs-updater in CMakeLists.txt"
grep -q "OFFS_VERSION" CMakeLists.txt && echo "OK: OFFS_VERSION in CMakeLists.txt"
```

- [ ] **Step 3: Verify liboffs builds cleanly**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/liboffs
mkdir -p build && cd build && cmake .. && cmake --build . -j$(nproc)
```

Expected: Build succeeds without offs-updater target.

- [ ] **Step 4: Verify OFFS builds cleanly**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/OFFS
mkdir -p build && cd build && cmake .. && cmake --build . -j$(nproc)
```

Expected: Build succeeds with offs-updater, offsd, and offs_cli targets.
