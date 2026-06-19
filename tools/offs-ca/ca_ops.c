//
// Created by victor on 5/26/26.
//

#include "ca_ops.h"

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/bn.h>
#include <openssl/asn1.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>
#include <openssl/ec.h>
#include "../../src/Platform/platform_posix_compat.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ---- key type helpers ---- */

typedef struct {
  int pkey_nid;
  int ec_curve_nid;  /* NID_undef if not EC */
  int rsa_bits;      /* 0 if not RSA */
} key_type_info_t;

static const key_type_info_t* _lookup_key_type(const char* key_type) {
  static const key_type_info_t types[] = {
    { EVP_PKEY_ED25519, NID_undef,        0 },
    { EVP_PKEY_RSA,     NID_undef,     2048 },
    { EVP_PKEY_RSA,     NID_undef,     4096 },
    { EVP_PKEY_EC,      NID_X9_62_prime256v1, 0 },
    { EVP_PKEY_EC,      NID_secp384r1,       0 },
    { EVP_PKEY_EC,      NID_secp521r1,       0 },
  };
  static const char* names[] = {
    "ed25519", "rsa", "rsa4096", "prime256v1", "secp384r1", "secp521r1"
  };

  if (!key_type) return &types[0];  /* default: ed25519 */

  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
    if (strcmp(key_type, names[i]) == 0) return &types[i];
  }
  return NULL;
}

static EVP_PKEY* _generate_key(const key_type_info_t* info) {
  if (!info) return NULL;

  EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(info->pkey_nid, NULL);
  if (!ctx) {
    fprintf(stderr, "Failed to create key context\n");
    return NULL;
  }

  if (EVP_PKEY_keygen_init(ctx) <= 0) {
    fprintf(stderr, "Keygen init failed\n");
    ERR_print_errors_fp(stderr);
    EVP_PKEY_CTX_free(ctx);
    return NULL;
  }

  if (info->pkey_nid == EVP_PKEY_RSA) {
    EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, info->rsa_bits);
  } else if (info->pkey_nid == EVP_PKEY_EC) {
    EVP_PKEY_CTX_set_ec_paramgen_curve_nid(ctx, info->ec_curve_nid);
  }

  EVP_PKEY* key = NULL;
  if (EVP_PKEY_keygen(ctx, &key) <= 0) {
    fprintf(stderr, "Key generation failed\n");
    ERR_print_errors_fp(stderr);
  }
  EVP_PKEY_CTX_free(ctx);
  return key;
}

static const EVP_MD* _digest_for_key(EVP_PKEY* key) {
  if (!key) return NULL;
  int nid = EVP_PKEY_get_id(key);
  if (nid == EVP_PKEY_ED25519 || nid == EVP_PKEY_ED448) return NULL;
  return EVP_sha256();
}

/* ---- helpers ---- */

static int _write_pem(void* obj, int type, const char* path) {
  BIO* bio = BIO_new_file(path, "w");
  if (!bio) {
    fprintf(stderr, "Failed to open %s for writing\n", path);
    return -1;
  }
  int ret;
  if (type == 0) {
    ret = PEM_write_bio_PrivateKey(bio, (EVP_PKEY*)obj, NULL, NULL, 0, NULL, NULL);
  } else if (type == 1) {
    ret = PEM_write_bio_X509(bio, (X509*)obj);
  } else {
    ret = PEM_write_bio_X509_REQ(bio, (X509_REQ*)obj);
  }
  BIO_free(bio);
  if (!ret) {
    fprintf(stderr, "Failed to write PEM to %s\n", path);
    return -1;
  }
  return 0;
}

static EVP_PKEY* _read_key(const char* path, int is_private) {
  BIO* bio = BIO_new_file(path, "r");
  if (!bio) {
    fprintf(stderr, "Failed to open %s for reading\n", path);
    return NULL;
  }
  EVP_PKEY* key;
  if (is_private) {
    key = PEM_read_bio_PrivateKey(bio, NULL, NULL, NULL);
  } else {
    key = PEM_read_bio_PUBKEY(bio, NULL, NULL, NULL);
  }
  BIO_free(bio);
  if (!key) {
    fprintf(stderr, "Failed to read key from %s\n", path);
  }
  return key;
}

static X509* _read_cert(const char* path) {
  BIO* bio = BIO_new_file(path, "r");
  if (!bio) {
    fprintf(stderr, "Failed to open %s for reading\n", path);
    return NULL;
  }
  X509* cert = PEM_read_bio_X509(bio, NULL, NULL, NULL);
  BIO_free(bio);
  if (!cert) {
    fprintf(stderr, "Failed to read certificate from %s\n", path);
  }
  return cert;
}

