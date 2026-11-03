#include "reimpl/controls.h"

#include "game.h"
#include "utils/logger.h"
#include "utils/settings.h"

#include <math.h>
#include <psp2/ctrl.h>
#include <psp2/kernel/clib.h>
#include <psp2/touch.h>
#include <so_util/so_util.h>
#include <stddef.h>
#include <stdlib.h>

#define FUSION_TOUCH_SLOTS 16
#define STICK_ACTIVITY_THRESHOLD 24
#define MOVEMENT_TOGGLE_BUTTONS \
    (SCE_CTRL_L1 | SCE_CTRL_R1 | SCE_CTRL_SELECT)
#define MAPPED_VITA_BUTTONS \
    (SCE_CTRL_UP | SCE_CTRL_DOWN | SCE_CTRL_LEFT | SCE_CTRL_RIGHT | \
     SCE_CTRL_CROSS | SCE_CTRL_CIRCLE | SCE_CTRL_SQUARE | \
     SCE_CTRL_TRIANGLE | SCE_CTRL_L1 | SCE_CTRL_R1 | \
     SCE_CTRL_START | SCE_CTRL_SELECT)

#define PLAYER_DISABLE_STICK_OFFSET 0x90
#define PLAYER_DISABLE_DPAD_OFFSET 0x91
#define PLAYER_VIRTUAL_DISPLAY_OFFSET 0x670
#define CHARACTER_COMPONENT_DATA_OFFSET 0x164
#define CHARACTER_RANGED_WEAPON_OBJECT_OFFSET 0x1d4
#define CHARACTER_STATE_SYSTEM_OFFSET 0x60
#define CHARACTER_CURRENT_STATE_OFFSET 0x88
#define GRAB_INSTANCE_DATA_OFFSET 0x3f8
#define GRAB_PENDING_FLAGS_OFFSET 0x5c
#define GRAB_THROW_PENDING_BIT 0x01
#define CHARACTER_WEAPON_PREFERENCE_OFFSET 0x3ec
#define AUTO_WEAPON_RESTORE_COUNTER_OFFSET 0x14
#define AUTO_WEAPON_RESTORE_READY_TICKS 40
#define CHARACTER_DODGE_ABILITY 58
#define EDGE_MENU_STATE_OFFSET 0x1c
#define EDGE_MENU_BUTTON_OFFSET 0xe0
#define EDGE_MENU_BUTTON_STRIDE 0x2c
#define EDGE_MENU_BUTTON_COUNT 10
#define EDGE_MENU_BUTTON_ACTIVE_OFFSET 0x00
#define EDGE_MENU_BUTTON_ID_OFFSET 0x18
#define EDGE_MENU_BUTTON_SIZE_OFFSET 0x1c
#define EDGE_MENU_RESUME_BUTTON_ID 3
#define EDGE_MENU_FOCUS_SCALE 1.22f
#define QUIT_CONFIRM_PANEL_ELEMENT_OFFSET 0x2c
#define QUIT_CONFIRM_UNFOCUSED_OPACITY 0.42f
#define EDGE_MENU_STATE_OFF 1
#define EDGE_MENU_STATE_QUIT_CONFIRM 5
#define EDGE_MENU_STATE_ON 6
#define EDGE_MENU_STATE_PAUSE_REQUEST 7

typedef void (*FusionBoolMethod)(void *self, bool enabled);
typedef uint8_t (*FusionGetPauseMenuMethod)(void *self);
typedef void *(*FusionGetCharacterDataMethod)(void *game_object);
typedef bool (*FusionAttemptGrabMethod)(void *game_object,
                                        void *character_data,
                                        bool test_only);
typedef bool (*FusionIsGrapplingMethod)(const void *game_object);
typedef bool (*FusionEdgeMenuCanPauseMethod)(void *self);
typedef void (*FusionEdgeMenuSetStateMethod)(void *self,
                                             unsigned int state);
typedef bool (*FusionIsWeaponDrawnMethod)(void *character_data,
                                          unsigned int weapon);
typedef void (*FusionEnableWeaponMethod)(void *game_object,
                                         unsigned int weapon,
                                         bool enabled, bool secondary);
typedef void (*FusionStateEnterMethod)(void *state, void *game_object);
typedef bool (*FusionHasAbilityMethod)(const void *character_data,
                                       unsigned int ability);
typedef void (*FusionDodgeStartMethod)(void *game_object, bool held);
typedef const float *(*FusionFlashGetVec2Method)(void *element);
typedef const float *(*FusionFlashGetWorldMatrixMethod)(void *element);
typedef void (*FusionFlashSetVec2Method)(void *element, const float *value);
typedef float (*FusionFlashGetFloatMethod)(void *element);
typedef void (*FusionFlashSetFloatMethod)(void *element, float value);

/*
 * fnaController_CreateDevice allocates 24 of these 0x14-byte records.  Its
 * fnInput_Poll path clears value/click state, calls fnaController_Poll, then
 * applies dead zones and derives clicked/released transitions.  Android ships
 * fnaController_Poll as an empty "bx lr", so the loader only has to populate
 * the current value at offset zero and let Fusion do the rest.
 */
typedef struct FusionInputControl {
    float value;
    uint32_t type;
    float deadzone;
    float previous_value;
    int16_t clicked;
    int16_t released;
} FusionInputControl;

typedef struct FusionInputDevice {
    uint32_t flags;
    uint32_t type;
    uint32_t index;
    uint32_t reserved;
    uint32_t control_count;
    FusionInputControl *controls;
} FusionInputDevice;

_Static_assert(sizeof(FusionInputControl) == 0x14,
               "Fusion input-control ABI changed");
_Static_assert(offsetof(FusionInputDevice, controls) == 0x14,
               "Fusion input-device ABI changed");

typedef struct FusionControlIndices {
    uint32_t left_stick_x;
    uint32_t left_stick_y;
    uint32_t right_stick_x;
    uint32_t right_stick_y;
    uint32_t dpad_up;
    uint32_t dpad_down;
    uint32_t dpad_left;
    uint32_t dpad_right;
    uint32_t pad_south;
    uint32_t pad_east;
    uint32_t pad_west;
    uint32_t pad_north;
    uint32_t left_shoulder;
    uint32_t right_shoulder;
    uint32_t start;
    uint32_t select;
} FusionControlIndices;

typedef struct FusionControlBinding {
    const char *symbol;
    uint32_t expected;
    uint32_t *destination;
} FusionControlBinding;

extern so_module so_mod;

static SceTouchData previous_touch;
static int8_t direct_touch_slots[UINT8_MAX + 1];
static SceCtrlData current_pad;
static FusionControlIndices fusion_controls;
static bool controller_ready;
static bool controller_sample_reported;
static bool controller_hook_reported;
static bool controller_symbols_validated;
static uint32_t previous_logged_buttons;
static uint32_t previous_pad_buttons;
static uint32_t controller_pressed_buttons;
static uint32_t controller_suppressed_buttons;
static unsigned int special_press_serial;
static unsigned int controller_change_reports;
static unsigned int dodge_delivery_reports;
static unsigned int dodge_direct_reports;
static unsigned int dodge_reject_reports;
static unsigned int block_delivery_reports;
static unsigned int secondary_weapon_reports;
static unsigned int secondary_preference_restore_reports;
static unsigned int weapon_animation_refresh_reports;
static uint32_t *fusion_cancel_control;
static uint32_t *fusion_confirm_control;
static void *fusion_game_loop;
static FusionGetPauseMenuMethod fusion_get_pause_menu;
static void **fusion_active_player;
static FusionGetCharacterDataMethod fusion_get_character_data;
static FusionAttemptGrabMethod fusion_attempt_grab;
static FusionIsGrapplingMethod fusion_is_grappling;
static void **fusion_edge_menu_system;
static FusionEdgeMenuCanPauseMethod fusion_edge_menu_can_pause;
static FusionEdgeMenuSetStateMethod fusion_edge_menu_set_state;
static uint8_t *fusion_hud_quit_panel_showing;
static uint8_t *fusion_quit_confirm_panel_yes;
static uint8_t *fusion_quit_confirm_panel_no;
static FusionFlashGetVec2Method fusion_flash_get_scale;
static FusionFlashGetWorldMatrixMethod fusion_flash_get_world_matrix;
static FusionFlashSetVec2Method fusion_flash_set_scale;
static FusionFlashGetFloatMethod fusion_flash_get_opacity;
static FusionFlashSetFloatMethod fusion_flash_set_opacity;
static FusionIsWeaponDrawnMethod fusion_is_weapon_drawn;
static FusionEnableWeaponMethod fusion_enable_weapon;
static FusionHasAbilityMethod fusion_has_ability;
static FusionDodgeStartMethod fusion_dodge_start;
static uint8_t *fusion_auto_weapon_holster_control_system;
static void *secondary_preference_player;
static bool secondary_ranged_preferred;
static uint8_t *fusion_player_control_system;
static FusionBoolMethod fusion_disable_stick_control;
static FusionBoolMethod fusion_disable_dpad_control;
static FusionBoolMethod fusion_set_virtual_control_display;
static bool movement_toggle_suppressed;
static bool movement_mode_reported;
static bool reported_dpad_movement;
static bool touch_ui_desired_visible;
static bool touch_ui_transition_pending;
static bool controller_is_active;
static VitaUIDirection queued_ui_direction;
static VitaUIDirection previous_stick_ui_direction;
static bool native_confirm_suppressed;
static bool native_confirm_release_latched;
static bool native_cancel_suppressed;
static bool native_cancel_release_latched;
static bool native_page_shoulders_suppressed;
static bool native_page_shoulders_release_latched;
static bool native_ui_navigation_suppressed;
static bool native_ui_navigation_release_latched;
static bool triangle_action_latched;
static bool dodge_action_latched;
static bool edge_menu_was_active;
static bool edge_menu_was_in_use;
static int edge_menu_focus = -1;
static unsigned int edge_menu_forced_button;
static float edge_menu_base_size[EDGE_MENU_BUTTON_COUNT][2];
static bool edge_menu_size_valid[EDGE_MENU_BUTTON_COUNT];
static bool quit_confirmation_was_active;
static int quit_confirmation_focus;
static void *quit_confirmation_element[2];
static float quit_confirmation_base_opacity[2];
static bool quit_confirmation_opacity_valid[2];
static void *quit_confirmation_forced_element;

