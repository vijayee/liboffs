//
// Created by victor on 5/22/26.
//

#ifndef OFFS_VALIDATION_H
#define OFFS_VALIDATION_H

#include <stddef.h>

#define OFFS_MAX_CONTENT_TYPE_LEN    256
#define OFFS_MAX_FILE_NAME_LEN       1024
#define OFFS_MAX_ORI_STRING_LEN      2048
#define OFFS_MAX_CBOR_MESSAGE_SIZE   (64 * 1024 * 1024)  // 64MB

int validate_content_type(const char* content_type);
int validate_file_name(const char* file_name);
int validate_ori_string(const char* ori);

#endif // OFFS_VALIDATION_H
