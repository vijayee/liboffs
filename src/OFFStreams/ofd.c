//
// Created by victor on 5/8/25.
//

#include "ofd.h"
#include "../Util/allocator.h"
#include <cbor.h>
#include <string.h>
#include <stdlib.h>

ofd_t* ofd_create(void) {
    ofd_t* ofd = get_clear_memory(sizeof(ofd_t));
    if (!ofd) return NULL;
    refcounter_init(&ofd->refcounter);
    vec_init(&ofd->entries);
    ofd->hash = NULL;
    ofd->expires_at = 0;
    return ofd;
}

void ofd_destroy(ofd_t* ofd) {
    if (!ofd) return;
    refcounter_dereference(&ofd->refcounter);
    if (refcounter_count(&ofd->refcounter) > 0) return;
    refcounter_destroy_lock(&ofd->refcounter);

    for (int i = 0; i < ofd->entries.length; i++) {
        ofd_entry_t* entry = &ofd->entries.data[i];
        free(entry->name);
        if (entry->type == OFD_ENTRY_FILE && entry->file_ori) {
            ori_destroy(entry->file_ori);
        } else if (entry->type == OFD_ENTRY_DIRECTORY && entry->dir_hash) {
            buffer_destroy(entry->dir_hash);
        }
    }
    vec_deinit(&ofd->entries);

    if (ofd->hash) buffer_destroy(ofd->hash);
    free(ofd);
}

void ofd_add_file(ofd_t* ofd, const char* name, ori_t* file_ori) {
    if (!ofd || !name || !file_ori) return;
    ofd_entry_t entry;
    entry.name = strdup(name);
    entry.type = OFD_ENTRY_FILE;
    entry.file_ori = (ori_t*)refcounter_reference((refcounter_t*)file_ori);
    vec_push(&ofd->entries, entry);
}

void ofd_add_directory(ofd_t* ofd, const char* name, buffer_t* dir_hash) {
    if (!ofd || !name || !dir_hash) return;
    ofd_entry_t entry;
    entry.name = strdup(name);
    entry.type = OFD_ENTRY_DIRECTORY;
    entry.dir_hash = (buffer_t*)refcounter_reference((refcounter_t*)dir_hash);
    vec_push(&ofd->entries, entry);
}

ofd_entry_t* ofd_find(ofd_t* ofd, const char* name) {
    if (!ofd || !name) return NULL;
    for (int i = 0; i < ofd->entries.length; i++) {
        if (strcmp(ofd->entries.data[i].name, name) == 0) {
            return &ofd->entries.data[i];
        }
    }
    return NULL;
}