enum {
    QUIT_CONFIRM_NO = 0,
    QUIT_CONFIRM_YES = 1,
    QUIT_CONFIRM_COUNT = 2,
};

static const size_t virtual_control_show_stream_offsets[] = {
    0x634, 0x6b0, 0x704, 0x758, 0x7ac,
};

static const size_t virtual_control_hide_stream_offsets[] = {
    0x638, 0x6b4, 0x708, 0x75c, 0x7b0,
};

static const FusionControlBinding fusion_control_bindings[] = {
    {"Controls_LeftStickX", 0, &fusion_controls.left_stick_x},
    {"Controls_LeftStickY", 1, &fusion_controls.left_stick_y},
    {"Controls_RightStickX", 2, &fusion_controls.right_stick_x},
    {"Controls_RightStickY", 3, &fusion_controls.right_stick_y},
    {"Controls_DPadUp", 12, &fusion_controls.dpad_up},
    {"Controls_DPadDown", 13, &fusion_controls.dpad_down},
    {"Controls_DPadLeft", 14, &fusion_controls.dpad_left},
    {"Controls_DPadRight", 15, &fusion_controls.dpad_right},
    {"Controls_PadSouth", 18, &fusion_controls.pad_south},
    {"Controls_PadEast", 17, &fusion_controls.pad_east},
    {"Controls_PadWest", 19, &fusion_controls.pad_west},
    {"Controls_PadNorth", 16, &fusion_controls.pad_north},
    {"Controls_LeftShoulder", 8, &fusion_controls.left_shoulder},
    {"Controls_RightShoulder", 10, &fusion_controls.right_shoulder},
    {"Controls_Start", 6, &fusion_controls.start},
    {"Controls_Select", 7, &fusion_controls.select},
};

static int find_touch(const SceTouchData *data, uint8_t id) {
    for (int index = 0; index < data->reportNum; ++index) {
        if (data->report[index].id == id) {
            return index;
        }
    }
    return -1;
}

static int allocate_direct_touch_slot(uint8_t vita_id) {
    int existing = direct_touch_slots[vita_id];
    if (existing >= 0) {
        return existing;
    }

    bool used[FUSION_TOUCH_SLOTS] = {false};
    for (int index = 0; index <= UINT8_MAX; ++index) {
        int slot = direct_touch_slots[index];
        if (slot >= 0 && slot < FUSION_TOUCH_SLOTS) {
            used[slot] = true;
        }
    }
    for (int slot = 0; slot < FUSION_TOUCH_SLOTS; ++slot) {
        if (!used[slot]) {
            direct_touch_slots[vita_id] = (int8_t)slot;
            return slot;
        }
    }
    return -1;
}

static void release_direct_touch_slot(uint8_t vita_id) {
    direct_touch_slots[vita_id] = -1;
}

static bool resolve_player_control_helpers(void) {
    fusion_player_control_system = (uint8_t *)so_symbol(
        &so_mod, "lePlayerControlSystem");
    fusion_disable_stick_control = (FusionBoolMethod)so_symbol(
        &so_mod,
        "_ZN21LEPLAYERCONTROLSYSTEM19disableStickControlEb");
    fusion_disable_dpad_control = (FusionBoolMethod)so_symbol(
        &so_mod,
        "_ZN21LEPLAYERCONTROLSYSTEM18disableDpadControlEb");
    fusion_set_virtual_control_display = (FusionBoolMethod)so_symbol(
        &so_mod,
        "_ZN21LEPLAYERCONTROLSYSTEM24setVirtualControlDisplayEb");
    if (!fusion_player_control_system || !fusion_disable_stick_control ||
        !fusion_disable_dpad_control ||
        !fusion_set_virtual_control_display) {
        l_error("Could not resolve the pinned 1057 player-control helpers; "
                "movement-mode selection and automatic touch HUD switching "
                "are disabled.");
        return false;
    }
    return true;
}

static void apply_movement_mode(bool report) {
    if (!fusion_player_control_system || !fusion_disable_stick_control ||
        !fusion_disable_dpad_control) {
        return;
    }

    bool disable_stick = setting_dpad_movement;
    bool disable_dpad = !setting_dpad_movement;
    if (fusion_player_control_system[PLAYER_DISABLE_STICK_OFFSET] !=
        (uint8_t)disable_stick) {
        fusion_disable_stick_control(fusion_player_control_system,
                                     disable_stick);
    }
    if (fusion_player_control_system[PLAYER_DISABLE_DPAD_OFFSET] !=
        (uint8_t)disable_dpad) {
        fusion_disable_dpad_control(fusion_player_control_system,
                                    disable_dpad);
    }

    if (report &&
        (!movement_mode_reported ||
         reported_dpad_movement != setting_dpad_movement)) {
        l_success("Player movement set to %s; press L+R+Select together "
                  "to switch modes.",
                  setting_dpad_movement ? "D-pad" : "left analog stick");
        movement_mode_reported = true;
        reported_dpad_movement = setting_dpad_movement;
    }
}

static bool virtual_control_streams_ready(bool visible) {
    if (!fusion_player_control_system ||
        !fusion_set_virtual_control_display) {
        return false;
    }

    const size_t *offsets = visible
        ? virtual_control_show_stream_offsets
        : virtual_control_hide_stream_offsets;
    size_t count = visible
        ? sizeof(virtual_control_show_stream_offsets) /
              sizeof(virtual_control_show_stream_offsets[0])
        : sizeof(virtual_control_hide_stream_offsets) /
              sizeof(virtual_control_hide_stream_offsets[0]);
    for (size_t i = 0; i < count; ++i) {
        if (!*(void **)(fusion_player_control_system + offsets[i])) {
            return false;
        }
    }
    return true;
}

static void select_input_source(bool touch) {
    if (!setting_auto_touch_controls ||
        touch_ui_desired_visible == touch) {
        return;
    }
    touch_ui_desired_visible = touch;
    touch_ui_transition_pending = true;
}

static void update_virtual_control_display(void) {
    if (!setting_auto_touch_controls || !fusion_player_control_system ||
        !fusion_set_virtual_control_display) {
        return;
    }

    bool currently_visible =
        fusion_player_control_system[PLAYER_VIRTUAL_DISPLAY_OFFSET] != 0;
    bool report_transition = touch_ui_transition_pending;
    bool needs_update = touch_ui_transition_pending ||
                        (!touch_ui_desired_visible && currently_visible);
    if (!needs_update ||
        !virtual_control_streams_ready(touch_ui_desired_visible)) {
        return;
    }

    fusion_set_virtual_control_display(fusion_player_control_system,
                                       touch_ui_desired_visible);
    touch_ui_transition_pending = false;
    if (report_transition) {
        l_info("Input source switched to %s; virtual combat controls %s.",
               touch_ui_desired_visible ? "front touch"
                                        : "Vita controller",
               touch_ui_desired_visible ? "shown" : "hidden");
    }
}

