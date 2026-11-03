#include "reimpl/opensles_diag.h"

#include "utils/logger.h"

#include <SLES/OpenSLES_Android.h>

#include <stdbool.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>

extern volatile uint32_t opensles_vita_output_buffers;
extern volatile uint32_t opensles_vita_nonzero_buffers;
extern volatile uint32_t opensles_vita_output_completions;
extern volatile uint32_t opensles_vita_waited_for_enqueue;
extern volatile int32_t opensles_vita_output_error;
extern volatile uint32_t opensles_vita_player_count;
extern volatile uint32_t opensles_vita_player_failures;
extern volatile uint32_t opensles_vita_enqueue_count;
extern volatile uint32_t opensles_vita_enqueue_failures;
extern volatile uint32_t opensles_vita_direct_enqueue_count;
extern volatile uint32_t opensles_vita_direct_nonzero_count;
extern volatile uint32_t opensles_vita_direct_enqueue_bytes;
extern volatile uint32_t opensles_vita_stream_enqueue_count;
extern volatile uint32_t opensles_vita_stream_enqueue_bytes;
extern volatile uint32_t opensles_vita_direct_mix_chunks;
extern volatile uint32_t opensles_vita_direct_muted_chunks;

static const struct SLObjectItf_ *original_object_vtable;
static struct SLObjectItf_ traced_object_vtable;
static bool object_vtable_ready;

static const struct SLEngineItf_ *original_engine_vtable;
static struct SLEngineItf_ traced_engine_vtable;
static bool engine_vtable_ready;
static bool repaired_negative_android_fd_reported;

static bool report_routine_audio_success(void) {
    uint32_t players = opensles_vita_player_count;
    return players <= 4 || (players & 31u) == 0;
}

static void install_object_trace(SLObjectItf object);
static void install_engine_trace(SLEngineItf engine);

static const char *interface_name(SLInterfaceID iid) {
    if (iid == SL_IID_PLAY) {
        return "PLAY";
    }
    if (iid == SL_IID_VOLUME) {
        return "VOLUME";
    }
    if (iid == SL_IID_SEEK) {
        return "SEEK";
    }
    if (iid == SL_IID_PLAYBACKRATE) {
        return "PLAYBACKRATE";
    }
    if (iid == SL_IID_PREFETCHSTATUS) {
        return "PREFETCHSTATUS";
    }
    if (iid == SL_IID_BUFFERQUEUE) {
        return "BUFFERQUEUE";
    }
    if (iid == SL_IID_ANDROIDSIMPLEBUFFERQUEUE) {
        return "ANDROIDSIMPLEBUFFERQUEUE";
    }
    return "UNKNOWN";
}

