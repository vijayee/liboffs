//
// Created by victor on 5/28/25.
//

#include "update_download.h"

#include "../Util/allocator.h"
#include "../Util/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
  #define CLOSE_SOCKET(fd) closesocket(fd)
#else
  #include <sys/socket.h>
  #include <netdb.h>
  #include <unistd.h>
  #define CLOSE_SOCKET(fd) close(fd)
#endif

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/evp.h>

#define FILE_READ_CHUNK_SIZE 8192

/* ---------------------------------------------------------------------------
 * Static helpers – SHA256
 * --------------------------------------------------------------------------- */

static bool _compute_sha256(const char* filepath, char out_hex[65]) {
  FILE* file = fopen(filepath, "rb");
  if (file == NULL) {
    return false;
  }

  EVP_MD_CTX* md_context = EVP_MD_CTX_new();
  if (md_context == NULL) {
    fclose(file);
    return false;
  }

  if (EVP_DigestInit_ex(md_context, EVP_sha256(), NULL) != 1) {
    EVP_MD_CTX_free(md_context);
    fclose(file);
    return false;
  }

  unsigned char buffer[FILE_READ_CHUNK_SIZE];
  size_t bytes_read = 0;

  while ((bytes_read = fread(buffer, 1, sizeof(buffer), file)) > 0) {
    EVP_DigestUpdate(md_context, buffer, bytes_read);
  }

  fclose(file);

  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int digest_length = 0;

  if (EVP_DigestFinal_ex(md_context, digest, &digest_length) != 1) {
    EVP_MD_CTX_free(md_context);
    return false;
  }

  EVP_MD_CTX_free(md_context);

  for (unsigned int index = 0; index < digest_length; index++) {
    snprintf(out_hex + (index * 2), 3, "%02x", digest[index]);
  }
  out_hex[64] = '\0';

  return true;
}

/* ---------------------------------------------------------------------------
 * Static helpers – HTTPS download
 * --------------------------------------------------------------------------- */

static void _https_free_context(SSL_CTX* ssl_context, SSL* ssl_connection,
                                int socket_fd) {
  if (ssl_connection != NULL) {
    SSL_shutdown(ssl_connection);
    SSL_free(ssl_connection);
  }
  if (ssl_context != NULL) {
    SSL_CTX_free(ssl_context);
  }
  if (socket_fd >= 0) {
    CLOSE_SOCKET(socket_fd);
  }
}

static void _parse_url(const char* url, char* host, size_t host_size,
                       char* path, size_t path_size) {
  const char* cursor = url;

  if (strncmp(cursor, "https://", 8) == 0) {
    cursor += 8;
  }

  const char* slash_pos = strchr(cursor, '/');
  size_t host_len = 0;

  if (slash_pos != NULL) {
    host_len = (size_t)(slash_pos - cursor);
  } else {
    host_len = strlen(cursor);
  }

  if (host_len >= host_size) {
    host_len = host_size - 1;
  }
  memcpy(host, cursor, host_len);
  host[host_len] = '\0';

  if (slash_pos != NULL) {
    const char* path_start = slash_pos;
    size_t path_len = strlen(path_start);
    if (path_len >= path_size) {
      path_len = path_size - 1;
    }
    memcpy(path, path_start, path_len);
    path[path_len] = '\0';
  } else {
    path[0] = '/';
    path[1] = '\0';
  }
}

