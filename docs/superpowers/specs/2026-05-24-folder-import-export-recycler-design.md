# Folder Import/Export & Recycler Support

## Summary

Add recycler and temporary support to the C client wire protocol and server PUT handler. Build folder import/export logic in the Dart example app layer using the existing HTTP API.

## Architecture

```
Dart layer (folder import/export logic)
  └─ OffApi service (HTTP PUT/GET with recycler/temporary headers)
       └─ HTTP Server (off_routes.c)
            └─ recycler_recipe_t → writeable_off_stream_t (recipe list)
```

The C client library (`offs_client`) gets recycler/temporary wire protocol additions but folder import/export is implemented in Dart where filesystem access and async orchestration are natural.

## C Layer: Wire Protocol Changes

### `client_api_wire.h` — extended PUT_REQUEST

Add to `client_api_put_request_t`:

```c
char**  recycler_urls;    // NULL or array of URL strings
size_t  recycler_count;   // 0 if no recycler
uint8_t temporary;        // 0 or 1
```

CBOR encoding appends optional fields: `[type, content_type, file_name, stream_length, server_address, data?, recycler_urls?, temporary?]`. Existing decoders ignore extra array elements.

New encode/decode and destroy functions handle the fields. Update `client_api_put_request_destroy` to free the `recycler_urls` array and its strings.

### `offs_client.h` — updated PUT API

Add a struct-based options API to avoid parameter explosion:

```c
typedef struct {
    const char* content_type;
    const char* file_name;
    size_t stream_length;
    const char* server_address;
    const char** recycler_urls;   // NULL or array
    size_t recycler_count;        // 0 if none
    uint8_t temporary;            // 0 or 1
} offs_put_options_t;
```

New functions:
- `offs_client_put_ex(client, options, data, data_len, callback, ctx)` — buffered PUT with all options
- `offs_client_put_stream_start_ex(client, options)` — streaming PUT start with all options

Existing `offs_client_put()` and `offs_client_put_stream_start()` retain their signatures and forward to the `_ex` variants with zeroed recycler/temporary fields.

## C Layer: Server PUT Handler Changes

### `off_routes.c` — `_off_put_handler` and `_off_put_headers_complete`

Both functions need the same change. After parsing existing headers:

1. Read `recycler` header — if present, parse as JSON array of URL strings
2. For each URL, call `off_url_parse()` to produce an `ori_t*`, push to `vec_ori_t`
3. Read `temporary` header — if `"true"`, set temporary flag
4. If recycler ORIs exist:
   - Create `recycler_recipe_t*` via `recycler_recipe_create(pool, bc, block_type, oris, network)`
   - Push it FIRST into the recipe vec: `vec_push(&recipes, (block_recipe_t*)recycler_recipe)`
5. Push `new_blocks_recipe_t` after recycler recipe (as currently done)
6. Pass the combined recipe vec to `writeable_off_stream_create()`

The recipe ordering is critical: **RecyclerRecipe must come before NewBlockRecipe** so the writeable stream tries to reuse existing blocks before allocating new ones.

## Dart Layer: Folder Import/Export

### `OffApi` — recycler/temporary support

Add `recyclerUrls` (List<String>?) and `temporary` (bool) parameters to `uploadFile()` and `uploadFileBuffered()`. Pass as HTTP headers `recycler` (JSON-encoded array) and `temporary` ("true"/"false").

### Folder Import (`importFolder`)

```
importFolder(folderPath):
  ofd = {}
  for each entry in recursive directory walk:
    if file:
      url = await uploadFile(file, ...)
      ofd[relativePath] = url
  ofdJson = jsonEncode(ofd)
  finalUrl = await uploadFileBuffered(
    fileName: basename(folderPath) + ".ofd",
    contentType: "offsystem/directory",
    bodyBytes: utf8.encode(ofdJson),
  )
  return finalUrl
```

Uses `dart:io` `Directory.list(recursive: true)` for directory walking.

### Folder Export (`exportFolder`)

```
exportFolder(offUrl, saveDir):
  response = await http.get(offUrl + "?ofd=raw")
  ofd = jsonDecode(response.body)
  dirName = offUrl.fileName without ".ofd"
  localDir = join(saveDir, dirName)
  for each (path, url) in ofd:
    data = await http.get(url)
    filePath = join(localDir, path)
    mkdirp(dirname(filePath))
    writeFile(filePath, data.bodyBytes)
```

## Data Flow: Import

```
User selects folder in Dart UI
  → walk directory tree
  → for each file: HTTP PUT to /offsystem (with optional recycler header)
  → server creates recycler_recipe + new_blocks_recipe, writes blocks
  → server returns OFF URL
  → replace file in OFD map with URL
  → serialize OFD as JSON
  → HTTP PUT ofdJson with content-type: offsystem/directory
  → server returns OFD URL
  → show URL in UI
```

## Data Flow: Export

```
User enters OFD URL in Dart UI
  → HTTP GET {url}?ofd=raw
  → server returns raw OFD block data
  → parse JSON {path: url} map
  → for each entry: HTTP GET url
  → write to saveDir/dirName/path (create subdirectories)
  → show completion in UI
```

## Error Handling

- **Wire protocol**: Invalid recycler URLs in CBOR → decode returns error, server rejects with 400
- **Server**: Recycler recipe creation failure → log error, fall back to new_blocks_recipe only
- **Dart import**: Individual file upload failure → abort entire import, report which file failed
- **Dart export**: Individual file download failure → report which file failed, skip and continue or abort based on user preference

## Testing

- **C wire protocol**: Unit tests for encode/decode round-trip with recycler_urls and temporary fields
- **Server**: Test PUT with recycler header, verify recycler_recipe is created and ordered before new_blocks_recipe
- **Dart**: Integration tests for folder import/export with a local server
- **Memory**: Run valgrind on server tests to verify no leaks from recycler URI parsing and recipe lifecycle
