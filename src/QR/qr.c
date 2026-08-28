// Created by victor on 8/27/26.
#include "qr.h"
#include <qrencode.h>
#include <quirc.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Pixel scale and quiet zone (ISO/IEC 18004 requires a 4-module margin).
   The pixel scale matches the rendering previously inline in
   HTTP/peer_routes.c; the quiet zone is added because without it the code
   touches the image border and no decoder (including quirc below) can find
   the finder patterns. Old images differ only by this white border and
   remain decodable. */
#define QR_PIXEL_SCALE 4
#define QR_QUIET_ZONE_MODULES 4

/* Strict P6: magic, whitespace, width, whitespace, height, whitespace,
   maxval (must be exactly 255), exactly one whitespace byte, then
   width*height*3 binary RGB bytes. The pixel-length check below is the real
   gate: it only requires that many bytes to be present, so trailing bytes
   after the pixel data are tolerated — the encoder in this file is the only
   producer we support. */
static const uint8_t* _ppm_skip_ws(const uint8_t* cursor, const uint8_t* end) {
  while (cursor < end && (*cursor == ' ' || *cursor == '\t' ||
                          *cursor == '\n' || *cursor == '\r')) {
    cursor++;
  }
  return cursor;
}

static const uint8_t* _ppm_read_int(const uint8_t* cursor, const uint8_t* end,
                                    int* out) {
  cursor = _ppm_skip_ws(cursor, end);
  if (cursor >= end || *cursor < '0' || *cursor > '9') return NULL;
  int value = 0;
  while (cursor < end && *cursor >= '0' && *cursor <= '9') {
    value = value * 10 + (*cursor - '0');
    if (value > 1000000) return NULL;  /* sane image size bound */
    cursor++;
  }
  *out = value;
  return cursor;
}

uint8_t* qr_encode_to_ppm(const uint8_t* payload, size_t payload_len,
                          size_t* out_len) {
  if (payload == NULL || payload_len == 0 || out_len == NULL) return NULL;
  if (payload_len > (size_t)INT_MAX) return NULL;

  QRcode* code = QRcode_encodeData((int)payload_len, payload, 0, QR_ECLEVEL_M);
  if (code == NULL) return NULL;

  int qr_size = code->width;
  int img_size = (qr_size + 2 * QR_QUIET_ZONE_MODULES) * QR_PIXEL_SCALE;
  size_t header_len =
      (size_t)snprintf(NULL, 0, "P6\n%d %d\n255\n", img_size, img_size);
  size_t ppm_size = header_len + (size_t)img_size * img_size * 3;

  uint8_t* ppm = malloc(ppm_size);
  if (ppm == NULL) {
    QRcode_free(code);
    return NULL;
  }
  snprintf((char*)ppm, header_len + 1, "P6\n%d %d\n255\n", img_size, img_size);
  /* Start from an all-white image (the quiet zone) and stamp the code in */
  memset(ppm + header_len, 255, ppm_size - header_len);
  for (int y = 0; y < qr_size; y++) {
    for (int sy = 0; sy < QR_PIXEL_SCALE; sy++) {
      for (int x = 0; x < qr_size; x++) {
        uint8_t pixel = (code->data[y * qr_size + x] & 1) ? 0 : 255;
        if (pixel == 255) continue;  /* quiet zone already white */
        for (int sx = 0; sx < QR_PIXEL_SCALE; sx++) {
          size_t row = ((size_t)(y + QR_QUIET_ZONE_MODULES) * QR_PIXEL_SCALE + sy) *
                       img_size;
          size_t col = (size_t)(x + QR_QUIET_ZONE_MODULES) * QR_PIXEL_SCALE + sx;
          uint8_t* dest = ppm + header_len + (row + col) * 3;
          dest[0] = pixel;
          dest[1] = pixel;
          dest[2] = pixel;
        }
      }
    }
  }
  QRcode_free(code);

  *out_len = ppm_size;
  return ppm;
}

uint8_t* qr_decode_from_ppm(const uint8_t* ppm_data, size_t ppm_len,
                            size_t* out_len) {
  if (ppm_data == NULL || ppm_len == 0 || out_len == NULL) return NULL;
  if (ppm_len < 2 || ppm_data[0] != 'P' || ppm_data[1] != '6') return NULL;

  const uint8_t* cursor = ppm_data + 2;
  const uint8_t* end = ppm_data + ppm_len;
  int width = 0, height = 0, maxval = 0;

  cursor = _ppm_read_int(cursor, end, &width);
  if (cursor == NULL || width <= 0) return NULL;
  cursor = _ppm_read_int(cursor, end, &height);
  if (cursor == NULL || height <= 0) return NULL;
  cursor = _ppm_read_int(cursor, end, &maxval);
  if (cursor == NULL || maxval != 255) return NULL;
  /* P6 requires exactly one whitespace between maxval and pixel data */
  if (cursor >= end || (*cursor != ' ' && *cursor != '\t' &&
                        *cursor != '\n' && *cursor != '\r')) {
    return NULL;
  }
  cursor++;

  size_t pixel_len = (size_t)width * height * 3;
  if ((size_t)(end - cursor) < pixel_len) return NULL;

  struct quirc* decoder = quirc_new();
  if (decoder == NULL) return NULL;
  if (quirc_resize(decoder, width, height) < 0) {
    quirc_destroy(decoder);
    return NULL;
  }

  int gray_width = 0, gray_height = 0;
  uint8_t* gray = quirc_begin(decoder, &gray_width, &gray_height);
  if (gray == NULL) {
    quirc_destroy(decoder);
    return NULL;
  }
  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      const uint8_t* rgb = cursor + ((size_t)y * width + x) * 3;
      /* ITU-R BT.601 luma, the standard PPM to grayscale conversion */
      gray[(size_t)y * width + x] =
          (uint8_t)((rgb[0] * 299 + rgb[1] * 587 + rgb[2] * 114) / 1000);
    }
  }
  quirc_end(decoder);

  uint8_t* result = NULL;
  size_t result_len = 0;
  int count = quirc_count(decoder);
  for (int index = 0; index < count && result == NULL; index++) {
    struct quirc_code code;
    struct quirc_data data;
    quirc_extract(decoder, index, &code);
    if (quirc_decode(&code, &data) == 0) {
      result = malloc(data.payload_len);
      if (result != NULL) {
        memcpy(result, data.payload, data.payload_len);
        result_len = data.payload_len;
      }
    }
  }
  quirc_destroy(decoder);

  if (result == NULL) return NULL;
  *out_len = result_len;
  return result;
}