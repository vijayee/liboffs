# Self-Update Supply Chain Implementation Plan (Stage 3)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the self-update path cryptographically verified end-to-end: TLS certificate verification, an ed25519 signed release manifest (compiled-in Prometheus-SCN release public key, fail-closed), per-file SHA256 from the verified manifest, path-contained in-process tar extraction, and a verified staged updater binary — so a MITM or a tampered release cannot achieve arbitrary code execution.

**Architecture:** The Prometheus-SCN release operator holds an offline ed25519 private key and signs each release manifest with `offs-release-sign`. The manifest (CBOR/JSON) lists every release file with its SHA256 and size. The updater fetches the manifest + signature over TLS-verified HTTPS, verifies the ed25519 signature against a public key compiled into the binary at build time (`-DOFFS_RELEASE_PUBKEY=<path>`, fail-closed if unset), then fetches each listed file and re-hashes it against the manifest. Extraction is a minimal in-process USTAR reader with realpath containment (no external `tar`, no path traversal). The staged `offs-updater` binary is itself a manifest-listed file; liboffs verifies its hash, then `update_actor` execs the staged absolute path (not `PATH`).

**Tech Stack:** C11, OpenSSL 3.x (`EVP_DigestSign`/`EVP_DigestVerify` one-shot ed25519, `EVP_sha256`, `SSL_CTX_set_verify`), cJSON (`deps/cJSON`), GoogleTest. No new external deps; the tar reader is ~150 lines of in-process USTAR parsing.

**Spec:** `docs/superpowers/specs/2026-08-22-production-readiness-fixes-design.md` (Section 6).

**Upstream:** release assets live on `github.com/Prometheus-SCN/OFFS` releases (the daemon repo; default `update_check_config.github_repo = "Prometheus-SCN/OFFS"` per `OFFS/src/offsd/main.c:1111`). The `offs-release-sign` tooling + `docs/RELEASE.md` document the Prometheus-SCN release process; the private key is held offline by the Prometheus-SCN release operator.

---

## File Structure

**Create:**
- `src/Update/update_verify.h`, `src/Update/update_verify.c` — ed25519 manifest signature verification (compile-in pubkey, fail-closed).
- `src/Update/update_manifest.h`, `src/Update/update_manifest.c` — manifest fetch + parse + per-file SHA256 list.
- `src/Update/update_extract.h`, `src/Update/update_extract.c` — in-process USTAR tar reader + realpath containment.
- `tools/offs-release-sign/main.c`, `tools/offs-release-sign/sign_ops.h`, `tools/offs-release-sign/sign_ops.c`, `tools/offs-release-sign/CMakeLists.txt` — the release-signing tool (ed25519 `EVP_DigestSign`).
- `test/test_update_verify.cpp`, `test/test_update_manifest.cpp`, `test/test_update_extract.cpp`.

**Modify:**
- `src/Update/update_check.c` — TLS verify; replace `"sha256:"` markdown scraping with manifest fetch.
- `src/Update/update_download.c` — TLS verify; replace `execlp("tar",...)` with `update_extract`; make SHA256 mandatory (drop the opt-out); verify each downloaded file against the manifest.
- `src/Update/update_actor.c` — exec the staged verified updater (absolute path) instead of `execlp("offs-updater",...)`; handle fork/exec failures.
- `src/Update/update_check.h` — extend `update_info_t` (add manifest fields; the `sha256[65]` single-file field becomes a per-file list carried by the manifest).
- `CMakeLists.txt` — `OFFS_RELEASE_PUBKEY` cache var → `OFFS_RELEASE_PUBKEY` compile define (fail-closed if unset); `add_subdirectory(tools/offs-release-sign)`.
- `test/CMakeLists.txt` — register the 3 new test files.

---

### Task 1: TLS certificate verification

**Files:**
- Modify: `src/Update/update_check.c:~166` (SSL context setup)
- Modify: `src/Update/update_download.c:~185` (SSL context setup)

- [ ] **Step 1: Write the failing test**

Add `test/test_update_verify.cpp` (new file, registered in Task 9). A direct TLS-verify test needs a mock HTTPS server with a self-signed cert → assert the fetch fails. That's heavy; instead test the helper. Extract the SSL-context setup into a testable helper `update_ssl_context_create()` that returns a configured `SSL_CTX*` (with verify enabled), then test that it sets `SSL_VERIFY_PEER`:

