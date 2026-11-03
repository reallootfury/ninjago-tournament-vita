/*
 * Minimal libsndfile-compatible reader for OpenSLES Android-FD MP3 sources.
 *
 * Ninjago stores its music and effects as MP3 byte ranges inside the OBB.
 * The Android game passes those ranges to OpenSL ES with
 * SL_DATALOCATOR_ANDROIDFD.  Keeping this narrow adapter in the loader avoids
 * linking VitaSDK's hard-float libsndfile into the soft-float Android ABI.
 */
#include "utils/logger.h"

#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_STDIO
#define MINIMP3_IMPLEMENTATION
#include <minimp3_ex.h>

#include <sndfile.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct sf_private_tag {
    mp3dec_ex_t decoder;
    mp3dec_io_t io;
    int fd;
    uint64_t base;
    uint64_t length;
    uint64_t position;
    int channels;
};

static unsigned int mp3_open_reports;
static unsigned int obb_reopen_reports;

static size_t android_fd_read(void *buffer, size_t size, void *user_data) {
    SNDFILE *file = (SNDFILE *)user_data;
    if (!file || !buffer || file->position >= file->length) {
        return 0;
    }

    uint64_t remaining = file->length - file->position;
    if ((uint64_t)size > remaining) {
        size = (size_t)remaining;
    }
    if (size > (size_t)INT_MAX) {
        size = (size_t)INT_MAX;
    }

    ssize_t result = pread(file->fd, buffer, size,
                           (off_t)(file->base + file->position));
    if (result <= 0) {
        return 0;
    }
    file->position += (uint64_t)result;
    return (size_t)result;
}

static int android_fd_seek(uint64_t position, void *user_data) {
    SNDFILE *file = (SNDFILE *)user_data;
    if (!file || position > file->length) {
        return -1;
    }
    file->position = position;
    return 0;
}

SNDFILE *sf_open_android_fd(int fd, int64_t offset, int64_t length,
                            int mode, SF_INFO *info) {
    if (fd < 0 || offset < 0 || !info || mode != SFM_READ) {
        return NULL;
    }

    int owned_fd = dup(fd);
    if (owned_fd < 0) {
        int duplicate_errno = errno;

        /*
         * Fusion closes its Java-side AssetFileDescriptor immediately after
         * CreateAudioPlayer succeeds.  The OpenSL object is realized later,
         * so the borrowed Android descriptor can already be invalid here.
         * Every Android-FD source in the pinned 1057 title is a byte range in
         * its matching OBB; reopen that immutable container and retain our
         * own descriptor for the decoder lifetime.
         */
        owned_fd = open(OBB_PATH, O_RDONLY);
        if (owned_fd < 0) {
            l_error("MP3 decoder could not duplicate Android fd %d "
                    "(errno=%d) or reopen %s (errno=%d).",
                    fd, duplicate_errno, OBB_PATH, errno);
            return NULL;
        }
        if (obb_reopen_reports < 4) {
            ++obb_reopen_reports;
            l_warn("Android audio fd %d expired before OpenSL Realize "
                   "(errno=%d); reopened pinned OBB %s.",
                   fd, duplicate_errno, OBB_PATH);
        }
    }

    struct stat status;
    if (fstat(owned_fd, &status) < 0 || status.st_size < offset) {
        l_error("MP3 decoder could not stat Android fd %d or offset %lld "
                "is outside it.", fd, (long long)offset);
        close(owned_fd);
        return NULL;
    }

    int64_t available = (int64_t)status.st_size - offset;
    if (length == -1) {
        length = available;
    }
    if (length <= 0 || length > available) {
        l_error("MP3 decoder rejected Android fd %d range offset=%lld "
                "length=%lld (available=%lld).", fd, (long long)offset,
                (long long)length, (long long)available);
        close(owned_fd);
        return NULL;
    }

    SNDFILE *file = (SNDFILE *)calloc(1, sizeof(*file));
    if (!file) {
        close(owned_fd);
        return NULL;
    }
    file->fd = owned_fd;
    file->base = (uint64_t)offset;
    file->length = (uint64_t)length;
    file->io.read = android_fd_read;
    file->io.read_data = file;
    file->io.seek = android_fd_seek;
    file->io.seek_data = file;

    /* Most effects never seek. Avoid scanning every MP3 during arena loads;
     * the decoder builds a precise index lazily if a non-zero seek occurs. */
    int result = mp3dec_ex_open_cb(&file->decoder, &file->io,
                                   MP3D_SEEK_TO_SAMPLE |
                                   MP3D_DO_NOT_SCAN);
    if (result != 0 || file->decoder.info.hz <= 0 ||
        (file->decoder.info.channels != 1 &&
         file->decoder.info.channels != 2)) {
        l_error("MP3 decoder failed for Android fd %d offset=%lld "
                "length=%lld: result=%d rate=%d channels=%d.",
                fd, (long long)offset, (long long)length, result,
                file->decoder.info.hz, file->decoder.info.channels);
        mp3dec_ex_close(&file->decoder);
        close(file->fd);
        free(file);
        return NULL;
    }

    file->channels = file->decoder.info.channels;
    memset(info, 0, sizeof(*info));
    info->samplerate = file->decoder.info.hz;
    info->channels = file->channels;
    info->frames = (sf_count_t)(file->decoder.samples /
                                (uint64_t)file->channels);
    info->format = SF_FORMAT_MPEG | SF_FORMAT_MPEG_LAYER_III;
    info->sections = 1;
    info->seekable = 1;

    if (mp3_open_reports < 12) {
        ++mp3_open_reports;
        l_success("Opened OBB MP3 %u: fd=%d offset=%lld length=%lld, "
                  "%d Hz, %d channel(s), known_frames=%lld.",
                  mp3_open_reports, fd, (long long)offset,
                  (long long)length, info->samplerate, info->channels,
                  (long long)info->frames);
    }
    return file;
}

