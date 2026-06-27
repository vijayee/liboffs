/* test_crash_catch.h — in-process crash backtrace for the libofs test suite.
 *
 * A silent native access violation (e.g. the intermittent TestBlockCache segfault)
 * dies with no stack on Windows when no debugger is attached. This installs a
 * vectored exception handler that, on a fatal exception, walks the faulting
 * stack with dbghelp and prints a symbolicated backtrace to stderr BEFORE the
 * process dies — so a background reproduction loop captures the faulting
 * function/file:line without cdb/windbg.
 *
 * Self-contained: dbghelp types come from <dbghelp.h> (header-only — no link
 * dependency), and the functions are loaded dynamically (LoadLibrary), so this
 * needs no CMake link change — just #include it and call test_crash_catch_init()
 * from main(). Gated by the OFFS_CRASH_TRACE env var: a no-op (handler never
 * installed) when unset, so normal test runs and CI are unaffected.
 *
 * This is debug instrumentation for the test executable, not production code.
 */
#ifndef TEST_CRASH_CATCH_H
#define TEST_CRASH_CATCH_H

#if defined(_WIN32)

#include <windows.h>
#include <dbghelp.h>
#include <stdio.h>
#include <stdlib.h>

/* Function pointer types for the dbghelp routines we load dynamically. */
typedef BOOL (WINAPI *pfnSymInitialize)(HANDLE, PCSTR, BOOL);
typedef BOOL (WINAPI *pfnSymSetSearchPath)(HANDLE, PCSTR);
typedef DWORD (WINAPI *pfnSymGetOptions)(void);
typedef DWORD (WINAPI *pfnSymSetOptions)(DWORD);
typedef BOOL (WINAPI *pfnSymFromAddr)(HANDLE, DWORD64, PDWORD64, PSYMBOL_INFO);
typedef BOOL (WINAPI *pfnSymGetLineFromAddr64)(HANDLE, DWORD64, PDWORD, PIMAGEHLP_LINE64);
typedef BOOL (WINAPI *pfnStackWalk64)(DWORD, HANDLE, HANDLE, LPSTACKFRAME64, PVOID,
                                      PREAD_PROCESS_MEMORY_ROUTINE64,
                                      PFUNCTION_TABLE_ACCESS_ROUTINE64,
                                      PGET_MODULE_BASE_ROUTINE64,
                                      PTRANSLATE_ADDRESS_ROUTINE64);
/* The two callbacks StackWalk64 needs are also resolved dynamically so the
   test executable never links dbghelp.lib (normal builds stay pristine). */
typedef PVOID (WINAPI *pfnSymFunctionTableAccess64)(HANDLE, DWORD64);
typedef DWORD64 (WINAPI *pfnSymGetModuleBase64)(HANDLE, DWORD64);

static pfnSymInitialize            g_SymInitialize;
static pfnSymSetSearchPath          g_SymSetSearchPath;
static pfnSymGetOptions            g_SymGetOptions;
static pfnSymSetOptions            g_SymSetOptions;
static pfnSymFromAddr              g_SymFromAddr;
static pfnSymGetLineFromAddr64     g_SymGetLineFromAddr64;
static pfnStackWalk64              g_StackWalk64;
static pfnSymFunctionTableAccess64 g_SymFunctionTableAccess64;
static pfnSymGetModuleBase64       g_SymGetModuleBase64;
static HMODULE g_dbghelp = NULL;

/* Symbol setup is deferred to the first fatal exception (see _tcc_ensure_symbols)
   so the per-run cost of test_crash_catch_init is just a LoadLibrary + a handful of
   GetProcAddress calls + AddVectoredExceptionHandler — microseconds, timing-neutral.
   Doing SymInitialize(process, NULL, TRUE) at init would auto-load PDBs for every
   loaded module on every test process startup, a heavy cost that perturbs scheduler
   timing enough to suppress narrow races (the very crashes this catcher exists to
   capture). Deferred, on-demand symbol load keeps the catcher from changing the
   reproduction conditions. */
static volatile LONG g_symbols_ready = 0;