static bool _download_file(const char* url, const char* token,
                           const char* output_path) {
  char host[256];
  char path[2048];
  _parse_url(url, host, sizeof(host), path, sizeof(path));

  struct addrinfo hints;
  struct addrinfo* address_info = NULL;
  int socket_fd = -1;
  SSL_CTX* ssl_context = NULL;
  SSL* ssl_connection = NULL;
  char* request = NULL;
  char* header_buffer = NULL;
  bool success = false;

  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  int getaddr_result = getaddrinfo(host, "443", &hints, &address_info);
  if (getaddr_result != 0 || address_info == NULL) {
    _https_free_context(ssl_context, ssl_connection, socket_fd);
    return false;
  }

  socket_fd = socket(address_info->ai_family, address_info->ai_socktype,
                     address_info->ai_protocol);
  if (socket_fd < 0) {
    freeaddrinfo(address_info);
    _https_free_context(ssl_context, ssl_connection, socket_fd);
    return false;
  }

  if (connect(socket_fd, address_info->ai_addr, address_info->ai_addrlen) < 0) {
    freeaddrinfo(address_info);
    _https_free_context(ssl_context, ssl_connection, socket_fd);
    return false;
  }

  freeaddrinfo(address_info);
  address_info = NULL;

  ssl_context = SSL_CTX_new(TLS_client_method());
  if (ssl_context == NULL) {
    _https_free_context(ssl_context, ssl_connection, socket_fd);
    return false;
  }

  ssl_connection = SSL_new(ssl_context);
  if (ssl_connection == NULL) {
    _https_free_context(ssl_context, ssl_connection, socket_fd);
    return false;
  }

  SSL_set_fd(ssl_connection, socket_fd);

  if (SSL_connect(ssl_connection) != 1) {
    _https_free_context(ssl_context, ssl_connection, socket_fd);
    return false;
  }

  /* Build and send HTTP request */
  size_t request_size = strlen(host) + strlen(path) + 512;
  request = get_memory(request_size);

  if (token != NULL && token[0] != '\0') {
    snprintf(request, request_size,
             "GET %s HTTP/1.1\r\n"
             "Host: %s\r\n"
             "User-Agent: offs-updater/1.0\r\n"
             "Accept: application/octet-stream\r\n"
             "Authorization: Bearer %s\r\n"
             "Connection: close\r\n"
             "\r\n",
             path, host, token);
  } else {
    snprintf(request, request_size,
             "GET %s HTTP/1.1\r\n"
             "Host: %s\r\n"
             "User-Agent: offs-updater/1.0\r\n"
             "Accept: application/octet-stream\r\n"
             "Connection: close\r\n"
             "\r\n",
             path, host);
  }

  SSL_write(ssl_connection, request, strlen(request));
  free(request);
  request = NULL;

  /* Read headers until we find \r\n\r\n */
  size_t header_capacity = 8192;
  size_t header_length = 0;
  header_buffer = get_memory(header_capacity);

  int bytes_read = 0;
  while ((bytes_read = SSL_read(ssl_connection,
                                header_buffer + header_length,
                                1)) > 0) {
    header_length += (size_t)bytes_read;

    if (header_length >= 4) {
      char* body_separator = strstr(header_buffer, "\r\n\r\n");
      if (body_separator != NULL) {
        break;
      }
    }

    if (header_length + 1 >= header_capacity) {
      /* Response too large or no body separator found */
      free(header_buffer);
      _https_free_context(ssl_context, ssl_connection, socket_fd);
      return false;
    }
  }

  /* Find the start of the body */
  char* body_separator = strstr(header_buffer, "\r\n\r\n");
  if (body_separator == NULL) {
    free(header_buffer);
    _https_free_context(ssl_context, ssl_connection, socket_fd);
    return false;
  }

  body_separator += 4;

  /* Open output file */
  FILE* output_file = fopen(output_path, "wb");
  if (output_file == NULL) {
    free(header_buffer);
    _https_free_context(ssl_context, ssl_connection, socket_fd);
    return false;
  }

  /* Write any bytes after \r\n\r\n that are already in the buffer */
  size_t remaining_header = header_length -
    (size_t)(body_separator - header_buffer);
  if (remaining_header > 0) {
    fwrite(body_separator, 1, remaining_header, output_file);
  }

  free(header_buffer);
  header_buffer = NULL;

  /* Stream the rest of the body directly to file */
  unsigned char file_buffer[FILE_READ_CHUNK_SIZE];
  while ((bytes_read = SSL_read(ssl_connection, file_buffer,
                                sizeof(file_buffer))) > 0) {
    fwrite(file_buffer, 1, (size_t)bytes_read, output_file);
  }

  fclose(output_file);
  _https_free_context(ssl_context, ssl_connection, socket_fd);

  success = true;
  return success;
}

/* ---------------------------------------------------------------------------
 * Static helpers – archive extraction
 * --------------------------------------------------------------------------- */

static bool _extract_archive(const char* archive_path, const char* dest_dir) {
  char command[1024];

#ifdef _WIN32
  snprintf(command, sizeof(command), "tar -xf \"%s\" -C \"%s\"",
           archive_path, dest_dir);
#else
  snprintf(command, sizeof(command), "tar -xzf \"%s\" -C \"%s\"",
           archive_path, dest_dir);
#endif

  int result = system(command);
  return result == 0;
}

/* ---------------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------------- */

bool update_download(const update_info_t* info,
                     const char* staging_dir,
                     const char* github_token) {
  if (info == NULL || staging_dir == NULL) {
    return false;
  }

  if (info->download_url[0] == '\0') {
    return false;
  }

  /* Create staging directory */
  char mkdir_command[1024];
#ifdef _WIN32
  snprintf(mkdir_command, sizeof(mkdir_command), "mkdir \"%s\" 2>nul",
           staging_dir);
#else
  snprintf(mkdir_command, sizeof(mkdir_command), "mkdir -p \"%s\"",
           staging_dir);
#endif
  system(mkdir_command);

  /* Build output path */
  char output_path[1024];
#ifdef _WIN32
  snprintf(output_path, sizeof(output_path), "%s\\update.zip",
           staging_dir);
#else
  snprintf(output_path, sizeof(output_path), "%s/update.tar.gz",
           staging_dir);
#endif

  /* Download */
  if (!_download_file(info->download_url, github_token, output_path)) {
    log_error("update_download: download from %s failed", info->download_url);
    return false;
  }

  /* Verify SHA256 if present */
  if (info->sha256[0] != '\0') {
    char computed_sha256[65];
    memset(computed_sha256, 0, sizeof(computed_sha256));

    if (!_compute_sha256(output_path, computed_sha256)) {
      log_error("update_download: SHA256 computation failed for %s", output_path);
      remove(output_path);
      return false;
    }

    if (strcmp(computed_sha256, info->sha256) != 0) {
      log_error("update_download: SHA256 verification failed for %s — expected %s, got %s",
                output_path, info->sha256, computed_sha256);
      remove(output_path);
      return false;
    }
  }

  /* Extract archive */
  if (!_extract_archive(output_path, staging_dir)) {
    remove(output_path);
    return false;
  }

  /* Clean up archive file */
  remove(output_path);

  return true;
}
