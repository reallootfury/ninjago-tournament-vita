/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2022-2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See licenses/upstream-MIT.txt for details.
 */

#include "settings.h"

#include "utils/logger.h"

#include <stdio.h>
#include <string.h>

#define CONFIG_FILE_PATH DATA_PATH"config.txt"

bool setting_dpad_movement;
bool setting_auto_touch_controls;
bool setting_circle_blocks;

void settings_reset(void) {
    setting_dpad_movement = false;
    setting_auto_touch_controls = true;
    setting_circle_blocks = false;
}

void settings_load(void) {
    settings_reset();

    FILE *config = fopen(CONFIG_FILE_PATH, "r");
    if (!config) {
        return;
    }

    char line[96];
    while (fgets(line, sizeof(line), config)) {
        char key[48];
        int value;
        if (sscanf(line, "%47s %d", key, &value) != 2) {
            continue;
        }
        if (strcmp(key, "dpad_movement") == 0) {
            setting_dpad_movement = value != 0;
        } else if (strcmp(key, "auto_touch_controls") == 0) {
            setting_auto_touch_controls = value != 0;
        } else if (strcmp(key, "circle_blocks") == 0) {
            setting_circle_blocks = value != 0;
        }
    }
    fclose(config);
}

void settings_save(void) {
    FILE *config = fopen(CONFIG_FILE_PATH, "w");
    if (!config) {
        l_warn("Could not save controller settings to %s.",
               CONFIG_FILE_PATH);
        return;
    }

    fprintf(config, "dpad_movement %d\n", setting_dpad_movement ? 1 : 0);
    fprintf(config, "auto_touch_controls %d\n",
            setting_auto_touch_controls ? 1 : 0);
    fprintf(config, "circle_blocks %d\n",
            setting_circle_blocks ? 1 : 0);
    fclose(config);
}