static void _tcc_print_frame(HANDLE proc, DWORD64 addr, int idx) {
  /* SYMBOL_INFO is variable-length (Name[1]); allocate room for a long name. */
  char sym_buf[sizeof(SYMBOL_INFO) + 512];
  PSYMBOL_INFO sym = (PSYMBOL_INFO)sym_buf;
  sym->SizeOfStruct = sizeof(SYMBOL_INFO);
  DWORD64 disp = 0;
  const char* fname = "?";
  if (g_SymFromAddr && g_SymFromAddr(proc, addr, &disp, sym)) {
    fname = sym->Name;
  }
  IMAGEHLP_LINE64 line;
  line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
  DWORD line_disp = 0;
  DWORD line_no = 0;
  const char* file = "";
  if (g_SymGetLineFromAddr64 && g_SymGetLineFromAddr64(proc, addr, &line_disp, &line)) {
    line_no = line.LineNumber;
    file = line.FileName ? line.FileName : "";
  }
  fprintf(stderr, "  #%02d 0x%016llx  %s+0x%llx  %s:%lu\n",
          idx, (unsigned long long)addr, fname, (unsigned long long)disp,
          file, (unsigned long)line_no);
  fflush(stderr);
}

/* Deferred, one-time dbghelp symbol setup — called from the handler on the first
   fatal exception only (guarded by g_symbols_ready). Kept out of
   test_crash_catch_init so the catcher does not perturb the timing of the very
   races it is meant to capture. */
static void _tcc_ensure_symbols(void) {
  if (InterlockedCompareExchange(&g_symbols_ready, 1, 0) != 0) return;
  if (g_SymGetOptions && g_SymSetOptions) {
    g_SymSetOptions(g_SymGetOptions() | SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS);
  }
  if (g_SymInitialize) {
    /* fInvadeProcess=TRUE: enumerate modules and load their symbols so
       SymFromAddr/SymGetLineFromAddr64 can resolve frames. Expensive, but this
       runs once at crash time (see _tcc_handler) where timing no longer matters. */
    g_SymInitialize(GetCurrentProcess(), NULL, TRUE);
  }
  if (g_SymSetSearchPath) {
    g_SymSetSearchPath(GetCurrentProcess(), ".");
  }
}

static LONG WINAPI _tcc_handler(PEXCEPTION_POINTERS ep) {
  DWORD code = ep->ExceptionRecord->ExceptionCode;
  /* Only act on genuinely fatal exceptions; pass first-chance non-fatal ones
     (e.g. C++ exceptions, guard-page faults used by allocators) through. */
  if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_STACK_OVERFLOW &&
      code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_IN_PAGE_ERROR &&
      code != EXCEPTION_PRIV_INSTRUCTION && code != EXCEPTION_DATATYPE_MISALIGNMENT) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  /* One-time symbol setup, deferred to the first fatal crash so init stays
     timing-neutral. fInvadeProcess=TRUE auto-loads PDBs for every loaded module,
     which is expensive — but it runs ONCE, here in the handler on a process that
     is already dying, so the cost does not perturb any race. (Doing it at init
     with TRUE would slow every test process startup and suppress narrow races;
     doing it here with FALSE would leave modules unregistered and break
     symbolication — TRUE-at-crash-time is the only option that is both cheap at
     init and resolves frames.) */
  _tcc_ensure_symbols();
  const char* ename = "EXCEPTION";
  const char* av = "";
  if (code == EXCEPTION_ACCESS_VIOLATION) {
    ename = "EXCEPTION_ACCESS_VIOLATION";
    if (ep->ExceptionRecord->NumberParameters >= 2) {
      av = (ep->ExceptionRecord->ExceptionInformation[0] == 0) ? " read" :
           (ep->ExceptionRecord->ExceptionInformation[0] == 1) ? " write" : " exec";
    }
  } else if (code == EXCEPTION_STACK_OVERFLOW) ename = "EXCEPTION_STACK_OVERFLOW";
  else if (code == EXCEPTION_ILLEGAL_INSTRUCTION) ename = "EXCEPTION_ILLEGAL_INSTRUCTION";
  else if (code == EXCEPTION_IN_PAGE_ERROR) ename = "EXCEPTION_IN_PAGE_ERROR";
  fprintf(stderr, "\n=== TEST_CRASH_CATCH: %s (0x%08lx) at %p%s",
          ename, (unsigned long)code, ep->ExceptionRecord->ExceptionAddress, av);
  if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2) {
    fprintf(stderr, " target=0x%016llx",
            (unsigned long long)ep->ExceptionRecord->ExceptionInformation[1]);
  }
  fprintf(stderr, " ===\n");
  fflush(stderr);

  if (g_StackWalk64) {
    HANDLE proc = GetCurrentProcess();
    HANDLE th = GetCurrentThread();
    CONTEXT ctx;
    memcpy(&ctx, ep->ContextRecord, sizeof(ctx));
    STACKFRAME64 sf;
    memset(&sf, 0, sizeof(sf));
    DWORD mach;
#if defined(_M_X64) || defined(__x86_64__)
    sf.AddrPC.Offset = ctx.Rip;     sf.AddrPC.Mode = AddrModeFlat;
    sf.AddrFrame.Offset = ctx.Rsp;  sf.AddrFrame.Mode = AddrModeFlat;
    sf.AddrStack.Offset = ctx.Rsp;  sf.AddrStack.Mode = AddrModeFlat;
    mach = IMAGE_FILE_MACHINE_AMD64;
#elif defined(_M_IX86) || defined(__i386__)
    sf.AddrPC.Offset = ctx.Eip;     sf.AddrPC.Mode = AddrModeFlat;
    sf.AddrFrame.Offset = ctx.Ebp;  sf.AddrFrame.Mode = AddrModeFlat;
    sf.AddrStack.Offset = ctx.Esp;  sf.AddrStack.Mode = AddrModeFlat;
    mach = IMAGE_FILE_MACHINE_I386;
#else
    sf.AddrPC.Offset = 0; sf.AddrPC.Mode = AddrModeFlat;
    mach = IMAGE_FILE_MACHINE_AMD64;
#endif
    fprintf(stderr, "Faulting stack:\n");
    fflush(stderr);
    int idx = 0;
    _tcc_print_frame(proc, sf.AddrPC.Offset, idx++);
    for (int i = 0; i < 48; i++) {
      if (!g_StackWalk64(mach, proc, th, &sf, &ctx, NULL,
                         (PFUNCTION_TABLE_ACCESS_ROUTINE64)g_SymFunctionTableAccess64,
                         (PGET_MODULE_BASE_ROUTINE64)g_SymGetModuleBase64, NULL)) {
        break;
      }
      if (sf.AddrPC.Offset == 0) break;
      _tcc_print_frame(proc, sf.AddrPC.Offset, idx++);
    }
    fflush(stderr);
  }
  /* Let the exception proceed to its normal (terminating) disposition so the
     reproduction loop observes a non-zero exit. */
  return EXCEPTION_CONTINUE_SEARCH;
}

