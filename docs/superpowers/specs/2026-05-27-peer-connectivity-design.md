# Peer Connectivity Design Spec

## Overview

Add peer-to-peer node connectivity to the node API: share local connection info as CBOR/Base58/QR code, connect to remote peers using that info, and manage pinned "friend" peers that persist across restarts.

## Peer Info Data Format

A CBOR-encoded structure representing everything needed to reach and verify a node:

```
PeerInfo = {
  node_id:    bstr(32),       // BLAKE3 hash of public key
  public_key: bstr,           // raw public key for salutation verification
  addresses:  [Address],      // list of reachable addresses, priority order
}

Address = {
  type:      uint,            // 0 = direct QUIC, 1 = relay
  host:      tstr,            // IP or hostname
  port:      uint,            // UDP port
  relay_id:  uint,            // endpoint ID on relay server (only for type=1)
}
```

### Encoding variants

- **Raw CBOR**: Used on the wire, in QR codes, and for programmatic use. Most compact.
- **Base58 text**: Human-facing transport encoding for copy/paste and manual entry. Wraps the raw CBOR bytes with Base58.
- **QR code PNG**: A QR code image encoding the raw CBOR bytes, generated server-side.

### Format negotiation

Clients specify the format they are sending. The server never auto-detects.

- HTTP: `Content-Type: application/cbor` vs `text/plain` for Base58, or query param `?format=cbor|base58|qrcode`
- Wire protocol: a format byte in the message (0=CBOR, 1=Base58)

## New Files

### `src/Network/peer_info.h` / `src/Network/peer_info.c`

- `peer_info_t` struct with node_id, public_key, addresses list
- `peer_info_encode(peer_info_t*)` → `cbor_item_t*`
- `peer_info_decode(cbor_item_t*)` → `peer_info_t*`
- `peer_info_to_base58(peer_info_t*)` → `char*` (CBOR encode → Base58)
- `peer_info_from_base58(const char*)` → `peer_info_t*` (Base58 decode → CBOR decode)
- `peer_info_destroy(peer_info_t*)`
- `peer_info_from_node(offs_node_t*)` → `peer_info_t*` (build from local node state)

## Friend Peers & Bootstrap

### Bootstrap peers (existing)

`authority_t.bootstrap_peers` — string array of host:port entries. Contacted on startup via `network_connect_peer()`. After initial connection, managed normally by Hebbian dynamics.

### Friend peers (new)

Peers that are pinned — immune to Hebbian decay eviction and auto-reconnected on disconnect.

**`authority_t` additions:**
```c
peer_info_t** friend_peers;
size_t friend_peer_count;
```

**`peer_connection_t` additions:**
```c
bool is_friend;
```

**`connection_manager_t` behavior:**
- `connection_manager_decay_tick()` skips peers where `is_friend == true`
- Friend disconnect triggers reconnect with exponential backoff (1s, 2s, 4s, ... max 60s)
- `connection_manager_add()` accepts an `is_friend` parameter

**`network_t` startup flow:**
1. Start QUIC listener (if configured)
2. Connect to relay (if configured)
3. Connect to bootstrap_peers — fire-and-forget, Hebbian manages thereafter
4. Connect to friend_peers — persistent, reconnect on drop forever

**Persistence:** Friend peers are saved/loaded alongside existing peer state in `authority_save_peers()` / `authority_load_peers()`.

## API Surface

All endpoints are auth-gated identically to the block API.

### HTTP REST API

| Method | Path | Description |
|--------|------|-------------|
| `GET` | `/peer/info?format=cbor\|base58\|qrcode` | Local node's peer info. `qrcode` returns PNG image. |
| `POST` | `/peer/connect` | Connect to peer. Body: CBOR or Base58. `Content-Type` distinguishes format. Returns status. |
| `GET` | `/peers` | List connected peers: node_id, address, friend status, RTT, connected_at |
| `POST` | `/friends` | Add a friend peer. Body: peer info. Saves to persistent config. |
| `DELETE` | `/friends/:node_id` | Remove a friend peer. Updates persistent config. |
| `GET` | `/friends` | List friend peers with connection status. |

