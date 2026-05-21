//
// Created by victor on 5/20/26.
//
#ifndef OFFS_WS_FRAME_H
#define OFFS_WS_FRAME_H

#include <stdint.h>
#include <stddef.h>
#include <unistd.h>

#define WS_OPCODE_TEXT   0x01
#define WS_OPCODE_BINARY 0x02
#define WS_OPCODE_CLOSE  0x08
#define WS_OPCODE_PING   0x09
#define WS_OPCODE_PONG   0x0A

typedef struct {
  uint8_t fin;
  uint8_t opcode;
  uint8_t mask;
  uint64_t payload_len;
  uint8_t mask_key[4];
  uint8_t* payload;  // decoded (unmasked) data, caller must free()
} ws_frame_t;

/* Parse a WebSocket frame from raw bytes.
 * Returns bytes consumed on success, 0 if frame is incomplete (sets *needed),
 * -1 on error. */
ssize_t ws_frame_parse(const uint8_t* data, size_t len, ws_frame_t* frame, size_t* needed);

/* Build a server-side WebSocket frame (never masked).
 * Returns heap-allocated buffer with frame bytes, caller must free().
 * Sets *out_len to the frame length. */
uint8_t* ws_frame_build(uint8_t opcode, const uint8_t* payload, size_t payload_len, size_t* out_len);

/* Build a client-side WebSocket frame (masked, as required by RFC 6455).
 * Returns heap-allocated buffer with frame bytes, caller must free().
 * Sets *out_len to the frame length. */
uint8_t* ws_frame_build_masked(uint8_t opcode, const uint8_t* payload, size_t payload_len, size_t* out_len);

void ws_frame_destroy(ws_frame_t* frame);

#endif // OFFS_WS_FRAME_H