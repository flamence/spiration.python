#pragma once

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#if defined(__cplusplus)
extern "C" {
#endif

__declspec(dllexport) int run(const char* name);

#if defined(__cplusplus)
}
#endif
