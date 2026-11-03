#ifndef LOGGER_TEST_PSP2_CLIB_H
#define LOGGER_TEST_PSP2_CLIB_H

#include <stdarg.h>
#include <stddef.h>

int sceClibPrintf(const char *format, ...);
int sceClibSnprintf(char *buffer, size_t size, const char *format, ...);
int sceClibVsnprintf(char *buffer, size_t size, const char *format, va_list args);

#endif