static X509_REQ* _read_csr(const char* path) {
  BIO* bio = BIO_new_file(path, "r");
  if (!bio) {
    fprintf(stderr, "Failed to open %s for reading\n", path);
    return NULL;
  }
  X509_REQ* csr = PEM_read_bio_X509_REQ(bio, NULL, NULL, NULL);
  BIO_free(bio);
  if (!csr) {
    fprintf(stderr, "Failed to read CSR from %s\n", path);
  }
  return csr;
}

static int _set_subject_name(X509_NAME* name, const char* subject_string) {
  /* Parse subject like "/CN=foo/O=bar/OU=baz" */
  if (!subject_string) return -1;
  int entries = 0;
  const char* cursor = subject_string;
  while (*cursor) {
    if (*cursor == '/') cursor++;
    if (*cursor == '\0') break;
    const char* eq = strchr(cursor, '=');
    if (!eq) {
      fprintf(stderr, "Malformed subject entry (missing '='): %s\n", cursor);
      return -1;
    }
    size_t key_len = (size_t)(eq - cursor);
    const char* val = eq + 1;
    const char* next = strchr(val, '/');
    size_t val_len = next ? (size_t)(next - val) : strlen(val);
    if (key_len == 0 || val_len == 0) {
      fprintf(stderr, "Malformed subject entry (empty key or value): %s\n", cursor);
      return -1;
    }
    char key[16];
    size_t copy_key = key_len < sizeof(key) - 1 ? key_len : sizeof(key) - 1;
    memcpy(key, cursor, copy_key);
    key[copy_key] = '\0';
    char* value_str = strndup(val, val_len);
    if (!value_str) return -1;
    int rc = X509_NAME_add_entry_by_txt(name, key, MBSTRING_ASC,
                                        (const unsigned char*)value_str, -1, -1, 0);
    free(value_str);
    if (!rc) return -1;
    entries++;
    cursor = next ? next : val + val_len;
  }
  if (entries == 0) {
    fprintf(stderr, "No valid subject entries parsed from: %s\n", subject_string);
    return -1;
  }
  return 0;
}

static void _set_random_serial(X509* cert) {
  uint8_t bytes[8];
  BIGNUM* bn = NULL;
  ASN1_INTEGER* serial = NULL;
  if (RAND_bytes(bytes, sizeof(bytes)) == 1) {
    bn = BN_bin2bn(bytes, sizeof(bytes), NULL);
  }
  if (bn) {
    serial = BN_to_ASN1_INTEGER(bn, NULL);
    BN_free(bn);
  }
  if (serial) {
    X509_set_serialNumber(cert, serial);
    ASN1_INTEGER_free(serial);
  } else {
    ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
  }
}

static int _set_subject_cn(X509_NAME* name, const char* common_name) {
  return X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                    (const unsigned char*)common_name, -1, -1, 0);
}

static void _add_ext(X509* cert, int nid, const char* value) {
  X509V3_CTX ctx;
  X509V3_set_ctx_nodb(&ctx);
  X509V3_set_ctx(&ctx, cert, cert, NULL, NULL, 0);
  X509_EXTENSION* ext = X509V3_EXT_conf_nid(NULL, &ctx, nid, value);
  if (ext) {
    X509_add_ext(cert, ext, -1);
    X509_EXTENSION_free(ext);
  }
}

/* ---- public API ---- */

int ca_generate(const char* subject_name, int days_valid,
                const char* key_type,
                const char* cert_path, const char* key_path) {
  if (!subject_name || !cert_path || !key_path) return -1;

  const key_type_info_t* info = _lookup_key_type(key_type);
  if (!info) {
    fprintf(stderr, "Unknown key type: %s\n", key_type);
    return -1;
  }

  EVP_PKEY* key = _generate_key(info);
  if (!key) return -1;

  X509* cert = X509_new();
  if (!cert) {
    EVP_PKEY_free(key);
    return -1;
  }

  _set_random_serial(cert);
  X509_gmtime_adj(X509_getm_notBefore(cert), 0);
  X509_gmtime_adj(X509_getm_notAfter(cert), (long)days_valid * 86400L);

  X509_set_pubkey(cert, key);

  X509_NAME* name = X509_get_subject_name(cert);
  if (_set_subject_name(name, subject_name) != 0) {
    EVP_PKEY_free(key);
    X509_free(cert);
    return -1;
  }

  X509_set_issuer_name(cert, name); /* self-signed */

  _add_ext(cert, NID_basic_constraints, "critical,CA:TRUE");
  _add_ext(cert, NID_key_usage, "critical,keyCertSign,cRLSign");
  _add_ext(cert, NID_subject_key_identifier, "hash");

  if (!X509_sign(cert, key, _digest_for_key(key))) {
    fprintf(stderr, "CA certificate signing failed\n");
    ERR_print_errors_fp(stderr);
    EVP_PKEY_free(key);
    X509_free(cert);
    return -1;
  }

  int ret = _write_pem(key, 0, key_path);
  if (ret == 0) ret = _write_pem(cert, 1, cert_path);

  EVP_PKEY_free(key);
  X509_free(cert);
  return ret;
}

