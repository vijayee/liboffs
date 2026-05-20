//
// Created by victor on 5/20/26.
//
#ifndef OFFS_CLIENT_H
#define OFFS_CLIENT_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque client handle */
typedef struct offs_client_t offs_client_t;

/* Callback types */
typedef void (*offs_put_response_cb_t)(void* ctx, const char* ori_string);
typedef void (*offs_get_data_cb_t)(void* ctx, const uint8_t* data, size_t len);
typedef void (*offs_get_end_cb_t)(void* ctx);
typedef void (*offs_error_cb_t)(void* ctx, uint8_t status_code, const char* message);

/* Connection lifecycle */
offs_client_t* offs_client_connect(const char* transport_url);
void offs_client_disconnect(offs_client_t* client);

/* Buffered PUT — sends data in a single request */
int offs_client_put(offs_client_t* client,
                    const char* content_type,
                    const char* file_name,
                    size_t stream_length,
                    const uint8_t* data,
                    size_t data_len,
                    offs_put_response_cb_t callback,
                    void* ctx);

/* Streaming PUT — three-phase upload */
int offs_client_put_stream_start(offs_client_t* client,
                                  const char* content_type,
                                  const char* file_name,
                                  size_t stream_length);
int offs_client_put_stream_data(offs_client_t* client,
                                 const uint8_t* data,
                                 size_t len);
int offs_client_put_stream_end(offs_client_t* client,
                                offs_put_response_cb_t callback,
                                void* ctx);

/* GET — retrieves data by ORI string */
int offs_client_get(offs_client_t* client,
                     const char* ori_string,
                     offs_get_data_cb_t data_cb,
                     offs_get_end_cb_t end_cb,
                     offs_error_cb_t error_cb,
                     void* ctx);

#ifdef __cplusplus
}
#endif

#endif /* OFFS_CLIENT_H */