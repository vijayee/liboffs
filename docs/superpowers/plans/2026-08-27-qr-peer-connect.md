# QR Peer Connect Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make QR codes a first-class, symmetric peer-info transport: any client API surface (HTTP, unix/TCP/WS/WT sockets, C client lib, JS client lib, `offs` CLI) can generate a QR image of local peer info and submit a QR image to connect/add a friend.

**Architecture:** Two vendored submodules (`deps/libqrencode` encoder, `deps/quirc` decoder) feed a new `src/QR/` codec module with exactly two operations (`qr_encode_to_ppm`, `qr_decode_from_ppm`). All transports call these two functions so behavior cannot drift. Wire format byte 2 = "PPM QR image" on `PEER_INFO_REQUEST/RESPONSE`, `PEER_CONNECT`, and `FRIEND_ADD`. HTTP routes dispatch by Content-Type on bodies and share one payload-decode helper with the socket handlers.

**Tech Stack:** C (liboffs), libqrencode (submodule), quirc (submodule), CMake, GTest, libcbor, JS (offs-client package), cURL (manual verification).

**Spec:** `docs/superpowers/specs/2026-08-27-qr-peer-connect-design.md`
**Harmony ticket:** OFFS-187

**Deviations from spec (decided during planning):**
- `qr_encode_to_ppm` / `qr_decode_from_ppm` return `malloc`'d `uint8_t*` + length instead of `buffer_t` — the wire structs own raw `uint8_t*` payloads, so plain malloc ownership transfers cleanly into `client_api_peer_info_response_t.data` (freed by `client_api_peer_info_response_destroy`) without refcounter surgery.
- The C client library has **no peer/friend functions today** — `peer_info`/`peer_connect`/`friend_add` (plus `_ex` and `_qr` forms) are new functions, not modifications.
- HTTP-level round-trip testing via GTest would require a full `offs_node_t` + CA + authority fixture; instead the shared decode helper and QR module get unit tests, and the HTTP path gets a concrete manual curl round-trip verification (Task 9) against the example server.

---

## File Structure

| File | Action | Responsibility |
|---|---|---|
| `deps/libqrencode` | Create (submodule) | QR encoder |
| `deps/quirc` | Create (submodule) | QR decoder |
| `CMakeLists.txt:393-406` | Modify | Replace pkg-config probe with submodule builds |
| `src/QR/qr.h`, `src/QR/qr.c` | Create | PPM QR encode/decode codec (only place qrencode/quirc are called) |
| `test/test_qr.cpp` | Create | Codec unit tests |
| `src/ClientAPI/client_api_wire.h/.c` | Modify | `PEER_INFO_REQUEST` format byte (encode + decode) |
| `test/test_qr_wire.cpp` | Create | Wire format-2 frame tests |
| `src/ClientAPI/peer_handlers.h/.c` | Modify | Shared `peer_info_from_payload` helper; format 2 in info/connect/friend handlers |
| `test/test_peer_payload.cpp` | Create | Payload-dispatch unit tests (incl. QR round trip) |
| `src/ClientAPI/HTTP/peer_routes.c` | Modify | Encode via `src/QR`; accept `image/x-portable-pixmap` bodies |
| `src/ClientLibs/c/offs_client.h/.c` | Modify | New peer/QR client functions + response dispatch |
| `src/ClientLibs/js/offs-client/src/wire.js` | Modify | `encodePeerInfoRequest(format)` |
| `src/ClientLibs/js/offs-client/src/transports/http-transport.js` | Modify | Content-Type by format; `qrcode` fetch |
| `src/ClientLibs/js/offs-client/src/index.js` | Modify | Pass format on CBOR transports; `peerConnectQr`/`friendAddQr` sugar |
| `OFFS/src/offs/commands/peer.c` | Modify | `--qr` on `info` and `connect` |
| `OFFS/src/offs/commands/friend.c` | Modify | `--qr` on `add` |
| `test/CMakeLists.txt` | Modify | Register new test files |
| `docs/OFFS_API_CLI_SPEC.md` | Modify | Document format byte 2 + PPM body support |

Note: `CMakeLists.txt` uses `file(GLOB_RECURSE C_SRC "src/*/*.c")` — new files under `src/QR/` are picked up automatically after re-running CMake.

---

### Task 1: Vendor libqrencode + quirc submodules and wire CMake

**Files:**
- Create: `deps/libqrencode` (submodule), `deps/quirc` (submodule)
- Modify: `CMakeLists.txt` (qrencode probe block, lines 393-406)

- [ ] **Step 1: Add the submodules**

```bash
git submodule add https://github.com/fukuchi/libqrencode.git deps/libqrencode
git submodule add https://github.com/dlbeer/quirc.git deps/quirc
```

This updates `.gitmodules` automatically. Do not commit yet (Task 1 Step 5 commits everything together).

- [ ] **Step 2: Replace the pkg-config qrencode probe in CMakeLists.txt**

Find this block (lines 393-406):

```cmake
# libqrencode for QR code generation (optional)
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(QRENCODE QUIET libqrencode)
endif()
if(QRENCODE_FOUND)
  target_compile_definitions(offs PRIVATE HAS_QRENCODE)
  target_include_directories(offs PRIVATE ${QRENCODE_INCLUDE_DIRS})
  target_link_libraries(offs PRIVATE ${QRENCODE_LIBRARIES})
  message(STATUS "libqrencode found — QR code generation enabled")
else()
  message(STATUS "libqrencode not found — QR code generation disabled")
endif()
```

Replace it with:

```cmake
# libqrencode — QR encoder, vendored submodule (deps/libqrencode). Required:
# QR peer-info generation is a first-class client-API feature, not an optional
# extra, so a missing submodule is a loud error like deps/bcrypt. Built
# without PNG support and without CLI tools — only the core encoder is used,
# via src/QR/qr.c.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/deps/libqrencode/CMakeLists.txt)
  set(WITH_TOOLS   OFF CACHE BOOL "" FORCE)
  set(WITH_TEST    OFF CACHE BOOL "" FORCE)
  set(WITHOUT_PNG  ON  CACHE BOOL "" FORCE)
  add_subdirectory(deps/libqrencode)
  target_include_directories(offs PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/deps/libqrencode)
  target_link_libraries(offs PRIVATE qrencode)
else()
  message(FATAL_ERROR "deps/libqrencode submodule missing. Run: git submodule update --init --recursive")
endif()

# quirc — QR decoder, vendored submodule (deps/quirc). Upstream ships no
# CMakeLists.txt (Makefile-only, see deps/quirc/Makefile LIB_OBJ), so compile
# the four decoder sources directly into a static library. Only decode is
# used (via src/QR/qr.c); quirc_encode.c is intentionally not built.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/deps/quirc/lib/quirc.c)
  add_library(quirc STATIC
    deps/quirc/lib/quirc.c
    deps/quirc/lib/decode.c
    deps/quirc/lib/identify.c
    deps/quirc/lib/version_db.c)
  target_include_directories(quirc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/deps/quirc/lib)
  target_link_libraries(offs PRIVATE quirc)
else()
  message(FATAL_ERROR "deps/quirc submodule missing. Run: git submodule update --init --recursive")
endif()
```

