/*
 * Copyright (C) 2025 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See licenses/upstream-MIT.txt for details.
 */

/**
 * @file  controls.h
 * @brief Implementations for the touch screen, buttons, and other controls.
 */

#ifndef SOLOADER_CONTROLS_H
#define SOLOADER_CONTROLS_H

#include <stdbool.h>
#include <stdint.h>

#define LEFT_ANALOG_DEADZONE  0.16f
#define RIGHT_ANALOG_DEADZONE 0.16f

typedef enum ControlsAction {
    CONTROLS_ACTION_UP = 0,
    CONTROLS_ACTION_DOWN = 1,
    CONTROLS_ACTION_MOVE = 2,
    CONTROLS_ACTION_CANCEL = 3
} ControlsAction;

typedef enum ControlsStickId {
    CONTROLS_STICK_LEFT = 0,
    CONTROLS_STICK_RIGHT = 1
} ControlsStickId;

extern void controls_handler_key(int32_t keycode, ControlsAction action);
extern void controls_handler_touch(int32_t id, float x, float y, ControlsAction action);
extern void controls_handler_analog(ControlsStickId which, float x, float y, ControlsAction action);
extern void controls_handler_exit_request(void);

enum {
    AKEYCODE_BACK = 4,
    AKEYCODE_DPAD_UP = 19,
    AKEYCODE_DPAD_DOWN = 20,
    AKEYCODE_DPAD_LEFT = 21,
    AKEYCODE_DPAD_RIGHT = 22,
    AKEYCODE_DPAD_CENTER = 23,
    AKEYCODE_A = 29,
    AKEYCODE_B = 30,
    AKEYCODE_BUTTON_A = 96,
    AKEYCODE_BUTTON_B = 97,
    AKEYCODE_BUTTON_X = 99,
    AKEYCODE_BUTTON_Y = 100,
    AKEYCODE_BUTTON_L1 = 102,
    AKEYCODE_BUTTON_R1 = 103,
    AKEYCODE_BUTTON_START = 108,
    AKEYCODE_BUTTON_SELECT = 109,
};

typedef struct {
    uint32_t sce_button;
    uint32_t android_button;
} ButtonMapping;

void controls_init();
void controls_poll();
/* Diagnostic sequence only; does not consume or alter native input. */
unsigned int controls_get_special_press_serial(void);

/* One-shot console UI actions consumed by the UI-module adapter. */
typedef enum VitaUIDirection {
    VITA_UI_DIRECTION_NONE = 0,
    VITA_UI_DIRECTION_UP,
    VITA_UI_DIRECTION_DOWN,
    VITA_UI_DIRECTION_LEFT,
    VITA_UI_DIRECTION_RIGHT,
} VitaUIDirection;

VitaUIDirection controls_take_ui_direction(void);
bool controls_take_confirm_press(void);
bool controls_take_cancel_press(void);
int controls_take_character_page_press(void);
bool controls_using_controller(void);

/* One-shot native Edge Menu action consumed by its buttonPressed hook. */
unsigned int controls_take_edge_menu_button(void);
/* Report Cross as TouchUp on the selected native pause-confirmation element. */
bool controls_take_quit_confirmation(void *element);
/* Reapply modal focus after Fusion's HUD animation has updated its scales. */
void controls_refresh_quit_confirmation_focus(void);
/* Let the combat adapter route Square through Select's explicit weapon mode. */
bool controls_use_ranged_attack_mode(void);
/* True while the same active player is holding physical Square in ranged mode. */
bool controls_ranged_attack_held(const void *player);

/* Route Cross only to the custom four-way UI adapter while it owns focus. */
void controls_set_native_confirm_suppressed(bool suppressed);
/* Route Circle only to the Red Brick adapter until its press is released. */
void controls_set_native_cancel_suppressed(bool suppressed);
/* Route physical L/R only to the active menu's explicit page/carousel path. */
void controls_set_native_page_shoulders_suppressed(bool suppressed);
/* Let Character Select own raw stick/D-pad edges through spatial navigation. */
void controls_set_native_ui_navigation_suppressed(bool suppressed);

/* Replaces Android's empty native joypad poll in libLEGO_M1.so. */
void fnaController_Poll_soloader(void *device);

#endif // SOLOADER_CONTROLS_H
