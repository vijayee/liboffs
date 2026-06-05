# OFFS Installer & Auto-Updater Design

## Context

OFFS currently has no version system, no installer packages, no auto-update mechanism, and no CI/CD pipeline. The daemon (`offsd`) and CLI (`offs`) must be manually compiled and started. This design adds production-grade installation, service management, and fully-automatic self-updating across all target platforms (Linux, macOS, Windows).

## Architecture

The daemon owns the entire update lifecycle via a new `update_actor_t` that runs on the scheduler pool. It checks, downloads, stages, drains open streams, and spawns the updater — fully automatic, no CLI intervention required (status-only visibility).

```
Trigger: startup / timer(6h) / SIGHUP
  → GitHub Releases API check (HTTPS, raw sockets + OpenSSL)
  → Download + SHA256 verify
  → Drain open OFF streams (reject new, wait for existing)
  → Spawn offs-updater → daemon exits
  → offs-updater: stop service → backup → replace binaries → restart → verify
```

## Where Code Lives

All new code goes in **liboffs** (`src/`):

| Module | Location | Purpose |
|--------|----------|---------|
| Version | `src/Version/version.h`, `version.c` | Semver parse/compare, `OFFS_VERSION` from CMake |
| Update | `src/Update/update_check.c`, `update_download.c`, `update_stage.c`, `update_actor.c` | GitHub API check, download, verify, stage, drain, spawn updater |
| Updater | `src/Updater/updater.c` | Standalone binary: stop service, backup, replace, restart, rollback |
| Service | `src/Service/service.h`, `service_linux.c`, `service_macos.c`, `service_windows.c` | Platform service control abstraction |

The OFFS repo daemon (`offsd`) creates the update actor and wires it into its lifecycle. The CLI gets an `update status` command via existing CBOR wire protocol.

## Component Details

### 1. Version System (`src/Version/`)

- `version_t` struct: major, minor, patch, prerelease string
- `version_parse("v1.2.3-rc.1")` → `version_t`
- `version_compare(a, b)` → -1/0/1
- `version_to_string(v)` → "1.2.3"
- `OFFS_VERSION` defined in root CMakeLists.txt, passed as compile definition
- `VERSION` file written to build output for the updater

### 2. Update Actor (`src/Update/`)

**Sub-modules:**
- `update_check.c` — HTTPS GET to GitHub Releases API, parses JSON with cJSON, extracts tag_name and platform asset URL + SHA256
- `update_download.c` — Downloads asset to `<staging_dir>/`, verifies SHA256, extracts tarball
- `update_stage.c` — Stages new binaries, creates backup of current install
- `update_actor.h/c` — Actor lifecycle: timer, SIGHUP, drain, spawn updater

**Configuration (JSON, in daemon config):**
```json
{
  "update": {
    "enabled": true,
    "github_repo": "Prometheus-SCN/OFFS",
    "github_api_url": "https://api.github.com",
    "channel": "stable",
    "check_interval_hours": 6,
    "staging_dir": "/var/lib/offs/updates",
    "backup_dir": "/var/lib/offs/backup",
    "drain_timeout_seconds": 300
  }
}
```

**Release channels** mapped from authority environment:

| Environment | API endpoint | Matches | Prerelease |
|-------------|-------------|---------|------------|
| `prod` | `/releases/latest` | Latest stable tag | No |
| `test` | `/releases` | Tags with `-rc` suffix | Yes |
| `dev` | `/releases` | Tags with `-dev` or `-alpha` suffix | Yes |

- `github_repo` — set to any `owner/repo` for forks or private deployments
- `github_api_url` — swap to `https://github.internal.company/api/v3` for GitHub Enterprise
- `channel` — `stable` (default), `rc`, `dev`
- Private repo authentication: daemon reads `GITHUB_TOKEN` from environment or config, sent as `Authorization: Bearer` header

**Drain mechanism:**
- Atomic `draining` flag on node struct
- HTTP PUT handler rejects new OFF stream requests (503) when draining
- Update actor polls open stream counter; proceeds when zero or timeout reached

### 3. Updater Binary (`src/Updater/updater.c`)

Standalone executable, no dependency on liboffs. Platform syscalls + service abstraction only.

