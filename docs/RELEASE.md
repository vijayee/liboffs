# Release Signing Process

This document describes how the Prometheus-SCN release operator signs release artifacts so that `offsd`'s self-update path can cryptographically verify them.

## Trust model

- The **Prometheus-SCN release operator** holds an **offline ed25519 private key**. This key never lives on a networked machine; it is generated and used on an air-gapped or otherwise isolated host.
- The corresponding **public key** is embedded in the `offsd` binary at build time (see [Build-time public key](#build-time-public-key)). Every running `offsd` verifies release manifests against this compiled-in key.
- Release artifacts live on `github.com/Prometheus-SCN/OFFS/releases` (the daemon repo). The updater fetches them over TLS-verified HTTPS, so the GitHub release is the distribution point, and the signature is the trust anchor (not the transport).

## One-time: generate the release keypair

On an isolated host with OpenSSL 3.x + the `offs-release-sign` tool built:

```bash
offs-release-sign --keygen --priv release-priv.pem --pub release-pub.pem
```

- `release-priv.pem` — the private key. Keep it offline. Back it up to durable offline media. This is the key that signs every release.
- `release-pub.pem` — the public key. This is what gets compiled into `offsd` (next section).

If the private key is ever compromised, rotate: generate a new keypair, ship a new `offsd` build compiled with the new public key, and (in a coordinated release) the new `offsd` will reject manifests signed by the old key. There is no key-revocation list; rotation is via a new signed release.

## Build-time public key

Downstream packagers and the Prometheus-SCN CI build `offsd` with the release public key compiled in:

```bash
cmake .. -DOFFS_RELEASE_PUBKEY=/path/to/release-pub.pem
cmake --build .
```

CMake reads the PEM at configure time and embeds it as a string literal (`OFFS_RELEASE_PUBKEY`) compiled into the `offs` library. The update verifier (`update_verify_load_pubkey`) returns this key; every manifest signature is checked against it.

**Fail-closed:** if `OFFS_RELEASE_PUBKEY` is not set at build time, CMake emits a warning and the verifier has no compiled-in key — `update_verify_manifest` returns false for every signature, so self-update refuses to install anything. A build without the release key can still run, but it cannot self-update.

## Per-release: sign the manifest

For each release (e.g. `v1.2.3`):

1. **Build the release artifacts** for each platform: `offs-daemon`, `offs-cli`, `offs-updater` (and any other distributable files). Place them in a release staging directory.

2. **Compute SHA256 + sizes** for every file and build the manifest. The manifest is a CBOR array:
   ```
   [version:1, release_tag:"v1.2.3", files:[[path, sha256, size], ...]]
   ```
   - `version` — must be `1`.
   - `release_tag` — the release tag (e.g. `"v1.2.3"`).
   - `files` — array of `[path, sha256, size]` triples, one per distributable file. `path` is the asset basename (e.g. `"offs-daemon"`); `sha256` is the lowercase-hex 64-char digest; `size` is the file size in bytes.

   A helper to build the manifest CBOR from a directory of files is planned; for now, build it with a small script using libcbor (or `cbor.py` / `pycbor`) that walks the staging directory, hashes each file (`sha256sum`), and emits the CBOR array. Save as `manifest.cbor`.

3. **Sign the manifest** with the offline private key:
   ```bash
   offs-release-sign --key release-priv.pem --manifest manifest.cbor
   ```
   This writes `manifest.cbor.sig` — the raw 64-byte ed25519 signature.

4. **Publish the release** on `github.com/Prometheus-SCN/OFFS/releases` (tag `v1.2.3`). Upload as release assets:
   - `manifest.cbor`
   - `manifest.cbor.sig`
   - `offs-daemon`, `offs-cli`, `offs-updater` (per-platform; name them with the platform suffix the updater expects, e.g. `offs-daemon-linux-x64`).

   The release body markdown no longer needs a `sha256:` line — the manifest is the source of truth for hashes.

## How the updater verifies a release

When `offsd` checks for updates (`update_check_fetch`):

1. Fetch the release's `manifest.cbor` + `manifest.cbor.sig` assets over TLS-verified HTTPS (system CA store; `SSL_VERIFY_PEER`).
2. Verify the ed25519 signature with `update_verify_manifest` using the compiled-in `OFFS_RELEASE_PUBKEY`. Fail-closed on any error.
3. Parse the manifest CBOR; validate `version == 1` + every file entry has `path` + `sha256` + `size`.

When `offsd` downloads an update (`update_download`):

4. For each manifest-listed file, download it over TLS-verified HTTPS, compute its SHA256, and compare against the manifest. Mismatch → reject + delete the staged file. No opt-out: a file not in the manifest, or a hash mismatch, aborts the update.
5. Extract any archive via the in-process USTAR reader (`update_extract`) with path containment — rejects absolute paths, `..`, symlinks, hardlinks, and oversized archives.
6. Exec the staged `offs-updater` (absolute path, hash-verified) — NOT a PATH lookup — which copies the new `offs-daemon`/`offs-cli` into the install dir and restarts the service.

A MITM (blocked by TLS), a tampered release asset (blocked by the per-file SHA256 vs the signed manifest), or a malicious release author without the private key (blocked by the ed25519 signature) cannot achieve arbitrary code execution via the update path.

## See also

- `docs/superpowers/specs/2026-08-22-production-readiness-fixes-design.md` Section 6 — the design spec for the self-update supply chain.
- `docs/superpowers/plans/2026-08-22-self-update-supply-chain.md` — the implementation plan.
- `tools/offs-release-sign/` — the signing tool source.