void controls_init(void) {
    sceCtrlSetSamplingModeExt(SCE_CTRL_MODE_ANALOG_WIDE);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, 1);
    sceClibMemset(&previous_touch, 0, sizeof(previous_touch));
    sceClibMemset(&current_pad, 0, sizeof(current_pad));
    sceClibMemset(direct_touch_slots, -1, sizeof(direct_touch_slots));
    controller_sample_reported = false;
    controller_hook_reported = false;
    controller_symbols_validated = false;
    previous_logged_buttons = 0;
    previous_pad_buttons = 0;
    controller_pressed_buttons = 0;
    controller_suppressed_buttons = 0;
    controller_change_reports = 0;
    dodge_delivery_reports = 0;
    dodge_direct_reports = 0;
    dodge_reject_reports = 0;
    block_delivery_reports = 0;
    secondary_weapon_reports = 0;
    secondary_preference_restore_reports = 0;
    weapon_animation_refresh_reports = 0;
    movement_toggle_suppressed = false;
    movement_mode_reported = false;
    touch_ui_desired_visible = true;
    touch_ui_transition_pending = false;
    controller_is_active = false;
    queued_ui_direction = VITA_UI_DIRECTION_NONE;
    previous_stick_ui_direction = VITA_UI_DIRECTION_NONE;
    native_confirm_suppressed = false;
    native_confirm_release_latched = false;
    native_cancel_suppressed = false;
    native_cancel_release_latched = false;
    native_page_shoulders_suppressed = false;
    native_page_shoulders_release_latched = false;
    native_ui_navigation_suppressed = false;
    native_ui_navigation_release_latched = false;
    triangle_action_latched = false;
    dodge_action_latched = false;
    edge_menu_was_active = false;
    edge_menu_was_in_use = false;
    edge_menu_focus = -1;
    edge_menu_forced_button = 0;
    secondary_preference_player = NULL;
    secondary_ranged_preferred = false;
    quit_confirmation_was_active = false;
    quit_confirmation_focus = QUIT_CONFIRM_NO;
    quit_confirmation_forced_element = NULL;
    sceClibMemset(quit_confirmation_element, 0,
                  sizeof(quit_confirmation_element));
    sceClibMemset(quit_confirmation_base_opacity, 0,
                  sizeof(quit_confirmation_base_opacity));
    sceClibMemset(quit_confirmation_opacity_valid, 0,
                  sizeof(quit_confirmation_opacity_valid));
    sceClibMemset(edge_menu_base_size, 0, sizeof(edge_menu_base_size));
    sceClibMemset(edge_menu_size_valid, 0,
                  sizeof(edge_menu_size_valid));

    settings_load();
    resolve_player_control_helpers();
    apply_movement_mode(true);

    fusion_cancel_control = (uint32_t *)so_symbol(
        &so_mod, "Controls_Cancel");
    fusion_confirm_control = (uint32_t *)so_symbol(
        &so_mod, "Controls_Confirm");
    fusion_game_loop = (void *)so_symbol(&so_mod, "GameLoop");
    fusion_get_pause_menu = (FusionGetPauseMenuMethod)so_symbol(
        &so_mod, "_ZN14GameLoopModule12GetPauseMenuEv");
    fusion_active_player = (void **)so_symbol(
        &so_mod, "GOPlayer_Active");
    fusion_get_character_data = (FusionGetCharacterDataMethod)so_symbol(
        &so_mod, "_Z15GOCharacterDataP12GEGAMEOBJECT");
    fusion_attempt_grab = (FusionAttemptGrabMethod)so_symbol(
        &so_mod,
        "_Z20GOCSGrab_AttemptGrabP12GEGAMEOBJECTP15GOCHARACTERDATAb");
    fusion_is_grappling = (FusionIsGrapplingMethod)so_symbol(
        &so_mod, "_Z20GOCSGrab_IsGrapplingPK12GEGAMEOBJECT");
    fusion_edge_menu_system = (void **)so_symbol(
        &so_mod, "pEdgeMenuSystem");
    fusion_edge_menu_can_pause = (FusionEdgeMenuCanPauseMethod)so_symbol(
        &so_mod, "_ZN14EDGEMENUSYSTEM15canGameBePausedEv");
    fusion_edge_menu_set_state = (FusionEdgeMenuSetStateMethod)so_symbol(
        &so_mod, "_ZN14EDGEMENUSYSTEM8SetStateENS_6_stateE");
    fusion_hud_quit_panel_showing = (uint8_t *)so_symbol(
        &so_mod, "HUD_QuitPanelShowing");
    fusion_quit_confirm_panel_yes = (uint8_t *)so_symbol(
        &so_mod, "m_quitConfirmPanelYes");
    fusion_quit_confirm_panel_no = (uint8_t *)so_symbol(
        &so_mod, "m_quitConfirmPanelNo");
    fusion_flash_get_scale = (FusionFlashGetVec2Method)so_symbol(
        &so_mod, "_Z23fnFlashElement_GetScaleP14fnFLASHELEMENT");
    fusion_flash_get_world_matrix =
        (FusionFlashGetWorldMatrixMethod)so_symbol(
            &so_mod,
            "_Z29fnFlashElement_GetWorldMatrixP14fnFLASHELEMENT");
    fusion_flash_set_scale = (FusionFlashSetVec2Method)so_symbol(
        &so_mod,
        "_Z23fnFlashElement_SetScaleP14fnFLASHELEMENTPK7f32vec2");
    fusion_flash_get_opacity = (FusionFlashGetFloatMethod)so_symbol(
        &so_mod, "_Z25fnFlashElement_GetOpacityP14fnFLASHELEMENT");
    fusion_flash_set_opacity = (FusionFlashSetFloatMethod)so_symbol(
        &so_mod, "_Z25fnFlashElement_SetOpacityP14fnFLASHELEMENTf");
    fusion_is_weapon_drawn = (FusionIsWeaponDrawnMethod)so_symbol(
        &so_mod,
        "_Z27leGOCharacter_IsWeaponDrawnP15GOCHARACTERDATA19GOCHARACTER_WEAPONS");
    fusion_enable_weapon = (FusionEnableWeaponMethod)so_symbol(
        &so_mod,
        "_Z24GOCharacter_EnableWeaponP12GEGAMEOBJECT19GOCHARACTER_WEAPONSbb");
    fusion_has_ability = (FusionHasAbilityMethod)so_symbol(
        &so_mod, "_Z22GOCharacter_HasAbilityPK15GOCHARACTERDATAj");
    fusion_dodge_start = (FusionDodgeStartMethod)so_symbol(
        &so_mod, "_Z15GOCSDodge_StartP12GEGAMEOBJECTb");
    fusion_auto_weapon_holster_control_system = (uint8_t *)so_symbol(
        &so_mod, "AutoWeaponHolsterControlSystem");
    if (!fusion_cancel_control || !fusion_confirm_control) {
        l_error("Controller binding is missing Fusion symbol "
                "Controls_Confirm or Controls_Cancel.");
        return;
    }
    if (!fusion_game_loop || !fusion_get_pause_menu) {
        l_error("Controller binding could not resolve the gameplay pause "
                "menu state; Circle/Select native Back fallback is disabled.");
    }
    if (!fusion_active_player || !fusion_get_character_data ||
        !fusion_attempt_grab || !fusion_is_grappling) {
        l_error("Controller binding could not resolve the native grab path; "
                "Triangle retains only its throw/ranged action.");
    } else {
        l_success("Resolved the active-player grab path for context-sensitive "
                  "Triangle grab/throw input.");
    }
    if (!fusion_edge_menu_system || !fusion_edge_menu_can_pause ||
        !fusion_edge_menu_set_state) {
        l_error("Controller binding could not resolve Fusion's native "
                "in-fight Edge Menu; Start pause is disabled.");
    } else {
        l_success("Resolved Fusion's native in-fight Edge Menu for Start, "
                  "D-pad, Cross, and Circle control.");
    }
    if (!fusion_hud_quit_panel_showing ||
        !fusion_quit_confirm_panel_yes ||
        !fusion_quit_confirm_panel_no) {
        l_error("Could not resolve Fusion's pause-quit Yes/No panels; "
                "Circle Back remains available but controller confirmation "
                "is disabled.");
    } else {
        l_success("Resolved Fusion's native pause-quit Yes/No panels for "
                  "D-pad, Cross, and Circle control.");
        if (!fusion_flash_get_opacity || !fusion_flash_set_opacity) {
            l_warn("Pause-quit opacity helpers are unavailable; its D-pad "
                   "focus will use scale only.");
        }
    }
    if (fusion_get_character_data && fusion_is_weapon_drawn &&
        fusion_enable_weapon &&
        fusion_auto_weapon_holster_control_system) {
        l_success("Resolved Fusion's ranged-weapon object and persistent "
                  "melee/ranged auto-holster path for safe Select "
                  "switching.");
    } else {
        l_error("Controller binding could not resolve the safe secondary-"
                "weapon path; Select will remain inactive.");
    }
    if (fusion_has_ability && fusion_dodge_start) {
        l_success("Resolved Fusion's guarded native dodge start path for "
                  "reliable Circle delivery.");
    } else {
        l_error("Controller binding could not resolve Fusion's native dodge "
                "start path; Circle retains the joypad fallback.");
    }

    for (size_t i = 0;
         i < sizeof(fusion_control_bindings) /
                 sizeof(fusion_control_bindings[0]);
         ++i) {
        const FusionControlBinding *binding = &fusion_control_bindings[i];
        uintptr_t address = so_symbol(&so_mod, binding->symbol);
        if (!address) {
            l_error("Controller binding is missing Fusion symbol %s.",
                    binding->symbol);
            return;
        }
        /* geControls_Init fills these globals during the first game update. */
        *binding->destination = binding->expected;
    }

    controller_ready = true;
    l_success("Native Fusion joypad enabled: left stick=move, right "
              "stick=camera, Cross=jump/confirm, Square=attack, "
              "%s, Triangle=grab/throw, R=special, Select draws or stows a "
              "character's secondary weapon, Start=pause, and D-pad=menu "
              "navigation or movement. L+R+Select switches movement mode.",
              setting_circle_blocks
                  ? "Circle=block/cancel and L=dodge"
                  : "Circle=dodge/cancel and L=block");
}

static void validate_controller_symbols(void) {
    if (controller_symbols_validated) {
        return;
    }

    for (size_t i = 0;
         i < sizeof(fusion_control_bindings) /
                 sizeof(fusion_control_bindings[0]);
         ++i) {
        const FusionControlBinding *binding = &fusion_control_bindings[i];
        uintptr_t address = so_symbol(&so_mod, binding->symbol);
        uint32_t index = address ? *(const uint32_t *)address : UINT32_MAX;
        if (index != binding->expected) {
            l_warn("Fusion initialized %s as %u; the pinned 1057 table uses "
                   "%u.", binding->symbol, index, binding->expected);
        }
    }
    uint32_t original_cancel = *fusion_cancel_control;
    uint32_t original_confirm = *fusion_confirm_control;
    uint32_t configured_cancel = setting_circle_blocks
        ? fusion_controls.select
        : fusion_controls.right_shoulder;
    *fusion_confirm_control = fusion_controls.pad_south;
    *fusion_cancel_control = configured_cancel;
    controller_symbols_validated = true;
    l_success("Validated the pinned 1057 Fusion controller table after "
              "geControls_Init.");
    l_success("Redirected Fusion menu Cancel from control %u to control %u "
              "so Circle serves as Back/Cancel as well as its configured "
              "combat action.",
              original_cancel, configured_cancel);
    l_success("Redirected Fusion menu Confirm from control %u to control %u "
              "so Cross activates the focused UI action.",
              original_confirm, fusion_controls.pad_south);
}