static void log_audio_endpoint(const char *name, const void *locator,
                               const void *format) {
    const SLuint32 locator_type =
        locator ? *(const SLuint32 *)locator : 0;
    const SLuint32 format_type =
        format ? *(const SLuint32 *)format : 0;

    l_error("OpenSL failed-player %s: locator=0x%08x, format=0x%08x.",
            name, (unsigned int)locator_type, (unsigned int)format_type);

    switch (locator_type) {
    case SL_DATALOCATOR_BUFFERQUEUE:
    case SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE:
        l_error("OpenSL failed-player %s buffer queue: buffers=%u.",
                name,
                (unsigned int)((const SLDataLocator_BufferQueue *)locator)
                    ->numBuffers);
        break;
    case SL_DATALOCATOR_ANDROIDFD: {
        const SLDataLocator_AndroidFD *fd =
            (const SLDataLocator_AndroidFD *)locator;
        l_error("OpenSL failed-player %s Android FD: fd=%d, "
                "offset=%lld, length=%lld.",
                name, (int)fd->fd, (long long)fd->offset,
                (long long)fd->length);
        break;
    }
    case SL_DATALOCATOR_URI:
        l_error("OpenSL failed-player %s URI pointer=%p.",
                name, ((const SLDataLocator_URI *)locator)->URI);
        break;
    case SL_DATALOCATOR_ADDRESS: {
        const SLDataLocator_Address *address =
            (const SLDataLocator_Address *)locator;
        l_error("OpenSL failed-player %s address: data=%p, length=%u.",
                name, address->pAddress, (unsigned int)address->length);
        break;
    }
    case SL_DATALOCATOR_OUTPUTMIX:
        l_error("OpenSL failed-player %s output mix=%p.",
                name,
                ((const SLDataLocator_OutputMix *)locator)->outputMix);
        break;
    default:
        break;
    }

    if (format_type == SL_DATAFORMAT_PCM) {
        const SLDataFormat_PCM *pcm = (const SLDataFormat_PCM *)format;
        l_error("OpenSL failed-player %s PCM: channels=%u, rate=%u mHz, "
                "bits=%u, container=%u, mask=0x%08x, endian=%u.",
                name, (unsigned int)pcm->numChannels,
                (unsigned int)pcm->samplesPerSec,
                (unsigned int)pcm->bitsPerSample,
                (unsigned int)pcm->containerSize,
                (unsigned int)pcm->channelMask,
                (unsigned int)pcm->endianness);
    } else if (format_type == SL_DATAFORMAT_MIME) {
        const SLDataFormat_MIME *mime =
            (const SLDataFormat_MIME *)format;
        l_error("OpenSL failed-player %s MIME: string=%p, container=%u.",
                name, mime->mimeType, (unsigned int)mime->containerType);
    }
}

static void log_failed_audio_player_request(
    const SLDataSource *audio_source,
    const SLDataSink *audio_sink,
    SLuint32 num_interfaces,
    const SLInterfaceID *interface_ids,
    const SLboolean *interface_required) {
    log_audio_endpoint(
        "source",
        audio_source ? audio_source->pLocator : NULL,
        audio_source ? audio_source->pFormat : NULL);
    log_audio_endpoint(
        "sink",
        audio_sink ? audio_sink->pLocator : NULL,
        audio_sink ? audio_sink->pFormat : NULL);

    for (SLuint32 i = 0; i < num_interfaces; ++i) {
        const SLInterfaceID iid =
            interface_ids ? interface_ids[i] : NULL;
        l_error("OpenSL failed-player interface %u: %s (%p), required=%u.",
                (unsigned int)i, interface_name(iid), iid,
                interface_required
                    ? (unsigned int)interface_required[i]
                    : 0U);
    }
    log_flush();
}

static void log_audio_counters(const char *context, SLObjectItf object) {
    l_info(
        "OpenSL object %p %s: output_error=0x%08x, output_buffers=%u, "
        "nonzero_buffers=%u, output_completions=%u, waited_for_enqueue=%u, "
        "players=%u, player_failures=%u, enqueues=%u, enqueue_failures=%u, "
        "direct_pcm=%u/%u nonzero (%u bytes), direct_mix=%u active/%u "
        "muted chunks, decoded_stream=%u (%u bytes).",
        object, context, (unsigned int)opensles_vita_output_error,
        (unsigned int)opensles_vita_output_buffers,
        (unsigned int)opensles_vita_nonzero_buffers,
        (unsigned int)opensles_vita_output_completions,
        (unsigned int)opensles_vita_waited_for_enqueue,
        (unsigned int)opensles_vita_player_count,
        (unsigned int)opensles_vita_player_failures,
        (unsigned int)opensles_vita_enqueue_count,
        (unsigned int)opensles_vita_enqueue_failures,
        (unsigned int)opensles_vita_direct_nonzero_count,
        (unsigned int)opensles_vita_direct_enqueue_count,
        (unsigned int)opensles_vita_direct_enqueue_bytes,
        (unsigned int)opensles_vita_direct_mix_chunks,
        (unsigned int)opensles_vita_direct_muted_chunks,
        (unsigned int)opensles_vita_stream_enqueue_count,
        (unsigned int)opensles_vita_stream_enqueue_bytes);
}

