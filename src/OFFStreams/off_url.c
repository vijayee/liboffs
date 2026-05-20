//
// Created by victor on 5/8/25.
//

#include "off_url.h"
#include "../Util/base58.h"
#include "../Util/allocator.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

off_url_t* off_url_create(void) {
    off_url_t* url = get_clear_memory(sizeof(off_url_t));
    if (!url) return NULL;
    url->server_address = strdup("http://localhost:23402");
    url->content_type = strdup("application/octet-stream");
    url->stream_length = 0;
    url->stream_offset = 0;
    url->file_hash = NULL;
    url->descriptor_hash = NULL;
    url->file_name = NULL;
    return url;
}

static int is_base58_char(char c) {
    if (c >= '1' && c <= '9') return 1;
    if (c >= 'A' && c <= 'H') return 1;
    if (c >= 'J' && c <= 'N') return 1;
    if (c >= 'P' && c <= 'Z') return 1;
    if (c >= 'a' && c <= 'k') return 1;
    if (c >= 'm' && c <= 'z') return 1;
    return 0;
}

off_url_t* off_url_parse(const char* url_string) {
    if (!url_string) return NULL;

    const char* prefix = "/offsystem/v3/";
    size_t prefix_len = strlen(prefix);
    if (strncmp(url_string, prefix, prefix_len) != 0) return NULL;

    const char* cursor = url_string + prefix_len;

    // Content type may contain '/' (e.g., "application/octet-stream").
    // Find the segment boundary by scanning for /<digits>/<Base58>/
    const char* type_end = NULL;
    const char* search = cursor;
    while (*search) {
        const char* slash = strchr(search, '/');
        if (!slash) break;
        const char* after_slash = slash + 1;
        char* endp;
        (void)strtol(after_slash, &endp, 10);
        if (endp != after_slash && *endp == '/') {
            const char* hash_start = endp + 1;
            if (*hash_start != '\0' && is_base58_char(*hash_start)) {
                type_end = slash;
                break;
            }
        }
        search = after_slash;
    }
    if (!type_end) return NULL;

    size_t type_len = type_end - cursor;
    char* content_type_raw = get_clear_memory(type_len + 1);
    memcpy(content_type_raw, cursor, type_len);
    char* decoded_type = malloc(type_len + 1);
    size_t decoded_type_len = 0;
    for (size_t i = 0; i < type_len; i++) {
        if (content_type_raw[i] == '%' && i + 2 < type_len && isxdigit((unsigned char)content_type_raw[i+1]) && isxdigit((unsigned char)content_type_raw[i+2])) {
            char hex[3] = {content_type_raw[i+1], content_type_raw[i+2], 0};
            decoded_type[decoded_type_len++] = (char)strtol(hex, NULL, 16);
            i += 2;
        } else {
            decoded_type[decoded_type_len++] = content_type_raw[i];
        }
    }
    decoded_type[decoded_type_len] = '\0';
    free(content_type_raw);

    cursor = type_end + 1;
    char* endp;
    long stream_length = strtol(cursor, &endp, 10);
    if (endp == cursor) { free(decoded_type); return NULL; }
    cursor = endp + 1;

    const char* slash3 = strchr(cursor, '/');
    if (!slash3) { free(decoded_type); return NULL; }
    size_t hash1_len = slash3 - cursor;
    char* file_hash_b58 = get_clear_memory(hash1_len + 1);
    memcpy(file_hash_b58, cursor, hash1_len);
    for (size_t i = 0; i < hash1_len; i++) {
        if (!is_base58_char(file_hash_b58[i])) { free(file_hash_b58); free(decoded_type); return NULL; }
    }

    cursor = slash3 + 1;
    const char* slash4 = strchr(cursor, '/');
    if (!slash4) { free(file_hash_b58); free(decoded_type); return NULL; }
    size_t hash2_len = slash4 - cursor;
    char* descriptor_hash_b58 = get_clear_memory(hash2_len + 1);
    memcpy(descriptor_hash_b58, cursor, hash2_len);
    for (size_t i = 0; i < hash2_len; i++) {
        if (!is_base58_char(descriptor_hash_b58[i])) { free(descriptor_hash_b58); free(file_hash_b58); free(decoded_type); return NULL; }
    }

    cursor = slash4 + 1;
    size_t name_len = strlen(cursor);
    char* file_name = get_clear_memory(name_len + 1);
    memcpy(file_name, cursor, name_len);

    size_t decoded_len = base58_decoded_length(hash1_len);
    uint8_t* file_hash_raw = get_clear_memory(decoded_len);
    size_t file_hash_bytes = 0;
    if (base58_decode(file_hash_b58, file_hash_raw, decoded_len, &file_hash_bytes) != 0) {
        free(file_hash_raw); free(file_hash_b58); free(descriptor_hash_b58); free(file_name); free(decoded_type);
        return NULL;
    }
    buffer_t* file_hash = buffer_create_from_existing_memory(file_hash_raw, file_hash_bytes);

    decoded_len = base58_decoded_length(hash2_len);
    uint8_t* desc_hash_raw = get_clear_memory(decoded_len);
    size_t desc_hash_bytes = 0;
    if (base58_decode(descriptor_hash_b58, desc_hash_raw, decoded_len, &desc_hash_bytes) != 0) {
        buffer_destroy(file_hash); free(file_hash_b58); free(descriptor_hash_b58); free(file_name); free(decoded_type);
        return NULL;
    }
    buffer_t* descriptor_hash = buffer_create_from_existing_memory(desc_hash_raw, desc_hash_bytes);

    off_url_t* url = off_url_create();
    free(url->content_type);
    url->content_type = decoded_type;
    url->stream_length = (size_t)stream_length;
    url->file_hash = file_hash;
    url->descriptor_hash = descriptor_hash;
    free(url->file_name);
    url->file_name = file_name;

    free(file_hash_b58);
    free(descriptor_hash_b58);
    return url;
}

