//
// Created by victor on 4/7/25.
//

#ifndef OFFS_ERROR_H
#define OFFS_ERROR_H

// The Windows SDK <wingdi.h> defines `ERROR` as the integer 0, which would
// break a function-style ERROR(...) macro. We expose the canonical name as
// OFFS_ERROR and keep ERROR as an alias for convenience, but the alias is
// only emitted if the Windows SDK has not already defined ERROR. This avoids
// surprises when a TU includes <windows.h> before this header.
#include "../RefCounter/refcounter.h"
typedef struct {
  refcounter_t refcounter;
  char* message;
  char* file;
  char* function;
  int line;
} async_error_t;

async_error_t* error_create(char* message, char* file, char* function, int line);
void error_destroy(async_error_t* error);

#define OFFS_ERROR(MESSAGE) error_create((MESSAGE), (char*)__FILE__, (char*)__func__, __LINE__)

// Only define ERROR(...) if the Windows SDK hasn't already taken the name.
// On non-Windows builds (or after carefully undef'ing first) this gives us
// the short form; otherwise callers must use OFFS_ERROR.
#ifndef _WIN32
  #define ERROR(MESSAGE) OFFS_ERROR(MESSAGE)
#endif

#endif //OFFS_ERROR_H