```cpp
#include <gtest/gtest.h>
extern "C" {
#include "Update/update_verify.h"
#include <openssl/ssl.h>
}
TEST(UpdateTls, ContextEnablesPeerVerification) {
  SSL_CTX* ctx = update_ssl_context_create();
  ASSERT_NE(ctx, nullptr);
  EXPECT_EQ(SSL_CTX_get_verify_mode(ctx), SSL_VERIFY_PEER);
  SSL_CTX_free(ctx);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd build-test && cmake --build . --target testliboffs && ./test/testliboffs --gtest_filter=UpdateTls.*`
Expected: FAIL — `update_ssl_context_create` not declared.

- [ ] **Step 3: Add the helper**

In `src/Update/update_verify.h`:
```c
#include <openssl/ssl.h>
// Create an SSL_CTX configured for TLS peer verification: loads default CA
// paths, enables SSL_VERIFY_PEER, sets verify depth. Returns NULL on failure.
// Caller frees with SSL_CTX_free.
SSL_CTX* update_ssl_context_create(void);
```

In `src/Update/update_verify.c`:
```c
#include "update_verify.h"
#include <openssl/err.h>

SSL_CTX* update_ssl_context_create(void) {
  SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
  if (ctx == NULL) return NULL;
  SSL_CTX_set_default_verify_paths(ctx);
  SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
  SSL_CTX_set_verify_depth(ctx, 4);
  return ctx;
}
```

- [ ] **Step 4: Use it in update_check.c and update_download.c**

In `update_check.c:~166`, replace `ssl_context = SSL_CTX_new(TLS_client_method());` + the manual setup with `ssl_context = update_ssl_context_create();`. After `SSL_new` + before `SSL_connect`, add SNI: `SSL_set_tlsext_host_name(ssl_connection, host)` (extract `host` from the URL — the existing code already parses host). After `SSL_connect`, check `SSL_get_verify_result(ssl_connection) != X509_V_OK` → log + fail (free context, return NULL). Same for `update_download.c:~185`.

- [ ] **Step 5: Run test + no regression**

Run: `cd build-test && cmake --build . --target testliboffs && ./test/testliboffs --gtest_filter=UpdateTls.*:*Update*`
Expected: PASS. (Existing update tests that hit the real GitHub API may now fail if they used a self-signed intercept — check; if they did, they need the real CA path, which `update_ssl_context_create` provides.)

- [ ] **Step 6: Commit**

```bash
git add src/Update/update_verify.h src/Update/update_verify.c src/Update/update_check.c src/Update/update_download.c test/test_update_verify.cpp
git commit -m "feat(update): TLS certificate verification for update check + download"
```

---

### Task 2: ed25519 manifest signature verification

**Files:**
- Modify: `src/Update/update_verify.h`, `update_verify.c` (add `update_verify_manifest`)
- Modify: `CMakeLists.txt` — `OFFS_RELEASE_PUBKEY` cache var + compile define (fail-closed)

- [ ] **Step 1: Write the failing tests**

Add to `test/test_update_verify.cpp`. Generate a test ed25519 keypair at test time (OpenSSL EVP), sign a manifest, verify with the pubkey; assert good signature passes, tampered signature fails, wrong pubkey fails.

```cpp
TEST(UpdateVerify, GoodSignaturePasses) {
  // Generate ed25519 keypair.
  EVP_PKEY* key = NULL;
  EVP_PKEY_CTX* kctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, NULL);
  EVP_PKEY_keygen_init(kctx);
  EVP_PKEY_keygen(kctx, &key);
  EVP_PKEY_CTX_free(kctx);
  ASSERT_NE(key, NULL);
  // Sign a manifest.
  uint8_t manifest[] = "{\"version\":1,\"files\":[]}";
  size_t manifest_len = sizeof(manifest) - 1;
  size_t sig_len = 0;
  EVP_DigestSign(NULL, &sig_len, manifest, manifest_len, NULL, key);
  uint8_t* sig = (uint8_t*)malloc(sig_len);
  EVP_DigestSign(sig, &sig_len, manifest, manifest_len, NULL, key);
  // Verify with update_verify_manifest (extract pubkey to PEM, pass as the compiled-in key).
  // For the test, extract the pubkey PEM and set the OFFS_RELEASE_PUBKEY env/define.
  // ... (see implementation: update_verify_manifest takes the pubkey bytes + sig + manifest)
  bool ok = update_verify_manifest(sig, sig_len, manifest, manifest_len, pubkey_pem, pubkey_pem_len);
  EXPECT_TRUE(ok);
  free(sig);
  EVP_PKEY_free(key);
}

TEST(UpdateVerify, TamperedSignatureFails) { /* flip a sig byte → false */ }
TEST(UpdateVerify, WrongPubkeyFails) { /* verify with a different keypair's pubkey → false */ }
```