static char* url_encode(const char* input, int encode_slash) {
    size_t len = strlen(input);
    size_t out_size = len * 3 + 1;
    char* out = malloc(out_size);
    size_t pos = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)input[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || (!encode_slash && c == '/')) {
            out[pos++] = c;
        } else {
            pos += snprintf(out + pos, out_size - pos, "%%%02X", c);
        }
    }
    out[pos] = '\0';
    return out;
}

char* off_url_to_string(off_url_t* url) {
    if (!url || !url->file_hash || !url->descriptor_hash) return NULL;

    size_t fh_b58_len = base58_encoded_length(url->file_hash->size);
    char* fh_b58 = malloc(fh_b58_len + 1);
    int fh_len = base58_encode(url->file_hash->data, url->file_hash->size, fh_b58, fh_b58_len + 1);
    if (fh_len < 0) { free(fh_b58); return NULL; }
    fh_b58[fh_len] = '\0';

    size_t dh_b58_len = base58_encoded_length(url->descriptor_hash->size);
    char* dh_b58 = malloc(dh_b58_len + 1);
    int dh_len = base58_encode(url->descriptor_hash->data, url->descriptor_hash->size, dh_b58, dh_b58_len + 1);
    if (dh_len < 0) { free(fh_b58); free(dh_b58); return NULL; }
    dh_b58[dh_len] = '\0';

    char* encoded_type = url_encode(url->content_type, 0);
    char* encoded_name = url_encode(url->file_name, 1);

    size_t total = strlen(url->server_address) + strlen("/offsystem/v3/") +
                   strlen(encoded_type) + 1 + 20 +
                   strlen(fh_b58) + 1 + strlen(dh_b58) + 1 +
                   strlen(encoded_name) + 1;
    char* result = malloc(total);
    snprintf(result, total, "%s/offsystem/v3/%s/%zu/%s/%s/%s",
             url->server_address, encoded_type, url->stream_length,
             fh_b58, dh_b58, encoded_name);

    free(fh_b58);
    free(dh_b58);
    free(encoded_type);
    free(encoded_name);
    return result;
}

off_url_t* off_url_from_headers(const char* type, const char* file_name, size_t stream_length, const char* server_address) {
    if (!type || !file_name || stream_length == 0) return NULL;

    off_url_t* url = off_url_create();
    free(url->content_type);
    url->content_type = strdup(type);
    free(url->file_name);
    url->file_name = strdup(file_name);
    url->stream_length = stream_length;
    if (server_address) {
        free(url->server_address);
        url->server_address = strdup(server_address);
    }
    return url;
}

void off_url_destroy(off_url_t* url) {
    if (!url) return;
    free(url->server_address);
    free(url->content_type);
    if (url->file_hash) buffer_destroy(url->file_hash);
    if (url->descriptor_hash) buffer_destroy(url->descriptor_hash);
    free(url->file_name);
    free(url);
}