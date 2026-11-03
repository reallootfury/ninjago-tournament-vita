#include "obb.h"

#include "utils/logger.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ZIP_EOCD_SIGNATURE UINT32_C(0x06054b50)
#define ZIP_CENTRAL_SIGNATURE UINT32_C(0x02014b50)
#define ZIP_LOCAL_SIGNATURE UINT32_C(0x04034b50)
#define ZIP_MAX_EOCD_SEARCH (UINT32_C(0xffff) + 22)

static uint16_t read_u16(const unsigned char *data) {
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t read_u32(const unsigned char *data) {
    return (uint32_t)data[0] |
           ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) |
           ((uint32_t)data[3] << 24);
}

static int local_data_offset(FILE *file, uint32_t local_offset,
                             uint64_t *data_offset) {
    unsigned char local[30];
    if (fseek(file, (long)local_offset, SEEK_SET) != 0 ||
        fread(local, 1, sizeof(local), file) != sizeof(local) ||
        read_u32(local) != ZIP_LOCAL_SIGNATURE) {
        return -1;
    }
    *data_offset = (uint64_t)local_offset + sizeof(local) +
                   read_u16(local + 26) + read_u16(local + 28);
    return 0;
}

int ninjago_register_obb(const char *path,
                         FusionAddPackage add_package,
                         FusionAddPackageEntry add_entry) {
    if (!path || !add_package || !add_entry) {
        l_error("OBB registration received an invalid argument.");
        return -1;
    }

    FILE *file = fopen(path, "rb");
    if (!file) {
        l_error("Could not open expansion file: %s", path);
        return -2;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return -3;
    }
    long file_size = ftell(file);
    if (file_size < 22) {
        fclose(file);
        return -4;
    }

    size_t search_size = (size_t)file_size;
    if (search_size > ZIP_MAX_EOCD_SEARCH) {
        search_size = ZIP_MAX_EOCD_SEARCH;
    }
    unsigned char *tail = malloc(search_size);
    if (!tail ||
        fseek(file, file_size - (long)search_size, SEEK_SET) != 0 ||
        fread(tail, 1, search_size, file) != search_size) {
        free(tail);
        fclose(file);
        return -5;
    }

    size_t eocd_position = search_size;
    for (size_t position = search_size - 22;; --position) {
        if (read_u32(tail + position) == ZIP_EOCD_SIGNATURE) {
            eocd_position = position;
            break;
        }
        if (position == 0) {
            break;
        }
    }
    if (eocd_position == search_size) {
        l_error("Expansion file has no ZIP end-of-central-directory record.");
        free(tail);
        fclose(file);
        return -6;
    }

    const unsigned char *eocd = tail + eocd_position;
    uint16_t entry_count = read_u16(eocd + 10);
    uint32_t central_offset = read_u32(eocd + 16);
    free(tail);

    int package_index = add_package(path, true);
    if (package_index < 0) {
        l_error("Fusion rejected expansion package %s.", path);
        fclose(file);
        return -7;
    }
    if (fseek(file, (long)central_offset, SEEK_SET) != 0) {
        fclose(file);
        return -8;
    }

    unsigned int registered = 0;
    for (uint16_t index = 0; index < entry_count; ++index) {
        unsigned char central[46];
        if (fread(central, 1, sizeof(central), file) != sizeof(central) ||
            read_u32(central) != ZIP_CENTRAL_SIGNATURE) {
            l_error("Invalid OBB central-directory entry %u.", index);
            fclose(file);
            return -9;
        }

        uint16_t method = read_u16(central + 10);
        uint32_t uncompressed_size = read_u32(central + 24);
        uint16_t name_length = read_u16(central + 28);
        uint16_t extra_length = read_u16(central + 30);
        uint16_t comment_length = read_u16(central + 32);
        uint32_t local_offset = read_u32(central + 42);
        if (name_length == 0 || name_length >= 512) {
            l_error("Unsupported OBB filename length at entry %u.", index);
            fclose(file);
            return -10;
        }

        char name[512];
        if (fread(name, 1, name_length, file) != name_length) {
            fclose(file);
            return -11;
        }
        name[name_length] = '\0';
        if (fseek(file, (long)extra_length + comment_length, SEEK_CUR) != 0) {
            fclose(file);
            return -12;
        }

        long next_central = ftell(file);
        if (method != 0) {
            l_error("OBB entry %s uses unsupported ZIP method %u.", name,
                    method);
            fclose(file);
            return -13;
        }

        uint64_t data_offset;
        if (local_data_offset(file, local_offset, &data_offset) < 0) {
            l_error("Could not locate OBB payload for %s.", name);
            fclose(file);
            return -14;
        }
        add_entry((uint32_t)package_index, name, data_offset,
                  (uint64_t)uncompressed_size);
        ++registered;

        if (fseek(file, next_central, SEEK_SET) != 0) {
            fclose(file);
            return -15;
        }
    }

    fclose(file);
    l_success("Registered OBB package %d with %u stored entries.",
              package_index, registered);
    return (int)registered;
}
