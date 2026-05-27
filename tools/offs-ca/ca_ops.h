//
// Created by victor on 5/26/26.
//

#ifndef OFFS_CA_OPS_H
#define OFFS_CA_OPS_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Generate a self-signed CA certificate and private key.
 *
 * @param subject_name  X509 subject name, e.g. "/CN=MyCA/O=MyOrg"
 * @param days_valid    Number of days until expiry
 * @param key_type      Key type: "ed25519", "rsa", "rsa4096", "prime256v1",
 *                      "secp384r1", "secp521r1". NULL defaults to "ed25519".
 * @param cert_path     Output path for the PEM certificate
 * @param key_path      Output path for the PEM private key
 * @return 0 on success, -1 on failure
 */
int ca_generate(const char* subject_name, int days_valid,
                const char* key_type,
                const char* cert_path, const char* key_path);

/**
 * Generate a node private key and Certificate Signing Request.
 *
 * @param common_name   Common Name for the node (CN=)
 * @param key_type      Key type: "ed25519", "rsa", "rsa4096", "prime256v1",
 *                      "secp384r1", "secp521r1". NULL defaults to "ed25519".
 * @param key_path      Output path for the PEM private key
 * @param csr_path      Output path for the PEM CSR
 * @return 0 on success, -1 on failure
 */
int ca_generate_csr(const char* common_name, const char* key_type,
                    const char* key_path, const char* csr_path);

/**
 * Sign a CSR with a CA certificate and key, producing a signed node certificate.
 *
 * @param csr_path      Path to the PEM CSR to sign
 * @param ca_cert_path  Path to the CA certificate PEM
 * @param ca_key_path   Path to the CA private key PEM
 * @param days_valid    Number of days until expiry
 * @param cert_path     Output path for the signed PEM certificate
 * @return 0 on success, -1 on failure
 */
int ca_sign_csr(const char* csr_path, const char* ca_cert_path,
                const char* ca_key_path, int days_valid,
                const char* cert_path);

#ifdef __cplusplus
}
#endif

#endif /* OFFS_CA_OPS_H */