static void traced_object_destroy(SLObjectItf self) {
    log_audio_counters("Destroy entered", self);
    original_object_vtable->Destroy(self);
    l_success("OpenSL object %p Destroy returned.", self);
}

static SLresult traced_object_realize(SLObjectItf self, SLboolean async) {
    bool report = report_routine_audio_success();
    if (report) {
        l_info("OpenSL object %p Realize(async=%u) entered.", self,
               (unsigned int)async);
    }
    SLresult result = original_object_vtable->Realize(self, async);
    if (result == SL_RESULT_SUCCESS) {
        if (report) {
            l_success("OpenSL object %p Realize returned successfully.",
                      self);
        }
    } else {
        l_error("OpenSL object %p Realize failed: 0x%x.", self,
                (unsigned int)result);
    }
    return result;
}

static SLresult traced_object_get_interface(SLObjectItf self,
                                            const SLInterfaceID iid,
                                            void *interface_out) {
    bool report = report_routine_audio_success();
    if (report) {
        l_info("OpenSL object %p GetInterface(iid=%p) entered.", self, iid);
    }
    SLresult result = original_object_vtable->GetInterface(
        self, iid, interface_out);
    if (result == SL_RESULT_SUCCESS) {
        if (report) {
            l_success("OpenSL object %p GetInterface returned %p.", self,
                      interface_out ? *(void **)interface_out : NULL);
        }
        if (iid == SL_IID_ENGINE && interface_out) {
            install_engine_trace(*(SLEngineItf *)interface_out);
        }
    } else {
        l_error("OpenSL object %p GetInterface failed: 0x%x.", self,
                (unsigned int)result);
    }
    return result;
}

static SLresult traced_engine_create_output_mix(
    SLEngineItf self,
    SLObjectItf *mix,
    SLuint32 num_interfaces,
    const SLInterfaceID *interface_ids,
    const SLboolean *interface_required) {
    l_info("OpenSL CreateOutputMix entered (%u interfaces).",
           (unsigned int)num_interfaces);
    SLresult result = original_engine_vtable->CreateOutputMix(
        self, mix, num_interfaces, interface_ids, interface_required);
    if (result == SL_RESULT_SUCCESS) {
        l_success("OpenSL CreateOutputMix returned object %p.",
                  mix ? *mix : NULL);
        if (mix) {
            install_object_trace(*mix);
        }
    } else {
        l_error("OpenSL CreateOutputMix failed: 0x%x.",
                (unsigned int)result);
    }
    return result;
}

static SLresult traced_engine_create_audio_player(
    SLEngineItf self,
    SLObjectItf *player,
    SLDataSource *audio_source,
    SLDataSink *audio_sink,
    SLuint32 num_interfaces,
    const SLInterfaceID *interface_ids,
    const SLboolean *interface_required) {
    bool report = report_routine_audio_success();
    if (report) {
        l_info("OpenSL CreateAudioPlayer entered (%u interfaces).",
               (unsigned int)num_interfaces);
    }

    /*
     * Fusion can pass its Java asset descriptor after that borrowed Android
     * descriptor has already become a negative stale value. The associated
     * offset and length still identify an immutable MP3 range in the pinned
     * 1057 OBB. OpenSL rejects a negative Android FD before our decoder gets a
     * chance to use its existing OBB-reopen fallback, so substitute an invalid
     * positive descriptor in a temporary locator. dup() will fail at Realize,
     * and sf_open_android_fd will then safely reopen and range-check the OBB.
     */
    SLDataLocator_AndroidFD repaired_locator;
    SLDataSource repaired_source;
    SLDataSource *source_for_opensles = audio_source;
    if (audio_source && audio_source->pLocator &&
        *(const SLuint32 *)audio_source->pLocator ==
            SL_DATALOCATOR_ANDROIDFD) {
        const SLDataLocator_AndroidFD *android_fd =
            (const SLDataLocator_AndroidFD *)audio_source->pLocator;
        if (android_fd->fd < 0 && android_fd->offset >= 0 &&
            (android_fd->length == -1 || android_fd->length > 0)) {
            repaired_locator = *android_fd;
            repaired_locator.fd = INT_MAX;
            repaired_source = *audio_source;
            repaired_source.pLocator = &repaired_locator;
            source_for_opensles = &repaired_source;
            if (!repaired_negative_android_fd_reported) {
                repaired_negative_android_fd_reported = true;
                l_warn("Repaired Fusion's stale Android audio fd %d; the "
                       "pinned OBB fallback will service its byte ranges.",
                       (int)android_fd->fd);
            }
        }
    }

    SLresult result = original_engine_vtable->CreateAudioPlayer(
        self, player, source_for_opensles, audio_sink, num_interfaces,
        interface_ids, interface_required);
    if (result == SL_RESULT_SUCCESS) {
        if (report) {
            l_success("OpenSL CreateAudioPlayer returned object %p.",
                      player ? *player : NULL);
        }
        if (player) {
            install_object_trace(*player);
        }
    } else {
        l_error("OpenSL CreateAudioPlayer failed: 0x%x.",
                (unsigned int)result);
        log_failed_audio_player_request(
            source_for_opensles, audio_sink, num_interfaces, interface_ids,
            interface_required);
    }
    return result;
}

