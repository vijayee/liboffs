//
// update_manifest.c — signed release manifest fetch + parse.
//
// Replaces the "sha256:" markdown scraping in update_check.c. The manifest is
// a CBOR-encoded array [version, release_tag, files[]] signed with ed25519
// and published as manifest.cbor + manifest.cbor.sig alongside the release
// binaries. update_manifest_fetch downloads both, verifies the signature with
// the compiled-in release key, and parses the CBOR into update_manifest_t.
//

#include "update_manifest.h"

#include "update_verify.h"
#include "../Util/allocator.h"
#include "../Util/log.h"

#include <cbor.h>
#include <cJSON.h>

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

#define RESPONSE_BUFFER_INITIAL_SIZE 8192
#define RESPONSE_READ_CHUNK_SIZE 4096

/* ---------------------------------------------------------------------------
 * Static helpers — HTTPS
 *
 * Duplicated from update_check.c because _https_get there returns a
 * null-terminated string (wrong for binary manifest/sig assets). The binary
 * variant returns a malloc'd buffer + length. ~60 lines of HTTPS glue is
 * preferable to refactoring update_check.c's static helpers into a shared
 * internal header (which would be a wider change than Task 4 needs).
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

/* HTTPS GET returning a null-terminated string (the response body). Used for
   the GitHub releases API JSON. Caller frees with free(). */
static char* _https_get(const char* host, const char* path,
                        const char* token) {
  struct addrinfo hints;
  struct addrinfo* address_info = NULL;
  int socket_fd = -1;
  SSL_CTX* ssl_context = NULL;
  SSL* ssl_connection = NULL;
  char* request = NULL;
  char* response_buffer = NULL;
  char* response = NULL;

  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  int getaddr_result = getaddrinfo(host, "443", &hints, &address_info);
  if (getaddr_result != 0 || address_info == NULL) {
    _https_free_context(ssl_context, ssl_connection, socket_fd);
    return NULL;
  }

  socket_fd = socket(address_info->ai_family, address_info->ai_socktype,
                     address_info->ai_protocol);
  if (socket_fd < 0) {
    freeaddrinfo(address_info);
    _https_free_context(ssl_context, ssl_connection, socket_fd);
    return NULL;
  }

  if (connect(socket_fd, address_info->ai_addr, address_info->ai_addrlen) < 0) {
    freeaddrinfo(address_info);
    _https_free_context(ssl_context, ssl_connection, socket_fd);
    return NULL;
  }

  freeaddrinfo(address_info);
  address_info = NULL;

  ssl_context = update_ssl_context_create();
  if (ssl_context == NULL) {
    _https_free_context(ssl_context, ssl_connection, socket_fd);
    return NULL;
  }

  ssl_connection = SSL_new(ssl_context);
  if (ssl_connection == NULL) {
    _https_free_context(ssl_context, ssl_connection, socket_fd);
    return NULL;
  }

  SSL_set_fd(ssl_connection, socket_fd);
  SSL_set_tlsext_host_name(ssl_connection, host);

  if (SSL_connect(ssl_connection) != 1) {
    log_error("update_manifest: TLS handshake failed for %s", host);
    _https_free_context(ssl_context, ssl_connection, socket_fd);
    return NULL;
  }

  if (SSL_get_verify_result(ssl_connection) != X509_V_OK) {
    log_error("update_manifest: TLS certificate verification failed for %s", host);
    _https_free_context(ssl_context, ssl_connection, socket_fd);
    return NULL;
  }

  size_t request_size = strlen(host) + strlen(path) + 512;
  request = get_memory(request_size);

  if (token != NULL && token[0] != '\0') {
    snprintf(request, request_size,
             "GET %s HTTP/1.1\r\n"
             "Host: %s\r\n"
             "User-Agent: offs-updater/1.0\r\n"
             "Accept: application/vnd.github+json\r\n"
             "Authorization: Bearer %s\r\n"
             "Connection: close\r\n"
             "\r\n",
             path, host, token);
  } else {
    snprintf(request, request_size,
             "GET %s HTTP/1.1\r\n"
             "Host: %s\r\n"
             "User-Agent: offs-updater/1.0\r\n"
             "Accept: application/vnd.github+json\r\n"
             "Connection: close\r\n"
             "\r\n",
             path, host);
  }

  SSL_write(ssl_connection, request, strlen(request));
  free(request);
  request = NULL;

  size_t response_capacity = RESPONSE_BUFFER_INITIAL_SIZE;
  size_t response_length = 0;
  response_buffer = get_memory(response_capacity);

  int bytes_read = 0;
  while ((bytes_read = SSL_read(ssl_connection,
                                response_buffer + response_length,
                                RESPONSE_READ_CHUNK_SIZE)) > 0) {
    response_length += (size_t)bytes_read;

    if (response_length + RESPONSE_READ_CHUNK_SIZE > response_capacity) {
      response_capacity *= 2;
      char* larger_buffer = get_memory(response_capacity);
      memcpy(larger_buffer, response_buffer, response_length);
      free(response_buffer);
      response_buffer = larger_buffer;
    }
  }

  _https_free_context(ssl_context, ssl_connection, socket_fd);
  ssl_context = NULL;
  ssl_connection = NULL;
  socket_fd = -1;

  if (response_length == 0) {
    free(response_buffer);
    return NULL;
  }

  if (response_length < response_capacity) {
    response_buffer[response_length] = '\0';
  } else {
    char* larger_buffer = get_memory(response_capacity + 1);
    memcpy(larger_buffer, response_buffer, response_length);
    larger_buffer[response_length] = '\0';
    free(response_buffer);
    response_buffer = larger_buffer;
  }

  char* body_start = strstr(response_buffer, "\r\n\r\n");
  if (body_start != NULL) {
    body_start += 4;
    response = strdup(body_start);
  } else {
    response = NULL;
  }

  free(response_buffer);
  return response;
}