int ca_generate_csr(const char* common_name, const char* key_type,
                    const char* key_path, const char* csr_path) {
  if (!common_name || !key_path || !csr_path) return -1;

  const key_type_info_t* info = _lookup_key_type(key_type);
  if (!info) {
    fprintf(stderr, "Unknown key type: %s\n", key_type);
    return -1;
  }

  EVP_PKEY* key = _generate_key(info);
  if (!key) return -1;

  X509_REQ* csr = X509_REQ_new();
  if (!csr) {
    EVP_PKEY_free(key);
    return -1;
  }

  X509_REQ_set_pubkey(csr, key);

  X509_NAME* name = X509_REQ_get_subject_name(csr);
  if (!_set_subject_cn(name, common_name)) {
    fprintf(stderr, "Failed to set CSR subject CN\n");
    EVP_PKEY_free(key);
    X509_REQ_free(csr);
    return -1;
  }

  if (!X509_REQ_sign(csr, key, _digest_for_key(key))) {
    fprintf(stderr, "CSR signing failed\n");
    ERR_print_errors_fp(stderr);
    EVP_PKEY_free(key);
    X509_REQ_free(csr);
    return -1;
  }

  int ret = _write_pem(key, 0, key_path);
  if (ret == 0) ret = _write_pem(csr, 2, csr_path);

  EVP_PKEY_free(key);
  X509_REQ_free(csr);
  return ret;
}

int ca_sign_csr(const char* csr_path, const char* ca_cert_path,
                const char* ca_key_path, int days_valid,
                const char* cert_path) {
  if (!csr_path || !ca_cert_path || !ca_key_path || !cert_path) return -1;

  X509_REQ* csr = _read_csr(csr_path);
  if (!csr) return -1;

  /* Verify CSR signature */
  EVP_PKEY* csr_pubkey = X509_REQ_get0_pubkey(csr);
  if (!csr_pubkey) {
    X509_REQ_free(csr);
    return -1;
  }
  if (X509_REQ_verify(csr, csr_pubkey) != 1) {
    fprintf(stderr, "CSR signature verification failed\n");
    X509_REQ_free(csr);
    return -1;
  }

  X509* ca_cert = _read_cert(ca_cert_path);
  if (!ca_cert) {
    X509_REQ_free(csr);
    return -1;
  }

  EVP_PKEY* ca_key = _read_key(ca_key_path, 1);
  if (!ca_key) {
    X509_free(ca_cert);
    X509_REQ_free(csr);
    return -1;
  }

  X509* cert = X509_new();
  if (!cert) {
    X509_free(ca_cert);
    EVP_PKEY_free(ca_key);
    X509_REQ_free(csr);
    return -1;
  }

  _set_random_serial(cert);
  X509_gmtime_adj(X509_getm_notBefore(cert), 0);
  X509_gmtime_adj(X509_getm_notAfter(cert), (long)days_valid * 86400L);

  X509_set_pubkey(cert, csr_pubkey);

  X509_NAME* subj = X509_REQ_get_subject_name(csr);
  X509_set_subject_name(cert, subj);
  X509_set_issuer_name(cert, X509_get_subject_name(ca_cert));

  _add_ext(cert, NID_basic_constraints, "critical,CA:FALSE");
  _add_ext(cert, NID_key_usage, "critical,digitalSignature,keyEncipherment");
  _add_ext(cert, NID_ext_key_usage, "serverAuth,clientAuth");

  if (!X509_sign(cert, ca_key, _digest_for_key(ca_key))) {
    fprintf(stderr, "Certificate signing failed\n");
    ERR_print_errors_fp(stderr);
    X509_free(ca_cert);
    EVP_PKEY_free(ca_key);
    X509_REQ_free(csr);
    X509_free(cert);
    return -1;
  }

  int ret = _write_pem(cert, 1, cert_path);

  X509_free(ca_cert);
  EVP_PKEY_free(ca_key);
  X509_REQ_free(csr);
  X509_free(cert);
  return ret;
}
