//
// Created by victor on 5/26/26.
//

#include "ca_ops.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define DEFAULT_CA_DAYS  3650  /* 10 years */
#define DEFAULT_NODE_DAYS 365  /* 1 year */

static void _print_usage(void) {
  printf(
    "offs-ca — offline certificate authority management\n"
    "\n"
    "Usage:\n"
    "  offs-ca init       [--days N] [--subject SUBJ] [--key-type TYPE]\n"
    "                     --cert PATH --key PATH\n"
    "  offs-ca new-node   <name> [--key-type TYPE] [--key PATH] [--csr PATH]\n"
    "  offs-ca sign       --csr PATH --ca-cert PATH --ca-key PATH\n"
    "                     [--days N] --cert PATH\n"
    "\n"
    "Subcommands:\n"
    "  init       Generate a self-signed CA certificate and key\n"
    "  new-node   Generate a node key and CSR\n"
    "  sign       Sign a CSR with the CA, producing a node certificate\n"
    "\n"
    "Key types:\n"
    "  ed25519 (default), rsa (2048-bit), rsa4096,\n"
    "  prime256v1, secp384r1, secp521r1\n"
  );
}

static int _cmd_init(int argc, char** argv) {
  const char* cert_path = NULL;
  const char* key_path = NULL;
  const char* subject = "/CN=offs-ca";
  const char* key_type = NULL;
  int days = DEFAULT_CA_DAYS;

  for (int i = 0; i < argc; i++) {
    if (strcmp(argv[i], "--cert") == 0 && i + 1 < argc) {
      cert_path = argv[++i];
    } else if (strcmp(argv[i], "--key") == 0 && i + 1 < argc) {
      key_path = argv[++i];
    } else if (strcmp(argv[i], "--days") == 0 && i + 1 < argc) {
      days = atoi(argv[++i]);
      if (days <= 0) {
        fprintf(stderr, "Error: --days must be a positive integer\n");
        return 1;
      }
    } else if (strcmp(argv[i], "--subject") == 0 && i + 1 < argc) {
      subject = argv[++i];
    } else if (strcmp(argv[i], "--key-type") == 0 && i + 1 < argc) {
      key_type = argv[++i];
    }
  }

  if (!cert_path || !key_path) {
    fprintf(stderr, "Error: --cert and --key are required\n");
    return 1;
  }

  if (ca_generate(subject, days, key_type, cert_path, key_path) != 0) {
    return 1;
  }

  printf("CA certificate: %s\nCA key: %s\n", cert_path, key_path);
  return 0;
}

static int _cmd_new_node(int argc, char** argv) {
  if (argc < 1 || argv[0][0] == '-') {
    fprintf(stderr, "Error: node name is required\n");
    return 1;
  }

  const char* name = argv[0];
  int arg_offset = 1;

  /* Build default output paths from the name */
  char* default_key = malloc(strlen(name) + 10);
  char* default_csr = malloc(strlen(name) + 6);
  if (!default_key || !default_csr) {
    free(default_key);
    free(default_csr);
    fprintf(stderr, "Error: out of memory\n");
    return 1;
  }
  sprintf(default_key, "%s_key.pem", name);
  sprintf(default_csr, "%s.csr", name);

  const char* key_path = default_key;
  const char* csr_path = default_csr;
  const char* key_type = NULL;

  for (int i = arg_offset; i < argc; i++) {
    if (strcmp(argv[i], "--key") == 0 && i + 1 < argc) {
      key_path = argv[++i];
    } else if (strcmp(argv[i], "--csr") == 0 && i + 1 < argc) {
      csr_path = argv[++i];
    } else if (strcmp(argv[i], "--key-type") == 0 && i + 1 < argc) {
      key_type = argv[++i];
    }
  }

  int ret = ca_generate_csr(name, key_type, key_path, csr_path);

  if (ret == 0) {
    printf("Node key: %s\nCSR: %s\n", key_path, csr_path);
  }

  free(default_key);
  free(default_csr);
  return ret != 0;
}

static int _cmd_sign(int argc, char** argv) {
  const char* csr_path = NULL;
  const char* ca_cert_path = NULL;
  const char* ca_key_path = NULL;
  const char* cert_path = NULL;
  int days = DEFAULT_NODE_DAYS;

  for (int i = 0; i < argc; i++) {
    if (strcmp(argv[i], "--csr") == 0 && i + 1 < argc) {
      csr_path = argv[++i];
    } else if (strcmp(argv[i], "--ca-cert") == 0 && i + 1 < argc) {
      ca_cert_path = argv[++i];
    } else if (strcmp(argv[i], "--ca-key") == 0 && i + 1 < argc) {
      ca_key_path = argv[++i];
    } else if (strcmp(argv[i], "--cert") == 0 && i + 1 < argc) {
      cert_path = argv[++i];
    } else if (strcmp(argv[i], "--days") == 0 && i + 1 < argc) {
      days = atoi(argv[++i]);
      if (days <= 0) {
        fprintf(stderr, "Error: --days must be a positive integer\n");
        return 1;
      }
    }
  }

  if (!csr_path || !ca_cert_path || !ca_key_path || !cert_path) {
    fprintf(stderr, "Error: --csr, --ca-cert, --ca-key, and --cert are required\n");
    return 1;
  }

  if (ca_sign_csr(csr_path, ca_cert_path, ca_key_path, days, cert_path) != 0) {
    return 1;
  }

  printf("Signed certificate: %s\n", cert_path);
  return 0;
}

int main(int argc, char** argv) {
  if (argc < 2) {
    _print_usage();
    return 1;
  }

  const char* cmd = argv[1];
  int remaining = argc - 2;
  char** args = argv + 2;

  if (strcmp(cmd, "init") == 0) {
    return _cmd_init(remaining, args);
  } else if (strcmp(cmd, "new-node") == 0) {
    return _cmd_new_node(remaining, args);
  } else if (strcmp(cmd, "sign") == 0) {
    return _cmd_sign(remaining, args);
  } else if (strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0) {
    _print_usage();
    return 0;
  } else {
    fprintf(stderr, "Error: unknown command '%s'\n", cmd);
    _print_usage();
    return 1;
  }
}
