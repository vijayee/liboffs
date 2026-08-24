# Operations Guide

## Known issues

### NULL-buffer heap corruption in HTTP body handlers (root cause under investigation)

**Symptom:** Historically, `buffer->data` was observed NULL inside the HTTP
body handling paths — `_on_body` (http_parser body callback in
`src/ClientAPI/HTTP/http_connection.c`), `_put_on_request_data` (streamed PUT
body handler in `src/ClientAPI/HTTP/off_routes.c`), and `_pipe_on_data`
(response piping callback in `src/ClientAPI/HTTP/http_response.c`). The
`buffer_t` struct had `capacity` set correctly but `data == NULL` and `size`
held garbage, suggesting the struct fields were overwritten by heap corruption
from an unknown source.

**Guards in place:** Defensive sentinels were restored at the entry of each
of those three handlers. A NULL `buffer->data` (or NULL `chunk`/`at` pointer)
now logs an `error`-level message identifying the handler and the suspicious
pointer values, then returns without dereferencing. In `_on_body` the parse
is aborted by returning `1` to http-parser; in the streamed-PUT and
response-pipe paths the chunk is dropped. This prevents the NULL dereference
crash but does not address the underlying corruption.

**Root cause status:** Under investigation. The corruption is not easily
reproducible under ASAN (ASAN redzones mask the bug), and the flaky
`TestStream*` segfaults observed in ASAN builds do not produce an ASAN
report (the SIGSEGV bypasses ASAN's signal handler, produces no core dump,
does not reproduce under gdb/strace/pty, and does not reproduce in non-ASAN
builds). The failing `TestStream*` tests (`TestPushFileStream.*`,
`TestPullFileStream.*`, `TestStreamActor.*`) are file-stream + scheduler
tests and do not exercise the HTTP body handlers directly, so the sentinel
guards do not resolve their segfaults — but the guards are retained as the
spec's accepted fallback for the historical NULL-buffer crash.

**Investigation notes:**

- The flaky ASAN segfault is timing-dependent (reproduces only when stdout
  is file-redirected, not under a pty; ~30% rate in isolated process runs).
- ASAN installs its SIGSEGV handler but does not fire a report when the
  segfault occurs, suggesting the fault happens in a state where ASAN's
  handler cannot safely run (e.g. during process teardown after main
  returns, or in a thread that hasn't registered its stack with ASAN).
- `buffer_ensure_capacity` aborts on OOM, so `buffer->data` is never NULL
  in normal operation — the NULL must come from external heap corruption.
- Candidates not yet ruled out: a missing `REFERENCE` on a `buffer_t*`
  crossing an actor boundary; a `stream_notify` CONSUME/yield ownership
  bug; a double-free in dispatch (per the
  `feedback_double_free_dispatch.md` memory note — `actor_run` frees
  `msg->payload`; dispatch must not also free it); a `stream_deactivate`
  freeing a buffer while a handler still reads it.

**Next steps for a future investigation:**

1. Run the `TestStream*` tests under ThreadSanitizer (TSAN) to catch the
   race that ASAN misses.
2. Audit `actor_run`'s payload destroy path against every dispatch handler
   in `src/Streams/` and `src/ClientAPI/HTTP/` for the double-free pattern
   documented in `feedback_double_free_dispatch.md`.
3. Stress-run the file-stream pipeline under valgrind with
   `--track-origins=yes` to capture the corruption source.
4. Once the root cause is found, remove the sentinel guards and replace
   with the minimal fix.