static bool poll_front_touch(void) {
    SceTouchData current;
    if (sceTouchPeek(SCE_TOUCH_PORT_FRONT, &current, 1) <= 0) {
        return false;
    }

    bool first_new_contact = previous_touch.reportNum == 0;
    for (int index = 0; index < current.reportNum; ++index) {
        const SceTouchReport *report = &current.report[index];
        float x = (float)report->x * 960.0f / 1920.0f;
        float y = (float)report->y * 544.0f / 1088.0f;
        int old_index = find_touch(&previous_touch, report->id);
        int slot = direct_touch_slots[report->id];
        if (old_index < 0) {
            slot = allocate_direct_touch_slot(report->id);
            if (slot < 0) {
                l_warn("No free Fusion touch slot for Vita touch ID %u.",
                       (unsigned int)report->id);
                continue;
            }
            ninjago_game_touch_down(slot, x, y, 1.0f,
                                     first_new_contact);
            first_new_contact = false;
        } else if (report->x != previous_touch.report[old_index].x ||
                   report->y != previous_touch.report[old_index].y) {
            if (slot >= 0) {
                ninjago_game_touch_move(slot, x, y, 1.0f);
            }
        }
    }

    for (int index = 0; index < previous_touch.reportNum; ++index) {
        const SceTouchReport *report = &previous_touch.report[index];
        if (find_touch(&current, report->id) >= 0) {
            continue;
        }
        float x = (float)report->x * 960.0f / 1920.0f;
        float y = (float)report->y * 544.0f / 1088.0f;
        int slot = direct_touch_slots[report->id];
        if (slot >= 0) {
            ninjago_game_touch_up(slot, x, y, 0.0f,
                                  current.reportNum == 0);
            release_direct_touch_slot(report->id);
        }
    }
    sceClibMemcpy(&previous_touch, &current, sizeof(current));
    return current.reportNum > 0;
}

static float stick_axis(uint8_t value) {
    int centered = (int)value - 128;
    return centered >= 0 ? (float)centered / 127.0f
                         : (float)centered / 128.0f;
}

static bool stick_is_active(uint8_t value) {
    int delta = (int)value - 128;
    return delta < -STICK_ACTIVITY_THRESHOLD ||
           delta > STICK_ACTIVITY_THRESHOLD;
}

static bool poll_controller(void) {
    int samples = sceCtrlPeekBufferPositiveExt2(0, &current_pad, 1);
    if (samples <= 0) {
        sceClibMemset(&current_pad, 0, sizeof(current_pad));
        current_pad.lx = 128;
        current_pad.ly = 128;
        current_pad.rx = 128;
        current_pad.ry = 128;
        controller_pressed_buttons = 0;
        controller_suppressed_buttons = 0;
        previous_pad_buttons = 0;
        native_confirm_release_latched = false;
        native_cancel_release_latched = false;
        native_page_shoulders_release_latched = false;
        native_ui_navigation_release_latched = false;
        triangle_action_latched = false;
        dodge_action_latched = false;
        queued_ui_direction = VITA_UI_DIRECTION_NONE;
        previous_stick_ui_direction = VITA_UI_DIRECTION_NONE;
        return false;
    }

    /* A UI direction is valid only during the frame that produced its edge. */
    queued_ui_direction = VITA_UI_DIRECTION_NONE;
    controller_pressed_buttons = current_pad.buttons &
                                 ~previous_pad_buttons;
    controller_suppressed_buttons = 0;
    previous_pad_buttons = current_pad.buttons;

    /* Keep directly dispatched actions out of Fusion until button release. */
    if ((current_pad.buttons & SCE_CTRL_TRIANGLE) == 0) {
        triangle_action_latched = false;
    } else if (triangle_action_latched) {
        controller_suppressed_buttons |= SCE_CTRL_TRIANGLE;
    }
    if ((current_pad.buttons & SCE_CTRL_CIRCLE) == 0) {
        dodge_action_latched = false;
    } else if (dodge_action_latched) {
        controller_suppressed_buttons |= SCE_CTRL_CIRCLE;
    }
    if ((current_pad.buttons & SCE_CTRL_CROSS) == 0) {
        native_confirm_release_latched = false;
    }
    if ((current_pad.buttons & SCE_CTRL_CIRCLE) == 0) {
        native_cancel_release_latched = false;
    }
    if ((current_pad.buttons & (SCE_CTRL_L1 | SCE_CTRL_R1)) == 0) {
        native_page_shoulders_release_latched = false;
    }
    if ((current_pad.buttons &
         (SCE_CTRL_UP | SCE_CTRL_DOWN | SCE_CTRL_LEFT | SCE_CTRL_RIGHT)) == 0 &&
        !stick_is_active(current_pad.lx) &&
        !stick_is_active(current_pad.ly)) {
        native_ui_navigation_release_latched = false;
    }

    /*
     * Fusion's Android menu helper flattens every screen into a Left/Right
     * list. Preserve a single four-way edge here so the UI adapter can choose
     * the nearest visible item in the requested screen-space direction. The
     * stick uses hysteresis: it must return near center before another edge.
     */
    VitaUIDirection physical_direction = VITA_UI_DIRECTION_NONE;
    if (controller_pressed_buttons & SCE_CTRL_UP) {
        physical_direction = VITA_UI_DIRECTION_UP;
    } else if (controller_pressed_buttons & SCE_CTRL_DOWN) {
        physical_direction = VITA_UI_DIRECTION_DOWN;
    } else if (controller_pressed_buttons & SCE_CTRL_LEFT) {
        physical_direction = VITA_UI_DIRECTION_LEFT;
    } else if (controller_pressed_buttons & SCE_CTRL_RIGHT) {
        physical_direction = VITA_UI_DIRECTION_RIGHT;
    }

    int stick_x = (int)current_pad.lx - 128;
    int stick_y = (int)current_pad.ly - 128;
    VitaUIDirection stick_direction = VITA_UI_DIRECTION_NONE;
    if (abs(stick_x) >= 64 || abs(stick_y) >= 64) {
        if (abs(stick_x) > abs(stick_y)) {
            stick_direction = stick_x < 0 ? VITA_UI_DIRECTION_LEFT
                                          : VITA_UI_DIRECTION_RIGHT;
        } else {
            stick_direction = stick_y < 0 ? VITA_UI_DIRECTION_UP
                                          : VITA_UI_DIRECTION_DOWN;
        }
    }
    if (physical_direction != VITA_UI_DIRECTION_NONE) {
        queued_ui_direction = physical_direction;
    } else if (stick_direction != VITA_UI_DIRECTION_NONE &&
               stick_direction != previous_stick_ui_direction) {
        queued_ui_direction = stick_direction;
    }
    previous_stick_ui_direction = stick_direction;

    if (!controller_sample_reported) {
        l_info("First Vita controller sample: buttons=0x%08X "
               "left=(%u,%u) right=(%u,%u).",
               current_pad.buttons, current_pad.lx, current_pad.ly,
               current_pad.rx, current_pad.ry);
        controller_sample_reported = true;
        previous_logged_buttons = current_pad.buttons;
    } else if (current_pad.buttons != previous_logged_buttons &&
               controller_change_reports < 24) {
        l_info("Vita controller transition %u: 0x%08X -> 0x%08X.",
               ++controller_change_reports, previous_logged_buttons,
               current_pad.buttons);
        previous_logged_buttons = current_pad.buttons;
    }

    uint32_t toggle_buttons = current_pad.buttons &
                              MOVEMENT_TOGGLE_BUTTONS;
    if (!movement_toggle_suppressed &&
        toggle_buttons == MOVEMENT_TOGGLE_BUTTONS) {
        movement_toggle_suppressed = true;
        setting_dpad_movement = !setting_dpad_movement;
        settings_save();
        apply_movement_mode(true);
    } else if (movement_toggle_suppressed && toggle_buttons == 0) {
        movement_toggle_suppressed = false;
    }

    return (current_pad.buttons & MAPPED_VITA_BUTTONS) != 0 ||
           stick_is_active(current_pad.lx) ||
           stick_is_active(current_pad.ly) ||
           stick_is_active(current_pad.rx) ||
           stick_is_active(current_pad.ry);
}

static void set_control(FusionInputDevice *device, uint32_t index,
                        float value) {
    if (index < device->control_count) {
        device->controls[index].value = value;
    }
}

static void set_buttons(FusionInputDevice *device, uint32_t index,
                        uint32_t vita_buttons) {
    uint32_t buttons = current_pad.buttons &
                       ~controller_suppressed_buttons;
    if (movement_toggle_suppressed) {
        buttons &= ~MOVEMENT_TOGGLE_BUTTONS;
    }
    set_control(device, index,
                (buttons & vita_buttons) ? 1.0f : 0.0f);
}

static void set_button(FusionInputDevice *device, uint32_t index,
                       uint32_t vita_button) {
    set_buttons(device, index, vita_button);
}