/* HTTPS GET returning a binary buffer (the response body as raw bytes).
   Used for manifest.cbor + manifest.cbor.sig. Caller frees with free().
   Returns NULL + *out_len unchanged on failure. */
static uint8_t* _https_get_binary(const char* host, const char* path,
                                  const char* token, size_t* out_len) {
  struct addrinfo hints;
  struct addrinfo* address_info = NULL;
  int socket_fd = -1;
  SSL_CTX* ssl_context = NULL;
  SSL* ssl_connection = NULL;
  char* request = NULL;
  char* response_buffer = NULL;
  uint8_t* body = NULL;

  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  int getaddr_result = getaddrinfo(host, "443", &hints, &address_info);
  if (getaddr_result != 0 || address_info == NULL) {
    _https_free_context(ssl_context, ssl_connection, socket_fd);
    return NULL;
  }

  socket_fd = socket(address_info->ai_family, address_info->ai_socktype,
                     address_info->ai_protocol);
  if (socket_fd < 0) {
    freeaddrinfo(address_info);
    _https_free_context(ssl_context, ssl_connection, socket_fd);
    return NULL;
  }

  if (connect(socket_fd, address_info->ai_addr, address_info->ai_addrlen) < 0) {
    freeaddrinfo(address_info);
    _https_free_context(ssl_context, ssl_connection, socket_fd);
    return NULL;
  }

  freeaddrinfo(address_info);
  address_info = NULL;

  ssl_context = update_ssl_context_create();
  if (ssl_context == NULL) {
    _https_free_context(ssl_context, ssl_connection, socket_fd);
    return NULL;
  }

  ssl_connection = SSL_new(ssl_context);
  if (ssl_connection == NULL) {
    _https_free_context(ssl_context, ssl_connection, socket_fd);
    return NULL;
  }

  SSL_set_fd(ssl_connection, socket_fd);
  SSL_set_tlsext_host_name(ssl_connection, host);

  if (SSL_connect(ssl_connection) != 1) {
    log_error("update_manifest: TLS handshake failed for %s", host);
    _https_free_context(ssl_context, ssl_connection, socket_fd);
    return NULL;
  }

  if (SSL_get_verify_result(ssl_connection) != X509_V_OK) {
    log_error("update_manifest: TLS certificate verification failed for %s", host);
    _https_free_context(ssl_context, ssl_connection, socket_fd);
    return NULL;
  }

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

  /* Read the full response (headers + body) into one buffer. The header is
     text, so strstr for \r\n\r\n is safe even though the body may contain
     NUL bytes. */
  size_t response_capacity = RESPONSE_BUFFER_INITIAL_SIZE;
  size_t response_length = 0;
  response_buffer = get_memory(response_capacity);

  int bytes_read = 0;
  while ((bytes_read = SSL_read(ssl_connection,
                                response_buffer + response_length,
                                RESPONSE_READ_CHUNK_SIZE)) > 0) {
    response_length += (size_t)bytes_read;

    if (response_length + RESPONSE_READ_CHUNK_SIZE > response_capacity) {
      response_capacity *= 2;
      char* larger_buffer = get_memory(response_capacity);
      memcpy(larger_buffer, response_buffer, response_length);
      free(response_buffer);
      response_buffer = larger_buffer;
    }
  }

  _https_free_context(ssl_context, ssl_connection, socket_fd);
  ssl_context = NULL;
  ssl_connection = NULL;
  socket_fd = -1;

  if (response_length == 0) {
    free(response_buffer);
    return NULL;
  }

  /* Find the body separator. The header section is always text, so strstr is
     safe up to the \r\n\r\n boundary. */
  char* body_start = strstr(response_buffer, "\r\n\r\n");
  if (body_start == NULL) {
    free(response_buffer);
    return NULL;
  }
  body_start += 4;

  size_t body_length = response_length - (size_t)(body_start - response_buffer);
  if (body_length == 0) {
    free(response_buffer);
    return NULL;
  }

  body = get_memory(body_length);
  if (body == NULL) {
    free(response_buffer);
    return NULL;
  }
  memcpy(body, body_start, body_length);
  *out_len = body_length;

  free(response_buffer);
  return body;
}

