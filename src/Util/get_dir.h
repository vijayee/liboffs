//
// Created by victor on 6/14/25.
//

#ifndef OFFS_GET_DIR_H
#define OFFS_GET_DIR_H
#include "vec.h"

vec_str_t* get_dir(const char* directory);
void destroy_files(vec_str_t* files);

#endif //OFFS_GET_DIR_H