**Flow:**
1. Daemon spawns `offs-updater <staging_dir> <install_dir> <backup_dir>`
2. Write PID file (prevent concurrent updates)
3. `service_stop()` — wait for daemon process exit (poll with timeout)
4. Backup current install to `<backup_dir>/previous/`
5. Copy new binaries from staging to install dir
6. Self-replace: copy self to temp, exec temp copy with `--finish`, temp replaces original
7. `service_start()`
8. Verify daemon started within 30s — on failure, restore from backup
9. Cleanup staging dir, remove PID, exit

**Logging:** All steps logged to `/var/log/offs-updater.log`.

### 4. Service Management (`src/Service/`)

Platform vtable:
```c
typedef struct {
  int (*stop)(void);
  int (*start)(void);
  int (*is_running)(void);
  int (*install)(void);
  int (*uninstall)(void);
} service_ops_t;
```

**Linux:** `systemctl stop/start/is-active/enable/disable offs-daemon`
**macOS:** `launchctl bootout/bootstrap system/com.offs.daemon`
**Windows:** `OpenSCManager → OpenService → ControlService(SERVICE_CONTROL_STOP) → QueryServiceStatus → StartService`

### 5. CLI Command

One new command in OFFS repo `src/offs/commands/update.c`:
- `offs update status` — CBOR message type 34 → reports {state, current_version, available_version, progress}

States: idle, checking, downloading, staged, draining, applying.

### 6. Packaging (`packaging/`)

**Linux .deb:**
```
packaging/linux/debian/
├── control              # Package: offs, Depends: libc6, openssl
├── postinst             # systemctl daemon-reload && enable --now offs-daemon
├── prerm                # systemctl stop offs-daemon
├── postrm               # systemctl disable offs-daemon && daemon-reload
└── offs-daemon.service  # Type=simple, Restart=on-failure, WantedBy=multi-user.target
```

**Linux .rpm:**
```
packaging/linux/rpm/
├── offs.spec            # %post/%preun/%postun scriptlets (same logic as .deb)
└── offs-daemon.service  # Same unit file
```

**macOS .pkg:**
```
packaging/macos/
├── preinstall           # launchctl bootout system/com.offs.daemon 2>/dev/null || true
├── postinstall          # cp plist to /Library/LaunchDaemons/, launchctl bootstrap
└── com.offs.daemon.plist # RunAtLoad, KeepAlive, StandardOutPath, StandardErrorPath
```

**Windows .msi:**
```
packaging/windows/
├── offs.wxs             # <ServiceInstall> Start="auto", <ServiceControl> Stop="both" Remove="uninstall"
└── offs.wxl             # Localized strings
```

All installers: stop existing daemon if running, replace binaries, register service, start daemon.

### 7. Release Process

No CI service. Local/scripted builds on three machines:

| Platform | Machine | Compiler | Output |
|----------|---------|----------|--------|
| Linux x64 | Dev machine | gcc | `offs-linux-x64.tar.gz`, `.deb`, `.rpm` |
| Windows x64 | Windows box | MSVC | `offs-windows-x64.zip`, `.msi` |
| macOS x64 | MacBook Air (2013) | Apple clang | `offs-macos-x64.tar.gz`, `.pkg` |

macOS arm64 is deferred — x86_64 binaries run on Apple Silicon via Rosetta 2.

**`scripts/release.sh`** on each machine: checkout tag → cmake build → create bundle (daemon + CLI + updater + VERSION + checksums) → build installer package.

**`scripts/collect-release.sh`** on Linux: gathers all platform artifacts → `gh release create <tag>` with SHA256 checksums in body.

## Release Bundle Format

Each platform tarball/zip contains:
```
offs-daemon       # main daemon binary
offs              # CLI tool
offs-updater      # standalone updater
VERSION           # plain text version string
checksums.sha256  # SHA256 of all files
```

## Verification

1. Build `offs-updater` as standalone binary, test self-replacement with temp-copy technique
2. Build `update_actor` into daemon, test drain-before-update flow with mock HTTP server
3. Build `.deb` and install on clean Linux system: verify daemon starts on boot, `offs status` works
4. Test full update cycle: run older daemon → trigger check → verify new version downloaded → verify daemon restarts with new version
5. Test rollback: corrupt the staged binary → verify updater restores from backup
6. Test drain timeout: open a long-running stream → verify updater proceeds after timeout