/* ---------------------------------------------------------------------------
 * Static helpers — JSON asset lookup
 * --------------------------------------------------------------------------- */

/* Find the browser_download_url for a named asset in the release JSON.
   Returns a malloc'd string the caller frees, or NULL if not found. */
static char* _find_asset_url(const cJSON* release_json, const char* asset_name) {
  const cJSON* assets = cJSON_GetObjectItem(release_json, "assets");
  if (assets == NULL || !cJSON_IsArray(assets)) {
    return NULL;
  }

  int asset_count = cJSON_GetArraySize(assets);
  for (int index = 0; index < asset_count; index++) {
    const cJSON* asset = cJSON_GetArrayItem(assets, index);
    if (asset == NULL) {
      continue;
    }

    const cJSON* name_item = cJSON_GetObjectItem(asset, "name");
    if (name_item == NULL || !cJSON_IsString(name_item)) {
      continue;
    }

    if (strcmp(name_item->valuestring, asset_name) == 0) {
      const cJSON* url_item = cJSON_GetObjectItem(asset, "browser_download_url");
      if (url_item != NULL && cJSON_IsString(url_item)) {
        return strdup(url_item->valuestring);
      }
      return NULL;
    }
  }

  return NULL;
}

/* ---------------------------------------------------------------------------
 * Public API — parse
 * --------------------------------------------------------------------------- */