buffer_t* ofd_encode(ofd_t* ofd) {
    if (!ofd) return NULL;

    cbor_item_t* root = cbor_new_definite_map(2);

    // Version
    cbor_item_t* version = cbor_build_uint8(1);
    cbor_item_t* version_key = cbor_build_string("v");
    (void)cbor_map_add(root, (struct cbor_pair){.key = version_key, .value = version});
    cbor_decref(&version_key);
    cbor_decref(&version);

    // Entries array
    cbor_item_t* entries_key = cbor_build_string("entries");
    cbor_item_t* entries_arr = cbor_new_indefinite_array();

    for (int i = 0; i < ofd->entries.length; i++) {
        ofd_entry_t* entry = &ofd->entries.data[i];
        int is_dir = entry->type == OFD_ENTRY_DIRECTORY;
        int num_fields = is_dir ? 3 : (entry->file_ori->descriptor_hash ? 8 : 7);
        cbor_item_t* entry_map = cbor_new_definite_map(num_fields);

        // name
        cbor_item_t* name_key = cbor_build_string("n");
        cbor_item_t* name_val = cbor_build_string(entry->name);
        (void)cbor_map_add(entry_map, (struct cbor_pair){.key = name_key, .value = name_val});
        cbor_decref(&name_key);
        cbor_decref(&name_val);

        // type
        cbor_item_t* type_key = cbor_build_string("t");
        cbor_item_t* type_val = cbor_build_uint8(is_dir ? 1 : 0);
        (void)cbor_map_add(entry_map, (struct cbor_pair){.key = type_key, .value = type_val});
        cbor_decref(&type_key);
        cbor_decref(&type_val);

        if (!is_dir) {
            ori_t* ori = entry->file_ori;

            // file_hash
            cbor_item_t* file_key = cbor_build_string("f");
            cbor_item_t* file_val = buffer_to_cbor(ori->file_hash);
            (void)cbor_map_add(entry_map, (struct cbor_pair){.key = file_key, .value = file_val});
            cbor_decref(&file_key);
            cbor_decref(&file_val);

            // descriptor_hash
            if (ori->descriptor_hash) {
                cbor_item_t* desc_key = cbor_build_string("D");
                cbor_item_t* desc_val = buffer_to_cbor(ori->descriptor_hash);
                (void)cbor_map_add(entry_map, (struct cbor_pair){.key = desc_key, .value = desc_val});
                cbor_decref(&desc_key);
                cbor_decref(&desc_val);
            }

            // final_byte
            cbor_item_t* size_key = cbor_build_string("s");
            cbor_item_t* size_val = cbor_build_uint64(ori->final_byte);
            (void)cbor_map_add(entry_map, (struct cbor_pair){.key = size_key, .value = size_val});
            cbor_decref(&size_key);
            cbor_decref(&size_val);

            // block_type
            cbor_item_t* btype_key = cbor_build_string("B");
            cbor_item_t* btype_val = cbor_build_uint64((uint64_t)ori->block_type);
            (void)cbor_map_add(entry_map, (struct cbor_pair){.key = btype_key, .value = btype_val});
            cbor_decref(&btype_key);
            cbor_decref(&btype_val);

            // tuple_size
            cbor_item_t* tsize_key = cbor_build_string("T");
            cbor_item_t* tsize_val = cbor_build_uint64(ori->tuple_size);
            (void)cbor_map_add(entry_map, (struct cbor_pair){.key = tsize_key, .value = tsize_val});
            cbor_decref(&tsize_key);
            cbor_decref(&tsize_val);

            // file_offset
            cbor_item_t* offset_key = cbor_build_string("o");
            cbor_item_t* offset_val = cbor_build_uint64(ori->file_offset);
            (void)cbor_map_add(entry_map, (struct cbor_pair){.key = offset_key, .value = offset_val});
            cbor_decref(&offset_key);
            cbor_decref(&offset_val);
        } else {
            cbor_item_t* dir_key = cbor_build_string("d");
            cbor_item_t* dir_val = buffer_to_cbor(entry->dir_hash);
            (void)cbor_map_add(entry_map, (struct cbor_pair){.key = dir_key, .value = dir_val});
            cbor_decref(&dir_key);
            cbor_decref(&dir_val);
        }

        (void)cbor_array_push(entries_arr, entry_map);
        cbor_decref(&entry_map);
    }

    (void)cbor_map_add(root, (struct cbor_pair){.key = entries_key, .value = entries_arr});
    cbor_decref(&entries_key);
    cbor_decref(&entries_arr);

    // Serialize
    unsigned char* serialized = NULL;
    size_t serialized_size = 0;
    cbor_serialize_alloc(root, &serialized, &serialized_size);

    cbor_decref(&root);

    buffer_t* result = buffer_create_from_pointer_copy(serialized, serialized_size);
    free(serialized);
    return result;
}

