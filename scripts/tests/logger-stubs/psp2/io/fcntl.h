#ifndef LOGGER_TEST_PSP2_FCNTL_H
#define LOGGER_TEST_PSP2_FCNTL_H

#include <stddef.h>
#include <sys/types.h>

typedef int SceUID;

#define SCE_O_WRONLY 0x0002
#define SCE_O_CREAT  0x0200
#define SCE_O_APPEND 0x0100

SceUID sceIoOpen(const char *path, int flags, int mode);
int sceIoRemove(const char *path);
ssize_t sceIoWrite(SceUID fd, const void *buffer, size_t size);

#endif