update_manifest_t* update_manifest_parse(const uint8_t* data, size_t len) {
  if (data == NULL || len == 0) {
    return NULL;
  }

  struct cbor_load_result load_result;
  cbor_item_t* root = cbor_load(data, len, &load_result);
  if (root == NULL || !cbor_isa_array(root)) {
    if (root != NULL) {
      cbor_decref(&root);
    }
    return NULL;
  }

  if (cbor_array_size(root) != 3) {
    cbor_decref(&root);
    return NULL;
  }

  /* element 0: version (uint, must be 1) */
  cbor_item_t* version_item = cbor_array_get(root, 0);
  bool version_ok = (version_item != NULL && cbor_isa_uint(version_item));
  uint64_t version_value = version_ok ? cbor_get_int(version_item) : 0;
  if (version_item != NULL) {
    cbor_decref(&version_item);
  }
  if (!version_ok || version_value != 1) {
    cbor_decref(&root);
    return NULL;
  }

  /* element 1: release_tag (string) */
  cbor_item_t* tag_item = cbor_array_get(root, 1);
  bool tag_ok = (tag_item != NULL && cbor_isa_string(tag_item));
  size_t tag_len = 0;
  const char* tag_data = NULL;
  if (tag_ok) {
    tag_len = cbor_string_length(tag_item);
    tag_data = (const char*)cbor_string_handle(tag_item);
    if (tag_len == 0 || tag_len >= 64) {
      tag_ok = false;
    }
  }
  if (!tag_ok) {
    if (tag_item != NULL) {
      cbor_decref(&tag_item);
    }
    cbor_decref(&root);
    return NULL;
  }

  /* element 2: files (array of [path, sha256, size]) */
  cbor_item_t* files_item = cbor_array_get(root, 2);
  bool files_ok = (files_item != NULL && cbor_isa_array(files_item));
  size_t file_count = files_ok ? cbor_array_size(files_item) : 0;
  if (!files_ok) {
    if (files_item != NULL) {
      cbor_decref(&files_item);
    }
    cbor_decref(&tag_item);
    cbor_decref(&root);
    return NULL;
  }

  update_manifest_t* manifest = get_clear_memory(sizeof(update_manifest_t));
  if (manifest == NULL) {
    cbor_decref(&files_item);
    cbor_decref(&tag_item);
    cbor_decref(&root);
    return NULL;
  }

  manifest->version = 1;
  memcpy(manifest->release_tag, tag_data, tag_len);
  manifest->release_tag[tag_len] = '\0';
  manifest->file_count = file_count;

  if (file_count > 0) {
    manifest->files = get_clear_memory(sizeof(manifest_file_t) * file_count);
    if (manifest->files == NULL) {
      cbor_decref(&files_item);
      cbor_decref(&tag_item);
      cbor_decref(&root);
      free(manifest);
      return NULL;
    }
  }

  bool parse_ok = true;
  for (size_t index = 0; index < file_count && parse_ok; index++) {
    cbor_item_t* file_entry = cbor_array_get(files_item, index);
    if (file_entry == NULL || !cbor_isa_array(file_entry) ||
        cbor_array_size(file_entry) != 3) {
      if (file_entry != NULL) {
        cbor_decref(&file_entry);
      }
      parse_ok = false;
      break;
    }

    cbor_item_t* path_item = cbor_array_get(file_entry, 0);
    cbor_item_t* sha_item = cbor_array_get(file_entry, 1);
    cbor_item_t* size_item = cbor_array_get(file_entry, 2);

    bool entry_ok = (path_item != NULL && cbor_isa_string(path_item) &&
                     sha_item != NULL && cbor_isa_string(sha_item) &&
                     size_item != NULL && cbor_isa_uint(size_item));

    if (entry_ok) {
      size_t path_len = cbor_string_length(path_item);
      size_t sha_len = cbor_string_length(sha_item);
      if (path_len == 0 || path_len >= 256 || sha_len == 0 || sha_len >= 65) {
        entry_ok = false;
      }
    }

    if (entry_ok) {
      const char* path_data = (const char*)cbor_string_handle(path_item);
      const char* sha_data = (const char*)cbor_string_handle(sha_item);
      memcpy(manifest->files[index].path, path_data, cbor_string_length(path_item));
      manifest->files[index].path[cbor_string_length(path_item)] = '\0';
      memcpy(manifest->files[index].sha256, sha_data, cbor_string_length(sha_item));
      manifest->files[index].sha256[cbor_string_length(sha_item)] = '\0';
      manifest->files[index].size = cbor_get_int(size_item);
    }

    if (path_item != NULL) {
      cbor_decref(&path_item);
    }
    if (sha_item != NULL) {
      cbor_decref(&sha_item);
    }
    if (size_item != NULL) {
      cbor_decref(&size_item);
    }
    cbor_decref(&file_entry);

    if (!entry_ok) {
      parse_ok = false;
    }
  }

  cbor_decref(&files_item);
  cbor_decref(&tag_item);
  cbor_decref(&root);

  if (!parse_ok) {
    update_manifest_free(manifest);
    return NULL;
  }

  return manifest;
}

