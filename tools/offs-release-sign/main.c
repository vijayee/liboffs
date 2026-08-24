// offs-release-sign: offline ed25519 release manifest signing CLI.
//
// Used by the Prometheus-SCN release operator to sign release manifests with
// an offline ed25519 private key. Generates keypairs (--keygen) or signs a
// manifest file (--key + --manifest), writing the raw 64-byte ed25519
// signature to <manifest>.sig.

#include "sign_ops.h"

#include <stdio.h>
#include <string.h>

static void _print_usage(void) {
  printf(
    "offs-release-sign — offline ed25519 release manifest signing\n"
    "\n"
    "Usage:\n"
    "  offs-release-sign --keygen --priv <priv.pem> --pub <pub.pem>\n"
    "  offs-release-sign --key <priv.pem> --manifest <manifest.cbor>\n"
    "\n"
    "Modes:\n"
    "  --keygen   Generate a new ed25519 keypair. Requires --priv and --pub.\n"
    "  --key      Sign a manifest with the given ed25519 private key. Requires\n"
    "             --manifest; writes the raw 64-byte signature to\n"
    "             <manifest>.sig.\n"
  );
}

int main(int argc, char** argv) {
  const char* priv_path = NULL;
  const char* pub_path = NULL;
  const char* manifest_path = NULL;
  int keygen = 0;
  int sign_mode = 0;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--keygen") == 0) {
      keygen = 1;
    } else if (strcmp(argv[i], "--key") == 0 && i + 1 < argc) {
      priv_path = argv[++i];
      sign_mode = 1;
    } else if (strcmp(argv[i], "--priv") == 0 && i + 1 < argc) {
      priv_path = argv[++i];
    } else if (strcmp(argv[i], "--pub") == 0 && i + 1 < argc) {
      pub_path = argv[++i];
    } else if (strcmp(argv[i], "--manifest") == 0 && i + 1 < argc) {
      manifest_path = argv[++i];
    } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
      _print_usage();
      return 0;
    } else {
      fprintf(stderr, "Error: unknown argument '%s'\n", argv[i]);
      _print_usage();
      return 1;
    }
  }

  if (keygen && sign_mode) {
    fprintf(stderr, "Error: --keygen and --key are mutually exclusive\n");
    return 1;
  }

  if (keygen) {
    if (!priv_path || !pub_path) {
      fprintf(stderr, "Error: --keygen requires --priv and --pub\n");
      _print_usage();
      return 1;
    }
    if (release_sign_keygen(priv_path, pub_path) != 0) {
      return 1;
    }
    printf("Private key: %s\nPublic key: %s\n", priv_path, pub_path);
    return 0;
  }

  if (sign_mode) {
    if (!priv_path || !manifest_path) {
      fprintf(stderr, "Error: --key requires --manifest\n");
      _print_usage();
      return 1;
    }
    if (release_sign_sign(priv_path, manifest_path) != 0) {
      return 1;
    }
    printf("Signed %s → %s.sig\n", manifest_path, manifest_path);
    return 0;
  }

  _print_usage();
  return 1;
}