- [ ] **Step 3: Configure and build to verify**

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build -j$(nproc) 2>&1 | tail -5
```

Expected: configures without errors; `qrencode` and `quirc` targets build; `offs` links. If libqrencode's CMake emits a `qrencode` shared/static target name other than `qrencode`, check `deps/libqrencode/CMakeLists.txt` for the actual target name (`add_library(qrencode ...)`) and adjust the `target_link_libraries` line.

- [ ] **Step 4: Verify test target still links** (CMake propagates PRIVATE deps of the static `offs` lib to `testliboffs` via `$<LINK_ONLY>`; confirm rather than assume)

```bash
cmake --build build --target testliboffs -j$(nproc) 2>&1 | tail -3
```

Expected: links cleanly. If it fails with undefined `QRcode_encodeData`/quirc symbols, add `target_link_libraries(testliboffs PRIVATE qrencode quirc)` next to the existing `target_link_libraries(testliboffs PRIVATE blake3)` line in `test/CMakeLists.txt`.

- [ ] **Step 5: Commit**

```bash
git add .gitmodules deps/libqrencode deps/quirc CMakeLists.txt test/CMakeLists.txt
git commit -m "build: vendor libqrencode and quirc as required submodules"
```

---

### Task 2: `src/QR` codec module (TDD)

**Files:**
- Create: `src/QR/qr.h`, `src/QR/qr.c`
- Create: `test/test_qr.cpp`
- Modify: `test/CMakeLists.txt` (add `test_qr.cpp` to the `add_executable(testliboffs ...)` source list)

- [ ] **Step 1: Write the failing test**

Create `test/test_qr.cpp`:

```cpp
#include <gtest/gtest.h>
#include <cstring>
#include <cstdlib>
#include <cstdio>

extern "C" {
#include "../src/QR/qr.h"
}

namespace qr_test {

/* Build a binary P6 PPM of the given dimensions filled with the given gray
   value (RGB triplets r=g=b=gray). Returns malloc'd buffer; caller frees. */
static uint8_t* _make_ppm(int width, int height, uint8_t gray, size_t* out_len) {
  char header[64];
  int header_len = snprintf(header, sizeof(header), "P6\n%d %d\n255\n", width, height);
  size_t pixel_len = (size_t)width * height * 3;
  uint8_t* ppm = (uint8_t*)malloc(header_len + pixel_len);
  if (ppm == NULL) return NULL;
  memcpy(ppm, header, header_len);
  for (size_t i = 0; i < pixel_len; i++) ppm[header_len + i] = gray;
  *out_len = header_len + pixel_len;
  return ppm;
}

TEST(QrEncode, RejectsNullAndEmpty) {
  size_t len = 0;
  EXPECT_TRUE(qr_encode_to_ppm(NULL, 10, &len) == NULL);
  uint8_t one = 0x01;
  EXPECT_TRUE(qr_encode_to_ppm(&one, 0, &len) == NULL);
}

TEST(QrRoundTrip, PayloadSurvivesEncodeDecode) {
  /* 64 pseudo-random bytes (deterministic LCG so the test never flakes) */
  uint8_t payload[64];
  uint32_t state = 12345;
  for (size_t i = 0; i < sizeof(payload); i++) {
    state = state * 1103515245 + 12345;
    payload[i] = (uint8_t)(state >> 16);
  }

  size_t ppm_len = 0;
  uint8_t* ppm = qr_encode_to_ppm(payload, sizeof(payload), &ppm_len);
  ASSERT_NE(ppm, nullptr);
  ASSERT_GT(ppm_len, 0u);
  /* Generated images are binary P6 */
  EXPECT_EQ(0, memcmp(ppm, "P6\n", 3));

  size_t decoded_len = 0;
  uint8_t* decoded = qr_decode_from_ppm(ppm, ppm_len, &decoded_len);
  free(ppm);
  ASSERT_NE(decoded, nullptr);
  EXPECT_EQ(decoded_len, sizeof(payload));
  EXPECT_EQ(0, memcmp(decoded, payload, sizeof(payload)));
  free(decoded);
}

TEST(QrDecode, RejectsBadMagic) {
  const char* not_ppm = "P5\n1 1\n255\n";
  size_t len = 0;
  EXPECT_TRUE(qr_decode_from_ppm((const uint8_t*)not_ppm, strlen(not_ppm), &len) == NULL);
}

TEST(QrDecode, RejectsWrongMaxval) {
  const char* ppm = "P6\n1 1\n65535\n";
  size_t len = 0;
  EXPECT_TRUE(qr_decode_from_ppm((const uint8_t*)ppm, strlen(ppm), &len) == NULL);
}

TEST(QrDecode, RejectsTruncatedPixels) {
  size_t full_len = 0;
  uint8_t* ppm = _make_ppm(10, 10, 255, &full_len);
  ASSERT_NE(ppm, nullptr);
  /* Header + less than w*h*3 pixel bytes */
  size_t header_len = full_len - 10u * 10u * 3u;
  size_t len = 0;
  EXPECT_TRUE(qr_decode_from_ppm(ppm, header_len + 10, &len) == NULL);
  free(ppm);
}

TEST(QrDecode, NoQrCodeInBlankImage) {
  size_t ppm_len = 0;
  uint8_t* ppm = _make_ppm(200, 200, 255, &ppm_len);
  ASSERT_NE(ppm, nullptr);
  size_t len = 0;
  EXPECT_TRUE(qr_decode_from_ppm(ppm, ppm_len, &len) == NULL);
  free(ppm);
}

TEST(QrDecode, RejectsNullAndEmpty) {
  size_t len = 0;
  EXPECT_TRUE(qr_decode_from_ppm(NULL, 10, &len) == NULL);
  EXPECT_TRUE(qr_decode_from_ppm((const uint8_t*)"P6\n1 1\n255\n", 0, &len) == NULL);
}

}  // namespace qr_test
```

Add `test_qr.cpp` to the `add_executable(testliboffs ...)` source list in `test/CMakeLists.txt` (next to `test_block.cpp`).

- [ ] **Step 2: Run test to verify it fails**

```bash
cmake --build build --target testliboffs -j$(nproc) && ./build/test/testliboffs --gtest_filter='Qr*'
```

Expected: **compile failure** — `src/QR/qr.h` does not exist yet.

- [ ] **Step 3: Write the header**

Create `src/QR/qr.h`:

```c
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
   (error-correction level M, 4x pixel scale — byte-compatible with the
   rendering previously inline in HTTP/peer_routes.c). Returns a malloc'd
   buffer (caller frees with free()) and sets *out_len, or NULL on failure. */
uint8_t* qr_encode_to_ppm(const uint8_t* payload, size_t payload_len,
                          size_t* out_len);

/* Parse a binary P6 PPM, locate the first decodable QR code, and return its
   payload bytes as a malloc'd buffer (caller frees with free()). Returns
   NULL and does not touch *out_len if the image is not valid P6, contains
   no QR code, or the QR payload fails to decode. */
uint8_t* qr_decode_from_ppm(const uint8_t* ppm_data, size_t ppm_len,
                            size_t* out_len);

#endif /* LIBOFFS_QR_H */
```

- [ ] **Step 4: Write the implementation**

Create `src/QR/qr.c`:

```c
#include "qr.h"
#include <qrencode.h>
#include <quirc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Matches the rendering previously inline in HTTP/peer_routes.c so images
   already in circulation stay decodable. */
#define QR_PIXEL_SCALE 4

/* Strict P6: magic, whitespace, width, whitespace, height, whitespace,
   maxval (must be exactly 255), exactly one whitespace byte, then
   width*height*3 binary RGB bytes. Anything else is rejected — the encoder
   below is the only producer we support. */
static const uint8_t* _ppm_skip_ws(const uint8_t* p, const uint8_t* end) {
  while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
  return p;
}

static const uint8_t* _ppm_read_int(const uint8_t* p, const uint8_t* end,
                                    int* out) {
  p = _ppm_skip_ws(p, end);
  if (p >= end || *p < '0' || *p > '9') return NULL;
  int value = 0;
  while (p < end && *p >= '0' && *p <= '9') {
    value = value * 10 + (*p - '0');
    if (value > 1000000) return NULL;  /* sane image size bound */
    p++;
  }
  *out = value;
  return p;
}