void fnaController_Poll_soloader(void *opaque_device) {
    FusionInputDevice *device = (FusionInputDevice *)opaque_device;
    if (!controller_ready || !device || device->type != 1 ||
        device->control_count < 20 || !device->controls) {
        return;
    }

    validate_controller_symbols();

    if (!controller_hook_reported) {
        l_info("Fusion controller poll reached Vita hook: device=%p "
               "type=%u controls=%u buttons=0x%08X.",
               device, device->type, device->control_count,
               current_pad.buttons);
        controller_hook_reported = true;
    }

    bool suppress_ui_navigation = native_ui_navigation_suppressed ||
        native_ui_navigation_release_latched;
    set_control(device, fusion_controls.left_stick_x,
                suppress_ui_navigation ? 0.0f : stick_axis(current_pad.lx));
    set_control(device, fusion_controls.left_stick_y,
                suppress_ui_navigation ? 0.0f : -stick_axis(current_pad.ly));
    set_control(device, fusion_controls.right_stick_x,
                stick_axis(current_pad.rx));
    set_control(device, fusion_controls.right_stick_y,
                -stick_axis(current_pad.ry));

    if (suppress_ui_navigation) {
        set_control(device, fusion_controls.dpad_up, 0.0f);
        set_control(device, fusion_controls.dpad_down, 0.0f);
        set_control(device, fusion_controls.dpad_left, 0.0f);
        set_control(device, fusion_controls.dpad_right, 0.0f);
    } else {
        set_button(device, fusion_controls.dpad_up, SCE_CTRL_UP);
        set_button(device, fusion_controls.dpad_down, SCE_CTRL_DOWN);
        set_button(device, fusion_controls.dpad_left, SCE_CTRL_LEFT);
        set_button(device, fusion_controls.dpad_right, SCE_CTRL_RIGHT);
    }

    /* Map physical buttons to the actions used by this game's combat code. */
    if (native_confirm_suppressed || native_confirm_release_latched) {
        set_control(device, fusion_controls.pad_south, 0.0f);
    } else {
        set_button(device, fusion_controls.pad_south, SCE_CTRL_CROSS);
    }
    bool suppress_cancel = native_cancel_suppressed ||
                           native_cancel_release_latched;
    bool suppress_page_shoulders = native_page_shoulders_suppressed ||
        native_page_shoulders_release_latched;
    /* PadEast did not perform a combat action in the hardware trace. */
    set_control(device, fusion_controls.pad_east, 0.0f);

    set_button(device, fusion_controls.pad_west, SCE_CTRL_SQUARE);
    set_button(device, fusion_controls.pad_north, SCE_CTRL_TRIANGLE);
    /* Ext2 reports the Vita shoulders as L1/R1, not the legacy trigger bits. */
    if (suppress_page_shoulders) {
        set_control(device, fusion_controls.left_shoulder, 0.0f);
    } else {
        set_button(device, fusion_controls.left_shoulder, SCE_CTRL_R1);
    }
    uint32_t block_buttons = 0;
    if (setting_circle_blocks) {
        if (!suppress_cancel) {
            block_buttons |= SCE_CTRL_CIRCLE;
        }
    } else if (!suppress_page_shoulders) {
        block_buttons |= SCE_CTRL_L1;
    }
    set_buttons(device, fusion_controls.select, block_buttons);
    if (!setting_circle_blocks && !suppress_page_shoulders &&
        (controller_pressed_buttons & SCE_CTRL_L1) != 0 &&
        block_delivery_reports < 8) {
        ++block_delivery_reports;
        l_info("L edge delivered to Fusion Select control %u, the held "
               "Block action confirmed by hardware.",
               fusion_controls.select);
    }

    if ((setting_circle_blocks && suppress_page_shoulders) ||
        (!setting_circle_blocks && suppress_cancel)) {
        set_control(device, fusion_controls.right_shoulder, 0.0f);
    } else if (setting_circle_blocks) {
        set_button(device, fusion_controls.right_shoulder, SCE_CTRL_L1);
    } else {
        set_button(device, fusion_controls.right_shoulder, SCE_CTRL_CIRCLE);
        if ((controller_pressed_buttons & SCE_CTRL_CIRCLE) != 0 &&
            dodge_delivery_reports < 8) {
            ++dodge_delivery_reports;
            l_info("Circle edge delivered to Fusion RightShoulder control "
                   "%u, the native Dodge input.",
                   fusion_controls.right_shoulder);
        }
    }
    /* Start is dispatched directly to EdgeMenu so it cannot open Challenges. */
    set_control(device, fusion_controls.start, 0.0f);
}

static uint8_t *edge_menu_button(void *edge_menu, int index) {
    if (!edge_menu || index < 0 || index >= EDGE_MENU_BUTTON_COUNT) {
        return NULL;
    }
    return (uint8_t *)edge_menu + EDGE_MENU_BUTTON_OFFSET +
           (unsigned int)index * EDGE_MENU_BUTTON_STRIDE;
}

static bool edge_menu_button_active(void *edge_menu, int index) {
    uint8_t *button = edge_menu_button(edge_menu, index);
    return button && button[EDGE_MENU_BUTTON_ACTIVE_OFFSET] != 0 &&
           *(const unsigned int *)(button + EDGE_MENU_BUTTON_ID_OFFSET) != 0;
}

static unsigned int edge_menu_button_id(void *edge_menu, int index) {
    uint8_t *button = edge_menu_button(edge_menu, index);
    return button
        ? *(const unsigned int *)(button + EDGE_MENU_BUTTON_ID_OFFSET) : 0;
}

static int edge_menu_find_button(void *edge_menu, unsigned int id) {
    for (int index = 0; index < EDGE_MENU_BUTTON_COUNT; ++index) {
        if (edge_menu_button_active(edge_menu, index) &&
            edge_menu_button_id(edge_menu, index) == id) {
            return index;
        }
    }
    return -1;
}

static void edge_menu_restore_sizes(void *edge_menu) {
    for (int index = 0; index < EDGE_MENU_BUTTON_COUNT; ++index) {
        uint8_t *button = edge_menu_button(edge_menu, index);
        if (button && edge_menu_size_valid[index]) {
            float *size = (float *)(button + EDGE_MENU_BUTTON_SIZE_OFFSET);
            size[0] = edge_menu_base_size[index][0];
            size[1] = edge_menu_base_size[index][1];
        }
    }
}

static void edge_menu_apply_focus(void *edge_menu) {
    edge_menu_restore_sizes(edge_menu);
    uint8_t *button = edge_menu_button(edge_menu, edge_menu_focus);
    if (!button || !edge_menu_size_valid[edge_menu_focus]) {
        return;
    }
    float *size = (float *)(button + EDGE_MENU_BUTTON_SIZE_OFFSET);
    size[0] = edge_menu_base_size[edge_menu_focus][0] *
              EDGE_MENU_FOCUS_SCALE;
    size[1] = edge_menu_base_size[edge_menu_focus][1] *
              EDGE_MENU_FOCUS_SCALE;
}

static void edge_menu_enter_controller_mode(void *edge_menu) {
    sceClibMemset(edge_menu_size_valid, 0,
                  sizeof(edge_menu_size_valid));
    for (int index = 0; index < EDGE_MENU_BUTTON_COUNT; ++index) {
        uint8_t *button = edge_menu_button(edge_menu, index);
        if (!button || !edge_menu_button_active(edge_menu, index)) {
            continue;
        }
        const float *size = (const float *)(
            button + EDGE_MENU_BUTTON_SIZE_OFFSET);
        if (isfinite(size[0]) && isfinite(size[1]) &&
            size[0] > 0.0f && size[1] > 0.0f) {
            edge_menu_base_size[index][0] = size[0];
            edge_menu_base_size[index][1] = size[1];
            edge_menu_size_valid[index] = true;
        }
    }
    edge_menu_focus = edge_menu_find_button(
        edge_menu, EDGE_MENU_RESUME_BUTTON_ID);
    if (edge_menu_focus < 0) {
        for (int index = 0; index < EDGE_MENU_BUTTON_COUNT; ++index) {
            if (edge_menu_button_active(edge_menu, index)) {
                edge_menu_focus = index;
                break;
            }
        }
    }
    edge_menu_apply_focus(edge_menu);
    l_info("Vita controller entered the in-fight pause rail on button %u.",
           edge_menu_button_id(edge_menu, edge_menu_focus));
}

static void edge_menu_move_focus(void *edge_menu, int step) {
    if (edge_menu_focus < 0) {
        edge_menu_enter_controller_mode(edge_menu);
        return;
    }
    int candidate = edge_menu_focus;
    while (true) {
        candidate += step;
        if (candidate < 0 || candidate >= EDGE_MENU_BUTTON_COUNT) {
            return;
        }
        if (edge_menu_button_active(edge_menu, candidate)) {
            edge_menu_focus = candidate;
            edge_menu_apply_focus(edge_menu);
            l_info("Pause-rail focus moved to native button %u.",
                   edge_menu_button_id(edge_menu, edge_menu_focus));
            return;
        }
    }
}

static unsigned int edge_menu_state(void *edge_menu) {
    return edge_menu
        ? *(const unsigned int *)((const uint8_t *)edge_menu +
                                  EDGE_MENU_STATE_OFFSET)
        : 0;
}

static void *quit_confirmation_panel_element(const uint8_t *panel) {
    return panel
        ? *(void *const *)(panel + QUIT_CONFIRM_PANEL_ELEMENT_OFFSET) : NULL;
}

static void quit_confirmation_restore_focus(void) {
    for (int index = 0; index < QUIT_CONFIRM_COUNT; ++index) {
        if (fusion_flash_set_opacity && quit_confirmation_element[index] &&
            quit_confirmation_opacity_valid[index]) {
            fusion_flash_set_opacity(quit_confirmation_element[index],
                                     quit_confirmation_base_opacity[index]);
        }
    }
}

static void quit_confirmation_abandon_focus(void) {
    quit_confirmation_restore_focus();
    sceClibMemset(quit_confirmation_opacity_valid, 0,
                  sizeof(quit_confirmation_opacity_valid));
}

static void quit_confirmation_apply_focus(void) {
    quit_confirmation_restore_focus();
    if (quit_confirmation_focus < 0 ||
        quit_confirmation_focus >= QUIT_CONFIRM_COUNT) {
        return;
    }
    /*
     * Fusion animates X/check sizes itself. The previously cached scale could
     * keep X enlarged even while Yes was focused, so distinguish the choices
     * by opacity without changing either element's authored size.
     */
    int unfocused = quit_confirmation_focus == QUIT_CONFIRM_NO
        ? QUIT_CONFIRM_YES : QUIT_CONFIRM_NO;
    if (fusion_flash_set_opacity &&
        quit_confirmation_element[unfocused] &&
        quit_confirmation_opacity_valid[unfocused]) {
        fusion_flash_set_opacity(
            quit_confirmation_element[unfocused],
            quit_confirmation_base_opacity[unfocused] *
                QUIT_CONFIRM_UNFOCUSED_OPACITY);
    }
}

void controls_refresh_quit_confirmation_focus(void) {
    /*
     * HUDSYSTEM::update animates the native Yes/No elements after the input
     * poll. Applying focus from controls_poll alone therefore lets Fusion
     * overwrite focus opacity before render. The HUD post-hook calls
     * this after that animation, while the guards avoid touching an element
     * after Cross, Circle, or touch has begun closing the panel.
     */
    if (quit_confirmation_was_active &&
        !quit_confirmation_forced_element) {
        quit_confirmation_apply_focus();
    }
}