/* URI players are not used by this title. Keep the ABI symbol for OpenSLES. */
SNDFILE *sf_open(const char *path, int mode, SF_INFO *info) {
    (void)path;
    (void)mode;
    (void)info;
    return NULL;
}

int sf_close(SNDFILE *file) {
    if (!file) {
        return 0;
    }
    mp3dec_ex_close(&file->decoder);
    close(file->fd);
    free(file);
    return 0;
}

sf_count_t sf_read_short(SNDFILE *file, short *samples,
                         sf_count_t items) {
    if (!file || !samples || items <= 0) {
        return 0;
    }
    size_t requested = items > (sf_count_t)SIZE_MAX
        ? SIZE_MAX
        : (size_t)items;
    return (sf_count_t)mp3dec_ex_read(&file->decoder,
                                      (mp3d_sample_t *)samples,
                                      requested);
}

sf_count_t sf_seek(SNDFILE *file, sf_count_t frames, int whence) {
    if (!file || file->channels <= 0) {
        return -1;
    }

    int64_t base_frames;
    switch (whence) {
    case SEEK_SET:
        base_frames = 0;
        break;
    case SEEK_CUR:
        base_frames = (int64_t)(file->decoder.cur_sample /
                                (uint64_t)file->channels);
        break;
    case SEEK_END:
        if (file->decoder.samples == 0) {
            return -1;
        }
        base_frames = (int64_t)(file->decoder.samples /
                                (uint64_t)file->channels);
        break;
    default:
        return -1;
    }

    if ((frames > 0 && base_frames > INT64_MAX - frames) ||
        (frames < 0 && frames < -base_frames)) {
        return -1;
    }
    int64_t target_frames = base_frames + frames;
    if (target_frames < 0 ||
        (uint64_t)target_frames > UINT64_MAX / (uint64_t)file->channels) {
        return -1;
    }
    uint64_t target_sample = (uint64_t)target_frames *
                             (uint64_t)file->channels;
    if (mp3dec_ex_seek(&file->decoder, target_sample) != 0) {
        return -1;
    }
    return (sf_count_t)target_frames;
}
