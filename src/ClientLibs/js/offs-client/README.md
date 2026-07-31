# offs-client

Browser-only JavaScript client for the OFFS daemon. Communicates over HTTP,
WebSocket, or WebTransport using the same wire protocol as the C client in
`src/ClientLibs/c/offs_client.h`. Integration tests run in Node.js with a
headless server fixture; the WebSocket transport exercises the full CBOR wire
protocol end-to-end.

## Installation

```bash
npm install offs-client
```

For direct browser use via CDN:

```html
<script src="https://unpkg.com/offs-client/dist/offs-client.umd.js"></script>
```

## Quick start

```javascript
import { OffsClient } from 'offs-client';

const client = new OffsClient('http://localhost:23402');
await client.connect();

const blob = new Blob(['hello world']);
const { oriString } = await client.put({
  contentType: 'text/plain',
  fileName: 'hello.txt',
  streamLength: blob.size
}, blob);

console.log(oriString);
```

## Transports

The transport is selected from the URL scheme:

- `http://` or `https://` — `fetch`-based REST transport.
- `ws://` or `wss://` — browser `WebSocket` with CBOR binary frames.
- `wt://` or `wts://` — browser `WebTransport` over HTTP/3 with length-prefixed
  CBOR frames. The server exposes this on `--wt-h3-port` (off_server) or
  `--wt-h3-port` (offsd). Because WebTransport requires TLS, provide a
  certificate with `--cert`/`--key` or the corresponding daemon TLS options.

## Folder uploads

Folder uploads follow the same algorithm as the Flutter example client in
`examples/off_client/lib/screens/import_screen.dart`:

1. Recursively walk the selected folder.
2. Upload each file individually.
3. Build an OFF File Descriptor (OFD) from the returned ORI URLs.
4. Upload the OFD with `contentType: 'offsystem/directory'`.

In the browser, use an `<input>` with the `webkitdirectory` attribute:

```html
<input type="file" id="folderInput" webkitdirectory />
```

```javascript
const input = document.getElementById('folderInput');
input.addEventListener('change', async () => {
  const client = new OffsClient('http://localhost:23402');
  await client.connect();
  const { oriString } = await client.putFolder(input.files, {
    onProgress: (name, uploaded, total) => {
      console.log(`${uploaded}/${total}: ${name}`);
    }
  });
  console.log('Folder URL:', oriString);
});
```

You can also pass an array of objects, a `FileList`, or a record mapping
relative paths to `Blob`s:

```javascript
await client.putFolder({
  'my-folder/file-a.txt': new Blob(['a']),
  'my-folder/file-b.txt': new Blob(['b'])
});
```

## API reference

### Connection

- `connect()` — open the transport.
- `disconnect()` — close the transport and cancel pending requests.
- `isConnected()` — return whether the transport is open.

### PUT

- `put(options, data)` — buffered upload.
- `putStreamStart(options)` — start a streaming upload.
- `putStreamData(chunk)` — send a chunk (WebSocket/WebTransport only).
- `putStreamEnd()` — finish a streaming upload.

### GET

- `get(oriString, callbacks, range?)` — download by ORI URL.

`callbacks` may implement:

- `onStart(contentType, contentLength, hasRange, rangeStart, rangeEnd)`
- `onData(chunk)`
- `onEnd()`
- `onError(statusCode, message)`

### Block cache

- `blockPut(data, encoding?)`
- `blockGet(hash)`
- `blockDelete(hash)`

### Peer and friend management

- `health()`
- `peerInfo(format?)`
- `peerConnect(peerInfo, format?)`
- `peerList()`
- `friendAdd(peerInfo, format?)`
- `friendRemove(nodeId)`
- `friendList()`

### Configuration

- `configShow()`
- `configSet(field, value)`
- `configReload()`

### Folder upload

- `putFolder(items, options?)` — recursively upload a folder tree and return
  the root directory's ORI URL.

## Scripts

- `npm run build` — produce ESM and UMD bundles.
- `npm run test:unit` — run Node unit tests.
- `npm run test:integration` — run Node-based integration tests against a real
  off_server (HTTP + WebSocket). WebTransport is skipped because Node does not
  provide a built-in WebTransport runtime.
- `npm run test:integration:large` — run the 1.7 GB video round-trip test over HTTP
  and WebSocket.
- `npm run test` — run unit + fast integration tests.
- `npm run lint` — lint source and tests.
