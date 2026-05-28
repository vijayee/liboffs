//
// Created by victor on 5/28/25.
//

#include "update_check.h"

#include "../Util/allocator.h"
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
 * Static helpers
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

  /* Extract host: everything before the first '/' */
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

  /* Remainder is the path */
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

static void _build_url(const update_check_config_t* config, char* buf,
                       size_t buf_size) {
  const char* api_url = config->github_api_url;

  if (config->channel == channel_stable) {
    snprintf(buf, buf_size, "%s/repos/%s/releases/latest",
             api_url, config->github_repo);
  } else {
    snprintf(buf, buf_size, "%s/repos/%s/releases",
             api_url, config->github_repo);
  }
}

static bool _matches_channel(const version_t* prerelease_version,
                             update_channel_e channel) {
  const char* prerelease = prerelease_version->prerelease;

  switch (channel) {
    case channel_stable:
      return prerelease[0] == '\0';
    case channel_rc:
      return strstr(prerelease, "rc") != NULL;
    case channel_dev:
      return prerelease[0] != '\0';
  }

  return true;
}

/*
 * _https_get
 *
 * Performs a GET request to the specified host/path over HTTPS using raw
 * sockets and OpenSSL. Returns a newly allocated string (the response body),
 * or NULL on error. The caller must free() the result.
 */
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

  /* TLS handshake */
  ssl_context = SSL_CTX_new(TLS_client_method());
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

  if (SSL_connect(ssl_connection) != 1) {
    _https_free_context(ssl_context, ssl_connection, socket_fd);
    return NULL;
  }

  /* Build and send HTTP request */
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

  /* Read response into a growing buffer */
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

  /* Terminate the body */
  if (response_length < response_capacity) {
    response_buffer[response_length] = '\0';
  } else {
    char* larger_buffer = get_memory(response_capacity + 1);
    memcpy(larger_buffer, response_buffer, response_length);
    larger_buffer[response_length] = '\0';
    free(response_buffer);
    response_buffer = larger_buffer;
  }

  /* Find response body after \r\n\r\n */
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

/* ---------------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------------- */

update_info_t* update_check_fetch(const update_check_config_t* config,
                                  const version_t* current_version) {
  if (config == NULL || current_version == NULL) {
    return NULL;
  }

  char api_url[2048];
  _build_url(config, api_url, sizeof(api_url));

  char host[256];
  char path[2048];
  _parse_url(api_url, host, sizeof(host), path, sizeof(path));

  char* json_body = _https_get(host, path, config->github_token);
  if (json_body == NULL) {
    return NULL;
  }

  cJSON* root = cJSON_Parse(json_body);
  free(json_body);
  json_body = NULL;

  if (root == NULL) {
    return NULL;
  }

  cJSON* release_entry = NULL;
  bool is_array = cJSON_IsArray(root);

  if (is_array) {
    /* Iterate to find first release matching the channel */
    int array_size = cJSON_GetArraySize(root);
    for (int index = 0; index < array_size; index++) {
      cJSON* entry = cJSON_GetArrayItem(root, index);
      if (entry == NULL) {
        continue;
      }

      cJSON* prerelease_item = cJSON_GetObjectItem(entry, "prerelease");
      cJSON* tag_name_item = cJSON_GetObjectItem(entry, "tag_name");
      bool is_prerelease = cJSON_IsTrue(prerelease_item);

      if (tag_name_item == NULL ||
          !cJSON_IsString(tag_name_item)) {
        continue;
      }

      version_t parsed_version;
      if (!version_parse(tag_name_item->valuestring, &parsed_version)) {
        continue;
      }

      if (is_prerelease && config->channel == channel_stable) {
        continue;
      }

      if (_matches_channel(&parsed_version, config->channel)) {
        release_entry = entry;
        break;
      }
    }
  } else {
    release_entry = root;
  }

  if (release_entry == NULL) {
    cJSON_Delete(root);
    return NULL;
  }

  /* Extract tag_name */
  cJSON* tag_name_item = cJSON_GetObjectItem(release_entry, "tag_name");
  if (tag_name_item == NULL || !cJSON_IsString(tag_name_item)) {
    cJSON_Delete(root);
    return NULL;
  }

  /* Parse version from tag */
  version_t release_version;
  if (!version_parse(tag_name_item->valuestring, &release_version)) {
    cJSON_Delete(root);
    return NULL;
  }

  /* Check if this release is newer than current */
  int comparison = version_compare(&release_version, current_version);
  bool is_newer = comparison > 0;

  if (!is_newer) {
    /* Not newer, but still return info with available=false */
  }

  update_info_t* info = get_clear_memory(sizeof(update_info_t));
  info->version = release_version;

  strncpy(info->tag_name, tag_name_item->valuestring,
          sizeof(info->tag_name) - 1);
  info->tag_name[sizeof(info->tag_name) - 1] = '\0';

  /* Extract prerelease flag */
  cJSON* prerelease_item = cJSON_GetObjectItem(release_entry, "prerelease");
  info->prerelease = cJSON_IsTrue(prerelease_item);

  /* Find platform asset */
  cJSON* assets = cJSON_GetObjectItem(release_entry, "assets");
  if (assets != NULL && cJSON_IsArray(assets)) {
    int asset_count = cJSON_GetArraySize(assets);
    const char* platform_identifier = NULL;

    /* Detect platform */
    #ifdef _WIN32
      platform_identifier = "windows-x64";
    #elif defined(__APPLE__)
      platform_identifier = "macos-x64";
    #else
      platform_identifier = "linux-x64";
    #endif

    for (int asset_index = 0; asset_index < asset_count; asset_index++) {
      cJSON* asset = cJSON_GetArrayItem(assets, asset_index);
      if (asset == NULL) {
        continue;
      }

      cJSON* asset_name = cJSON_GetObjectItem(asset, "name");
      if (asset_name == NULL || !cJSON_IsString(asset_name)) {
        continue;
      }

      if (strstr(asset_name->valuestring, platform_identifier) != NULL) {
        cJSON* download_url = cJSON_GetObjectItem(asset,
                                                   "browser_download_url");
        if (download_url != NULL && cJSON_IsString(download_url)) {
          strncpy(info->download_url, download_url->valuestring,
                  sizeof(info->download_url) - 1);
          info->download_url[sizeof(info->download_url) - 1] = '\0';
        }
        break;
      }
    }
  }

  /* Extract SHA256 from release body */
  cJSON* body_item = cJSON_GetObjectItem(release_entry, "body");
  if (body_item != NULL && cJSON_IsString(body_item)) {
    const char* body_text = body_item->valuestring;
    const char* sha_line = strstr(body_text, "sha256:");

    if (sha_line != NULL) {
      sha_line += 7; /* skip "sha256:" */

      /* Skip whitespace */
      while (*sha_line == ' ' || *sha_line == '\t') {
        sha_line++;
      }

      /* Copy up to 64 hex characters */
      size_t sha_index = 0;
      while (sha_index < sizeof(info->sha256) - 1 &&
             *sha_line != '\0' && *sha_line != '\n' && *sha_line != '\r') {
        info->sha256[sha_index++] = *sha_line++;
      }
      info->sha256[sha_index] = '\0';
    }
  }

  info->available = is_newer;

  cJSON_Delete(root);
  return info;
}

void update_info_free(update_info_t* info) {
  if (info != NULL) {
    free(info);
  }
}