ofd_t* ofd_decode(buffer_t* data) {
    if (!data || !data->data || data->size == 0) return NULL;

    /* Reject data that isn't a CBOR map (major type 5: 0xA0-0xBF).
       This guards against legacy JSON-encoded OFDs being fed to the CBOR parser. */
    uint8_t first_byte = data->data[0];
    if ((first_byte >> 5) != 5) return NULL;

    struct cbor_load_result load_result;
    cbor_item_t* root = cbor_load(data->data, data->size, &load_result);
    if (!root || !cbor_isa_map(root)) {
        if (root) cbor_decref(&root);
        return NULL;
    }

    ofd_t* ofd = ofd_create();
    if (!ofd) {
        cbor_decref(&root);
        return NULL;
    }

    // Find version
    struct cbor_pair* pairs = cbor_map_handle(root);
    size_t map_size = cbor_map_size(root);
    cbor_item_t* entries_arr = NULL;

    for (size_t i = 0; i < map_size; i++) {
        cbor_item_t* key = pairs[i].key;
        if (cbor_isa_string(key)) {
            size_t key_len = cbor_string_length(key);
            const char* key_str = (const char*)cbor_string_handle(key);
            if (key_len == 1 && key_str[0] == 'v') {
                // Version field - we only support version 1
                if (cbor_isa_uint(pairs[i].value)) {
                    uint8_t version = cbor_get_uint8(pairs[i].value);
                    if (version != 1) {
                        cbor_decref(&root);
                        ofd_destroy(ofd);
                        return NULL;
                    }
                }
            } else if (key_len == 7 && strncmp(key_str, "entries", 7) == 0) {
                entries_arr = pairs[i].value;
            }
        }
    }

    if (!entries_arr || !cbor_isa_array(entries_arr)) {
        cbor_decref(&root);
        ofd_destroy(ofd);
        return NULL;
    }

    size_t num_entries = cbor_array_size(entries_arr);
    for (size_t i = 0; i < num_entries; i++) {
        cbor_item_t* entry_map = cbor_array_get(entries_arr, i);
        if (!cbor_isa_map(entry_map)) {
            cbor_decref(&entry_map);
            continue;
        }

        struct cbor_pair* entry_pairs = cbor_map_handle(entry_map);
        size_t entry_map_size = cbor_map_size(entry_map);

        char* name = NULL;
        ofd_entry_type_t type = OFD_ENTRY_FILE;
        buffer_t* hash = NULL;
        buffer_t* descriptor_hash = NULL;
        uint64_t final_byte = 0;
        uint64_t block_type_val = (uint64_t)standard;
        uint64_t tuple_size = 3;
        uint64_t file_offset = 0;
        ori_t* file_ori = NULL;

        for (size_t j = 0; j < entry_map_size; j++) {
            cbor_item_t* ekey = entry_pairs[j].key;
            cbor_item_t* eval = entry_pairs[j].value;

            if (!cbor_isa_string(ekey)) continue;
            size_t ekey_len = cbor_string_length(ekey);
            const char* ekey_str = (const char*)cbor_string_handle(ekey);

            if (ekey_len == 1 && ekey_str[0] == 'n' && cbor_isa_string(eval)) {
                size_t name_len = cbor_string_length(eval);
                name = get_clear_memory(name_len + 1);
                memcpy(name, cbor_string_handle(eval), name_len);
            } else if (ekey_len == 1 && ekey_str[0] == 't' && cbor_isa_uint(eval)) {
                type = cbor_get_uint8(eval) == 1 ? OFD_ENTRY_DIRECTORY : OFD_ENTRY_FILE;
            } else if (ekey_len == 1 && ekey_str[0] == 'f' && cbor_isa_bytestring(eval)) {
                hash = cbor_to_buffer(eval);
            } else if (ekey_len == 1 && ekey_str[0] == 'd' && cbor_isa_bytestring(eval)) {
                hash = cbor_to_buffer(eval);
            } else if (ekey_len == 1 && ekey_str[0] == 'D' && cbor_isa_bytestring(eval)) {
                descriptor_hash = cbor_to_buffer(eval);
            } else if (ekey_len == 1 && ekey_str[0] == 's' && cbor_isa_uint(eval)) {
                final_byte = cbor_get_int(eval);
            } else if (ekey_len == 1 && ekey_str[0] == 'B' && cbor_isa_uint(eval)) {
                block_type_val = cbor_get_int(eval);
            } else if (ekey_len == 1 && ekey_str[0] == 'T' && cbor_isa_uint(eval)) {
                tuple_size = cbor_get_int(eval);
            } else if (ekey_len == 1 && ekey_str[0] == 'o' && cbor_isa_uint(eval)) {
                file_offset = cbor_get_int(eval);
            }
        }

        if (name && hash) {
            if (type == OFD_ENTRY_FILE) {
                file_ori = ori_create((size_t)final_byte);
                file_ori->file_hash = hash;
                file_ori->descriptor_hash = descriptor_hash
                    ? descriptor_hash
                    : buffer_copy(hash);
                file_ori->file_name = strdup(name);
                file_ori->block_type = (block_size_e)block_type_val;
                file_ori->tuple_size = (size_t)tuple_size;
                file_ori->file_offset = (size_t)file_offset;
                ofd_add_file(ofd, name, file_ori);
                DESTROY(file_ori, ori);
            } else {
                if (descriptor_hash) {
                    DESTROY(descriptor_hash, buffer);
                }
                ofd_add_directory(ofd, name, hash);
                DESTROY(hash, buffer);
            }
        } else {
            if (hash) {
                DESTROY(hash, buffer);
            }
            if (descriptor_hash) {
                DESTROY(descriptor_hash, buffer);
            }
        }
        free(name);
        cbor_decref(&entry_map);
    }

    cbor_decref(&root);
    return ofd;
}