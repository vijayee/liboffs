#include "platform_local.h"
#include "platform_file.h"

#ifndef _WIN32
  #include <stdlib.h>
  #include <string.h>
  #include <unistd.h>

  platform_socket_t* platform_local_listen(const char* path) {
    platform_socket_t* sock = platform_socket_create(PLATFORM_AF_LOCAL, 1);
    if (sock == NULL) {
      return NULL;
    }

    platform_address_t addr;
    memset(&addr, 0, sizeof(addr));
    addr.family = PLATFORM_AF_LOCAL;
    strncpy(addr.local.path, path, sizeof(addr.local.path) - 1);

    platform_file_unlink(path); /* remove stale socket file */

    if (platform_socket_bind(sock, &addr) != 0 ||
        platform_socket_listen(sock, 128) != 0) {
      platform_socket_destroy(sock);
      return NULL;
    }
    return sock;
  }

  platform_socket_t* platform_local_accept(platform_socket_t* listener) {
    return platform_socket_accept(listener, NULL);
  }

  platform_socket_t* platform_local_connect(const char* path) {
    platform_socket_t* sock = platform_socket_create(PLATFORM_AF_LOCAL, 1);
    if (sock == NULL) {
      return NULL;
    }

    platform_address_t addr;
    memset(&addr, 0, sizeof(addr));
    addr.family = PLATFORM_AF_LOCAL;
    strncpy(addr.local.path, path, sizeof(addr.local.path) - 1);

    if (platform_socket_connect(sock, &addr) != 0) {
      platform_socket_destroy(sock);
      return NULL;
    }
    return sock;
  }

  void platform_local_cleanup(const char* path) {
    platform_file_unlink(path);
  }

  /* POSIX AF_UNIX listeners don't need rearming after accept — the
   * listening fd stays in the listening state and the next accept
   * can proceed immediately. The rearm concept exists only for the
   * Windows named-pipe backend where each accept consumes a pipe
   * instance that must be recreated. Return 0 so unix_transport.c
   * can call this unconditionally without a platform guard. */
  int platform_local_rearm(platform_socket_t* listener) {
    (void)listener;
    return 0;
  }
