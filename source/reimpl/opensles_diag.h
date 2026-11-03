#ifndef NINJAGO_OPENSL_ES_DIAG_H
#define NINJAGO_OPENSL_ES_DIAG_H

#include <SLES/OpenSLES.h>

#ifdef __cplusplus
extern "C" {
#endif

SLresult slCreateEngine_soloader(
    SLObjectItf *engine,
    SLuint32 num_options,
    const SLEngineOption *engine_options,
    SLuint32 num_interfaces,
    const SLInterfaceID *interface_ids,
    const SLboolean *interface_required);

#ifdef __cplusplus
}
#endif

#endif