The exact `update_verify_manifest` signature: `bool update_verify_manifest(const uint8_t* sig, size_t sig_len, const uint8_t* manifest, size_t manifest_len, const char* pubkey_pem, size_t pubkey_pem_len)`. The test extracts the signing key's public part to PEM and passes it. For the compile-in path, `update_verify_load_pubkey()` returns the `OFFS_RELEASE_PUBKEY` define (or NULL if unset → fail-closed).

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd build-test && cmake --build . --target testliboffs && ./test/testliboffs --gtest_filter=UpdateVerify.*`
Expected: FAIL — `update_verify_manifest` not declared.

- [ ] **Step 3: Add the CMake define (fail-closed)**

In `CMakeLists.txt` (near the option patterns, ~line 320):
```cmake
set(OFFS_RELEASE_PUBKEY "" CACHE STRING "Path to a PEM ed25519 public key for verifying release manifests (Prometheus-SCN release key)")
if(OFFS_RELEASE_PUBKEY)
  file(READ ${OFFS_RELEASE_PUBKEY} _offs_release_pubkey_pem)
  target_compile_definitions(offs PRIVATE OFFS_RELEASE_PUBKEY="${_offs_release_pubkey_pem}")
else()
  message(WARNING "OFFS_RELEASE_PUBKEY not set — self-update will be fail-closed (no release key compiled in)")
endif()
```
(Reading the PEM file at configure time and embedding it as a string define. The fail-closed behavior: `update_verify_load_pubkey()` returns NULL if `OFFS_RELEASE_PUBKEY` is not defined, and `update_verify_manifest` returns false on NULL pubkey.)

- [ ] **Step 4: Add the verify routine**

In `src/Update/update_verify.h`:
```c
// The compiled-in release public key (PEM), or NULL if OFFS_RELEASE_PUBKEY
// was not set at build time (fail-closed: verification always fails).
const char* update_verify_load_pubkey(void);

// Verify an ed25519 signature over a manifest. pubkey_pem is the PEM-encoded
// public key (or NULL to use the compiled-in release key). Returns true on
// valid signature, false on any failure (bad sig, wrong key, NULL pubkey).
bool update_verify_manifest(const uint8_t* sig, size_t sig_len,
                            const uint8_t* manifest, size_t manifest_len,
                            const char* pubkey_pem, size_t pubkey_pem_len);
```

In `src/Update/update_verify.c`:
```c
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <string.h>

#ifndef OFFS_RELEASE_PUBKEY
#define OFFS_RELEASE_PUBKEY ""
#endif

const char* update_verify_load_pubkey(void) {
  return (OFFS_RELEASE_PUBKEY[0] != '\0') ? OFFS_RELEASE_PUBKEY : NULL;
}