#else
  /* Windows implementation — named-pipe backend by default, with AF_UNIX
   * (Windows 10 1803+) available as an opt-in. The OFFS_FORCE_NAMED_PIPE
   * environment variable ("1") explicitly forces pipes; OFFS_USE_AF_UNIX
   * ("1") opts into AF_UNIX when the platform supports it. Unset, the
   * backend is named pipes. See _use_af_unix for why pipes are the
   * default. */
  #include <windows.h>
  #include <winsock2.h>
  #include <afunix.h>
  #include <stdlib.h>
  #include <string.h>
  #include <stdio.h>

  #include "platform_internal.h"
  #include "platform_socket_internal.h"
  #include "../Util/log.h"

  /* Pipe name length cap. Windows supports up to 256 characters in
   * "\\.\pipe\<name>" form; we cap at 240 to leave room for the prefix
   * and a NUL terminator. */
  #define OFFS_PIPE_NAME_MAX 240
  #define OFFS_PIPE_PREFIX   "\\\\.\\pipe\\liboffs-"

  /* Returns non-zero if the OFFS_FORCE_NAMED_PIPE environment variable
   * is set to a truthy value ("1", "true", "yes", "on"). Explicitly
   * forces the named-pipe backend regardless of the default. */
  static int _force_named_pipe(void) {
    const char* v = getenv("OFFS_FORCE_NAMED_PIPE");
    if (v == NULL) return 0;
    if (v[0] == '1' || v[0] == 't' || v[0] == 'T' ||
        v[0] == 'y' || v[0] == 'Y') return 1;
    return 0;
  }

  /* Returns non-zero if the OFFS_USE_AF_UNIX environment variable is set
   * to a truthy value. Opts into the AF_UNIX backend on Windows 10 1803+
   * instead of the named-pipe default.
   *
   * The Windows local-IPC default is the named-pipe backend. AF_UNIX is
   * available as an opt-in for advanced/testing use. AF_UNIX cross-
   * process I/O on an accepted socket works provided the linked poll-
   * dancer includes the IOCP fd->HANDLE fix (poll-dancer 50a9e38: cast
   * the Winsock SOCKET to a HANDLE directly instead of calling
   * _get_osfhandle, which aborted with STATUS_STACK_BUFFER_OVERRUN on
   * the first accepted socket and killed the daemon; see the windows-
   * afunix-cli-rpc-broken memory). The named-pipe backend stays the
   * default because it works on every Windows SKU (AF_UNIX needs
   * Windows 10 1803+) and uses a simpler HANDLE/ReadFile IOCP path
   * independent of Winsock. Set the env on BOTH processes to opt into
   * AF_UNIX. */
  static int _use_af_unix(void) {
    const char* v = getenv("OFFS_USE_AF_UNIX");
    if (v == NULL) return 0;
    if (v[0] == '1' || v[0] == 't' || v[0] == 'T' ||
        v[0] == 'y' || v[0] == 'Y') return 1;
    return 0;
  }

  /* Convert a liboffs local-path string to a Windows pipe name. Strips
   * the directory portion (everything up to and including the last
   * path separator) and uses the basename as the pipe name's
   * identifying component. Replaces characters illegal in pipe names
   * with '-'. Returns 0 on success and writes a NUL-terminated pipe
   * name into out (which must be at least OFFS_PIPE_NAME_MAX + 1
   * bytes). Returns -1 if the path is empty. */
  static int _encode_pipe_name(const char* path, char* out, size_t out_len) {
    if (path == NULL || path[0] == '\0') return -1;

    /* Find the basename: the part after the last '/' or '\\'. Windows
     * pipe names cannot contain path separators. */
    const char* base = path;
    for (const char* p = path; *p != '\0'; p++) {
      if (*p == '/' || *p == '\\') base = p + 1;
    }

    /* Build "\\\\.\\pipe\\liboffs-<basename>" by copying allowed chars
     * and converting any other byte to '-'. Windows pipe names are
     * case-insensitive; preserve case for display. */
    int n = snprintf(out, out_len, "%s", OFFS_PIPE_PREFIX);
    if (n < 0 || (size_t)n >= out_len) return -1;
    size_t pi = (size_t)n;

    for (const char* p = base; *p != '\0' && pi < out_len - 1; p++) {
      unsigned char c = (unsigned char)*p;
      /* Allow alnum, dot, underscore, dash. Replace others with '-'. */
      if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-') {
        out[pi++] = (char)c;
      } else {
        out[pi++] = '-';
      }
    }
    out[pi] = '\0';
    return 0;
  }

  /* Helper: wrap an existing pipe HANDLE in a new platform_socket_t
   * flagged as pipe-backed, used by both accept and connect.
   *
   * `owns_handle` is 1 when the caller is creating a fresh HANDLE that
   * the wrapper is responsible for closing, and 0 when wrapping a
   * HANDLE that is owned elsewhere (e.g. the accept path returns a
   * wrapper around the listener's HANDLE — the listener keeps it
   * alive and reuses it after the next rearm). */
  static platform_socket_t* _wrap_pipe(HANDLE handle, int owns_handle) {
    platform_socket_t* sock = (platform_socket_t*)calloc(1, sizeof(platform_socket_t));
    if (sock == NULL) {
      if (owns_handle && handle != NULL && handle != INVALID_HANDLE_VALUE) {
        CloseHandle(handle);
      }
      return NULL;
    }
    sock->fd = INVALID_SOCKET;
    sock->handle = handle;
    sock->family = PLATFORM_AF_LOCAL;
    sock->is_pipe = 1;
    sock->owns_handle = owns_handle;
    return sock;
  }

  /* ---- AF_UNIX path (existing) ---- */

  static platform_socket_t* _afunix_listen(const char* path) {
    if (!_platform_has_af_unix()) {
      log_warn("platform_local_listen: AF_UNIX not available on this Windows version");
      return NULL;
    }
    platform_socket_t* sock = platform_socket_create(PLATFORM_AF_LOCAL, 1);
    if (sock == NULL) return NULL;

    platform_address_t addr;
    memset(&addr, 0, sizeof(addr));
    addr.family = PLATFORM_AF_LOCAL;
    strncpy(addr.local.path, path, sizeof(addr.local.path) - 1);

    platform_file_unlink(path);

    if (platform_socket_bind(sock, &addr) != 0 ||
        platform_socket_listen(sock, 128) != 0) {
      platform_socket_destroy(sock);
      return NULL;
    }
    return sock;
  }

  static platform_socket_t* _afunix_connect(const char* path) {
    if (!_platform_has_af_unix()) {
      log_warn("platform_local_connect: AF_UNIX not available on this Windows version");
      return NULL;
    }
    platform_socket_t* sock = platform_socket_create(PLATFORM_AF_LOCAL, 1);
    if (sock == NULL) return NULL;

    platform_address_t addr;
    memset(&addr, 0, sizeof(addr));
    addr.family = PLATFORM_AF_LOCAL;
    strncpy(addr.local.path, path, sizeof(addr.local.path) - 1);

    if (platform_socket_connect(sock, &addr) != 0) {
      platform_socket_destroy(sock);
      return NULL;
    }
    return sock;
  }

  /* ---- Named-pipe path (new) ----
   *
   * The Windows named-pipe server model: each call to CreateNamedPipe
   * (with the same name) returns the next instance. The kernel supports
   * up to nMaxInstances instances per pipe name; the server creates
   * them on demand. The first call returns instance 1 in "listening"
   * state; subsequent calls return instances 2..N. After a client
   * connects to instance K, the server uses that HANDLE for I/O, and
   * closes it when done. The next call to CreateNamedPipe returns the
   * next free instance.
   *
   * This avoids the per-instance DisconnectNamedPipe dance — closing
   * the HANDLE is sufficient to free the instance and let the next
   * CreateNamedPipe call succeed.
   */

  static HANDLE _create_pipe_instance(const char* pipe_name) {
    /* PIPE_UNLIMITED_INSTANCES is the upper bound (255). The liboffs
     * transport caps active connections separately; this is just the
     * per-name instance count the kernel enforces. */
    return CreateNamedPipeA(
      pipe_name,
      PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
      PIPE_UNLIMITED_INSTANCES,
      65536,       /* nOutBufferSize */
      65536,       /* nInBufferSize */
      0,           /* nDefaultTimeOut */
      NULL         /* lpSecurityAttributes */
    );
  }

  static platform_socket_t* _pipe_listen(const char* path) {
    char pipe_name[OFFS_PIPE_NAME_MAX + 1];
    if (_encode_pipe_name(path, pipe_name, sizeof(pipe_name)) != 0) {
      log_warn("platform_local_listen: invalid path for pipe encoding");
      return NULL;
    }

    HANDLE h = _create_pipe_instance(pipe_name);
    if (h == INVALID_HANDLE_VALUE) {
      log_warn("platform_local_listen: CreateNamedPipe failed (err=%lu)", GetLastError());
      return NULL;
    }

    platform_socket_t* sock = _wrap_pipe(h, 1);  /* listener owns the HANDLE */
    if (sock == NULL) {
      /* _wrap_pipe already closed `h` on calloc failure. */
      return NULL;
    }
    sock->pipe_name = _strdup(pipe_name);
    if (sock->pipe_name == NULL) {
      log_warn("platform_local_listen: out of memory storing pipe name");
      platform_socket_destroy(sock);
      return NULL;
    }
    return sock;
  }

  static platform_socket_t* _pipe_accept(platform_socket_t* listener) {
    if (listener == NULL || !platform_socket_is_pipe(listener)) {
      log_warn("platform_local_accept: listener is not a pipe");
      return NULL;
    }
    HANDLE h = (HANDLE)platform_socket_handle(listener);
    if (h == NULL || h == INVALID_HANDLE_VALUE) {
      return NULL;
    }

    /* Issue ConnectNamedPipe and wait synchronously for the result. */
    OVERLAPPED ov;
    memset(&ov, 0, sizeof(ov));
    BOOL ok = ConnectNamedPipe(h, &ov);
    if (!ok) {
      DWORD err = GetLastError();
      if (err == ERROR_PIPE_CONNECTED) {
        goto accept_done;
      }
      if (err != ERROR_IO_PENDING) {
        log_warn("platform_local_accept: ConnectNamedPipe failed (err=%lu)", err);
        return NULL;
      }
      /* Pending — wait for the connection. This call blocks (bWait=TRUE)
       * because the server thread runs a dedicated accept loop; a
       * separate _pipe_loop_thread services the transport's IOCP for
       * per-connection I/O. Splitting the work into two threads is
       * what allows the connection's read completions to be processed
       * while this thread blocks here. */
      DWORD bytes = 0;
      ok = GetOverlappedResult(h, &ov, &bytes, TRUE);
      if (!ok) {
        DWORD err2 = GetLastError();
        if (err2 == ERROR_PIPE_CONNECTED) {
          goto accept_done;
        }
        return NULL;
      }
    }
accept_done:
    /* H is now in connected state. Create a new instance to replace
     * the one we just accepted, so the listener is ready for the next
     * client even before the caller rearms. The new instance lives
     * in listener->handle; the connected HANDLE is handed to the
     * connection wrapper. */
    if (listener->pipe_name != NULL) {
      HANDLE next = _create_pipe_instance(listener->pipe_name);
      if (next != INVALID_HANDLE_VALUE) {
        listener->handle = next;
      } else {
        log_warn("platform_local_accept: next-instance CreateNamedPipe failed (err=%lu)", GetLastError());
        /* Continue: the caller still gets the connected wrapper. The
         * listener's handle field is left in the "connected" state,
         * and any subsequent accept will fail until the caller
         * rearms. The transport calls platform_local_rearm on every
         * accept path so this is recoverable. */
      }
    }
    return _wrap_pipe(h, 1);
  }

  static int _pipe_rearm(platform_socket_t* listener) {
    if (listener == NULL || !platform_socket_is_pipe(listener)) return -1;
    if (listener->pipe_name == NULL) return -1;
    HANDLE h = (HANDLE)platform_socket_handle(listener);
    /* The normal case: _pipe_accept already pre-created the next
     * instance and replaced listener->handle, so this is a no-op. The
     * fallback case (next-instance creation failed during _pipe_accept)
     * leaves the listener in the "connected" state; we need a fresh
     * instance. Detect that by checking whether the HANDLE is in
     * "connected" state via a non-blocking peek. If the listener has
     * an active client, no rearm is needed yet. */
    if (h == NULL || h == INVALID_HANDLE_VALUE) {
      HANDLE next = _create_pipe_instance(listener->pipe_name);
      if (next == INVALID_HANDLE_VALUE) {
        log_warn("platform_local_rearm: CreateNamedPipe failed (err=%lu)", GetLastError());
        return -1;
      }
      listener->handle = next;
    }
    /* Heuristic: if there is no current client, the listener should
     * be in listening state. If a previous accept left it in connected
     * state (recovery case), this rearm creates a fresh instance. */
    return 0;
  }

  static platform_socket_t* _pipe_connect(const char* path) {
    char pipe_name[OFFS_PIPE_NAME_MAX + 1];
    if (_encode_pipe_name(path, pipe_name, sizeof(pipe_name)) != 0) {
      log_warn("platform_local_connect: invalid path for pipe encoding");
      return NULL;
    }

    /* CreateFile on a named pipe. If the server hasn't created its
     * end yet, ERROR_PIPE_BUSY fires — wait briefly and retry. */
    HANDLE h = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 5; attempt++) {
      h = CreateFileA(
        pipe_name,
        GENERIC_READ | GENERIC_WRITE,
        0,
        NULL,
        OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED,
        NULL
      );
      if (h != INVALID_HANDLE_VALUE) break;
      DWORD err = GetLastError();
      if (err != ERROR_PIPE_BUSY) {
        log_warn("platform_local_connect: CreateFile failed (err=%lu)", err);
        return NULL;
      }
      if (!WaitNamedPipeA(pipe_name, 1000)) {
        /* Timeout waiting for the server to create its end. Yield the
         * CPU briefly so a tight ConnectFile/WaitNamedPipeA loop does
         * not pin a core while the server-side CreateNamedPipe call is
         * still in progress. WaitNamedPipeA has already blocked for
         * up to 1 second; an extra short sleep is harmless. */
        Sleep(50);
      }
    }
    if (h == INVALID_HANDLE_VALUE) {
      log_warn("platform_local_connect: CreateFile exhausted retries");
      return NULL;
    }

    /* PIPE_NOWAIT makes synchronous WriteFile return immediately with
     * ERROR_NO_DATA when the pipe buffer is full, so the calling
     * thread doesn't block. ReadFile behavior with PIPE_NOWAIT is
     * similar — the loop uses IOCP, but actor threads doing direct
     * reads will see ERROR_NO_DATA instead of blocking. */
    DWORD mode = PIPE_READMODE_BYTE | PIPE_NOWAIT;
    if (!SetNamedPipeHandleState(h, &mode, NULL, NULL)) {
      DWORD err = GetLastError();
      log_warn("platform_local_connect: SetNamedPipeHandleState failed (err=%lu)", err);
      CloseHandle(h);
      return NULL;
    }

    platform_socket_t* sock = _wrap_pipe(h, 1);  /* client owns its HANDLE */
    if (sock == NULL) return NULL;
    return sock;
  }

  /* ---- Public API dispatch ---- */

  platform_socket_t* platform_local_listen(const char* path) {
    /* Default backend is named pipes. AF_UNIX is opt-in (OFFS_USE_AF_UNIX);
     * OFFS_FORCE_NAMED_PIPE explicitly forces pipes. When AF_UNIX is
     * selected, still fall through to pipes if the platform lacks AF_UNIX
     * or the bind/listen fails (older Windows, stripped server SKUs). */
    if (!_use_af_unix() || _force_named_pipe()) {
      return _pipe_listen(path);
    }
    platform_socket_t* sock = _afunix_listen(path);
    if (sock != NULL) return sock;
    return _pipe_listen(path);
  }

  platform_socket_t* platform_local_accept(platform_socket_t* listener) {
    if (listener == NULL) return NULL;
    if (platform_socket_is_pipe(listener)) {
      return _pipe_accept(listener);
    }
    return platform_socket_accept(listener, NULL);
  }

  platform_socket_t* platform_local_connect(const char* path) {
    /* Mirror platform_local_listen's backend selection so client and
     * server use the same transport. Default is named pipes; AF_UNIX is
     * opt-in (OFFS_USE_AF_UNIX); OFFS_FORCE_NAMED_PIPE forces pipes. When
     * AF_UNIX is selected but the connect fails (e.g. the server fell
     * back to pipes because its AF_UNIX listen failed), fall back to
     * pipes so an opted-in client can still reach a pipe-backed server. */
    if (!_use_af_unix() || _force_named_pipe()) {
      return _pipe_connect(path);
    }
    if (!_platform_has_af_unix()) {
      return _pipe_connect(path);
    }
    platform_socket_t* sock = _afunix_connect(path);
    if (sock != NULL) return sock;
    return _pipe_connect(path);
  }

  int platform_local_rearm(platform_socket_t* listener) {
    if (listener == NULL) return -1;
    if (!platform_socket_is_pipe(listener)) return 0;  /* no-op for AF_UNIX */
    return _pipe_rearm(listener);
  }

  void platform_local_cleanup(const char* path) {
    platform_file_unlink(path);
  }
#endif
