#ifndef OFFS_PLATFORM_COMPILER_H
#define OFFS_PLATFORM_COMPILER_H

#if defined(_MSC_VER)
  /* MSVC */
  #define PLATFORM_NORETURN       __declspec(noreturn)
  #define PLATFORM_UNUSED         /* MSVC: suppress C4100 via /wd4100, no per-symbol equivalent */
  #define PLATFORM_STRUCT_PACKED  __declspec(align(1))
  #define PLATFORM_ALIGNED(n)     __declspec(align(n))
  #define PLATFORM_PRINTF(f, a)   /* no MSVC equivalent */
  #define PLATFORM_FFS(x)         (_BitScanForward((unsigned long*)&(x), (x)) ? 0 : 0) /* placeholder */

  #define PLATFORM_DIAGNOSTIC_PUSH         __pragma(warning(push))
  #define PLATFORM_DIAGNOSTIC_POP          __pragma(warning(pop))
  #define PLATFORM_DIAGNOSTIC_IGNORE(w)    __pragma(warning(disable: w))
#else
  /* GCC / Clang */
  #define PLATFORM_NORETURN       __attribute__((noreturn))
  #define PLATFORM_UNUSED         __attribute__((unused))
  #define PLATFORM_STRUCT_PACKED  __attribute__((packed))
  #define PLATFORM_ALIGNED(n)     __attribute__((aligned(n)))
  #define PLATFORM_PRINTF(f, a)   __attribute__((format(printf, f, a)))
  #define PLATFORM_FFS(x)         __builtin_ffs(x)

  #define PLATFORM_DIAGNOSTIC_PUSH         _Pragma("GCC diagnostic push")
  #define PLATFORM_DIAGNOSTIC_POP          _Pragma("GCC diagnostic pop")
  #define PLATFORM_DIAGNOSTIC_IGNORE_STR(w) #w
  #define PLATFORM_DIAGNOSTIC_IGNORE(w)    _Pragma(PLATFORM_DIAGNOSTIC_IGNORE_STR(GCC diagnostic ignored w))
#endif

#endif /* OFFS_PLATFORM_COMPILER_H */