static void quit_confirmation_enter(void) {
    quit_confirmation_focus = QUIT_CONFIRM_NO;
    quit_confirmation_forced_element = NULL;
    quit_confirmation_element[QUIT_CONFIRM_NO] =
        quit_confirmation_panel_element(fusion_quit_confirm_panel_no);
    quit_confirmation_element[QUIT_CONFIRM_YES] =
        quit_confirmation_panel_element(fusion_quit_confirm_panel_yes);
    sceClibMemset(quit_confirmation_opacity_valid, 0,
                  sizeof(quit_confirmation_opacity_valid));
    if (fusion_flash_get_opacity) {
        for (int index = 0; index < QUIT_CONFIRM_COUNT; ++index) {
            if (!quit_confirmation_element[index]) {
                continue;
            }
            float opacity = fusion_flash_get_opacity(
                quit_confirmation_element[index]);
            if (isfinite(opacity) && opacity >= 0.0f) {
                quit_confirmation_base_opacity[index] = opacity;
                quit_confirmation_opacity_valid[index] = true;
            }
        }
    }
    quit_confirmation_apply_focus();
    quit_confirmation_was_active = true;
    l_info("Vita controller entered the pause-quit confirmation on No "
           "(No=%p opacity=%.2f, Yes=%p opacity=%.2f).",
           quit_confirmation_element[QUIT_CONFIRM_NO],
           quit_confirmation_opacity_valid[QUIT_CONFIRM_NO]
               ? quit_confirmation_base_opacity[QUIT_CONFIRM_NO] : -1.0f,
           quit_confirmation_element[QUIT_CONFIRM_YES],
           quit_confirmation_opacity_valid[QUIT_CONFIRM_YES]
               ? quit_confirmation_base_opacity[QUIT_CONFIRM_YES] : -1.0f);
}

static void quit_confirmation_move_focus(VitaUIDirection direction) {
    if (direction == VITA_UI_DIRECTION_NONE) {
        return;
    }

    int target = quit_confirmation_focus;
    const float *no_matrix = fusion_flash_get_world_matrix &&
        quit_confirmation_element[QUIT_CONFIRM_NO]
        ? fusion_flash_get_world_matrix(
              quit_confirmation_element[QUIT_CONFIRM_NO]) : NULL;
    const float *yes_matrix = fusion_flash_get_world_matrix &&
        quit_confirmation_element[QUIT_CONFIRM_YES]
        ? fusion_flash_get_world_matrix(
              quit_confirmation_element[QUIT_CONFIRM_YES]) : NULL;
    if (no_matrix && yes_matrix &&
        isfinite(no_matrix[12]) && isfinite(no_matrix[13]) &&
        isfinite(yes_matrix[12]) && isfinite(yes_matrix[13])) {
        if (direction == VITA_UI_DIRECTION_LEFT) {
            target = no_matrix[12] <= yes_matrix[12]
                ? QUIT_CONFIRM_NO : QUIT_CONFIRM_YES;
        } else if (direction == VITA_UI_DIRECTION_RIGHT) {
            target = no_matrix[12] >= yes_matrix[12]
                ? QUIT_CONFIRM_NO : QUIT_CONFIRM_YES;
        } else if (direction == VITA_UI_DIRECTION_UP) {
            target = no_matrix[13] <= yes_matrix[13]
                ? QUIT_CONFIRM_NO : QUIT_CONFIRM_YES;
        } else if (direction == VITA_UI_DIRECTION_DOWN) {
            target = no_matrix[13] >= yes_matrix[13]
                ? QUIT_CONFIRM_NO : QUIT_CONFIRM_YES;
        }
    } else {
        target = (direction == VITA_UI_DIRECTION_LEFT ||
                  direction == VITA_UI_DIRECTION_UP)
            ? QUIT_CONFIRM_NO : QUIT_CONFIRM_YES;
    }

    if (target != quit_confirmation_focus) {
        quit_confirmation_focus = target;
        quit_confirmation_apply_focus();
        l_info("Pause-quit focus moved to %s.",
               target == QUIT_CONFIRM_YES ? "Yes" : "No");
    }
}

static bool edge_menu_is_active(void *edge_menu) {
    return edge_menu_state(edge_menu) == EDGE_MENU_STATE_ON;
}

static bool edge_menu_is_transitioning(void *edge_menu) {
    unsigned int state = edge_menu_state(edge_menu);
    return state > EDGE_MENU_STATE_OFF && state <=
        EDGE_MENU_STATE_PAUSE_REQUEST;
}

static bool handle_edge_menu_controls(bool touch_input) {
    void *edge_menu = fusion_edge_menu_system
        ? *fusion_edge_menu_system : NULL;
    bool active = edge_menu_is_active(edge_menu);
    bool confirmation = edge_menu_state(edge_menu) ==
                            EDGE_MENU_STATE_QUIT_CONFIRM &&
                        fusion_hud_quit_panel_showing &&
                        *fusion_hud_quit_panel_showing;
    bool transitioning = edge_menu_is_transitioning(edge_menu) &&
                         !active && !confirmation;
    bool in_use = active || confirmation || transitioning;

    if ((controller_pressed_buttons & SCE_CTRL_START) != 0) {
        controller_pressed_buttons &= ~SCE_CTRL_START;
        controller_suppressed_buttons |= SCE_CTRL_START;
        if (active) {
            edge_menu_forced_button = EDGE_MENU_RESUME_BUTTON_ID;
            l_info("Start queued the native Resume action.");
        } else if (!transitioning &&
                   edge_menu_state(edge_menu) == EDGE_MENU_STATE_OFF &&
                   fusion_edge_menu_can_pause && fusion_edge_menu_set_state &&
                   fusion_active_player &&
                   *fusion_active_player &&
                   fusion_edge_menu_can_pause(edge_menu)) {
            fusion_edge_menu_set_state(edge_menu,
                                       EDGE_MENU_STATE_PAUSE_REQUEST);
            transitioning = edge_menu_is_transitioning(edge_menu);
            in_use = active || transitioning;
            l_info("Start requested Fusion's native in-fight pause rail "
                   "through state %u.", edge_menu_state(edge_menu));
        }
    }

    if (confirmation) {
        if (edge_menu_was_active) {
            edge_menu_restore_sizes(edge_menu);
            edge_menu_focus = -1;
            edge_menu_forced_button = 0;
            edge_menu_was_active = false;
            l_info("Fusion's in-fight pause rail became hidden.");
        }
        edge_menu_was_in_use = true;
        if (touch_input) {
            /* Touch may unload these Flash elements before the next frame.
             * Reacquire them if the dialog remains open and the user returns
             * to the controller; otherwise D-pad moves an invisible focus. */
            if (quit_confirmation_was_active) {
                quit_confirmation_abandon_focus();
                quit_confirmation_was_active = false;
                sceClibMemset(quit_confirmation_element, 0,
                              sizeof(quit_confirmation_element));
            }
            quit_confirmation_forced_element = NULL;
            controller_suppressed_buttons |= MAPPED_VITA_BUTTONS;
            controls_set_native_confirm_suppressed(true);
            controls_set_native_cancel_suppressed(true);
            controls_set_native_page_shoulders_suppressed(true);
            return true;
        }
        if (!quit_confirmation_was_active) {
            quit_confirmation_enter();
        }

        quit_confirmation_move_focus(controls_take_ui_direction());
        if (controls_take_confirm_press() && quit_confirmation_focus >= 0 &&
            quit_confirmation_focus < QUIT_CONFIRM_COUNT) {
            quit_confirmation_abandon_focus();
            quit_confirmation_forced_element =
                quit_confirmation_element[quit_confirmation_focus];
            controller_suppressed_buttons |= SCE_CTRL_CROSS;
            l_info("Cross queued Fusion's native pause-quit %s action.",
                   quit_confirmation_focus == QUIT_CONFIRM_YES
                       ? "Yes" : "No");
        }
        if (controls_take_cancel_press()) {
            quit_confirmation_abandon_focus();
            controller_suppressed_buttons |= SCE_CTRL_CIRCLE;
            ninjago_game_back();
            l_info("Circle sent native Back/No to the pause-quit "
                   "confirmation.");
        }

        controller_suppressed_buttons |= MAPPED_VITA_BUTTONS;
        controls_set_native_confirm_suppressed(true);
        controls_set_native_cancel_suppressed(true);
        controls_set_native_page_shoulders_suppressed(true);
        return true;
    }

    if (!active) {
        if (quit_confirmation_was_active) {
            quit_confirmation_restore_focus();
            quit_confirmation_was_active = false;
            quit_confirmation_forced_element = NULL;
            sceClibMemset(quit_confirmation_element, 0,
                          sizeof(quit_confirmation_element));
        }
        if (edge_menu_was_active) {
            edge_menu_restore_sizes(edge_menu);
            edge_menu_focus = -1;
            edge_menu_forced_button = 0;
            edge_menu_was_active = false;
            l_info("Fusion's in-fight pause rail became hidden.");
        }
        if (transitioning) {
            /* Opening/closing states remain modal but have no buttons yet. */
            edge_menu_was_in_use = true;
            controller_suppressed_buttons |= MAPPED_VITA_BUTTONS;
            controls_set_native_confirm_suppressed(true);
            controls_set_native_cancel_suppressed(true);
            controls_set_native_page_shoulders_suppressed(true);
        } else if (edge_menu_was_in_use) {
            edge_menu_was_in_use = false;
            controls_set_native_confirm_suppressed(false);
            controls_set_native_cancel_suppressed(false);
            controls_set_native_page_shoulders_suppressed(false);
            l_info("Vita controller left Fusion's in-fight pause state.");
        }
        return in_use;
    }

    edge_menu_was_in_use = true;
    if (!edge_menu_was_active) {
        edge_menu_was_active = true;
        edge_menu_enter_controller_mode(edge_menu);
    }

    VitaUIDirection direction = controls_take_ui_direction();
    if (direction == VITA_UI_DIRECTION_UP ||
        direction == VITA_UI_DIRECTION_LEFT) {
        edge_menu_move_focus(edge_menu, -1);
    } else if (direction == VITA_UI_DIRECTION_DOWN ||
               direction == VITA_UI_DIRECTION_RIGHT) {
        edge_menu_move_focus(edge_menu, 1);
    }

    if (controls_take_confirm_press() && edge_menu_focus >= 0) {
        edge_menu_forced_button = edge_menu_button_id(
            edge_menu, edge_menu_focus);
        controller_suppressed_buttons |= SCE_CTRL_CROSS;
        l_info("Cross queued native pause-rail button %u.",
               edge_menu_forced_button);
    }
    if (controls_take_cancel_press()) {
        controller_suppressed_buttons |= SCE_CTRL_CIRCLE;
        ninjago_game_back();
        l_info("Circle sent native Back to the in-fight pause rail.");
    }

    /* No combat action may leak through the modal pause rail. */
    controller_suppressed_buttons |= MAPPED_VITA_BUTTONS;
    controls_set_native_confirm_suppressed(true);
    controls_set_native_cancel_suppressed(true);
    controls_set_native_page_shoulders_suppressed(true);
    return true;
}

