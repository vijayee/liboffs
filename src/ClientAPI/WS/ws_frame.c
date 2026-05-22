//
// Created by victor on 5/20/26.
//
#include "ws_frame.h"
#include "../../Util/allocator.h"
#include "../../Platform/platform.h"
#include <string.h>

void ws_frame_destroy(ws_frame_t* frame) {
  if (frame == NULL) return;
  if (frame->payload != NULL) {
    free(frame->payload);
    frame->payload = NULL;
  }
}

ssize_t ws_frame_parse(const uint8_t* data, size_t len, ws_frame_t* frame, size_t* needed) {
  if (frame == NULL) {
    return -1;
  }
  if (data == NULL || len == 0) {
    if (needed != NULL) *needed = 2;
    return 0;
  }

  memset(frame, 0, sizeof(ws_frame_t));

  /* Minimum frame header is 2 bytes */
  if (len < 2) {
    if (needed != NULL) *needed = 2;
    return 0;
  }

  uint8_t first_byte = data[0];
  uint8_t second_byte = data[1];

  frame->fin = (first_byte >> 7) & 0x01;
  frame->opcode = first_byte & 0x0F;
  frame->mask = (second_byte >> 7) & 0x01;

  uint64_t payload_len = (uint64_t)(second_byte & 0x7F);
  size_t header_len = 2;

  if (payload_len == 126) {
    if (len < 4) {
      if (needed != NULL) *needed = 4;
      return 0;
    }
    payload_len = ((uint64_t)data[2] << 8) | (uint64_t)data[3];
    header_len = 4;
  } else if (payload_len == 127) {
    if (len < 10) {
      if (needed != NULL) *needed = 10;
      return 0;
    }
    payload_len = 0;
    for (int i = 0; i < 8; i++) {
      payload_len = (payload_len << 8) | (uint64_t)data[2 + i];
    }
    header_len = 10;
  }

  if (frame->mask) {
    header_len += 4;
  }

  if (len < header_len) {
    if (needed != NULL) *needed = header_len;
    return 0;
  }

  if (payload_len > WS_MAX_PAYLOAD_SIZE) {
    return -1;
  }

  if (len < header_len + payload_len) {
    if (needed != NULL) *needed = header_len + payload_len;
    return 0;
  }

  size_t mask_offset = header_len - (frame->mask ? 4 : 0);
  if (frame->mask) {
    memcpy(frame->mask_key, data + mask_offset, 4);
  }

  frame->payload_len = payload_len;

  if (payload_len > 0) {
    frame->payload = get_memory(payload_len);
    if (frame->mask) {
      for (uint64_t i = 0; i < payload_len; i++) {
        frame->payload[i] = data[mask_offset + 4 + i] ^ frame->mask_key[i % 4];
      }
    } else {
      memcpy(frame->payload, data + mask_offset + (frame->mask ? 4 : 0), payload_len);
    }
  } else {
    frame->payload = NULL;
  }

  return (ssize_t)(header_len + payload_len);
}

uint8_t* ws_frame_build(uint8_t opcode, const uint8_t* payload, size_t payload_len, size_t* out_len) {
  size_t header_len = 2;

  if (payload_len <= 125) {
    header_len = 2;
  } else if (payload_len <= 65535) {
    header_len = 4;
  } else {
    header_len = 10;
  }

  size_t frame_len = header_len + payload_len;
  uint8_t* frame = get_memory(frame_len);

  /* First byte: FIN=1, RSV1-3=0, opcode */
  frame[0] = 0x80 | (opcode & 0x0F);

  /* Second byte: MASK=0 (server frames are never masked), payload length */
  if (payload_len <= 125) {
    frame[1] = (uint8_t)payload_len;
  } else if (payload_len <= 65535) {
    frame[1] = 126;
    frame[2] = (uint8_t)((payload_len >> 8) & 0xFF);
    frame[3] = (uint8_t)(payload_len & 0xFF);
  } else {
    frame[1] = 127;
    for (int i = 0; i < 8; i++) {
      frame[2 + i] = (uint8_t)((payload_len >> (56 - 8 * i)) & 0xFF);
    }
  }

  if (payload != NULL && payload_len > 0) {
    memcpy(frame + header_len, payload, payload_len);
  }

  if (out_len != NULL) {
    *out_len = frame_len;
  }
  return frame;
}

uint8_t* ws_frame_build_masked(uint8_t opcode, const uint8_t* payload, size_t payload_len, size_t* out_len) {
  size_t header_len;
  if (payload_len <= 125) {
    header_len = 6; /* FIN+opcode(1) + MASK+length(1) + mask_key(4) */
  } else if (payload_len <= 65535) {
    header_len = 8; /* FIN+opcode(1) + MASK+126(1) + extended_length(2) + mask_key(4) */
  } else {
    header_len = 14; /* FIN+opcode(1) + MASK+127(1) + extended_length(8) + mask_key(4) */
  }

  size_t frame_len = header_len + payload_len;
  uint8_t* frame = get_memory(frame_len);
  size_t pos = 0;

  /* First byte: FIN=1, RSV1-3=0, opcode */
  frame[pos++] = 0x80 | (opcode & 0x0F);

  /* Second byte: MASK=1, payload length */
  if (payload_len <= 125) {
    frame[pos++] = 0x80 | (uint8_t)payload_len;
  } else if (payload_len <= 65535) {
    frame[pos++] = 0x80 | 126;
    frame[pos++] = (uint8_t)((payload_len >> 8) & 0xFF);
    frame[pos++] = (uint8_t)(payload_len & 0xFF);
  } else {
    frame[pos++] = 0x80 | 127;
    for (int i = 0; i < 8; i++) {
      frame[pos++] = (uint8_t)((payload_len >> (56 - 8 * i)) & 0xFF);
    }
  }

  /* Masking key: 4 random bytes per RFC 6455 Section 5.3 */
  uint8_t mask_key[4];
  (void)platform_random_bytes(mask_key, sizeof(mask_key));
  memcpy(frame + pos, mask_key, 4);
  pos += 4;

  /* Payload: XOR with mask key */
  if (payload != NULL && payload_len > 0) {
    for (size_t idx = 0; idx < payload_len; idx++) {
      frame[pos + idx] = payload[idx] ^ mask_key[idx % 4];
    }
  }

  if (out_len != NULL) {
    *out_len = frame_len;
  }
  return frame;
}