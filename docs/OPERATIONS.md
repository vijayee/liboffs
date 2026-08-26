# Operations Guide

## Known issues

### NULL-buffer heap corruption in HTTP body handlers (root cause: data races — fixed)

**Symptom:** Historically, `buffer->data` was observed NULL inside the HTTP
body handling paths — `_on_body` (http_parser body callback in
`src/ClientAPI/HTTP/http_connection.c`), `_put_on_request_data` (streamed PUT
body handler in `src/ClientAPI/HTTP/off_routes.c`), and `_pipe_on_data`
(response piping callback in `src/ClientAPI/HTTP/http_response.c`). The
`buffer_t` struct had `capacity` set correctly but `data == NULL` and `size`
held garbage.

**Root cause:** The NULL `buffer->data` was a downstream symptom of heap
corruption from two cross-thread data races, not a `buffer_t` bug. TSAN
caught both (ASAN and valgrind miss them):

1. **`connection->sock` use-after-free** — `_connection_close_fd` (worker
   thread) freed the socket and set `connection->sock = NULL` while
   `_connection_read_callback` (I/O thread) read it. Fixed by making
   `connection->sock` an `ATOMIC(platform_socket_t*)` and deferring the
   socket's close+free to the I/O thread's destroy stack
   (`http_server_defer_socket_destroy`), mirroring the existing
   watcher/timer deferral.

2. **`pipe_notifiers` use-after-free WRITE** — `readable_push_stream_pipe` /
   `writeable_pull_stream_pipe` called `on_pipe`/`on_piped` synchronously on
   the caller's thread, writing `pipe_notifiers` while
   `stream_unsubscribe_pipe_notifiers` (worker thread) freed it. Fixed by
   routing pipe/piped through the stream actor via the already-declared
   `STREAM_PIPE`/`STREAM_PIPED` messages and `stream_pipe_internal` /
   `stream_piped_internal`.

**Guards in place:** The defensive sentinels at the entry of the three body
handlers remain as cheap no-op checks (http-parser never legitimately passes
NULL `at`/`length`), but they are no longer the fix — the underlying races
are resolved.

**Verification:** `TestPushFileStream.*`, `TestPullFileStream.*`,
`TestStreamActor.*`, `TestHttpServer.*`, `TestOffRoutes.*`, and
`TestHttpServerSsl.*` all pass under TSAN with zero data-race reports, and
the full 849-test suite passes. The GET-path pipeline refcount leak in
`_setup_stream_pipeline` (off_routes.c) that previously leaked 48 bytes
direct + 209 bytes indirect per GET request has also been fixed — the
`get_pipeline_t` refcount now reaches zero in all paths via a `desc_done`
flag that ensures desc contributes exactly one deref whether close or
error fires first.