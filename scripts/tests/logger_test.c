/* Exercise the real logger; only Vita platform calls are replaced on the host. */
#include "utils/logger.h"
#include <psp2/io/fcntl.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/threadmgr.h>

#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef NINJAGO_RUNTIME_LOGGING
#define MODE "logging-on"
#else
#define MODE "logging-off"
#endif

static unsigned file_opens, file_writes, file_removes;
static char console[8192];
static size_t console_used;

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s: %s\n", MODE, message); \
        return 1; \
    } \
} while (0)

static void require(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL %s platform stub: %s\n", MODE, message);
        exit(2);
    }
}

static void check_log_path(const char *path) {
    require(!strcmp(path, "ux0:data/ninjago/loader.log") ||
            !strcmp(path, "ux0:/data/ninjago/loader.log"),
            "unexpected file path");
}

SceUID sceIoOpen(const char *path, int flags, int mode) {
    check_log_path(path);
    require(flags == (SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND),
            "log must be opened for append");
    ++file_opens;
    return open("loader.log", O_WRONLY | O_CREAT | O_APPEND, mode);
}

int sceIoRemove(const char *path) {
    check_log_path(path);
    ++file_removes;
    return unlink("loader.log");
}

ssize_t sceIoWrite(SceUID fd, const void *buffer, size_t size) {
    ++file_writes;
    return write(fd, buffer, size);
}

int sceClibPrintf(const char *format, ...) {
    va_list args;
    va_start(args, format);
    int count = vsnprintf(console + console_used, sizeof(console) - console_used,
                          format, args);
    va_end(args);
    require(count >= 0 && (size_t)count < sizeof(console) - console_used,
            "console capture overflow");
    console_used += (size_t)count;
    return count;
}

int sceClibSnprintf(char *buffer, size_t size, const char *format, ...) {
    va_list args;
    va_start(args, format);
    int count = vsnprintf(buffer, size, format, args);
    va_end(args);
    return count;
}

int sceClibVsnprintf(char *buffer, size_t size, const char *format, va_list args) {
    return vsnprintf(buffer, size, format, args);
}

int sceKernelCreateLwMutex(SceKernelLwMutexWork *mutex, const char *name,
                         unsigned int attributes, int count,
                         const void *options) {
    (void)name;
    (void)attributes;
    (void)options;
    require(count == 0, "unexpected initial mutex count");
    mutex->locked = 0;
    return 0;
}

int sceKernelLockLwMutex(SceKernelLwMutexWork *mutex, int count, void *timeout) {
    (void)timeout;
    require(count == 1 && !mutex->locked, "invalid log mutex lock");
    mutex->locked = 1;
    return 0;
}

int sceKernelUnlockLwMutex(SceKernelLwMutexWork *mutex, int count) {
    require(count == 1 && mutex->locked, "invalid log mutex unlock");
    mutex->locked = 0;
    return 0;
}

static int no_file_activity(const char *operation) {
    if (!file_opens && !file_writes && !file_removes) {
        return 1;
    }
    fprintf(stderr, "FAIL %s: %s touched loader.log "
                    "(open=%u write=%u remove=%u)\n", MODE, operation,
            file_opens, file_writes, file_removes);
    return 0;
}

static void read_log(char *buffer, size_t size) {
    FILE *file = fopen("loader.log", "rb");
    require(file != NULL, "log file is missing");
    size_t count = fread(buffer, 1, size - 1, file);
    require(!ferror(file) && count < size - 1, "log read failed or overflowed");
    buffer[count] = '\0';
    require(fclose(file) == 0, "log read close failed");
}

int main(void) {
    static const char old_log[] = "previous-session-marker\n";
    FILE *file = fopen("loader.log", "wb");
    CHECK(file != NULL, "cannot seed previous log");
    CHECK(fputs(old_log, file) >= 0 && fclose(file) == 0,
          "cannot write previous log");

    log_write_raw("before-session-marker\n");
    log_flush();
    l_info("before-session-info");
    CHECK(no_file_activity("logging before session"),
          "logging must stay disabled before session start");

    log_start_session();
#ifndef NINJAGO_RUNTIME_LOGGING
    /* This catches an unguarded session enabling raw and error file writes. */
    if (!no_file_activity("log_start_session")) {
        return 1;
    }
#endif
    log_write_raw("raw-message-marker\n");
    log_write_raw(NULL);
    log_flush();
    int info_arguments = 0;
    l_info("info-message-%d", ++info_arguments);
    l_error("error-message-%d", 13);
    log_flush();

    char contents[8192];
    read_log(contents, sizeof(contents));
#ifdef NINJAGO_RUNTIME_LOGGING
    CHECK(file_opens > 0 && file_writes > 0 && file_removes > 0,
          "session must replace the old log and persist messages");
    CHECK(!strstr(contents, "previous-session-marker"),
          "old session was not removed");
    CHECK(strstr(contents, "loader session"), "session header is missing");
    CHECK(strstr(contents, "raw-message-marker"), "raw message was not logged");
    CHECK(strstr(contents, "info-message-1"), "info message was not logged");
    CHECK(strstr(contents, "error-message-13"), "error message was not logged");
    CHECK(info_arguments == 1, "enabled info arguments were not evaluated");
#else
    CHECK(no_file_activity("logging APIs"),
          "standard mode must never access the persistent log");
    CHECK(!strcmp(contents, old_log), "standard mode modified the existing log");
    CHECK(info_arguments == 0, "disabled info arguments were evaluated");
    CHECK(!strstr(console, "info-message"), "disabled info reached the console");
#endif
    CHECK(!strstr(contents, "before-session"), "pre-session message was logged");
    CHECK(strstr(console, "error-message-13"), "error console output was lost");
    printf("PASS %s: persistent session logging %s; error console preserved\n",
           MODE,
#ifdef NINJAGO_RUNTIME_LOGGING
           "enabled"
#else
           "disabled"
#endif
    );
    return 0;
}