void update_manifest_free(update_manifest_t* manifest) {
  if (manifest != NULL) {
    if (manifest->files != NULL) {
      free(manifest->files);
    }
    free(manifest);
  }
}

const manifest_file_t* update_manifest_find_file(const update_manifest_t* manifest,
                                                  const char* path) {
  if (manifest == NULL || path == NULL || manifest->files == NULL) {
    return NULL;
  }

  for (size_t index = 0; index < manifest->file_count; index++) {
    if (strcmp(manifest->files[index].path, path) == 0) {
      return &manifest->files[index];
    }
  }

  return NULL;
}

/* ---------------------------------------------------------------------------
 * Public API — fetch
 * --------------------------------------------------------------------------- */

update_manifest_t* update_manifest_fetch(const char* release_tag,
                                         const update_check_config_t* config) {
  if (release_tag == NULL || release_tag[0] == '\0' || config == NULL) {
    return NULL;
  }

  /* Build the API URL: {api_url}/repos/{repo}/releases/tags/{tag} */
  char api_path[2048];
  snprintf(api_path, sizeof(api_path), "%s/repos/%s/releases/tags/%s",
           config->github_api_url, config->github_repo, release_tag);

  char api_host[256];
  char api_path_only[2048];
  _parse_url(api_path, api_host, sizeof(api_host),
             api_path_only, sizeof(api_path_only));

  char* json_body = _https_get(api_host, api_path_only, config->github_token);
  if (json_body == NULL) {
    log_error("update_manifest: GitHub API request failed for tag %s", release_tag);
    return NULL;
  }

  cJSON* release_json = cJSON_Parse(json_body);
  free(json_body);
  json_body = NULL;

  if (release_json == NULL) {
    log_error("update_manifest: failed to parse release JSON for tag %s", release_tag);
    return NULL;
  }

  /* Find the manifest.cbor + manifest.cbor.sig asset URLs. */
  char* manifest_url = _find_asset_url(release_json, "manifest.cbor");
  char* sig_url = _find_asset_url(release_json, "manifest.cbor.sig");

  cJSON_Delete(release_json);

  if (manifest_url == NULL || sig_url == NULL) {
    log_error("update_manifest: manifest assets not found for tag %s", release_tag);
    if (manifest_url != NULL) {
      free(manifest_url);
    }
    if (sig_url != NULL) {
      free(sig_url);
    }
    return NULL;
  }

  /* Download both assets as binary. */
  char manifest_host[256];
  char manifest_path[2048];
  _parse_url(manifest_url, manifest_host, sizeof(manifest_host),
             manifest_path, sizeof(manifest_path));

  char sig_host[256];
  char sig_path[2048];
  _parse_url(sig_url, sig_host, sizeof(sig_host),
             sig_path, sizeof(sig_path));

  size_t manifest_len = 0;
  size_t sig_len = 0;
  uint8_t* manifest_data = _https_get_binary(manifest_host, manifest_path,
                                             config->github_token, &manifest_len);
  uint8_t* sig_data = _https_get_binary(sig_host, sig_path,
                                        config->github_token, &sig_len);

  free(manifest_url);
  free(sig_url);

  if (manifest_data == NULL || sig_data == NULL) {
    log_error("update_manifest: failed to download manifest assets for tag %s", release_tag);
    if (manifest_data != NULL) {
      free(manifest_data);
    }
    if (sig_data != NULL) {
      free(sig_data);
    }
    return NULL;
  }

  /* Verify the ed25519 signature with the compiled-in release key. */
  bool sig_ok = update_verify_manifest(sig_data, sig_len,
                                       manifest_data, manifest_len, NULL, 0);
  if (!sig_ok) {
    log_error("update_manifest: signature verification failed for tag %s", release_tag);
    free(manifest_data);
    free(sig_data);
    return NULL;
  }

  /* Parse the CBOR manifest. */
  update_manifest_t* manifest = update_manifest_parse(manifest_data, manifest_len);

  free(manifest_data);
  free(sig_data);

  if (manifest == NULL) {
    log_error("update_manifest: CBOR parse failed for tag %s", release_tag);
    return NULL;
  }

  return manifest;
}