bool update_verify_manifest(const uint8_t* sig, size_t sig_len,
                            const uint8_t* manifest, size_t manifest_len,
                            const char* pubkey_pem, size_t pubkey_pem_len) {
  if (sig == NULL || manifest == NULL) return false;
  const char* pem = pubkey_pem;
  size_t pem_len = pubkey_pem_len;
  if (pem == NULL) {
    pem = update_verify_load_pubkey();
    if (pem == NULL) return false;  // fail-closed: no release key
    pem_len = strlen(pem);
  }
  // Load the pubkey from PEM.
  BIO* bio = BIO_new_mem_buf(pem, (int)pem_len);
  if (bio == NULL) return false;
  EVP_PKEY* key = PEM_read_bio_PUBKEY(bio, NULL, NULL, NULL);
  BIO_free(bio);
  if (key == NULL) return false;
  // ed25519 one-shot verify.
  int ok = EVP_DigestVerify(key, sig, sig_len, manifest, manifest_len);
  EVP_PKEY_free(key);
  return ok == 1;
}
```

(`EVP_DigestVerify` with a NULL digest is the ed25519 one-shot — no pre-hash.)

- [ ] **Step 5: Run tests + commit**

Run: `cd build-test && cmake --build . --target testliboffs && ./test/testliboffs --gtest_filter=UpdateVerify.*`
Expected: 3 PASS (good, tampered, wrong-pubkey). Commit:
```bash
git add CMakeLists.txt src/Update/update_verify.h src/Update/update_verify.c test/test_update_verify.cpp
git commit -m "feat(update): ed25519 manifest signature verification (compile-in pubkey, fail-closed)"
```

---

### Task 3: `offs-release-sign` tool

**Files:**
- Create: `tools/offs-release-sign/main.c`, `sign_ops.h`, `sign_ops.c`, `CMakeLists.txt`
- Modify: `CMakeLists.txt` — `add_subdirectory(tools/offs-release-sign)`

- [ ] **Step 1: Write the tool**

`tools/offs-release-sign/sign_ops.h`:
```c
// Sign a manifest file with an ed25519 private key (PEM). Writes the signature
// to <manifest_path>.sig as raw 64 bytes. Returns 0 on success.
int release_sign_sign(const char* key_path, const char* manifest_path);
// Generate a new ed25519 keypair, write private key PEM to <priv_path> and
// public key PEM to <pub_path>. Returns 0 on success.
int release_sign_keygen(const char* priv_path, const char* pub_path);
```

`tools/offs-release-sign/sign_ops.c` — implement `release_sign_keygen` (EVP_PKEY_ED25519 keygen + PEM_write_bio_PrivateKey + PEM_write_bio_PUBKEY) and `release_sign_sign` (PEM_read_bio_PrivateKey + EVP_DigestSign one-shot + write 64-byte sig). Reuse the offs-ca OpenSSL patterns (`tools/offs-ca/ca_ops.c`).

`tools/offs-release-sign/main.c` — CLI: `offs-release-sign --key <priv.pem> --manifest <manifest.cbor>` (sign) or `--keygen --priv <priv.pem> --pub <pub.pem>` (generate a new keypair).

`tools/offs-release-sign/CMakeLists.txt` — mirror `tools/offs-ca/CMakeLists.txt` (find OpenSSL, add_executable, link OpenSSL + offs).

- [ ] **Step 2: Wire into top-level CMakeLists**

In `CMakeLists.txt` near line 394 (`add_subdirectory(tools/offs-ca)`), add `add_subdirectory(tools/offs-release-sign)`.

- [ ] **Step 3: Test (integration — generate keypair, sign, verify with Task 2's routine)**

Add to `test/test_update_verify.cpp`:
```cpp
TEST(ReleaseSignTool, KeygenSignVerifyRoundTrip) {
  // Use the tool's ops functions directly (link the tool's sign_ops.c into the
  // test, or exec the tool binary). Generate a keypair to /tmp, sign a
  // manifest, verify with update_verify_manifest using the pubkey.
  // ... assert verify true; tamper manifest → verify false.
}
```
(If linking the tool's `sign_ops.c` into the test is awkward, exec the tool binary via `system` in the test. Prefer linking for unit-test cleanliness.)

- [ ] **Step 4: Build + run + commit**

```bash
cd build-test && cmake --build . --target testliboffs && ./test/testliboffs --gtest_filter=ReleaseSignTool.*:UpdateVerify.*
```
Commit:
```bash
git add tools/offs-release-sign/ CMakeLists.txt test/test_update_verify.cpp
git commit -m "feat(tools): offs-release-sign — ed25519 release manifest signing"
```

---

### Task 4: Manifest fetch + parse + per-file SHA256

**Files:**
- Create: `src/Update/update_manifest.h`, `update_manifest.c`
- Modify: `src/Update/update_check.c` — replace the `"sha256:"` markdown scraping (lines 417-439) with manifest fetch
- Modify: `src/Update/update_check.h` — `update_info_t` carries the manifest (list of `{path, sha256[65], size}`)

- [ ] **Step 1: Write the failing tests**

`test/test_update_manifest.cpp`:
```cpp
TEST(UpdateManifest, ParseValidManifest) {
  // A CBOR/JSON manifest: {"version":1,"release_tag":"v1.2.3","files":[{"path":"offs-daemon","sha256":"abc...","size":12345}, ...]}
  // Parse → assert version, tag, file count, per-file sha256/size.
}
TEST(UpdateManifest, RejectsBadVersion) { /* version != 1 → NULL */ }
TEST(UpdateManifest, RejectsMissingFile) { /* a file entry missing sha256 → NULL */ }
```

- [ ] **Step 2: Add the manifest module**

`src/Update/update_manifest.h`:
```c
typedef struct {
  char path[256];
  char sha256[65];
  uint64_t size;
} manifest_file_t;