Auth: All endpoints require `Authorization: Bearer <token>` (existing middleware). Routes only register if `config->api_key_hash` is set.

Implementation: New file `src/ClientAPI/HTTP/peer_routes.h/.c`, registered in `http_server` setup. Follows the same pattern as `block_routes.c`.

### Client API Wire Protocol

New message types in `client_api_wire.h`:

```c
#define CLIENT_API_PEER_INFO_REQUEST   21
#define CLIENT_API_PEER_INFO_RESPONSE  22
#define CLIENT_API_PEER_CONNECT        23
#define CLIENT_API_PEER_CONNECT_RESULT 24
#define CLIENT_API_PEER_LIST_REQUEST   25
#define CLIENT_API_PEER_LIST_RESPONSE  26
#define CLIENT_API_FRIEND_ADD          27
#define CLIENT_API_FRIEND_REMOVE       28
#define CLIENT_API_FRIEND_LIST         29
```

New file `src/ClientAPI/peer_handlers.h/.c` following the same pattern as `block_handlers.c`:
- Each handler checks `is_authenticated` before processing
- Wire messages carry a format byte where peer info is accepted as input

### Peer Connect Result Codes

```
0 = connected (new connection established)
1 = already connected
2 = invalid peer info (decode failed)
3 = connection failed (timeout or unreachable)
4 = connection rejected (remote refused or salutation verification failed)
```

## QR Code Generation

Server-side PNG generation using libqrencode.

### Dependency

libqrencode — small, no dependencies beyond libpng. Added to CMakeLists.txt as optional (`HAS_QRENCODE`).

### Endpoint

`GET /peer/info?format=qrcode`
- Returns `Content-Type: image/png`
- Encodes raw CBOR bytes of local peer info in a QR code
- Uses QR code version appropriate to data size (auto-select)
- If libqrencode is not available, returns 501 Not Implemented

### Flutter: Saving and Uploading QR Images

**Display (peer info screen):**
- Fetches QR PNG from `/peer/info?format=qrcode`
- Displays the image
- Shows Base58 text below for copy/paste
- Button to save image to device

**Connect (connect screen):**
- Text field for pasting Base58 peer info
- Button to upload a QR code image file
- Flutter decodes the QR image locally using the `qr` package (no server call for decode)
- Extracted CBOR bytes are sent to `POST /peer/connect` as `application/cbor`
- Shows connect result

## Flutter App Changes

### Files modified

- `lib/screens/connect_screen.dart` — implement actual connectivity
- `lib/services/off_api.dart` — add peer/friend API methods

### Files added (optional, as needed)

- Peer info display widget (could be on an existing screen)

### `OffApi` additions

```dart
Future<Uint8List> getPeerInfo({String format = 'cbor'});
Future<String> connectPeer(Uint8List peerInfoCbor);
Future<String> connectPeerBase58(String peerInfoBase58);
Future<List<PeerInfo>> listPeers();
Future<void> addFriend(Uint8List peerInfoCbor);
Future<void> removeFriend(String nodeId);
Future<List<PeerInfo>> listFriends();
```

### `ConnectScreen` implementation

- Paste Base58 string → `connectPeerBase58()`
- Upload QR image → decode with `qr` package → `connectPeer()`
- Display result status

## Error Handling

- Invalid peer info (decode failure) → error response with code 2
- Connection timeout → code 3, peer info preserved for retry
- Friend reconnect failures → exponential backoff, log each attempt
- Missing libqrencode → `/peer/info?format=qrcode` returns 501
- Auth failure → 401/403, same behavior as block API

## Testing

- Unit tests for `peer_info` encode/decode round-trip (CBOR and Base58)
- Unit tests for `peer_info_from_node()` correctness
- Unit tests for friend peer persistence (save/load cycle)
- Integration test: connect two local nodes via peer info exchange
- Integration test: friend reconnect after disconnect
- Integration test: friend immune to Hebbian decay eviction
- Integration test: QR code encode/decode round-trip (if libqrencode available)
