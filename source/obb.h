#ifndef NINJAGO_OBB_H
#define NINJAGO_OBB_H

#include <stdbool.h>
#include <stdint.h>

typedef int (*FusionAddPackage)(const char *path, bool expansion_file);
typedef void (*FusionAddPackageEntry)(uint32_t package_index,
                                      const char *name,
                                      uint64_t data_offset,
                                      uint64_t length);

int ninjago_register_obb(const char *path,
                         FusionAddPackage add_package,
                         FusionAddPackageEntry add_entry);

#endif
