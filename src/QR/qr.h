//
// QR codec: encode payloads into QR images (libqrencode) and decode QR
// images back into payloads (quirc). The image format both directions
// produce and accept is binary P6 PPM — the daemon never accepts an image
// format it does not itself generate. The only callers are the client API
// handlers (HTTP/peer_routes.c and ClientAPI/peer_handlers.c); this module
// knows nothing about peer info, CBOR, or transports.
//

#ifndef LIBOFFS_QR_H
#define LIBOFFS_QR_H

#include <stddef.h>
#include <stdint.h>

/* Encode payload bytes into a QR code rendered as a binary P6 PPM image
   (error-correction level M, 4x pixel scale, 4-module quiet zone). The
   4x scale matches the rendering previously inline in HTTP/peer_routes.c;
   the quiet zone is new — old images differ only by that white border. */
uint8_t* qr_encode_to_ppm(const uint8_t* payload, size_t payload_len,
                          size_t* out_len);

/* Parse a binary P6 PPM, locate the first decodable QR code, and return its
   payload bytes as a malloc'd buffer (caller frees with free()). Returns
   NULL and does not touch *out_len if the image is not valid P6, contains
   no QR code, or the QR payload fails to decode. */
uint8_t* qr_decode_from_ppm(const uint8_t* ppm_data, size_t ppm_len,
                            size_t* out_len);

#endif /* LIBOFFS_QR_H */