static bool queue_native_grab_throw(void *character_data) {
    if (!character_data) {
        return false;
    }
    uint8_t *component_data = *(uint8_t **)(
        (uint8_t *)character_data + CHARACTER_COMPONENT_DATA_OFFSET);
    if (!component_data) {
        return false;
    }
    uint8_t *grab_data = *(uint8_t **)(
        component_data + GRAB_INSTANCE_DATA_OFFSET);
    if (!grab_data) {
        return false;
    }
    grab_data[GRAB_PENDING_FLAGS_OFFSET] |= GRAB_THROW_PENDING_BIT;
    return true;
}

static uint8_t *character_component_data(void *character_data) {
    return character_data
        ? *(uint8_t **)((uint8_t *)character_data +
                        CHARACTER_COMPONENT_DATA_OFFSET)
        : NULL;
}

static bool character_has_ranged_weapon(void *character_data) {
    return character_data &&
           *(void **)((uint8_t *)character_data +
                      CHARACTER_RANGED_WEAPON_OBJECT_OFFSET) != NULL;
}

static uint16_t character_current_state(void *character_data) {
    return character_data
        ? *(const uint16_t *)((const uint8_t *)character_data +
                             CHARACTER_CURRENT_STATE_OFFSET)
        : 0;
}

static int refresh_weapon_locomotion(void *player, void *character_data) {
    if (!player || !character_data) {
        return -1;
    }
    uint16_t state = character_current_state(character_data);
    /* Pinned 1057 state IDs 1-3 are the ordinary idle/walk/run states. */
    if (state < 1 || state > 3) {
        return -1;
    }

    /*
     * setState(current) is a no-op because geGOSTATESYSTEM compares state
     * pointers before calling enter. Resolve the pinned current state through
     * its native stack, verify its cached ID, then rerun only that locomotion
     * state's enter method. This is the same animation-selection work Fusion
     * performs after the player stops and begins moving again, without leaving
     * or replacing the current gameplay state.
     */
    uint8_t *state_system = (uint8_t *)character_data +
                            CHARACTER_STATE_SYSTEM_OFFSET;
    uint8_t state_count = state_system[0x0c];
    if (state_count == 0 || state_count > 3) {
        return 0;
    }
    void *state_object = *(void **)(state_system +
                                    (state_count - 1u) * sizeof(void *));
    if (!state_object ||
        ((*(const uint16_t *)((const uint8_t *)state_object + 6)) & 0x3fffu) !=
            state) {
        return 0;
    }
    void **vtable = *(void ***)state_object;
    FusionStateEnterMethod enter = vtable
        ? (FusionStateEnterMethod)vtable[2] : NULL;
    if (!enter) {
        return 0;
    }
    enter(state_object, player);
    return 1;
}

static bool character_state_accepts_dodge(void *character_data) {
    uint16_t state = character_current_state(character_data);
    return (state >= 1 && state <= 3) || state == 218 ||
           (state >= 382 && state <= 383);
}

void controls_poll(void) {
    bool physical_input = poll_controller();
    bool touch_input = poll_front_touch();
    bool edge_menu_active = handle_edge_menu_controls(touch_input);

    void *active_player = fusion_active_player
        ? *fusion_active_player : NULL;
    void *active_character_data = active_player && fusion_get_character_data
        ? fusion_get_character_data(active_player) : NULL;
    uint8_t *active_component_data = character_component_data(
        active_character_data);
    if (active_player != secondary_preference_player) {
        secondary_preference_player = active_player;
        /* Every newly active character starts in the console's melee mode. */
        secondary_ranged_preferred = false;
    }
    if (active_player && active_component_data) {
        unsigned int preferred_weapon = secondary_ranged_preferred ? 0u : 1u;
        uint8_t preferred_auto_holster =
            secondary_ranged_preferred ? 2u : 1u;
        bool preferred_drawn = fusion_is_weapon_drawn &&
            fusion_is_weapon_drawn(active_character_data,
                                    preferred_weapon);
        bool repaired_preference =
            active_component_data[CHARACTER_WEAPON_PREFERENCE_OFFSET] !=
                preferred_auto_holster;
        /*
         * Proximity attacks and special moves may temporarily draw another
         * weapon. Preserve Select's explicit mode in Fusion's own field so
         * AutoWeaponHolster restores melee as reliably as it restores ranged.
         */
        active_component_data[CHARACTER_WEAPON_PREFERENCE_OFFSET] =
            preferred_auto_holster;

        /*
         * Fusion normally waits until its 16-bit counter at +0x14 exceeds 39
         * before restoring the requested weapon. Prime that native counter
         * and let updateControls perform the switch only when its own
         * action/state checks say it is safe.
         */
        if (!preferred_drawn &&
            fusion_auto_weapon_holster_control_system) {
            *(uint16_t *)(fusion_auto_weapon_holster_control_system +
                          AUTO_WEAPON_RESTORE_COUNTER_OFFSET) =
                AUTO_WEAPON_RESTORE_READY_TICKS;
        }
        if ((repaired_preference || !preferred_drawn) &&
            secondary_preference_restore_reports < 8) {
            ++secondary_preference_restore_reports;
            l_info("Armed Fusion's safe %s restore after startup, "
                   "proximity, or an animation selected another weapon.",
                   secondary_ranged_preferred ? "ranged" : "melee");
        }
    }

    /*
     * The generic EnableWeapon path hides the current weapon even when a
     * character has no ranged weapon. GOCharacter_EnableRangedWeapon itself
     * checks the ranged object at character-data +0x1d4 only after hiding
     * melee, so validate that exact native field before making the call.
     * Returning to melee remains explicit so auto-holster has a stable target.
     */
    if (!edge_menu_active &&
        (controller_pressed_buttons & SCE_CTRL_SELECT) != 0 &&
        !movement_toggle_suppressed && fusion_active_player &&
        *fusion_active_player &&
        fusion_get_character_data && fusion_is_weapon_drawn &&
        fusion_enable_weapon) {
        void *player = *fusion_active_player;
        void *character_data = player == active_player
            ? active_character_data
            : (fusion_get_character_data
                ? fusion_get_character_data(player) : NULL);
        /*
         * Toggle the player's persistent mode, not the model drawn during the
         * current animation. Proximity/special actions can temporarily stow
         * ranged; treating that model state as the toggle would select ranged
         * a second time instead of returning to melee.
         */
        bool switch_to_ranged = !secondary_ranged_preferred;
        int handled = 0;
        uint8_t *component_data = character_component_data(character_data);
        if (switch_to_ranged) {
            if (character_has_ranged_weapon(character_data)) {
                fusion_enable_weapon(player, 0u, true, false);
                if (component_data) {
                    component_data[CHARACTER_WEAPON_PREFERENCE_OFFSET] = 2;
                }
                secondary_preference_player = player;
                secondary_ranged_preferred = true;
                handled = 1;
            } else {
                /* Repair a stale preference without ever hiding melee. */
                fusion_enable_weapon(player, 1u, true, false);
                if (component_data) {
                    component_data[CHARACTER_WEAPON_PREFERENCE_OFFSET] = 1;
                }
                secondary_preference_player = player;
                secondary_ranged_preferred = false;
            }
        } else {
            fusion_enable_weapon(player, 1u, true, false);
            if (component_data) {
                component_data[CHARACTER_WEAPON_PREFERENCE_OFFSET] = 1;
            }
            secondary_preference_player = player;
            secondary_ranged_preferred = false;
            handled = 1;
        }
        int animation_refresh = handled
            ? refresh_weapon_locomotion(player, character_data) : -1;
        if (animation_refresh >= 0 &&
            weapon_animation_refresh_reports < 8) {
            ++weapon_animation_refresh_reports;
            l_info("Select refreshed locomotion state %u for the %s running "
                   "animation (result=%d).",
                   character_current_state(character_data),
                   switch_to_ranged ? "ranged" : "melee",
                   animation_refresh);
        }
        controller_pressed_buttons &= ~SCE_CTRL_SELECT;
        controller_suppressed_buttons |= SCE_CTRL_SELECT;
        if (secondary_weapon_reports < 8) {
            ++secondary_weapon_reports;
            if (switch_to_ranged && !handled) {
                l_info("Select kept the melee weapon because the active "
                       "character has no available ranged action.");
            } else {
                l_info("Select switched to the %s weapon and updated "
                       "Fusion's auto-holster preference (result=%d).",
                       switch_to_ranged ? "ranged" : "melee", handled);
            }
        }
    }

    /*
     * The latest hardware trace proves Circle reaches RightShoulder control
     * 10, but that indirect input does not consistently dispatch PadDodge.
     * On a Circle edge, call Fusion's own Dodge_Start only after reproducing
     * PadDodge's state and ability guards. Latch the edge so the held joypad
     * value cannot start a second dodge on the following frame.
     */
    if (!edge_menu_active && !setting_circle_blocks &&
        (controller_pressed_buttons & SCE_CTRL_CIRCLE) != 0 &&
        (controller_suppressed_buttons & SCE_CTRL_CIRCLE) == 0 &&
        active_player && active_character_data && fusion_has_ability &&
        fusion_dodge_start) {
        uint16_t state = character_current_state(active_character_data);
        bool has_dodge = fusion_has_ability(active_character_data,
                                            CHARACTER_DODGE_ABILITY);
        if (has_dodge &&
            character_state_accepts_dodge(active_character_data)) {
            fusion_dodge_start(active_player, false);
            controller_pressed_buttons &= ~SCE_CTRL_CIRCLE;
            controller_suppressed_buttons |= SCE_CTRL_CIRCLE;
            dodge_action_latched = true;
            if (dodge_direct_reports < 8) {
                ++dodge_direct_reports;
                l_info("Circle directly started Fusion's native dodge from "
                       "state %u.", state);
            }
        } else if (dodge_reject_reports < 8) {
            ++dodge_reject_reports;
            l_info("Circle dodge remained unavailable in Fusion state %u "
                   "(native dodge ability=%d).", state, has_dodge);
        }
    }

    /*
     * The Android combat code exposes throwing through PadNorth, but grabbing
     * exists only in its pinch-gesture path. On a Triangle edge, ask the game's
     * target validator to begin a grab; while already grappling, set its native
     * pending-throw flag for the grab-idle state to consume. If neither handles,
     * leave Triangle unsuppressed so PadNorth keeps the normal ranged action.
     * This is native gameplay logic, not a synthesized touch gesture.
     */
    if ((controller_pressed_buttons & SCE_CTRL_TRIANGLE) != 0 &&
        fusion_active_player && *fusion_active_player &&
        fusion_get_character_data && fusion_attempt_grab &&
        fusion_is_grappling) {
        void *player = *fusion_active_player;
        void *character_data = fusion_get_character_data(player);
        bool handled = false;
        if (character_data && fusion_is_grappling(player)) {
            handled = queue_native_grab_throw(character_data);
            if (handled) {
                l_info("Triangle queued Fusion's native grab-throw request.");
            } else {
                l_warn("Triangle could not locate Fusion's active grab data "
                       "to queue a throw.");
            }
        } else if (character_data) {
            handled = fusion_attempt_grab(player, character_data, false);
            if (handled) {
                l_info("Triangle started a native context-sensitive grab.");
            }
        }
        if (handled) {
            controller_pressed_buttons &= ~SCE_CTRL_TRIANGLE;
            controller_suppressed_buttons |= SCE_CTRL_TRIANGLE;
            triangle_action_latched = true;
        }
    }

    /*
     * The in-fight HUD consumes Android Back rather than Controls_Cancel when
     * a pause menu is open. Restrict this fallback to an actual non-zero pause
     * menu so Circle remains Dodge during ordinary gameplay. Select is routed
     * separately to Fusion's native secondary-weapon event.
     */
    uint32_t back_press = controller_pressed_buttons & SCE_CTRL_CIRCLE;
    if (back_press && fusion_game_loop && fusion_get_pause_menu &&
        fusion_get_pause_menu(fusion_game_loop) != 0) {
        controller_suppressed_buttons |= back_press;
        ninjago_game_back();
        l_info("Vita Back closed the active gameplay pause menu.");
    }

    if (touch_input) {
        /* Active touch is authoritative even if a stick is also moving. */
        controls_set_native_confirm_suppressed(false);
        controls_set_native_cancel_suppressed(false);
        controls_set_native_page_shoulders_suppressed(false);
        controller_is_active = false;
        select_input_source(true);
    } else if (physical_input) {
        controller_is_active = true;
        select_input_source(false);
    }

    apply_movement_mode(false);
    update_virtual_control_display();
    if (active_player && !edge_menu_active && !movement_toggle_suppressed &&
        !native_page_shoulders_suppressed &&
        !native_page_shoulders_release_latched &&
        (controller_pressed_buttons & ~controller_suppressed_buttons &
         SCE_CTRL_R1) != 0) {
        ++special_press_serial;
    }
}

