#ifndef LOGGER_TEST_PSP2_THREADMGR_H
#define LOGGER_TEST_PSP2_THREADMGR_H

typedef struct {
    int locked;
} SceKernelLwMutexWork;

int sceKernelCreateLwMutex(SceKernelLwMutexWork *mutex, const char *name,
                         unsigned int attributes, int count,
                         const void *options);
int sceKernelLockLwMutex(SceKernelLwMutexWork *mutex, int count, void *timeout);
int sceKernelUnlockLwMutex(SceKernelLwMutexWork *mutex, int count);

#endif
