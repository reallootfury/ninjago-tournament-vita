/*
 * Copyright (C) 2022-2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See licenses/upstream-MIT.txt for details.
 */

/**
 * @file  settings.h
 * @brief Loader settings that can be set via a configurator app.
 */

#ifndef SOLOADER_SETTINGS_H
#define SOLOADER_SETTINGS_H

#include "stdbool.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Use the D-pad instead of the left analog stick for player movement. */
extern bool setting_dpad_movement;

/* Hide Fusion's virtual combat buttons while physical controls are active. */
extern bool setting_auto_touch_controls;

/* Swap the default Circle=Dodge/L=Block combat pair when enabled. */
extern bool setting_circle_blocks;

void settings_load(void);
void settings_save(void);
void settings_reset(void);

#ifdef __cplusplus
};
#endif

#endif // SOLOADER_SETTINGS_H