static void test_crash_catch_init(void) {
  const char* e = getenv("OFFS_CRASH_TRACE");
  if (e == NULL || e[0] == '\0') return; /* gated: no-op unless opted in */
  g_dbghelp = LoadLibraryA("dbghelp.dll");
  if (g_dbghelp == NULL) return;
  g_SymInitialize        = (pfnSymInitialize)GetProcAddress(g_dbghelp, "SymInitialize");
  g_SymSetSearchPath      = (pfnSymSetSearchPath)GetProcAddress(g_dbghelp, "SymSetSearchPath");
  g_SymGetOptions         = (pfnSymGetOptions)GetProcAddress(g_dbghelp, "SymGetOptions");
  g_SymSetOptions         = (pfnSymSetOptions)GetProcAddress(g_dbghelp, "SymSetOptions");
  g_SymFromAddr           = (pfnSymFromAddr)GetProcAddress(g_dbghelp, "SymFromAddr");
  g_SymGetLineFromAddr64  = (pfnSymGetLineFromAddr64)GetProcAddress(g_dbghelp, "SymGetLineFromAddr64");
  g_StackWalk64           = (pfnStackWalk64)GetProcAddress(g_dbghelp, "StackWalk64");
  g_SymFunctionTableAccess64 = (pfnSymFunctionTableAccess64)GetProcAddress(g_dbghelp, "SymFunctionTableAccess64");
  g_SymGetModuleBase64       = (pfnSymGetModuleBase64)GetProcAddress(g_dbghelp, "SymGetModuleBase64");
  /* Symbol setup is deferred to the first fatal exception (_tcc_ensure_symbols)
     so this init stays cheap and timing-neutral — see the note above. Register
     as the FIRST vectored handler so we run before any frame-based SEH a
     framework installs, guaranteeing we see the faulting context. */
  AddVectoredExceptionHandler(1, _tcc_handler);
  fprintf(stderr, "[test_crash_catch] vectored handler installed (OFFS_CRASH_TRACE=%s)\n", e);
  fflush(stderr);
}

#else  /* !defined(_WIN32) */

/* On non-Windows platforms the vectored exception handler has no equivalent
   (Unix test runs already get a symbolicated backtrace from SIGSEGV/SIGABRT
   via the shell). Provide a no-op so test_main.cpp can call it unconditionally
   without a platform guard. */
static inline void test_crash_catch_init(void) {}

#endif /* defined(_WIN32) */

#endif /* TEST_CRASH_CATCH_H */