uint8_t* qr_encode_to_ppm(const uint8_t* payload, size_t payload_len,
                          size_t* out_len) {
  if (payload == NULL || payload_len == 0 || out_len == NULL) return NULL;

  QRcode* code = QRcode_encodeData((int)payload_len, payload, 0, QR_ECLEVEL_M);
  if (code == NULL) return NULL;

  int qr_size = code->width;
  int img_size = qr_size * QR_PIXEL_SCALE;
  size_t header_len = (size_t)snprintf(NULL, 0, "P6\n%d %d\n255\n", img_size, img_size);
  size_t ppm_size = header_len + (size_t)img_size * img_size * 3;

  uint8_t* ppm = malloc(ppm_size);
  if (ppm == NULL) {
    QRcode_free(code);
    return NULL;
  }
  int printed = snprintf((char*)ppm, header_len + 1, "P6\n%d %d\n255\n", img_size, img_size);
  size_t offset = (size_t)printed;
  for (int y = 0; y < qr_size; y++) {
    for (int sy = 0; sy < QR_PIXEL_SCALE; sy++) {
      for (int x = 0; x < qr_size; x++) {
        uint8_t pixel = (code->data[y * qr_size + x] & 1) ? 0 : 255;
        for (int sx = 0; sx < QR_PIXEL_SCALE; sx++) {
          ppm[offset++] = pixel;
          ppm[offset++] = pixel;
          ppm[offset++] = pixel;
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

  int gray_w = 0, gray_h = 0;
  uint8_t* gray = quirc_begin(decoder, &gray_w, &gray_h);
  if (gray == NULL) {
    quirc_destroy(decoder);
    return NULL;
  }
  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      const uint8_t* rgb = cursor + ((size_t)y * width + x) * 3;
      /* ITU-R BT.601 luma, matching standard PPM→grayscale conversion */
      gray[y * width + x] =
          (uint8_t)((rgb[0] * 299 + rgb[1] * 587 + rgb[2] * 114) / 1000);
    }
  }
  quirc_end(decoder);

  uint8_t* result = NULL;
  size_t result_len = 0;
  int count = quirc_count(decoder);
  for (int i = 0; i < count && result == NULL; i++) {
    struct quirc_code code;
    struct quirc_data data;
    quirc_extract(decoder, i, &code);
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
```

- [ ] **Step 5: Run tests to verify they pass**

```bash
cmake --build build --target testliboffs -j$(nproc) && ./build/test/testliboffs --gtest_filter='Qr*'
```

Expected: all `Qr*` tests PASS.

- [ ] **Step 6: Commit**

```bash
git add src/QR test/test_qr.cpp test/CMakeLists.txt
git commit -m "feat(qr): add PPM QR codec module (libqrencode encode, quirc decode)"
```

---

### Task 3: Wire protocol — `PEER_INFO_REQUEST` format byte (TDD)

**Files:**
- Modify: `src/ClientAPI/client_api_wire.h` (comment at ~line 240, decls at ~line 378)
- Modify: `src/ClientAPI/client_api_wire.c:1077-1083`
- Create: `test/test_qr_wire.cpp`
- Modify: `test/CMakeLists.txt` (add `test_qr_wire.cpp`)

Note: `client_api_peer_connect_decode` and `client_api_friend_add_decode` already accept any format byte (they only validate "is a uint") and their payloads are already bstr — **no changes needed there**; only the request frame gains format support.

- [ ] **Step 1: Write the failing test**

Create `test/test_qr_wire.cpp`:

```cpp
#include <gtest/gtest.h>
#include <cstring>

extern "C" {
#include "../src/ClientAPI/client_api_wire.h"
#include <cbor.h>
}

namespace qr_wire_test {

TEST(PeerInfoRequestWire, BareFrameDecodesToFormatCbor) {
  cbor_item_t* frame = client_api_peer_info_request_encode();
  uint8_t format = 99;
  ASSERT_EQ(0, client_api_peer_info_request_decode(frame, &format));
  EXPECT_EQ(0, format);  /* backward compatible default: raw CBOR */
  cbor_decref(&frame);
}

TEST(PeerInfoRequestWire, FormatTwoRoundTrips) {
  cbor_item_t* frame = client_api_peer_info_request_encode_format(2);
  uint8_t format = 0;
  ASSERT_EQ(0, client_api_peer_info_request_decode(frame, &format));
  EXPECT_EQ(2, format);
  cbor_decref(&frame);
}

TEST(PeerInfoRequestWire, FormatOneRoundTrips) {
  cbor_item_t* frame = client_api_peer_info_request_encode_format(1);
  uint8_t format = 0;
  ASSERT_EQ(0, client_api_peer_info_request_decode(frame, &format));
  EXPECT_EQ(1, format);
  cbor_decref(&frame);
}

TEST(PeerInfoRequestWire, UnknownFormatRejected) {
  cbor_item_t* array = cbor_new_definite_array(2);
  cbor_item_t* type = cbor_build_uint8(CLIENT_API_PEER_INFO_REQUEST);
  cbor_item_t* fmt = cbor_build_uint8(7);
  (void)cbor_array_push(array, type);
  (void)cbor_array_push(array, fmt);
  cbor_decref(&type);
  cbor_decref(&fmt);
  uint8_t format = 0;
  EXPECT_NE(0, client_api_peer_info_request_decode(array, &format));
  cbor_decref(&array);
}

TEST(PeerInfoRequestWire, ExtraElementRejected) {
  cbor_item_t* array = cbor_new_definite_array(3);
  cbor_item_t* type = cbor_build_uint8(CLIENT_API_PEER_INFO_REQUEST);
  cbor_item_t* fmt = cbor_build_uint8(2);
  cbor_item_t* extra = cbor_build_uint8(7);
  (void)cbor_array_push(array, type);
  (void)cbor_array_push(array, fmt);
  (void)cbor_array_push(array, extra);
  cbor_decref(&type);
  cbor_decref(&fmt);
  cbor_decref(&extra);
  uint8_t format = 0;
  EXPECT_NE(0, client_api_peer_info_request_decode(array, &format));
  cbor_decref(&array);
}

TEST(PeerConnectWire, FormatTwoPassesThrough) {
  /* PEER_CONNECT/FRIEND_ADD decoders already accept any format byte —
     pin that behavior so format 2 (PPM image) flows through unchanged. */
  cbor_item_t* array = cbor_new_definite_array(3);
  cbor_item_t* type = cbor_build_uint8(CLIENT_API_PEER_CONNECT);
  cbor_item_t* fmt = cbor_build_uint8(2);
  const uint8_t image_bytes[] = {'P', '6', '\n'};
  cbor_item_t* data = cbor_build_bytestring(image_bytes, sizeof(image_bytes));
  (void)cbor_array_push(array, type);
  (void)cbor_array_push(array, fmt);
  (void)cbor_array_push(array, data);
  cbor_decref(&type);
  cbor_decref(&fmt);
  cbor_decref(&data);

  client_api_peer_connect_t msg;
  ASSERT_EQ(0, client_api_peer_connect_decode(array, &msg));
  EXPECT_EQ(2, msg.format);
  EXPECT_EQ(sizeof(image_bytes), msg.data_size);
  client_api_peer_connect_destroy(&msg);
  cbor_decref(&array);
}

}  // namespace qr_wire_test
```

Add `test_qr_wire.cpp` to the test source list in `test/CMakeLists.txt`.

- [ ] **Step 2: Run test to verify it fails**

```bash
cmake --build build --target testliboffs -j$(nproc)
```

Expected: **compile failure** — `client_api_peer_info_request_encode_format` / `client_api_peer_info_request_decode` undeclared.

- [ ] **Step 3: Update the header**

In `src/ClientAPI/client_api_wire.h`, update the frame comments (~line 239-247):

```c
// --- Peer Info Request ---
// [type] or [type, format: uint]
// format: 0 = raw CBOR (default), 1 = Base58 text, 2 = PPM QR image

// --- Peer Info Response ---
// [type, format_byte, data: bstr]
// format_byte: 0 = raw CBOR, 1 = Base58 text, 2 = PPM QR image
```

and add declarations next to the existing one (~line 378):

```c
cbor_item_t* client_api_peer_info_request_encode(void);
/* Same frame with an explicit response format byte:
   0 = raw CBOR, 1 = base58 text, 2 = PPM QR image. */
cbor_item_t* client_api_peer_info_request_encode_format(uint8_t format);
/* Decode [type] or [type, format]; *format defaults to 0 for the 1-element
   form. Rejects unknown formats and frames with extra elements. */
int client_api_peer_info_request_decode(cbor_item_t* item, uint8_t* format);
```

- [ ] **Step 4: Implement in client_api_wire.c**

Replace `client_api_peer_info_request_encode` (lines 1077-1083) with:

```c
cbor_item_t* client_api_peer_info_request_encode(void) {
  return client_api_peer_info_request_encode_format(0);
}

cbor_item_t* client_api_peer_info_request_encode_format(uint8_t format) {
  cbor_item_t* array = cbor_new_definite_array(2);
  cbor_item_t* item = cbor_build_uint8(CLIENT_API_PEER_INFO_REQUEST);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);
  item = cbor_build_uint8(format);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);
  return array;
}

int client_api_peer_info_request_decode(cbor_item_t* item, uint8_t* format) {
  if (item == NULL || format == NULL || !cbor_isa_array(item)) return -1;
  size_t size = cbor_array_size(item);
  if (size < 1 || size > 2) return -1;

  cbor_item_t* type_item = cbor_array_get(item, 0);
  if (!cbor_isa_uint(type_item) ||
      cbor_get_uint8(type_item) != CLIENT_API_PEER_INFO_REQUEST) {
    cbor_decref(&type_item);
    return -1;
  }
  cbor_decref(&type_item);

  *format = 0;  /* bare [type] frame means raw CBOR, as before */
  if (size == 2) {
    cbor_item_t* format_item = cbor_array_get(item, 1);
    if (!cbor_isa_uint(format_item) || cbor_get_uint8(format_item) > 2) {
      cbor_decref(&format_item);
      return -1;
    }
    *format = cbor_get_uint8(format_item);
    cbor_decref(&format_item);
  }
  return 0;
}
```

- [ ] **Step 5: Run tests to verify they pass**

```bash
cmake --build build --target testliboffs -j$(nproc) && ./build/test/testliboffs --gtest_filter='PeerInfoRequestWire*:PeerConnectWire*'
```

Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add src/ClientAPI/client_api_wire.h src/ClientAPI/client_api_wire.c test/test_qr_wire.cpp test/CMakeLists.txt
git commit -m "feat(wire): PEER_INFO_REQUEST gains response format byte (2 = PPM QR image)"
```

---

### Task 4: Shared payload helper + daemon handler format 2 (TDD)

**Files:**
- Modify: `src/ClientAPI/peer_handlers.h` (add decl)
- Modify: `src/ClientAPI/peer_handlers.c:96-156, 19-94, 223-306`
- Create: `test/test_peer_payload.cpp`
- Modify: `test/CMakeLists.txt` (add `test_peer_payload.cpp`)

- [ ] **Step 1: Write the failing test**

Create `test/test_peer_payload.cpp`:

```cpp
#include <gtest/gtest.h>
#include <cstring>
#include <cstdlib>

extern "C" {
#include "../src/ClientAPI/peer_handlers.h"
#include "../src/QR/qr.h"
#include "../src/Network/peer_info.h"
#include <cbor.h>
}

namespace peer_payload_test {

/* A minimal valid peer_info: one DIRECT address, 4-byte public key. */
static peer_info_t _make_info() {
  peer_info_t info;
  memset(&info, 0, sizeof(info));
  for (size_t i = 0; i < NODE_ID_HASH_SIZE; i++) info.node_id.hash[i] = (uint8_t)(i + 1);
  snprintf(info.node_id.str, NODE_ID_STRING_SIZE, "testnode");
  static const uint8_t key[4] = {0xDE, 0xAD, 0xBE, 0xEF};
  info.public_key = (uint8_t*)key;
  info.public_key_len = sizeof(key);
  info.address_count = 1;
  info.addresses = (peer_address_t*)calloc(1, sizeof(peer_address_t));
  info.addresses[0].type = PEER_ADDR_DIRECT;
  info.addresses[0].host = strdup("10.0.0.1");
  info.addresses[0].port = 23401;
  return info;
}

static void _free_info(peer_info_t* info) {
  free(info->addresses[0].host);
  free(info->addresses);
}

TEST(PeerInfoFromPayload, FormatTwoQrImageRoundTrips) {
  peer_info_t original = _make_info();

  cbor_item_t* encoded = peer_info_encode(&original);
  ASSERT_NE(encoded, nullptr);
  size_t serialized_len = cbor_serialized_size(encoded);
  uint8_t* serialized = (uint8_t*)malloc(serialized_len);
  ASSERT_GT(cbor_serialize(encoded, serialized, serialized_len), 0u);
  cbor_decref(&encoded);

  size_t ppm_len = 0;
  uint8_t* ppm = qr_encode_to_ppm(serialized, serialized_len, &ppm_len);
  free(serialized);
  ASSERT_NE(ppm, nullptr);

  peer_info_t decoded;
  memset(&decoded, 0, sizeof(decoded));
  ASSERT_EQ(0, peer_info_from_payload(2, ppm, ppm_len, &decoded));
  free(ppm);

  EXPECT_EQ(0, memcmp(decoded.node_id.hash, original.node_id.hash, NODE_ID_HASH_SIZE));
  EXPECT_EQ(1u, decoded.address_count);
  EXPECT_STREQ(decoded.addresses[0].host, "10.0.0.1");
  EXPECT_EQ(23401, decoded.addresses[0].port);
  peer_info_destroy(&decoded);
  _free_info(&original);
}

TEST(PeerInfoFromPayload, FormatTwoGarbageImageRejected) {
  peer_info_t decoded;
  memset(&decoded, 0, sizeof(decoded));
  const char* garbage = "not an image at all";
  EXPECT_NE(0, peer_info_from_payload(2, (const uint8_t*)garbage, strlen(garbage), &decoded));
}

TEST(PeerInfoFromPayload, FormatTwoNonPeerInfoQrRejected) {
  /* A valid QR whose payload is not peer_info CBOR */
  const char* payload = "hello, not a peer info map";
  size_t ppm_len = 0;
  uint8_t* ppm = qr_encode_to_ppm((const uint8_t*)payload, strlen(payload), &ppm_len);
  ASSERT_NE(ppm, nullptr);

  peer_info_t decoded;
  memset(&decoded, 0, sizeof(decoded));
  EXPECT_NE(0, peer_info_from_payload(2, ppm, ppm_len, &decoded));
  free(ppm);
}

TEST(PeerInfoFromPayload, UnknownFormatRejected) {
  peer_info_t decoded;
  memset(&decoded, 0, sizeof(decoded));
  EXPECT_NE(0, peer_info_from_payload(7, (const uint8_t*)"x", 1, &decoded));
}

}  // namespace peer_payload_test
```

Add `test_peer_payload.cpp` to the test source list.

- [ ] **Step 2: Run test to verify it fails**

```bash
cmake --build build --target testliboffs -j$(nproc)
```

Expected: **compile failure** — `peer_info_from_payload` undeclared.

- [ ] **Step 3: Add the helper declaration**

In `src/ClientAPI/peer_handlers.h`, add:

```c
#include "../Network/peer_info.h"
#include <stddef.h>
#include <stdint.h>

/* Decode a peer_info payload by wire format byte: 0 = raw CBOR peer_info
   map, 1 = base58 text, 2 = PPM QR image (decoded via src/QR, then parsed
   as CBOR peer_info). Returns 0 on success, -1 if the payload is not
   decodable in the given format. Shared by the socket handlers and the
   HTTP routes so both transports accept exactly the same inputs. */
int peer_info_from_payload(uint8_t format, const uint8_t* data,
                           size_t data_size, peer_info_t* info);
```

(Adjust includes to whatever the header already has.)

- [ ] **Step 4: Implement the helper and refactor the handlers**

In `src/ClientAPI/peer_handlers.c`, add the include and the helper, and replace the inline format branches:

```c
#include "../QR/qr.h"

int peer_info_from_payload(uint8_t format, const uint8_t* data,
                           size_t data_size, peer_info_t* info) {
  if (info == NULL) return -1;

  if (format == 0) {
    /* CBOR bytes */
    struct cbor_load_result load_result;
    cbor_item_t* decoded = cbor_load(data, data_size, &load_result);
    if (decoded == NULL || load_result.error.code != CBOR_ERR_NONE) {
      if (decoded != NULL) cbor_decref(&decoded);
      return -1;
    }
    int rc = peer_info_decode(decoded, info);
    cbor_decref(&decoded);
    return rc;
  }

  if (format == 1) {
    /* Base58 text */
    char* b58_str = get_clear_memory(data_size + 1);
    if (b58_str == NULL) return -1;
    memcpy(b58_str, data, data_size);
    b58_str[data_size] = '\0';
    int rc = peer_info_from_base58(b58_str, info);
    free(b58_str);
    return rc;
  }

  if (format == 2) {
    /* PPM QR image → payload bytes → CBOR peer_info */
    size_t payload_len = 0;
    uint8_t* payload = qr_decode_from_ppm(data, data_size, &payload_len);
    if (payload == NULL) return -1;
    struct cbor_load_result load_result;
    cbor_item_t* decoded = cbor_load(payload, payload_len, &load_result);
    free(payload);
    if (decoded == NULL || load_result.error.code != CBOR_ERR_NONE) {
      if (decoded != NULL) cbor_decref(&decoded);
      return -1;
    }
    int rc = peer_info_decode(decoded, info);
    cbor_decref(&decoded);
    return rc;
  }

  return -1;
}
```

In `peer_handle_connect` (lines 108-131), replace the `decode_ok` block:

```c
  int decode_ok = peer_info_from_payload(msg.format, msg.data, msg.data_size,
                                         &remote_info);

  client_api_peer_connect_destroy(&msg);
```

(delete the old `if (msg.format == 0) {...} else if (msg.format == 1) {...}` branches and the `memset(&remote_info, ...)` stays before the call).

In `peer_handle_friend_add` (lines 243-262), make the same replacement:

```c
  int decode_ok = peer_info_from_payload(msg.format, msg.data, msg.data_size,
                                         new_friend);

  client_api_friend_add_destroy(&msg);
```

In `peer_handle_info_request` (line 19): stop discarding the frame and honor the requested format.

Add after the auth check:

```c
  uint8_t format = 0;
  if (client_api_peer_info_request_decode(frame, &format) != 0) {
    ctx->send_error(ctx->conn, CLIENT_API_STATUS_BAD_REQUEST,
                    "Invalid peer info request");
    return;
  }
```

Remove the `(void)frame;` line. Then change the response-building tail (lines 84-93) to:

```c
  /* Build and send response */
  client_api_peer_info_response_t response;
  memset(&response, 0, sizeof(response));

  if (format == 2) {
    /* PPM QR image — ownership of the encoded image transfers to the
       response struct, which frees it in client_api_peer_info_response_destroy. */
    size_t ppm_len = 0;
    uint8_t* ppm = qr_encode_to_ppm(serialized, bytes_serialized, &ppm_len);
    free(serialized);
    if (ppm == NULL) {
      ctx->send_error(ctx->conn, CLIENT_API_STATUS_INTERNAL_ERROR,
                      "QR encoding failed");
      return;
    }
    response.format = 2;
    response.data = ppm;
    response.data_size = ppm_len;
  } else {
    response.format = format;  /* 0 = raw CBOR */
    response.data = serialized;
    response.data_size = bytes_serialized;
  }

  cbor_item_t* out_frame = client_api_peer_info_response_encode(&response);
  ctx->send_frame(ctx->conn, out_frame);
```

(The `free(serialized)` before the QR branch replaces the old ownership: previously `serialized` was handed to the response directly — in format 0 it still is.)

- [ ] **Step 5: Run tests to verify they pass**

```bash
cmake --build build --target testliboffs -j$(nproc) && ./build/test/testliboffs --gtest_filter='PeerInfoFromPayload*:PeerInfoRequestWire*'
```

Expected: PASS.

- [ ] **Step 6: Run the full suite to catch regressions in the refactored handlers**

```bash
./build/test/testliboffs
```

Expected: all tests PASS (no new failures — the unix/ws transport peer tests exercise these handlers).

- [ ] **Step 7: Commit**

```bash
git add src/ClientAPI/peer_handlers.h src/ClientAPI/peer_handlers.c test/test_peer_payload.cpp test/CMakeLists.txt
git commit -m "feat(peers): decode QR images (format 2) in socket peer handlers"
```

---

### Task 5: HTTP peer routes — vendored encoder + PPM bodies

**Files:**
- Modify: `src/ClientAPI/HTTP/peer_routes.c` (includes at 22-24; `_peer_info_handler` 155-260; `_decode_peer_info_body` 105-125)

- [ ] **Step 1: Swap the qrencode include for the QR module**

Remove:

```c
#ifdef HAS_QRENCODE
#include <qrencode.h>
#endif
```

Add (with the other project includes):

```c
#include "../../QR/qr.h"
```

- [ ] **Step 2: Simplify `_peer_info_handler`'s QR branch**

Delete both `#ifdef HAS_QRENCODE` ... `#endif` regions (the render block at ~179-244 and the 501 stub at ~246-254) and replace the QR branch with:

```c
  if (strcmp(format, "qrcode") == 0) {
    cbor_item_t* cbor_map = peer_info_encode(info);
    if (cbor_map == NULL) {
      http_response_set_status(response, HTTP_STATUS_INTERNAL_SERVER_ERROR);
      http_response_set_header(response, "Content-Type", "text/plain");
      http_response_write(response, "Failed to encode peer info", 25);
      http_response_end(response);
      peer_info_destroy(info);
      free(info);
      return;
    }

    uint8_t* serialized = NULL;
    size_t serialized_len = _serialize_cbor(cbor_map, &serialized);
    cbor_decref(&cbor_map);
    if (serialized_len == 0) {
      http_response_set_status(response, HTTP_STATUS_INTERNAL_SERVER_ERROR);
      http_response_set_header(response, "Content-Type", "text/plain");
      http_response_write(response, "CBOR serialization failed", 25);
      http_response_end(response);
      peer_info_destroy(info);
      free(info);
      return;
    }

    size_t ppm_len = 0;
    uint8_t* ppm = qr_encode_to_ppm(serialized, serialized_len, &ppm_len);
    free(serialized);
    if (ppm == NULL) {
      http_response_set_status(response, HTTP_STATUS_INTERNAL_SERVER_ERROR);
      http_response_set_header(response, "Content-Type", "text/plain");
      http_response_write(response, "QR encoding failed", 18);
      http_response_end(response);
      peer_info_destroy(info);
      free(info);
      return;
    }

    http_response_set_status(response, HTTP_STATUS_OK);
    http_response_set_header(response, "Content-Type", "image/x-portable-pixmap");
    http_response_write(response, (const char*)ppm, ppm_len);
    free(ppm);
    http_response_end(response);
    peer_info_destroy(info);
    free(info);
    return;
  }
```

The generated image stays byte-identical to the old output (same `QR_ECLEVEL_M`, same 4x scale — Task 2 moved the rendering verbatim).

- [ ] **Step 3: Accept PPM bodies in `_decode_peer_info_body`**

Add this branch at the top of the Content-Type dispatch (before the base58 default), and make the CBOR branch use the shared helper so both transports accept identical inputs:

```c
#include "../peer_handlers.h"   /* peer_info_from_payload */
```

```c
static int _decode_peer_info_body(http_request_t* request, peer_info_t* info) {
  const char* content_type = http_request_header(request, "Content-Type");

  if (content_type != NULL && strstr(content_type, "image/x-portable-pixmap") != NULL) {
    /* QR image body — decode via the shared payload helper (format 2) */
    if (request->body == NULL || request->body->size == 0) return -1;
    return peer_info_from_payload(2, request->body->data, request->body->size, info);
  }

  if (content_type != NULL && strstr(content_type, "application/cbor") != NULL) {
    if (request->body == NULL || request->body->size == 0) return -1;
    return peer_info_from_payload(0, request->body->data, request->body->size, info);
  }

  /* Default: base58 text (text/plain or no Content-Type) */
  if (request->body == NULL || request->body->size == 0) return -1;
  return peer_info_from_payload(1, request->body->data, request->body->size, info);
}
```

This replaces the existing inline CBOR/base58 branches in `_decode_peer_info_body` (lines 105-125) — the old code is functionally identical to formats 0/1 of the helper, so the two transports can no longer drift.

- [ ] **Step 4: Build and run the full suite**

```bash
cmake --build build -j$(nproc) && ./build/test/testliboffs
```

Expected: builds without `HAS_QRENCODE` anywhere; all tests PASS.

- [ ] **Step 5: Confirm the 501 path is gone**

```bash
grep -rn "HAS_QRENCODE\|QR code generation not available" src/
```

Expected: no output.

- [ ] **Step 6: Commit**

```bash
git add src/ClientAPI/HTTP/peer_routes.c
git commit -m "feat(http): QR peer info via vendored encoder; accept PPM QR bodies on connect/friend"
```

---

### Task 6: C client library — peer/QR functions

**Files:**
- Modify: `src/ClientLibs/c/offs_client.h` (callback decls ~52-61, new functions after `offs_client_health` at ~124)
- Modify: `src/ClientLibs/c/offs_client.c` (struct fields ~151-166, `_handle_frame` switch ~530, new functions after `offs_client_health` at ~1855)

The C client currently has **no** peer functions — these are all new, following the `offs_client_health` pattern (store callback under lock, encode request, `_send_frame`, dispatch in `_handle_frame`).

- [ ] **Step 1: Add callback typedefs and function decls to offs_client.h**

After the existing callback typedefs (~line 61):

```c
typedef void (*offs_peer_info_cb_t)(void* ctx, uint8_t format, const uint8_t* data, size_t data_len);
typedef void (*offs_peer_connect_cb_t)(void* ctx, uint8_t status);
typedef void (*offs_friend_list_cb_t)(void* ctx, cbor_item_t* friends);
```

After `offs_client_health` (~line 124):

```c
/* Peer operations. format: 0 = raw CBOR peer_info, 1 = base58 text,
   2 = PPM QR image. The _qr forms are sugar for format 2. */
int offs_client_peer_info(offs_client_t* client, offs_peer_info_cb_t callback, void* ctx);
int offs_client_peer_info_ex(offs_client_t* client, uint8_t format,
                             offs_peer_info_cb_t callback, void* ctx);
int offs_client_peer_connect(offs_client_t* client, uint8_t format,
                             const uint8_t* data, size_t data_len,
                             offs_peer_connect_cb_t callback, void* ctx);
int offs_client_peer_connect_qr(offs_client_t* client, const uint8_t* ppm, size_t ppm_len,
                                offs_peer_connect_cb_t callback, void* ctx);
int offs_client_friend_add(offs_client_t* client, uint8_t format,
                           const uint8_t* data, size_t data_len,
                           offs_peer_connect_cb_t callback, void* ctx);
int offs_client_friend_add_qr(offs_client_t* client, const uint8_t* ppm, size_t ppm_len,
                              offs_peer_connect_cb_t callback, void* ctx);
```

(`offs_client.h` already transitively sees `cbor.h` via wire includes; add `#include <cbor.h>` if not.)

- [ ] **Step 2: Add struct fields and dispatch**

In the `offs_client` struct (offs_client.c ~line 151-166) add:

```c
  offs_peer_info_cb_t peer_info_cb;
  void* peer_info_cb_ctx;
  offs_peer_connect_cb_t peer_connect_cb;
  void* peer_connect_cb_ctx;
```

In the `_handle_frame` callback-snapshot block (after `health_cb_ctx`) add:

```c
  offs_peer_info_cb_t peer_info_cb = client->peer_info_cb;
  void* peer_info_cb_ctx = client->peer_info_cb_ctx;
  offs_peer_connect_cb_t peer_connect_cb = client->peer_connect_cb;
  void* peer_connect_cb_ctx = client->peer_connect_cb_ctx;
```

In the switch, add two cases before `default:`:

```c
    case CLIENT_API_PEER_INFO_RESPONSE: {
      client_api_peer_info_response_t msg;
      memset(&msg, 0, sizeof(msg));
      if (client_api_peer_info_response_decode(frame, &msg) == 0) {
        if (peer_info_cb != NULL) {
          peer_info_cb(peer_info_cb_ctx, msg.format, msg.data, msg.data_size);
        }
        client_api_peer_info_response_destroy(&msg);
      }
      break;
    }
    case CLIENT_API_PEER_CONNECT_RESULT: {
      client_api_peer_connect_result_t msg;
      memset(&msg, 0, sizeof(msg));
      if (client_api_peer_connect_result_decode(frame, &msg) == 0) {
        if (peer_connect_cb != NULL) {
          peer_connect_cb(peer_connect_cb_ctx, msg.status);
        }
        client_api_peer_connect_result_destroy(&msg);
      }
      break;
    }
```

(`FRIEND_ADD` replies reuse `CLIENT_API_PEER_CONNECT_RESULT` — `peer_handle_friend_add` sends exactly that frame, so one callback type covers both.)

- [ ] **Step 3: Implement the functions**

After `offs_client_health` (offs_client.c ~line 1855):

```c
int offs_client_peer_info_ex(offs_client_t* client, uint8_t format,
                             offs_peer_info_cb_t callback, void* ctx) {
  if (client == NULL || !client->connected) return -1;

  platform_mutex_lock(client->lock);
  client->peer_info_cb = callback;
  client->peer_info_cb_ctx = ctx;
  platform_mutex_unlock(client->lock);

  cbor_item_t* frame = (format == 0)
      ? client_api_peer_info_request_encode()
      : client_api_peer_info_request_encode_format(format);
  _send_frame(client, frame);
  return 0;
}

int offs_client_peer_info(offs_client_t* client,
                          offs_peer_info_cb_t callback, void* ctx) {
  return offs_client_peer_info_ex(client, 0, callback, ctx);
}

int offs_client_peer_connect(offs_client_t* client, uint8_t format,
                             const uint8_t* data, size_t data_len,
                             offs_peer_connect_cb_t callback, void* ctx) {
  if (client == NULL || !client->connected || data == NULL || data_len == 0) return -1;

  platform_mutex_lock(client->lock);
  client->peer_connect_cb = callback;
  client->peer_connect_cb_ctx = ctx;
  platform_mutex_unlock(client->lock);

  client_api_peer_connect_t msg;
  memset(&msg, 0, sizeof(msg));
  msg.format = format;
  msg.data = (uint8_t*)data;
  msg.data_size = data_len;

  cbor_item_t* frame = client_api_peer_connect_encode(&msg);
  _send_frame(client, frame);
  return 0;
}

int offs_client_peer_connect_qr(offs_client_t* client, const uint8_t* ppm, size_t ppm_len,
                                offs_peer_connect_cb_t callback, void* ctx) {
  return offs_client_peer_connect(client, 2, ppm, ppm_len, callback, ctx);
}

int offs_client_friend_add(offs_client_t* client, uint8_t format,
                           const uint8_t* data, size_t data_len,
                           offs_peer_connect_cb_t callback, void* ctx) {
  if (client == NULL || !client->connected || data == NULL || data_len == 0) return -1;

  platform_mutex_lock(client->lock);
  client->peer_connect_cb = callback;
  client->peer_connect_cb_ctx = ctx;
  platform_mutex_unlock(client->lock);

  client_api_friend_add_t msg;
  memset(&msg, 0, sizeof(msg));
  msg.format = format;
  msg.data = (uint8_t*)data;
  msg.data_size = data_len;

  cbor_item_t* frame = client_api_friend_add_encode(&msg);
  _send_frame(client, frame);
  return 0;
}

int offs_client_friend_add_qr(offs_client_t* client, const uint8_t* ppm, size_t ppm_len,
                              offs_peer_connect_cb_t callback, void* ctx) {
  return offs_client_friend_add(client, 2, ppm, ppm_len, callback, ctx);
}
```

- [ ] **Step 4: Build and run the suite**

```bash
cmake --build build -j$(nproc) && ./build/test/testliboffs --gtest_filter='OffsClient*'
```

Expected: builds; existing client tests still PASS (the new callback fields default to NULL and the new response cases are additive).

- [ ] **Step 5: Commit**

```bash
git add src/ClientLibs/c/offs_client.h src/ClientLibs/c/offs_client.c
git commit -m "feat(client): C client library peer info/connect/friend-add with QR format support"
```

---

### Task 7: JS client QR support

**Files:**
- Modify: `src/ClientLibs/js/offs-client/src/wire.js:269-270`
- Modify: `src/ClientLibs/js/offs-client/src/transports/http-transport.js:275-330`
- Modify: `src/ClientLibs/js/offs-client/src/index.js:396-460`

- [ ] **Step 1: `wire.js` — request frame gains the format byte**

Replace:

```js
export function encodePeerInfoRequest() {
  return encoder.encode([MSG.PEER_INFO_REQUEST]);
}
```

with:

```js
export function encodePeerInfoRequest(format = 0) {
  if (format === 0) {
    return encoder.encode([MSG.PEER_INFO_REQUEST]);   // old 1-element shape
  }
  return encoder.encode([MSG.PEER_INFO_REQUEST, format]);
}
```

(Keep the 1-element shape for format 0 so old daemons can parse new clients' requests.)

- [ ] **Step 2: `http-transport.js` — Content-Type by format, and pass format through**

Update the body-dispatch helper used by `peerConnect` (line 292-299) and `friendAdd` (line 321-328). Both currently send `format === 1 ? text : peerInfo` with a single hard-coded Content-Type. Change both to:

```js
const CONTENT_TYPES = { 0: 'application/cbor', 1: 'text/plain', 2: 'image/x-portable-pixmap' };
```

and in each method:

```js
const response = await fetch(this.url('/peer/connect'), {
  method: 'POST',
  headers: { 'Content-Type': CONTENT_TYPES[format] ?? 'application/cbor' },
  body: format === 1 ? new TextDecoder().decode(peerInfo) : peerInfo,
});
```

(same for `/friends`). Also verify `peerInfo(format)` (line 275): it already passes the string through to `?format=${format}` — the daemon accepts `cbor|base58|qrcode`, so `'qrcode'` needs no change. Make it return the raw PPM `Uint8Array` for `qrcode` if it doesn't already (check the existing response handling and keep its behavior; only the Content-Type dispatch above is a required change).

- [ ] **Step 3: `index.js` — pass format on CBOR transports, add QR sugar**

In `peerInfo` (line 396), map the JS format string to the wire byte and pass it:

```js
const FORMAT_WIRE = { cbor: 0, base58: 1, qrcode: 2 };

async peerInfo(format = 'cbor') {
  if (this.transport instanceof HttpTransport) {
    return this.transport.peerInfo(format);
  }

  const requestBytes = wire.encodePeerInfoRequest(FORMAT_WIRE[format] ?? 0);
  const responseBytes = await this._sendAndWait(requestBytes, wire.MSG.PEER_INFO_RESPONSE);
  return wire.decodePeerInfoResponse(responseBytes);
}
```

Add sugar methods after `peerConnect`:

```js
/**
 * Connect to a peer from a QR image (binary P6 PPM bytes).
 * @param {Uint8Array} ppmBytes
 * @returns {Promise<{status: number}>}
 */
async peerConnectQr(ppmBytes) {
  return this.peerConnect(ppmBytes, 2);
}

/**
 * Add a friend from a QR image (binary P6 PPM bytes).
 * @param {Uint8Array} ppmBytes
 * @returns {Promise<void>}
 */
async friendAddQr(ppmBytes) {
  return this.friendAdd(ppmBytes, 2);
}
```

- [ ] **Step 4: Build the package**

```bash
cd src/ClientLibs/js/offs-client && npm install && npm run build
```

Expected: build succeeds; `dist/offs-client.esm.js` and `dist/offs-client.umd.js` regenerate (they are tracked in git).

- [ ] **Step 5: Commit**

```bash
git add src/ClientLibs/js/offs-client
git commit -m "feat(js-client): QR peer info/connect/friend support (format byte 2)"
```

---

### Task 8: CLI `--qr` flags

**Files:**
- Modify: `OFFS/src/offs/commands/peer.c` (info at 25-56, connect at 85-120)
- Modify: `OFFS/src/offs/commands/friend.c` (add at 18-48)

- [ ] **Step 1: `offs peer info --qr <file>`**

In `cmd_peer`, replace the `info` block's fixed request with flag parsing:

```c
  if (strcmp(subcommand, "info") == 0) {
    uint8_t format = 0;
    const char* qr_path = NULL;
    for (int i = 1; i < argc; i++) {
      if (strcmp(argv[i], "--qr") == 0 && i + 1 < argc) {
        format = 2;   /* PPM QR image */
        qr_path = argv[++i];
      } else {
        printf("Usage: offs peer info [--qr <file>|-]\n");
        return 1;
      }
    }

    cbor_item_t* request = client_api_peer_info_request_encode_format(format);
    cbor_item_t* response = cli_client_send(client, request);
    cbor_decref(&request);

    if (response != NULL) {
      uint8_t type = client_api_wire_get_type(response);
      if (type == CLIENT_API_PEER_INFO_RESPONSE) {
        client_api_peer_info_response_t peer_resp;
        memset(&peer_resp, 0, sizeof(peer_resp));
        if (client_api_peer_info_response_decode(response, &peer_resp) == 0) {
          if (format == 2) {
            /* Write the PPM image to qr_path ("-" = stdout) */
            FILE* out = (strcmp(qr_path, "-") == 0)
                ? stdout
                : fopen(qr_path, "wb");
            if (out == NULL) {
              fprintf(stderr, "cannot open %s\n", qr_path);
            } else {
              fwrite(peer_resp.data, 1, peer_resp.data_size, out);
              if (out != stdout) fclose(out);
            }
          } else {
            /* existing base58 output path, unchanged */
            size_t b58_len = base58_encoded_length(peer_resp.data_size) + 1;
            char* b58 = (char*)malloc(b58_len);
            if (b58 != NULL) {
              int enc_rc = base58_encode(peer_resp.data, peer_resp.data_size,
                                b58, b58_len);
              if (enc_rc > 0) {
                b58[enc_rc] = '\0';
                printf("%s\n", L10N_PEER_INFO_PROMPT);
                printf("  Data: %s\n", b58);
              }
              free(b58);
            }
          }
          client_api_peer_info_response_destroy(&peer_resp);
        }
      } else if (type == CLIENT_API_ERROR) {
        client_api_error_t err_msg;
        memset(&err_msg, 0, sizeof(err_msg));
        if (client_api_error_decode(response, &err_msg) == 0) {
          fprintf(stderr, "%s: %s\n", L10N_ERROR, err_msg.message);
          client_api_error_destroy(&err_msg);
        }
      }
      cbor_decref(&response);
    }
    return 0;
  }
```

- [ ] **Step 2: `offs peer connect --qr <file>`**

Replace the fixed `peer_con` block (lines 89-95) with:

```c
    uint8_t format = 1;  /* default: base58 text (existing behavior) */
    uint8_t* file_data = NULL;
    size_t file_size = 0;

    if (strcmp(argv[1], "--qr") == 0) {
      if (argc < 3) {
        fprintf(stderr, "%s\n", L10N_PEER_CONNECT_USAGE);
        return 1;
      }
      FILE* input = fopen(argv[2], "rb");
      if (input == NULL) {
        fprintf(stderr, "cannot open %s\n", argv[2]);
        return 1;
      }
      fseek(input, 0, SEEK_END);
      long file_len = ftell(input);
      fseek(input, 0, SEEK_SET);
      file_data = (uint8_t*)malloc((size_t)file_len);
      if (file_data == NULL || fread(file_data, 1, (size_t)file_len, input) != (size_t)file_len) {
        fprintf(stderr, "cannot read %s\n", argv[2]);
        fclose(input);
        free(file_data);
        return 1;
      }
      fclose(input);
      file_size = (size_t)file_len;
      format = 2;
    }

    client_api_peer_connect_t peer_con;
    memset(&peer_con, 0, sizeof(peer_con));
    peer_con.format = format;
    peer_con.data = file_data != NULL ? file_data : (uint8_t*)argv[1];
    peer_con.data_size = file_data != NULL ? file_size : strlen(argv[1]);
```

and before every `return` in the connect branch, free the buffer: add `free(file_data);` after the response handling (the data pointer is only borrowed by the encode — `client_api_peer_connect_encode` copies). The cleanest spot: immediately after `cbor_decref(&request)`.

- [ ] **Step 3: `offs friend add [--qr <file> | <base58>]`**

Same pattern in `cmd_friend`'s `add` branch: if `argv[1] == "--qr"`, read `argv[2]` into a malloc'd buffer (identical code to Step 2), set `friend_req.format = 2` and point `friend_req.data`/`data_size` at the buffer; otherwise keep the existing format-0/base58 path. Free the buffer after `cbor_decref(&request)`.

- [ ] **Step 4: Rebuild the CLI**

The `offs` CLI lives in the sibling OFFS project (`OFFS/deps/liboffs` is a checkout of this repo). Rebuild there per its build docs; if the OFFS build is not available in this environment, verify by inspection and note it in the ticket.

```bash
cmake --build OFFS/deps/liboffs/build -j$(nproc) 2>&1 | tail -3 || echo "OFFS build not configured in this environment"
```

- [ ] **Step 5: Commit**

```bash
git add OFFS/src/offs/commands/peer.c OFFS/src/offs/commands/friend.c
git commit -m "feat(cli): offs peer info/connect and friend add accept --qr PPM files"
```

---

### Task 9: Manual end-to-end verification (HTTP + CLI)

**Files:** none (verification only)

- [ ] **Step 1: Start the example server**

```bash
cmake --build build -j$(nproc) && ./build/examples/off_server --port 23482 &
```

(Use a non-default port so this doesn't collide with a running daemon. Note: the example server registers peer routes only with auth configured — if peer routes are missing, run it with `--api-key testkey` and pass `-H "Authorization: Bearer testkey"` to every curl below.)

- [ ] **Step 2: HTTP generate → HTTP decode round trip**

```bash
curl -s -H "Authorization: Bearer testkey" \
  "http://127.0.0.1:23482/peer/info?format=qrcode" -o /tmp/peer_qr.ppm
file /tmp/peer_qr.ppm   # or: head -c 20 /tmp/peer_qr.ppm
curl -s -H "Authorization: Bearer testkey" \
  -H "Content-Type: image/x-portable-pixmap" \
  --data-binary @/tmp/peer_qr.ppm \
  "http://127.0.0.1:23482/peer/connect"
```

Expected: the file starts with `P6`; the connect response is JSON `{"status": ..., "message": ...}` — status 0/3/4 are all acceptable here (the peer is ourselves / unreachable); the important part is it is **not** a 400 decode failure.

```bash
curl -s -H "Authorization: Bearer testkey" \
  -H "Content-Type: image/x-portable-pixmap" \
  --data-binary "garbage" \
  "http://127.0.0.1:23482/peer/connect" -w "\n%{http_code}\n"
```

Expected: `400` (image decode failure).

- [ ] **Step 3: Verify base58 path still works** (backward compat)

```bash
curl -s -H "Authorization: Bearer testkey" \
  "http://127.0.0.1:23482/peer/info?format=base58" -o /tmp/peer_b58.txt
curl -s -H "Authorization: Bearer testkey" \
  --data-binary @/tmp/peer_b58.txt \
  "http://127.0.0.1:23482/peer/connect"
```

Expected: JSON status response, not 400.

- [ ] **Step 4: Stop the server**

```bash
kill %1
```

- [ ] **Step 5: Post verification evidence to the ticket**

```bash
H=/home/victor/.claude/skills/harmony/harmony
$H comment add OFFS-187 "Manual HTTP round trip verified: GET /peer/info?format=qrcode → P6 PPM; POST /peer/connect with Content-Type image/x-portable-pixmap decodes it; garbage image → 400; base58 path unchanged."
```

---

### Task 10: Docs, leak check, ticket close

**Files:**
- Modify: `docs/OFFS_API_CLI_SPEC.md`

- [ ] **Step 1: Update the API spec doc**

In `docs/OFFS_API_CLI_SPEC.md`:
- §2.4 `GET /peer/info`: remove the "requires `HAS_QRENCODE`; else `501`" caveat — QR generation is now always available (vendored libqrencode).
- §2.4 `POST /peer/connect` and `POST /friends`: add third accepted body type: `image/x-portable-pixmap` — daemon decodes the QR (P6 PPM) and parses the peer info; 400 on decode failure.
- §3 wire table: `PEER_INFO_REQUEST` is `[21]` or `[21, format]` (0 = raw CBOR, 1 = base58, **2 = PPM QR image**); `PEER_CONNECT`/`FRIEND_ADD` accept format 2 with a PPM image payload.
- §9 checklist: mark QR display/scan as supported server-side (HTTP `?format=qrcode`, wire format 2, `--qr` CLI flags).

- [ ] **Step 2: Run the full test suite under valgrind** (project convention: rebuild with `-gdwarf-4` if valgrind chokes on DWARF5)

```bash
cd build && cmake -DCMAKE_C_FLAGS="-gdwarf-4" -DCMAKE_CXX_FLAGS="-gdwarf-4" . && make testliboffs -j$(nproc) && valgrind --leak-check=full --error-exitcode=1 ./test/testliboffs --gtest_filter='Qr*:PeerInfoFromPayload*:PeerInfoRequestWire*'
```

Expected: 0 leaks, 0 errors in the new tests. (Pre-existing scheduler shutdown error at scheduler.c:119 is a known non-issue — see project memory.)

- [ ] **Step 3: Run the de-wonk audit**

Invoke the `de-wonk` skill per CLAUDE.md before declaring the work done, and resolve anything it finds (in particular: no stray `HAS_QRENCODE` conditionals, no TODOs in touched files, `dist/` rebuilt and committed).

- [ ] **Step 4: Close the Harmony ticket**

```bash
H=/home/victor/.claude/skills/harmony/harmony
$H ticket close OFFS-187 "QR peer connect shipped: libqrencode+quirc vendored, src/QR codec, wire format byte 2 on PEER_INFO/PEER_CONNECT/FRIEND_ADD, HTTP PPM bodies, C client peer ops, JS client QR methods, CLI --qr flags. Tests: test_qr, test_qr_wire, test_peer_payload + manual HTTP round trip."
```

Then execute the required actions the close response emits (`write_session_summary`, `tag_notify_list`).

- [ ] **Step 5: Final commit**

```bash
git add docs/OFFS_API_CLI_SPEC.md
git commit -m "docs: document QR peer-info format 2 across API spec"
```

---

## Self-Review Notes

- **Spec coverage:** spec §1 (deps/build) → Task 1; §2 (src/QR) → Task 2; §3 (wire) → Task 3; §4 daemon handlers → Task 4; §4 HTTP → Task 5; §5 C client → Task 6; §5 JS → Task 7; §6 CLI → Task 8; §7 errors → covered by helper statuses in Tasks 4/5 and verified in Task 9; §8 testing → Tasks 2-4 unit tests + Task 9 manual + Task 10 valgrind. Spec §8's "test_peer_routes.cpp HTTP round trip" is replaced by the manual curl verification (deviation noted in the header).
- **Type consistency:** format byte 2 used uniformly; `peer_info_from_payload(format, data, size, info)` signature consistent across Tasks 4/5; `qr_encode_to_ppm`/`qr_decode_from_ppm` signatures match between Tasks 2, 4, 5.
- **Backward compat:** `client_api_peer_info_request_encode()` keeps its old signature; 1-element frames still decode to format 0; `peer_connect`/`friend_add` decoders never validated format values, so old frames flow unchanged.