static void install_object_trace(SLObjectItf object) {
    if (!object) {
        return;
    }
    const struct SLObjectItf_ **vtable_slot =
        (const struct SLObjectItf_ **)(void *)object;
    if (*vtable_slot == &traced_object_vtable) {
        return;
    }
    if (!object_vtable_ready) {
        original_object_vtable = *vtable_slot;
        traced_object_vtable = *original_object_vtable;
        traced_object_vtable.Destroy = traced_object_destroy;
        traced_object_vtable.Realize = traced_object_realize;
        traced_object_vtable.GetInterface = traced_object_get_interface;
        object_vtable_ready = true;
    } else if (*vtable_slot != original_object_vtable) {
        l_warn("OpenSL object %p uses an unexpected object vtable; "
               "leaving it unmodified.", object);
        return;
    }
    *vtable_slot = &traced_object_vtable;
}

static void install_engine_trace(SLEngineItf engine) {
    if (!engine) {
        return;
    }
    const struct SLEngineItf_ **vtable_slot =
        (const struct SLEngineItf_ **)(void *)engine;
    if (*vtable_slot == &traced_engine_vtable) {
        return;
    }
    if (!engine_vtable_ready) {
        original_engine_vtable = *vtable_slot;
        traced_engine_vtable = *original_engine_vtable;
        traced_engine_vtable.CreateOutputMix =
            traced_engine_create_output_mix;
        traced_engine_vtable.CreateAudioPlayer =
            traced_engine_create_audio_player;
        engine_vtable_ready = true;
    } else if (*vtable_slot != original_engine_vtable) {
        l_warn("OpenSL engine interface %p uses an unexpected vtable; "
               "leaving it unmodified.", engine);
        return;
    }
    *vtable_slot = &traced_engine_vtable;
}

SLresult slCreateEngine_soloader(
    SLObjectItf *engine,
    SLuint32 num_options,
    const SLEngineOption *engine_options,
    SLuint32 num_interfaces,
    const SLInterfaceID *interface_ids,
    const SLboolean *interface_required) {
    l_info("OpenSL slCreateEngine entered (%u options, %u interfaces).",
           (unsigned int)num_options, (unsigned int)num_interfaces);
    SLresult result = slCreateEngine(
        engine, num_options, engine_options, num_interfaces, interface_ids,
        interface_required);
    if (result == SL_RESULT_SUCCESS) {
        l_success("OpenSL slCreateEngine returned object %p.",
                  engine ? *engine : NULL);
        if (engine) {
            install_object_trace(*engine);
        }
    } else {
        l_error("OpenSL slCreateEngine failed: 0x%x.",
                (unsigned int)result);
    }
    return result;
}