unsigned int controls_get_special_press_serial(void) {
    return special_press_serial;
}

unsigned int controls_take_edge_menu_button(void) {
    unsigned int button = edge_menu_forced_button;
    edge_menu_forced_button = 0;
    return button;
}

bool controls_take_quit_confirmation(void *element) {
    if (!element || element != quit_confirmation_forced_element) {
        return false;
    }
    if (quit_confirmation_focus == QUIT_CONFIRM_YES) {
        /*
         * The active-player pointer can remain valid while Fusion tears down
         * an arena. Keep that pointer as the known melee-preference owner so
         * the next poll cannot rediscover its still-drawn ranged model and
         * carry the old choice into the next match.
         */
        void *exiting_player = fusion_active_player
            ? *fusion_active_player : NULL;
        secondary_preference_player = exiting_player;
        secondary_ranged_preferred = false;
        if (exiting_player && fusion_get_character_data) {
            uint8_t *component_data = character_component_data(
                fusion_get_character_data(exiting_player));
            if (component_data) {
                component_data[CHARACTER_WEAPON_PREFERENCE_OFFSET] = 1;
            }
        }
        l_info("Cleared Select's ranged preference after confirmed "
               "gameplay exit.");
    }
    quit_confirmation_forced_element = NULL;
    return true;
}

bool controls_use_ranged_attack_mode(void) {
    return controller_is_active && secondary_ranged_preferred;
}

bool controls_ranged_attack_held(const void *player) {
    return player && controller_is_active && secondary_ranged_preferred &&
        fusion_active_player && *fusion_active_player == player &&
        (current_pad.buttons & SCE_CTRL_SQUARE) != 0 &&
        (controller_suppressed_buttons & SCE_CTRL_SQUARE) == 0;
}

bool controls_take_confirm_press(void) {
    if ((controller_pressed_buttons & SCE_CTRL_CROSS) == 0 ||
        (controller_suppressed_buttons & SCE_CTRL_CROSS) != 0) {
        return false;
    }
    controller_pressed_buttons &= ~SCE_CTRL_CROSS;
    return true;
}

VitaUIDirection controls_take_ui_direction(void) {
    VitaUIDirection direction = queued_ui_direction;
    queued_ui_direction = VITA_UI_DIRECTION_NONE;

    uint32_t dpad = controller_pressed_buttons &
                    (SCE_CTRL_UP | SCE_CTRL_DOWN |
                     SCE_CTRL_LEFT | SCE_CTRL_RIGHT);
    controller_pressed_buttons &= ~dpad;
    controller_suppressed_buttons |= dpad;
    return direction;
}

bool controls_take_cancel_press(void) {
    if ((controller_pressed_buttons & SCE_CTRL_CIRCLE) == 0 ||
        (controller_suppressed_buttons & SCE_CTRL_CIRCLE) != 0) {
        return false;
    }
    controller_pressed_buttons &= ~SCE_CTRL_CIRCLE;
    controller_suppressed_buttons |= SCE_CTRL_CIRCLE;
    return true;
}

int controls_take_character_page_press(void) {
    uint32_t page_press = controller_pressed_buttons &
                          (SCE_CTRL_L1 | SCE_CTRL_R1);
    if (movement_toggle_suppressed || page_press == 0 ||
        page_press == (SCE_CTRL_L1 | SCE_CTRL_R1)) {
        return 0;
    }

    controller_pressed_buttons &= ~page_press;
    controller_suppressed_buttons |= page_press;
    return (page_press & SCE_CTRL_L1) ? -1 : 1;
}

bool controls_using_controller(void) {
    return controller_is_active;
}

void controls_set_native_confirm_suppressed(bool suppressed) {
    native_confirm_suppressed = suppressed;
    if (!suppressed && (current_pad.buttons & SCE_CTRL_CROSS) != 0) {
        /* Do not leak a held custom confirm into Fusion on the next frame. */
        native_confirm_release_latched = true;
    }
}

void controls_set_native_cancel_suppressed(bool suppressed) {
    native_cancel_suppressed = suppressed;
    if (!suppressed && (current_pad.buttons & SCE_CTRL_CIRCLE) != 0) {
        /* Do not leak a held custom cancel into Fusion on the next screen. */
        native_cancel_release_latched = true;
    }
}

void controls_set_native_page_shoulders_suppressed(bool suppressed) {
    native_page_shoulders_suppressed = suppressed;
    if (!suppressed &&
        (current_pad.buttons & (SCE_CTRL_L1 | SCE_CTRL_R1)) != 0) {
        /* A held page-change press must not become a combat shoulder edge. */
        native_page_shoulders_release_latched = true;
    }
}

void controls_set_native_ui_navigation_suppressed(bool suppressed) {
    native_ui_navigation_suppressed = suppressed;
    if (!suppressed &&
        (((current_pad.buttons &
           (SCE_CTRL_UP | SCE_CTRL_DOWN | SCE_CTRL_LEFT | SCE_CTRL_RIGHT)) !=
          0) || stick_is_active(current_pad.lx) ||
         stick_is_active(current_pad.ly))) {
        /* Do not leak a held portrait direction into the next UI module. */
        native_ui_navigation_release_latched = true;
    }
}

void controls_draw_cursor(void) {
    /* The first milestone intentionally does not modify Fusion's GL state. */
}

void coord_normalize(float *x, float *y, float deadzone) {
    float magnitude = sqrtf((*x * *x) + (*y * *y));
    if (magnitude < deadzone) {
        *x = 0.0f;
        *y = 0.0f;
        return;
    }
    *x /= magnitude;
    *y /= magnitude;
}