typedef struct {
  uint32_t version;          // must be 1
  char release_tag[64];
  manifest_file_t* files;
  size_t file_count;
} update_manifest_t;

// Fetch + verify + parse the manifest from a release's assets. Downloads
// manifest.cbor + manifest.cbor.sig from the release (using the same HTTPS
// helper as update_check), verifies the sig with update_verify_manifest,
// parses the CBOR. Returns NULL on any failure (bad sig, bad format, etc.).
// Caller frees with update_manifest_free.
update_manifest_t* update_manifest_fetch(const char* release_tag,
                                          const update_check_config_t* config);
void update_manifest_free(update_manifest_t* manifest);
```

`src/Update/update_manifest.c` — fetch `manifest.cbor` + `manifest.cbor.sig` from the release assets (reuse `_https_get` from update_check.c, or fetch via the asset `browser_download_url`). Verify the sig with `update_verify_manifest`. Parse the CBOR (use libcbor, already a dep) — extract version, release_tag, files[]. Validate version==1, each file has path+sha256+size. Return the struct.

- [ ] **Step 3: Wire into update_check.c**

In `update_check.c:update_check_fetch`, after selecting the release + platform asset, REPLACE the `"sha256:"` markdown scraping (lines 417-439) with a call to `update_manifest_fetch(release_tag, config)`. Store the manifest on the `update_info_t` (add a `update_manifest_t* manifest` field). The single `sha256[65]` field is replaced by the manifest's per-file list — `update_download` will look up the platform asset's sha256 in the manifest.

- [ ] **Step 4: Run tests + commit**

```bash
cd build-test && cmake --build . --target testliboffs && ./test/testliboffs --gtest_filter=UpdateManifest.*
```
Commit:
```bash
git add src/Update/update_manifest.h src/Update/update_manifest.c src/Update/update_check.c src/Update/update_check.h test/test_update_manifest.cpp
git commit -m "feat(update): signed manifest fetch + parse (replaces sha256 markdown scraping)"
```

---

### Task 5: SHA256 mandatory + per-file verification in download

**Files:**
- Modify: `src/Update/update_download.c` — drop the `if (info->sha256[0] != '\0')` opt-out (line 393); look up the file's sha256 in the manifest; fail if not found.

- [ ] **Step 1: Make SHA256 mandatory**

In `update_download.c`, the SHA256 verification at line 393 is gated on `info->sha256[0] != '\0'`. Replace: look up the downloaded file's path in `info->manifest->files[]`; if not found → fail (log + remove the file + return false). Compare the computed SHA256 against the manifest's. No opt-out.

- [ ] **Step 2: Test**

Add to `test/test_update_manifest.cpp` or `test_update_verify.cpp`:
```cpp
TEST(UpdateDownload, RejectsFileNotInManifest) { /* download a file not listed → fail */ }
TEST(UpdateDownload, RejectsHashMismatch) { /* manifest sha256 != computed → fail */ }
```
(These need a mock HTTPS server or a local-file download path; if heavy, test the lookup+compare helper directly.)

- [ ] **Step 3: Run + commit**

```bash
git add src/Update/update_download.c test/test_update_manifest.cpp
git commit -m "fix(update): SHA256 mandatory, verified against signed manifest"
```

---

### Task 6: In-process path-contained tar extraction

**Files:**
- Create: `src/Update/update_extract.h`, `update_extract.c`
- Modify: `src/Update/update_download.c` — replace `execlp("tar",...)` (line 345) with `update_extract`

- [ ] **Step 1: Write the failing tests**

`test/test_update_extract.cpp`:
```cpp
TEST(UpdateExtract, ExtractsValidTarball) { /* build a USTAR tar in memory with 2 files → extract to tmp dir → assert both present + contents */ }
TEST(UpdateExtract, RejectsAbsolutePath) { /* a tar entry "/etc/passwd" → rejected */ }
TEST(UpdateExtract, RejectsDotDot) { /* "../../etc/passwd" → rejected */ }
TEST(UpdateExtract, RejectsSymlinkEscape) { /* a symlink pointing to ../../etc → rejected */ }
TEST(UpdateExtract, RejectsOversized) { /* total size > cap → rejected */ }
```

- [ ] **Step 2: Add the extractor**

`src/Update/update_extract.h`:
```c
// Extract a USTAR tar archive into dest_dir with path containment: reject
// absolute paths, ".." components, and symlinks escaping dest_dir. Cap total
// extracted size at max_bytes. Returns 0 on success, -1 on any violation.
int update_extract(const char* archive_path, const char* dest_dir, uint64_t max_bytes);
```

`src/Update/update_extract.c` — a minimal USTAR reader: open the archive, read 512-byte header blocks, parse `name`, `size` (octal), `typeflag`; for regular files ('0'/'\0'), read `size` bytes (padded to 512), resolve `dest_dir + "/" + name` via `realpath` containment check (reject if the resolved path isn't under `dest_dir`'s canonical path), write the file. For directories ('5'), mkdir. For symlinks ('1'), reject if target resolves outside. For hardlinks ('2'), reject. For unknown types, reject. Stop at a zero block (end of archive). Cap total bytes. No external `tar`.

The containment check: canonicalize `dest_dir` with `realpath` once; for each entry, build `dest_dir + "/" + entry_name`, canonicalize the parent (the entry may not exist yet, so `realpath` the parent + append the basename), and verify the result starts with `dest_dir`'s canonical path + '/'. Reject absolute entry names (leading '/') and any `..` component (string check before realpath, plus realpath confirmation).

- [ ] **Step 3: Replace the tar exec**

In `update_download.c:_extract_archive` (lines 311-352), replace the `fork`/`execlp("tar",...)` with `update_extract(archive_path, dest_dir, MAX_EXTRACT_BYTES)` (define `MAX_EXTRACT_BYTES` to e.g. 1 GiB). Remove the Windows `CreateProcessW` tar path too. Return the `update_extract` result.

- [ ] **Step 4: Run tests + commit**

```bash
cd build-test && cmake --build . --target testliboffs && ./test/testliboffs --gtest_filter=UpdateExtract.*
```
Commit:
```bash
git add src/Update/update_extract.h src/Update/update_extract.c src/Update/update_download.c test/test_update_extract.cpp
git commit -m "feat(update): in-process path-contained USTAR extraction (replaces external tar)"
```

---

### Task 7: Verified staged updater binary

**Files:**
- Modify: `src/Update/update_download.c` — stage `offs-updater` + verify its hash against the manifest
- Modify: `src/Update/update_actor.c:_apply_update` — exec the staged absolute path instead of `execlp("offs-updater",...)`; handle fork/exec failures

- [ ] **Step 1: Stage + verify the updater**

In `update_download.c`, the manifest lists `offs-updater` (the updater is a release asset). The download flow already downloads each manifest-listed file + verifies its hash (Task 5). Ensure `offs-updater` is among the downloaded+verified files (it should be, if it's in the manifest's files[]). The staged path is `<staging_dir>/offs-updater`.

- [ ] **Step 2: Exec the staged updater**

In `update_actor.c:_apply_update` (lines 154-167), replace:
```c
execlp("offs-updater", "offs-updater", ua->staging_dir, ua->install_dir, ua->backup_dir, NULL);
```
with an absolute path to the staged, verified updater:
```c
char staged_updater[OFFS_PATH_MAX];
snprintf(staged_updater, sizeof(staged_updater), "%s/offs-updater", ua->staging_dir);
// Verify the staged updater exists + is executable.
if (!platform_file_exists(staged_updater)) {
  log_error("update_apply: staged updater missing: %s", staged_updater);
  return;  // do NOT exit(0) — the daemon stays alive to retry
}
execl(staged_updater, "offs-updater", ua->staging_dir, ua->install_dir, ua->backup_dir, NULL);
log_error("update_apply: exec failed: %s", strerror(errno));
_exit(127);
```
Also fix the fork-failure path (`pid < 0`): log + return (do not `exit(0)`). And the parent should NOT `exit(0)` immediately — it should wait briefly for the child to confirm the exec succeeded (e.g. a pipe the child closes after exec, or a short sleep + check the staged updater is running). At minimum, log the exec + don't `exit(0)` on fork failure.

- [ ] **Step 3: Test + commit**

Add a test that the staged-updater path is used (mock the staged updater as a script that writes a marker file; assert the marker is created). Commit:
```bash
git add src/Update/update_download.c src/Update/update_actor.c test/test_update_verify.cpp
git commit -m "fix(update): exec staged verified updater (not PATH); handle fork/exec failures"
```

---

### Task 8: `docs/RELEASE.md` + README update

**Files:**
- Create: `docs/RELEASE.md` — the Prometheus-SCN release-signing process
- Modify: `README.md` — document the signed-manifest update flow

- [ ] **Step 1: Write `docs/RELEASE.md`**

Cover: key custody (offline ed25519 private key held by the Prometheus-SCN release operator; public key embedded at build via `OFFS_RELEASE_PUBKEY`); `offs-release-sign --keygen` (one-time key generation); `offs-release-sign --key <priv.pem> --manifest <manifest.cbor>` (per-release signing); manifest format (version, release_tag, files[] with path+sha256+size); publishing `manifest.cbor` + `manifest.cbor.sig` as release assets alongside the binaries on `github.com/Prometheus-SCN/OFFS/releases`; the build-time pubkey embedding for downstream packagers.

- [ ] **Step 2: Update README**

Add a "Self-update" section: the updater fetches a signed manifest over TLS-verified HTTPS, verifies the ed25519 signature against the compiled-in Prometheus-SCN release key, verifies each file's SHA256 against the manifest, and extracts via a path-contained in-process reader. Note the fail-closed behavior if `OFFS_RELEASE_PUBKEY` is unset at build time.

- [ ] **Step 3: Commit**

```bash
git add docs/RELEASE.md README.md
git commit -m "docs: release-signing process + self-update flow"
```

---

### Task 9: Register tests + de-wonk + valgrind

**Files:**
- Modify: `test/CMakeLists.txt` — register `test_update_verify.cpp`, `test_update_manifest.cpp`, `test_update_extract.cpp`

- [ ] **Step 1: Register the test files** in `test/CMakeLists.txt` (the `testliboffs` sources list).

- [ ] **Step 2: De-wonk** — run the de-wonk skill on the Stage 3 changes. Fix any unimplemented/stubbed/disabled/broken/weird code. No TODOs.

- [ ] **Step 3: ASAN + valgrind** — build ASAN + run the Update suite; valgrind (DWARF-4) on the Update suite. 0 leaks, 0 errors.

- [ ] **Step 4: Commit** any de-wonk fixes + the test registration.

---

## Self-Review

**Spec coverage (Section 6):**
- 6.1 TLS verification → Task 1. ✓
- 6.2 ed25519 signed manifest (tool + verify + compile-in pubkey + fail-closed) → Tasks 2, 3, 4. ✓
- 6.3 path-contained extraction → Task 6. ✓
- 6.4 verified updater binary → Task 7. ✓
- 6.5 SHA256 mandatory → Task 5. ✓
- 6.6 tests → each task has tests; Task 9 registers + de-wonks. ✓

**Key risks flagged inline:**
- Task 2's `OFFS_RELEASE_PUBKEY` CMake `file(READ)` embedding — confirm the PEM is embedded as a C string literal (escape backslashes/newlines). Test with a real keypair.
- Task 4's manifest fetch reuses `_https_get` from update_check.c (currently `static`) — either expose it or duplicate the HTTPS fetch in update_manifest.c.
- Task 6's USTAR reader must handle the `prefix[155] + name[100]` concatenation (long names), octal `size` parsing, and the end-of-archive zero blocks. The containment check must use `realpath` on the parent dir (the entry doesn't exist yet). Test the rejection cases thoroughly.
- Task 7's parent-shouldn't-`exit(0)` change: the existing flow `exit(0)`s the daemon so the child takes over. Changing this to keep the daemon alive on exec failure is a behavior change — confirm the update_actor's state machine handles "apply failed, still running" (it should set `update_state_failed` and continue, not crash the daemon).

**Type consistency:** `update_verify_manifest`, `update_manifest_t`, `manifest_file_t`, `update_extract`, `release_sign_*` used consistently across tasks.