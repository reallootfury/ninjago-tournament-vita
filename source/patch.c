#include "game.h"
#include "reimpl/sys.h"
#include "reimpl/controls.h"
#include "utils/logger.h"
#include "utils/glutil.h"
#include "utils/flashclip.h"
#include "utils/utils.h"

#include <kubridge.h>
#include <so_util/so_util.h>

#include <float.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <psp2/kernel/processmgr.h>

#define ARM_KUSER_MEMORY_BARRIER UINT32_C(0xffff0fa0)
#define ARM_KUSER_CMPXCHG UINT32_C(0xffff0fc0)

extern so_module so_mod;
static uint32_t *fusion_ui_resolution;

typedef void (*FusionUIModuleUpdate)(void *module, float delta_time);
typedef void (*FusionUIHandleButton)(void *module, uint32_t button);
typedef void *(*FusionGetCurrentModule)(void);
typedef const float *(*FusionFlashGetVec2)(void *element);
typedef const float *(*FusionFlashGetWorldMatrix)(void *element);
typedef void (*FusionFlashSetVec2)(void *element, const float *value);
typedef void (*FusionFlashSetVisibility)(void *element, bool visible);
typedef void (*FusionFlashSetOpacity)(void *element, float opacity);
typedef bool (*FusionFlashIsVisible)(void *element, bool inherited);
typedef int (*FusionCharacterLevelSelect)(void *module, int level);
typedef void (*FusionLevelSelectMove)(void *module, int target);
typedef int (*FusionRedBrickSelect)(void *module, int red_brick);
typedef void (*FusionRedBrickSetUp)(void *module);
typedef bool (*FusionRedBrickIsBought)(unsigned int red_brick,
                                       bool include_debug);
typedef void *(*FusionGetActiveOptions)(void);
typedef void (*FusionHitSfx)(void *game_object);
typedef int (*FusionMapPortraitId)(int portrait_id);
typedef int (*FusionCharacterSelectPortrait)(void *module, int page,
                                             int slot);
typedef unsigned int (*FusionEdgeMenuButtonPressed)(void *edge_menu,
                                                     void *touch);
typedef bool (*FusionTouchUpInsideElement)(void *element);
typedef void (*FusionHudUpdate)(void *hud, void *world, float delta_time);
typedef bool (*FusionCombatPadHandler)(void *event, void *player,
                                       const void *combat_data);
typedef void (*FusionSetBossHeartCount)(void *icon, unsigned int count);
typedef void (*FusionSetHeartCount)(unsigned int count);
typedef bool (*FusionIsCharacter)(const void *game_object);
typedef void *(*FusionCharacterData)(void *game_object);
typedef unsigned int (*FusionAnimationFrameCount)(void *stream);
typedef unsigned int (*FusionAnimationStreamStatus)(void *stream);
typedef void *(*FusionAnimationFindPlaying)(void *stream);
typedef float (*FusionAnimationGetStreamNextFrame)(void *stream, int offset);
typedef int (*FusionAnimationObjectUpdate)(void *animation, void *object);
typedef void (*FusionPlayAnimSafe)(void *stream, unsigned int type,
                                   uint16_t start_frame, uint16_t end_frame,
                                   float speed, float blend, void *user_data,
                                   uint16_t flags);
typedef void (*FusionFlashUpdateGraph)(void *object);
typedef void (*FusionFlashUpdateInternalGraph)(void *object,
                                               const void *matrix);
typedef unsigned int (*FusionFlashRender)(void *object, const void *matrix,
                                          unsigned int render_pass,
                                          unsigned int flags);
typedef float (*FusionFlashGetOpacity)(void *element);
typedef const float *(*FusionFlashGetUV)(void *element);
typedef void (*FusionFlashRenderTexture)(void *element, const void *colour,
                                         float opacity, float scale, bool flag);
typedef void (*FusionUpdatePadControls)(void *system, void *player,
                                       void *character_data,
                                       void *player_data_header);
typedef void (*FusionCharacterOrient)(void *player,
                                      void *player_data_header,
                                      int16_t turn_step, bool force);
typedef int (*FusionCharacterGetTurnSpeed)(void);

static FusionUIModuleUpdate fusion_ui_module_update;
static FusionUIHandleButton fusion_character_select_handle_button;
static FusionGetCurrentModule fusion_get_current_module;
static FusionFlashGetVec2 fusion_flash_get_scale;
static FusionFlashGetWorldMatrix fusion_flash_get_world_matrix;
static FusionFlashSetVec2 fusion_flash_set_scale;
static FusionFlashSetVisibility fusion_flash_set_visibility;
static FusionFlashSetOpacity fusion_flash_set_opacity;
static FusionFlashIsVisible fusion_flash_is_visible;
static FusionCharacterLevelSelect fusion_character_level_select;
static FusionLevelSelectMove fusion_level_select_move;
static FusionRedBrickSelect fusion_redbrick_select;
static FusionRedBrickSetUp fusion_redbrick_set_up;
static FusionRedBrickIsBought fusion_redbrick_is_bought;
static FusionGetActiveOptions fusion_get_active_options;
static FusionHitSfx fusion_hit_sfx;
static FusionMapPortraitId fusion_map_portrait_id;
static FusionCharacterSelectPortrait fusion_character_select_portrait;
static FusionEdgeMenuButtonPressed fusion_edge_menu_button_pressed;
static FusionTouchUpInsideElement fusion_touch_up_inside_element;
static FusionHudUpdate fusion_hud_update;
static FusionCombatPadHandler fusion_melee_tap;
static FusionCombatPadHandler fusion_melee_held;
static FusionCombatPadHandler fusion_ranged_tap;
static FusionCombatPadHandler fusion_ranged_held;
static FusionSetBossHeartCount fusion_set_boss_heart_count;
static FusionSetHeartCount fusion_set_heart_count;
static FusionIsCharacter fusion_is_character;
static FusionCharacterData fusion_character_data;
static FusionAnimationFrameCount fusion_animation_frame_count;
static FusionAnimationStreamStatus fusion_animation_stream_status;
static FusionAnimationFindPlaying fusion_animation_find_playing;
static FusionAnimationGetStreamNextFrame fusion_animation_next_frame;
static FusionAnimationObjectUpdate fusion_animation_position_update;
static FusionAnimationObjectUpdate fusion_animation_render_update;
static FusionPlayAnimSafe fusion_play_anim_safe;
static FusionFlashUpdateGraph fusion_flash_update_graph;
static FusionFlashUpdateInternalGraph fusion_flash_update_internal_graph;
static FusionFlashRender fusion_flash_render;
static FusionFlashGetOpacity fusion_flash_get_opacity;
static FusionFlashGetUV fusion_flash_get_uv;
static FusionFlashRenderTexture fusion_flash_render_texture;
static FusionUpdatePadControls fusion_update_pad_controls;
static FusionCharacterOrient fusion_character_orient;
static FusionCharacterGetTurnSpeed fusion_character_get_turn_speed;
static void **fusion_boss_hearts_go;
static void *fusion_boss_hearts_item;
static uint8_t *fusion_player_bar;
static const uint32_t *fusion_percent_per_heart;
static void *fusion_last_boss;
static unsigned int fusion_last_boss_health = UINT32_MAX;
static unsigned int fusion_boss_heart_reports;
static unsigned int fusion_boss_heart_post_reports;
static unsigned int fusion_player_heart_reports;
static unsigned int fusion_last_player_health = UINT32_MAX;
/* Captured only while Fusion's native heart setter owns live HUD objects.
 * Rendering compares pointer identities; it never dereferences old roots. */
static void *heart_clip_roots[6];
static bool heart_quad_clip_enabled = false;
static unsigned int fusion_heart_transition_serial;
static unsigned int fusion_active_heart_transition;
static unsigned int fusion_heart_anim_reports;
static uint64_t fusion_ranged_autofire_last_us;
static unsigned int fusion_ranged_autofire_reports;
static unsigned int fusion_stationary_aim_reports;
static void *fusion_ranged_autofire_event;
static void *fusion_ranged_autofire_player;
static const void *fusion_ranged_autofire_combat_data;

static void vita_set_boss_heart_count(void *icon, unsigned int count);
static void vita_set_heart_count(unsigned int count);
static unsigned int fusion_ranged_route_reports;
static unsigned int fusion_melee_route_reports;
static volatile uint32_t fusion_force_targetless_melee;
static void **fusion_character_select_instance;
static void **fusion_character_level_instance;
static void **fusion_level_select_instance;
static void **fusion_frontend_instance;
static void **fusion_attract_instance;
static void **fusion_redbrick_instance;
static void **fusion_prize_picker_instance;
static uintptr_t fusion_character_select_update;
static uintptr_t fusion_character_level_update;
static uintptr_t fusion_level_select_update;
static uintptr_t fusion_frontend_update;
static uintptr_t fusion_attract_update;
static uintptr_t fusion_redbrick_update;
static uintptr_t fusion_prize_picker_update;
static uint8_t *fusion_runtime_sound_volume;

enum {
    FUSION_UI_BUTTON_COUNT = 28,
    FUSION_UI_SELECTED_OFFSET = 0x648,
    FUSION_UI_BUTTON_STRIDE = 0x28,
    FUSION_UI_BUTTON_ELEMENT_OFFSET = 0x8c,
    FUSION_UI_BUTTON_POSITION_OFFSET = 0x4e8,
    FUSION_UI_MODULE_UPDATE_VTABLE_SLOT = 7,
    FUSION_UI_CHARACTER_PAGE_PREVIOUS = 17,
    FUSION_UI_CHARACTER_PAGE_NEXT = 18,
    FUSION_CHARACTER_PAGE_OFFSET = 0x1618,
    FUSION_CHARACTER_SELECTED_OFFSET = 0x1620,
    FUSION_CHARACTER_PORTRAIT_COUNT = 13,
    FUSION_CHARACTER_PORTRAIT_ELEMENT_OFFSET = 0x1368,
    FUSION_CHARACTER_PORTRAIT_STRIDE = 0x18,
    FUSION_LEVEL_CAROUSEL_COUNT = 3,
    FUSION_LEVEL_CAROUSEL_SELECTED_OFFSET = 0x92c,
    FUSION_LEVEL_CAROUSEL_PENDING_OFFSET = 0x930,
    FUSION_LEVEL_CAROUSEL_SCROLLING_OFFSET = 0x957,
    FUSION_LEVEL_ITEM_COUNT = 5,
    FUSION_LEVEL_ITEM_ELEMENT_OFFSET = 0x6ac,
    FUSION_LEVEL_SELECTED_OFFSET = 0x724,
    FUSION_LEVEL_BACK_BUTTON = 13,
    FUSION_LEVEL_BUY_BUTTON = 15,
    FUSION_LEVEL_PLAY_BUTTON = 16,
    FUSION_FRONTEND_SCREEN_OFFSET = 0x970,
    FUSION_FRONTEND_OPTIONS_SCREEN = 5,
    FUSION_FRONTEND_SETTINGS_VISUAL_BUTTON = 6,
    FUSION_FRONTEND_SETTINGS_ACTION = 7,
    FUSION_FRONTEND_BACK_BUTTON = 14,
    FUSION_ATTRACT_SCREEN_OFFSET = 0x7d8,
    FUSION_ATTRACT_MAIN_SCREEN = 2,
    FUSION_ATTRACT_OPTIONS_STATE_OFFSET = 0x7e4,
    FUSION_ATTRACT_OPTIONS_OPEN = 1,
    FUSION_ATTRACT_OPTIONS_CLOSED = 3,
    FUSION_ATTRACT_SETTINGS_BUTTON = 0,
    FUSION_ATTRACT_SAVE_SLOT_BUTTON = 7,
    FUSION_ATTRACT_OPTION_COUNT = 4,
    FUSION_REDBRICK_COUNT = 9,
    FUSION_REDBRICK_COLUMNS = 3,
    FUSION_REDBRICK_SELECTED_OFFSET = 0x66c,
    FUSION_REDBRICK_RECORD_OFFSET = 0x670,
    FUSION_REDBRICK_RECORD_STRIDE = 0x1c,
    FUSION_REDBRICK_HIGHLIGHT_OFFSET = 0x10,
    FUSION_REDBRICK_VALUE_OFFSET = 0x18,
    FUSION_REDBRICK_SLOT_COUNT = 3,
    FUSION_REDBRICK_SLOT_RECORD_OFFSET = 0x794,
    FUSION_REDBRICK_SLOT_RECORD_STRIDE = 0x1c,
    FUSION_REDBRICK_SLOT_HIGHLIGHT_OFFSET = 0x10,
    FUSION_REDBRICK_SLOT_POWER_INDEX_OFFSET = 0x18,
    FUSION_REDBRICK_AVAILABLE_OFFSET = 0x7f8,
    FUSION_REDBRICK_AVAILABLE_COUNT_OFFSET = 0x820,
    FUSION_REDBRICK_BACK_BUTTON = 11,
    FUSION_REDBRICK_BUY_BUTTON = 12,
    FUSION_REDBRICK_PLAY_BUTTON = 16,
    FUSION_PRIZE_STATE_OFFSET = 0x658,
    FUSION_PRIZE_FREE_DRAW_OFFSET = 0x64c,
    FUSION_PRIZE_SELECTED_OFFSET = 0x670,
    FUSION_PRIZE_CHOICE_ELEMENT_OFFSET = 0x68c,
    FUSION_PRIZE_CHOICE_STRIDE = 0x24,
    FUSION_PRIZE_CHOICE_COUNT = 3,
    FUSION_PRIZE_PICK_STATE = 4,
    FUSION_PRIZE_PAID_REVEAL_STATE = 5,
    FUSION_PRIZE_FREE_REVEAL_STATE = 6,
    FUSION_PRIZE_DRAW_BUTTON = 11,
    FUSION_FOCUS_EXTRA_COUNT =
        FUSION_REDBRICK_COUNT + FUSION_REDBRICK_SLOT_COUNT,
    FUSION_FOCUS_SLOT_COUNT =
        FUSION_UI_BUTTON_COUNT + FUSION_FOCUS_EXTRA_COUNT,
    FUSION_FOCUS_MODULE_COUNT = 12,
};

typedef struct FusionFocusState {
    void *module;
    void *element[FUSION_FOCUS_SLOT_COUNT];
    float base_scale[FUSION_FOCUS_SLOT_COUNT][2];
    int focused_slot;
} FusionFocusState;

static FusionFocusState fusion_focus_states[FUSION_FOCUS_MODULE_COUNT];
static unsigned int fusion_focus_replacement;
static float fusion_focus_phase;
static void *fusion_last_top_module;
static bool fusion_level_row_focused;
static bool fusion_redbrick_grid_focused;
static int fusion_redbrick_slot_focus = -1;
static int fusion_redbrick_grid_slot = -1;
static int fusion_prize_choice;
static int fusion_last_settings_screen = -1;
static bool fusion_settings_active;
static int fusion_attract_option_focus = -1;
static bool fusion_audio_settings_reported;
static unsigned int fusion_hit_sfx_count;

/* Top-to-bottom order of the four icons in Attract's opened Settings rail. */
static const uint8_t fusion_attract_option_buttons[
    FUSION_ATTRACT_OPTION_COUNT] = {6, 4, 2, 1};

static void determine_vita_ui_resolution(void) {
    /* 0=low, 1=medium, 2=high; the 1057 OBB contains medium and high UI FIBs. */
    *fusion_ui_resolution = 1;
}

static FusionFocusState *get_fusion_focus_state(void *module) {
    FusionFocusState *free_state = NULL;
    for (unsigned int i = 0; i < FUSION_FOCUS_MODULE_COUNT; ++i) {
        if (fusion_focus_states[i].module == module) {
            return &fusion_focus_states[i];
        }
        if (!fusion_focus_states[i].module && !free_state) {
            free_state = &fusion_focus_states[i];
        }
    }
    if (!free_state) {
        free_state = &fusion_focus_states[
            fusion_focus_replacement++ % FUSION_FOCUS_MODULE_COUNT];
    }
    memset(free_state, 0, sizeof(*free_state));
    free_state->module = module;
    free_state->focused_slot = -1;
    return free_state;
}

static void clear_fusion_focus(void *module) {
    if (!module || !fusion_flash_set_scale) {
        return;
    }
    FusionFocusState *state = get_fusion_focus_state(module);
    int slot = state->focused_slot;
    if (slot >= 0 && slot < FUSION_FOCUS_SLOT_COUNT &&
        state->element[slot]) {
        fusion_flash_set_scale(state->element[slot],
                               state->base_scale[slot]);
    }
    state->focused_slot = -1;
}

static void forget_fusion_focus(void *module) {
    if (!module) {
        return;
    }
    for (unsigned int i = 0; i < FUSION_FOCUS_MODULE_COUNT; ++i) {
        if (fusion_focus_states[i].module == module) {
            memset(&fusion_focus_states[i], 0,
                   sizeof(fusion_focus_states[i]));
            fusion_focus_states[i].focused_slot = -1;
            return;
        }
    }
}

static void set_fusion_element_focus(void *module, uint32_t slot,
                                     void *element, bool focused,
                                     float pulse) {
    if (!fusion_flash_get_scale || !fusion_flash_set_scale ||
        !element || slot >= FUSION_FOCUS_SLOT_COUNT) {
        return;
    }

    FusionFocusState *state = get_fusion_focus_state(module);
    if (state->element[slot] != element) {
        /* SetScale writes the live multiplier, not the authored transform. */
        const float *base = fusion_flash_get_scale(element);
        if (!base || !isfinite(base[0]) || !isfinite(base[1])) {
            return;
        }
        state->element[slot] = element;
        state->base_scale[slot][0] = base[0];
        state->base_scale[slot][1] = base[1];
    }

    if (focused && state->focused_slot != (int)slot) {
        int previous = state->focused_slot;
        if (previous >= 0 && previous < FUSION_FOCUS_SLOT_COUNT &&
            state->element[previous]) {
            fusion_flash_set_scale(state->element[previous],
                                   state->base_scale[previous]);
        }
        state->focused_slot = (int)slot;
    } else if (!focused && state->focused_slot == (int)slot) {
        state->focused_slot = -1;
    }

    float scale[2] = {
        state->base_scale[slot][0],
        state->base_scale[slot][1],
    };
    if (focused) {
        scale[0] *= pulse;
        scale[1] *= pulse;
    }
    fusion_flash_set_scale(element, scale);
}

static void *fusion_button_element(void *module, uint32_t button) {
    if (button >= FUSION_UI_BUTTON_COUNT) {
        return NULL;
    }
    uint8_t *record = (uint8_t *)module +
                      button * FUSION_UI_BUTTON_STRIDE;
    return *(void **)(record + FUSION_UI_BUTTON_ELEMENT_OFFSET);
}

static bool fusion_element_visible(void *element) {
    return element &&
           (!fusion_flash_is_visible ||
            fusion_flash_is_visible(element, true));
}

static bool fusion_button_visible(void *module, uint32_t button) {
    return fusion_element_visible(fusion_button_element(module, button));
}

static void set_fusion_button_focus(void *module, uint32_t button,
                                    bool focused, float pulse) {
    set_fusion_element_focus(module, button,
                             fusion_button_element(module, button),
                             focused, pulse);
}

static bool fusion_button_position(void *module, uint32_t button,
                                   float *x, float *y) {
    if (!fusion_button_visible(module, button)) {
        return false;
    }
    const float *position = (const float *)((const uint8_t *)module +
        FUSION_UI_BUTTON_POSITION_OFFSET + button * 3 * sizeof(float));
    if (!isfinite(position[0]) || !isfinite(position[1]) ||
        (position[0] == 0.0f && position[1] == 0.0f)) {
        return false;
    }
    *x = position[0];
    *y = position[1];
    return true;
}

static bool is_fusion_module_type(void *module, void **instance,
                                  uintptr_t update_method) {
    if (!module) {
        return false;
    }
    if (instance && *instance == module) {
        return true;
    }
    const uintptr_t *vtable = *(const uintptr_t **)module;
    return vtable && update_method &&
           vtable[FUSION_UI_MODULE_UPDATE_VTABLE_SLOT] == update_method;
}

static bool is_character_select_module(void *module) {
    return is_fusion_module_type(module, fusion_character_select_instance,
                                 fusion_character_select_update);
}

static bool is_character_level_module(void *module) {
    return is_fusion_module_type(module, fusion_character_level_instance,
                                 fusion_character_level_update);
}

static bool is_level_select_module(void *module) {
    return is_fusion_module_type(module, fusion_level_select_instance,
                                 fusion_level_select_update);
}

static bool is_frontend_module(void *module) {
    return is_fusion_module_type(module, fusion_frontend_instance,
                                 fusion_frontend_update);
}

static bool is_attract_module(void *module) {
    return is_fusion_module_type(module, fusion_attract_instance,
                                 fusion_attract_update);
}

static bool is_redbrick_module(void *module) {
    return is_fusion_module_type(module, fusion_redbrick_instance,
                                 fusion_redbrick_update);
}

static bool is_prize_picker_module(void *module) {
    return is_fusion_module_type(module, fusion_prize_picker_instance,
                                 fusion_prize_picker_update);
}

static const char *fusion_module_type_name(void *module) {
    if (is_character_select_module(module)) {
        return "character-select";
    }
    if (is_character_level_module(module)) {
        return "power-select";
    }
    if (is_level_select_module(module)) {
        return "level-select";
    }
    if (is_frontend_module(module)) {
        return "front-end";
    }
    if (is_attract_module(module)) {
        return "attract-menu";
    }
    if (is_redbrick_module(module)) {
        return "red-brick-power-select";
    }
    if (is_prize_picker_module(module)) {
        return "daily-draw";
    }
    return "generic";
}

static bool is_settings_host_module(void *module) {
    return is_frontend_module(module) || is_attract_module(module);
}

static int fusion_settings_screen(void *module) {
    size_t offset = is_attract_module(module)
        ? FUSION_ATTRACT_SCREEN_OFFSET
        : FUSION_FRONTEND_SCREEN_OFFSET;
    return *(const int *)((const uint8_t *)module + offset);
}

static int fusion_attract_options_state(void *module) {
    return *(const int *)((const uint8_t *)module +
                          FUSION_ATTRACT_OPTIONS_STATE_OFFSET);
}

static bool is_attract_option_button(uint32_t button) {
    for (int index = 0; index < FUSION_ATTRACT_OPTION_COUNT; ++index) {
        if (fusion_attract_option_buttons[index] == button) {
            return true;
        }
    }
    return false;
}

static bool fusion_button_allowed(void *module, uint32_t button) {
    if (is_character_level_module(module)) {
        return button == FUSION_LEVEL_BACK_BUTTON ||
               button == FUSION_LEVEL_BUY_BUTTON ||
               button == FUSION_LEVEL_PLAY_BUTTON;
    }
    if (is_redbrick_module(module)) {
        return button == FUSION_REDBRICK_BACK_BUTTON ||
               button == FUSION_REDBRICK_BUY_BUTTON ||
               button == FUSION_REDBRICK_PLAY_BUTTON;
    }
    if (is_attract_module(module) &&
        fusion_settings_screen(module) == FUSION_ATTRACT_MAIN_SCREEN) {
        if (fusion_settings_active) {
            return is_attract_option_button(button);
        }
        /* Child icons report visible while their parent rail is closed. */
        return button != FUSION_ATTRACT_SAVE_SLOT_BUTTON &&
               !is_attract_option_button(button);
    }
    if (is_settings_host_module(module)) {
        int screen = fusion_settings_screen(module);
        if (screen == FUSION_FRONTEND_OPTIONS_SCREEN ||
            fusion_settings_active) {
            return (button >= 1 && button <= 6) ||
                   button == FUSION_FRONTEND_BACK_BUTTON;
        }
    }
    return true;
}

static uint32_t nearest_visible_button(void *module, float x, float y) {
    uint32_t best_button = FUSION_UI_BUTTON_COUNT;
    float best_score = FLT_MAX;
    for (uint32_t button = 0; button < FUSION_UI_BUTTON_COUNT; ++button) {
        float candidate_x;
        float candidate_y;
        if (!fusion_button_allowed(module, button) ||
            !fusion_button_position(module, button,
                                    &candidate_x, &candidate_y)) {
            continue;
        }
        float dx = candidate_x - x;
        float dy = candidate_y - y;
        float score = dx * dx + dy * dy;
        if (score < best_score) {
            best_score = score;
            best_button = button;
        }
    }
    return best_button;
}

static uint32_t move_fusion_button_focus(void *module, uint32_t current,
                                         VitaUIDirection direction) {
    uint32_t original = current;
    float current_x;
    float current_y;
    if (!fusion_button_allowed(module, current) ||
        !fusion_button_position(module, current, &current_x, &current_y)) {
        current = nearest_visible_button(module, 480.0f, 272.0f);
        if (!fusion_button_position(module, current,
                                    &current_x, &current_y)) {
            return original;
        }
    }
    if (direction == VITA_UI_DIRECTION_NONE) {
        return current;
    }

    uint32_t best_button = FUSION_UI_BUTTON_COUNT;
    float best_score = FLT_MAX;
    for (uint32_t button = 0; button < FUSION_UI_BUTTON_COUNT; ++button) {
        float x;
        float y;
        if (button == current ||
            !fusion_button_allowed(module, button) ||
            !fusion_button_position(module, button, &x, &y)) {
            continue;
        }
        float dx = x - current_x;
        float dy = y - current_y;
        float primary = 0.0f;
        float perpendicular = 0.0f;
        switch (direction) {
        case VITA_UI_DIRECTION_LEFT:
            primary = -dx;
            perpendicular = fabsf(dy);
            break;
        case VITA_UI_DIRECTION_RIGHT:
            primary = dx;
            perpendicular = fabsf(dy);
            break;
        case VITA_UI_DIRECTION_UP:
            primary = -dy;
            perpendicular = fabsf(dx);
            break;
        case VITA_UI_DIRECTION_DOWN:
            primary = dy;
            perpendicular = fabsf(dx);
            break;
        default:
            break;
        }
        if (primary <= 1.0f) {
            continue;
        }
        float score = primary +
            4.0f * perpendicular * perpendicular / (primary + 1.0f);
        if (score < best_score) {
            best_score = score;
            best_button = button;
        }
    }

    /* Wrap to the opposite edge if a row or column has no forward item. */
    if (best_button == FUSION_UI_BUTTON_COUNT) {
        for (uint32_t button = 0; button < FUSION_UI_BUTTON_COUNT; ++button) {
            float x;
            float y;
            if (button == current ||
                !fusion_button_allowed(module, button) ||
                !fusion_button_position(module, button, &x, &y)) {
                continue;
            }
            float edge;
            float perpendicular;
            if (direction == VITA_UI_DIRECTION_LEFT) {
                edge = -x;
                perpendicular = fabsf(y - current_y);
            } else if (direction == VITA_UI_DIRECTION_RIGHT) {
                edge = x;
                perpendicular = fabsf(y - current_y);
            } else if (direction == VITA_UI_DIRECTION_UP) {
                edge = -y;
                perpendicular = fabsf(x - current_x);
            } else {
                edge = y;
                perpendicular = fabsf(x - current_x);
            }
            float score = edge * 4096.0f + perpendicular;
            if (score < best_score) {
                best_score = score;
                best_button = button;
            }
        }
    }
    return best_button < FUSION_UI_BUTTON_COUNT ? best_button : current;
}

static void *fusion_level_element(void *module, int level) {
    if (level < 0 || level >= FUSION_LEVEL_ITEM_COUNT) {
        return NULL;
    }
    return *(void **)((uint8_t *)module +
                      FUSION_LEVEL_ITEM_ELEMENT_OFFSET +
                      (unsigned int)level * sizeof(void *));
}

static uint8_t *fusion_redbrick_record(void *module, int index) {
    if (index < 0 || index >= FUSION_REDBRICK_COUNT) {
        return NULL;
    }
    return (uint8_t *)module + FUSION_REDBRICK_RECORD_OFFSET +
           (unsigned int)index * FUSION_REDBRICK_RECORD_STRIDE;
}

static void *fusion_redbrick_element(void *module, int index) {
    uint8_t *record = fusion_redbrick_record(module, index);
    return record ? *(void **)record : NULL;
}

static void *fusion_redbrick_highlight(void *module, int index) {
    uint8_t *record = fusion_redbrick_record(module, index);
    return record
        ? *(void **)(record + FUSION_REDBRICK_HIGHLIGHT_OFFSET)
        : NULL;
}

static int fusion_redbrick_value(void *module, int index) {
    uint8_t *record = fusion_redbrick_record(module, index);
    return record
        ? *(const int *)(record + FUSION_REDBRICK_VALUE_OFFSET)
        : -1;
}

static int fusion_redbrick_index(void *module) {
    int selected = *(const int *)((const uint8_t *)module +
                                  FUSION_REDBRICK_SELECTED_OFFSET);
    for (int index = 0; index < FUSION_REDBRICK_COUNT; ++index) {
        if (fusion_redbrick_value(module, index) == selected) {
            return index;
        }
    }
    return 0;
}

static int fusion_redbrick_index_for_value(void *module, int red_brick) {
    for (int index = 0; index < FUSION_REDBRICK_COUNT; ++index) {
        if (fusion_redbrick_value(module, index) == red_brick) {
            return index;
        }
    }
    return -1;
}

static bool fusion_redbrick_position(void *module, int index,
                                     float *x, float *y) {
    void *element = fusion_redbrick_element(module, index);
    if (!element || !fusion_element_visible(element) ||
        !fusion_flash_get_world_matrix) {
        return false;
    }
    const float *matrix = fusion_flash_get_world_matrix(element);
    if (!matrix || !isfinite(matrix[12]) || !isfinite(matrix[13])) {
        return false;
    }
    *x = matrix[12];
    *y = matrix[13];
    return true;
}

static void *fusion_character_portrait_element(void *module, int slot) {
    if (!module || slot < 0 ||
        slot >= FUSION_CHARACTER_PORTRAIT_COUNT) {
        return NULL;
    }
    return *(void **)((uint8_t *)module +
                      FUSION_CHARACTER_PORTRAIT_ELEMENT_OFFSET +
                      (unsigned int)slot *
                          FUSION_CHARACTER_PORTRAIT_STRIDE);
}

static bool fusion_character_portrait_position(void *module, int slot,
                                                float *x, float *y) {
    void *element = fusion_character_portrait_element(module, slot);
    if (!element || !fusion_element_visible(element) ||
        !fusion_flash_get_world_matrix) {
        return false;
    }
    const float *matrix = fusion_flash_get_world_matrix(element);
    if (!matrix || !isfinite(matrix[12]) || !isfinite(matrix[13])) {
        return false;
    }
    *x = matrix[12];
    *y = matrix[13];
    return true;
}

static int fusion_character_slot_for_character(void *module, int character) {
    if (!fusion_map_portrait_id) {
        return -1;
    }
    int page = *(const int *)((const uint8_t *)module +
                              FUSION_CHARACTER_PAGE_OFFSET);
    for (int slot = 0; slot < FUSION_CHARACTER_PORTRAIT_COUNT; ++slot) {
        if (fusion_map_portrait_id(
                page * FUSION_CHARACTER_PORTRAIT_COUNT + slot) ==
            character) {
            return slot;
        }
    }
    return -1;
}

static int move_fusion_character_portrait(void *module, int current,
                                           VitaUIDirection direction) {
    float current_x;
    float current_y;
    if (current < 0 ||
        !fusion_character_portrait_position(module, current,
                                            &current_x, &current_y)) {
        return current;
    }
    int best = -1;
    float best_score = FLT_MAX;
    for (int slot = 0; slot < FUSION_CHARACTER_PORTRAIT_COUNT; ++slot) {
        float x;
        float y;
        if (slot == current ||
            !fusion_character_portrait_position(module, slot, &x, &y)) {
            continue;
        }
        float dx = x - current_x;
        float dy = y - current_y;
        float primary;
        switch (direction) {
            case VITA_UI_DIRECTION_LEFT:
                primary = -dx;
                break;
            case VITA_UI_DIRECTION_RIGHT:
                primary = dx;
                break;
            case VITA_UI_DIRECTION_UP:
                primary = -dy;
                break;
            case VITA_UI_DIRECTION_DOWN:
                primary = dy;
                break;
            default:
                return current;
        }
        if (primary <= 1.0f) {
            continue;
        }
        /* Choose the closest visible portrait in the requested half-plane. */
        float score = dx * dx + dy * dy;
        if (score < best_score) {
            best_score = score;
            best = slot;
        }
    }
    return best >= 0 ? best : current;
}

static int move_fusion_redbrick_grid_focus(void *module, int current,
                                            VitaUIDirection direction) {
    if (direction == VITA_UI_DIRECTION_NONE) {
        return current;
    }

    float current_x;
    float current_y;
    if (fusion_redbrick_position(module, current,
                                 &current_x, &current_y)) {
        int best = -1;
        float best_score = FLT_MAX;
        for (int index = 0; index < FUSION_REDBRICK_COUNT; ++index) {
            float x;
            float y;
            if (index == current ||
                !fusion_redbrick_position(module, index, &x, &y)) {
                continue;
            }
            float dx = x - current_x;
            float dy = y - current_y;
            float primary = 0.0f;
            float perpendicular = 0.0f;
            switch (direction) {
            case VITA_UI_DIRECTION_LEFT:
                primary = -dx;
                perpendicular = fabsf(dy);
                break;
            case VITA_UI_DIRECTION_RIGHT:
                primary = dx;
                perpendicular = fabsf(dy);
                break;
            case VITA_UI_DIRECTION_UP:
                primary = -dy;
                perpendicular = fabsf(dx);
                break;
            case VITA_UI_DIRECTION_DOWN:
                primary = dy;
                perpendicular = fabsf(dx);
                break;
            default:
                break;
            }
            if (primary <= 1.0f) {
                continue;
            }
            float score = primary +
                4.0f * perpendicular * perpendicular / (primary + 1.0f);
            if (score < best_score) {
                best_score = score;
                best = index;
            }
        }
        if (best >= 0) {
            return best;
        }
        return current;
    }

    /* Pinned-layout fallback if a Flash element has not built its matrix yet. */
    if (direction == VITA_UI_DIRECTION_LEFT &&
        current % FUSION_REDBRICK_COLUMNS != 0) {
        return current - 1;
    }
    if (direction == VITA_UI_DIRECTION_RIGHT &&
        current % FUSION_REDBRICK_COLUMNS !=
            FUSION_REDBRICK_COLUMNS - 1) {
        return current + 1;
    }
    if (direction == VITA_UI_DIRECTION_UP &&
        current >= FUSION_REDBRICK_COLUMNS) {
        return current - FUSION_REDBRICK_COLUMNS;
    }
    if (direction == VITA_UI_DIRECTION_DOWN &&
        current + FUSION_REDBRICK_COLUMNS < FUSION_REDBRICK_COUNT) {
        return current + FUSION_REDBRICK_COLUMNS;
    }
    return current;
}

static uint8_t *fusion_redbrick_slot_record(void *module, int slot) {
    if (slot < 0 || slot >= FUSION_REDBRICK_SLOT_COUNT) {
        return NULL;
    }
    return (uint8_t *)module + FUSION_REDBRICK_SLOT_RECORD_OFFSET +
           (unsigned int)slot * FUSION_REDBRICK_SLOT_RECORD_STRIDE;
}

static void *fusion_redbrick_slot_element(void *module, int slot) {
    uint8_t *record = fusion_redbrick_slot_record(module, slot);
    return record ? *(void **)record : NULL;
}

static void *fusion_redbrick_slot_highlight(void *module, int slot) {
    uint8_t *record = fusion_redbrick_slot_record(module, slot);
    return record
        ? *(void **)(record + FUSION_REDBRICK_SLOT_HIGHLIGHT_OFFSET)
        : NULL;
}

static int fusion_redbrick_available_count(void *module) {
    int count = *(const int *)((const uint8_t *)module +
                               FUSION_REDBRICK_AVAILABLE_COUNT_OFFSET);
    if (count < 0) {
        return 0;
    }
    if (count > FUSION_REDBRICK_COUNT + 1) {
        return FUSION_REDBRICK_COUNT + 1;
    }
    return count;
}

static int fusion_redbrick_available_value(void *module, int index) {
    int count = fusion_redbrick_available_count(module);
    if (index < 0 || index >= count) {
        return -1;
    }
    return *(const int *)((const uint8_t *)module +
                          FUSION_REDBRICK_AVAILABLE_OFFSET +
                          (unsigned int)index * sizeof(int));
}

static int fusion_redbrick_available_index(void *module, int red_brick) {
    int count = fusion_redbrick_available_count(module);
    for (int index = 0; index < count; ++index) {
        if (fusion_redbrick_available_value(module, index) == red_brick) {
            return index;
        }
    }
    return -1;
}

static int fusion_redbrick_slot_value(void *module, int slot) {
    uint8_t *record = fusion_redbrick_slot_record(module, slot);
    if (!record) {
        return -1;
    }
    int available_index = *(const int *)(
        record + FUSION_REDBRICK_SLOT_POWER_INDEX_OFFSET);
    return fusion_redbrick_available_value(module, available_index);
}

static bool fusion_redbrick_used_by_other_slot(void *module,
                                                int red_brick,
                                                int target_slot) {
    if (red_brick <= 0) {
        return false;
    }
    for (int slot = 0; slot < FUSION_REDBRICK_SLOT_COUNT; ++slot) {
        if (slot != target_slot &&
            fusion_redbrick_slot_value(module, slot) == red_brick) {
            return true;
        }
    }
    return false;
}

static bool fusion_redbrick_choice_available(void *module, int index,
                                               int target_slot) {
    int red_brick = fusion_redbrick_value(module, index);
    return red_brick > 0 &&
           (!fusion_redbrick_is_bought ||
            fusion_redbrick_is_bought((unsigned int)red_brick, true)) &&
           !fusion_redbrick_used_by_other_slot(
               module, red_brick, target_slot);
}

static int fusion_redbrick_first_available_grid_index(void *module,
                                                       int target_slot) {
    for (int index = 0; index < FUSION_REDBRICK_COUNT; ++index) {
        if (fusion_redbrick_choice_available(module, index, target_slot)) {
            return index;
        }
    }
    return -1;
}

static void set_fusion_redbrick_slot_highlight(void *module, int slot,
                                                bool focused) {
    void *highlight = fusion_redbrick_slot_highlight(module, slot);
    if (!highlight || !fusion_flash_set_visibility ||
        !fusion_flash_set_opacity) {
        return;
    }
    fusion_flash_set_visibility(highlight, focused);
    fusion_flash_set_opacity(highlight, focused ? 1.0f : 0.0f);
}

static void set_fusion_redbrick_grid_highlight(void *module, int index,
                                                bool focused) {
    void *highlight = fusion_redbrick_highlight(module, index);
    if (!highlight || !fusion_flash_set_visibility ||
        !fusion_flash_set_opacity) {
        return;
    }
    fusion_flash_set_visibility(highlight, focused);
    fusion_flash_set_opacity(highlight, focused ? 1.0f : 0.0f);
}

static void clear_fusion_redbrick_highlights(void *module) {
    if (!module) {
        return;
    }
    for (int slot = 0; slot < FUSION_REDBRICK_SLOT_COUNT; ++slot) {
        set_fusion_redbrick_slot_highlight(module, slot, false);
    }
    for (int index = 0; index < FUSION_REDBRICK_COUNT; ++index) {
        set_fusion_redbrick_grid_highlight(module, index, false);
    }
}

static void leave_fusion_redbrick_focus(void *module) {
    clear_fusion_redbrick_highlights(module);
    clear_fusion_focus(module);
    fusion_redbrick_grid_focused = false;
    fusion_redbrick_slot_focus = -1;
    fusion_redbrick_grid_slot = -1;
    controls_set_native_confirm_suppressed(false);
    controls_set_native_cancel_suppressed(false);
}

static void sync_custom_confirm_route(void) {
    controls_set_native_confirm_suppressed(
        fusion_level_row_focused || fusion_redbrick_grid_focused ||
        fusion_redbrick_slot_focus >= 0);
}

static bool fusion_redbrick_assign_slot(void *module, int slot,
                                        int red_brick) {
    if (!fusion_redbrick_set_up || red_brick <= 0 ||
        (fusion_redbrick_is_bought &&
         !fusion_redbrick_is_bought((unsigned int)red_brick, true)) ||
        fusion_redbrick_used_by_other_slot(module, red_brick, slot)) {
        return false;
    }

    int available_index = fusion_redbrick_available_index(module, red_brick);
    uint8_t *record = fusion_redbrick_slot_record(module, slot);
    if (!record || available_index < 0) {
        return false;
    }

    *(int *)(record + FUSION_REDBRICK_SLOT_POWER_INDEX_OFFSET) =
        available_index;
    /* Rebuild and activate the three-slot loadout through Fusion's own path. */
    fusion_redbrick_set_up(module);
    return fusion_redbrick_slot_value(module, slot) == red_brick;
}

static void *fusion_prize_choice_element(void *module, int choice) {
    if (choice < 0 || choice >= FUSION_PRIZE_CHOICE_COUNT) {
        return NULL;
    }
    return *(void **)((uint8_t *)module +
                      FUSION_PRIZE_CHOICE_ELEMENT_OFFSET +
                      (unsigned int)choice * FUSION_PRIZE_CHOICE_STRIDE);
}

/*
 * Android's UI modules poll Fusion's controller state, but flatten menu focus
 * into a Left/Right list. Run the original base update first, then use the
 * module's stored button positions for native four-way D-pad navigation
 * without synthesizing touch coordinates.
 *
 * The continuation trampoline preserves the exact (object, float) ARM ABI and
 * avoids rewriting/flushing the original code on every UI frame.
 */
static void vita_ui_module_update(void *module, float delta_time) {
    uint32_t old_button = *((const uint8_t *)module +
                            FUSION_UI_SELECTED_OFFSET);
    bool was_character = is_character_select_module(module);
    int character_before = was_character
        ? *(const int *)((const uint8_t *)module +
                        FUSION_CHARACTER_SELECTED_OFFSET)
        : -1;
    fusion_ui_module_update(module, delta_time);

    /* Only the top Fusion module may consume global one-shot Vita input. */
    if (fusion_get_current_module &&
        fusion_get_current_module() != module) {
        if (fusion_last_top_module == module) {
            fusion_last_top_module = NULL;
            fusion_level_row_focused = false;
            fusion_redbrick_grid_focused = false;
            fusion_redbrick_slot_focus = -1;
            fusion_redbrick_grid_slot = -1;
            controls_set_native_confirm_suppressed(false);
            controls_set_native_cancel_suppressed(false);
            controls_set_native_page_shoulders_suppressed(false);
            controls_set_native_ui_navigation_suppressed(false);
        }
        clear_fusion_focus(module);
        return;
    }

    bool top_module_changed = fusion_last_top_module != module;
    if (top_module_changed) {
        memset(heart_clip_roots, 0, sizeof(heart_clip_roots));
        /*
         * The old module has already completed its native transition by this
         * point. Drop cached pointers without calling Flash methods on UI
         * elements that may now be owned by an asynchronous teardown/load.
         */
        forget_fusion_focus(fusion_last_top_module);
        fusion_last_top_module = module;
        fusion_level_row_focused = false;
        fusion_redbrick_grid_focused = false;
        fusion_redbrick_slot_focus = -1;
        fusion_redbrick_grid_slot = -1;
        fusion_prize_choice = 1;
        fusion_last_settings_screen = -1;
        fusion_settings_active = false;
        fusion_attract_option_focus = -1;
        controls_set_native_confirm_suppressed(false);
        controls_set_native_cancel_suppressed(false);

        const uintptr_t *vtable = *(const uintptr_t **)module;
        uintptr_t update = vtable
            ? vtable[FUSION_UI_MODULE_UPDATE_VTABLE_SLOT] : 0;
        l_info("Fusion controller entered %s module: object=%p, "
               "vtable=%p, update=%p.",
               fusion_module_type_name(module), module, (const void *)vtable,
               (void *)update);
    }

    VitaUIDirection direction = controls_take_ui_direction();
    if (direction != VITA_UI_DIRECTION_NONE) {
        /*
         * UI_Module's dormant console helper also turns every D-pad edge into
         * a flat bottom-button move. Screen-specific modules then process the
         * same edge for portraits or panels, producing two simultaneous focus
         * targets. Keep the original update's animation work, but undo only
         * that base selection change before applying the correct screen path.
         * Touch selection is left untouched because it has no queued Vita
         * direction.
         */
        *((uint8_t *)module + FUSION_UI_SELECTED_OFFSET) =
            (uint8_t)old_button;
    }
    bool is_character = is_character_select_module(module);
    bool is_level = is_character_level_module(module);
    bool is_level_select = is_level_select_module(module);
    bool is_redbrick = is_redbrick_module(module);
    bool is_settings_host = is_settings_host_module(module);
    bool is_prize = is_prize_picker_module(module);
    controls_set_native_ui_navigation_suppressed(is_character);
    controls_set_native_page_shoulders_suppressed(
        is_character || is_level_select);
    /*
     * Red Brick owns Circle at every stage. Suppress Fusion's remapped native
     * Cancel before the next poll so one physical press cannot both close the
     * 3x3 grid and pop the whole module back to Character Select.
     */
    controls_set_native_cancel_suppressed(is_redbrick);

    if (!fusion_audio_settings_reported && is_settings_host &&
        fusion_get_active_options) {
        const uint8_t *options = (const uint8_t *)fusion_get_active_options();
        if (options) {
            unsigned int saved_sfx = options[0] & 0x0f;
            unsigned int saved_music = options[0] >> 4;
            unsigned int runtime_sfx = fusion_runtime_sound_volume
                ? *fusion_runtime_sound_volume : UINT8_MAX;
            l_info("Fusion audio settings: SFX=%u/10, music=%u/10, "
                   "runtime SFX=%u%s.",
                   saved_sfx, saved_music, runtime_sfx,
                   fusion_runtime_sound_volume ? "/10" : " (unavailable)");
            if (saved_sfx == 0) {
                l_warn("The active save has sound effects disabled; use the "
                       "Sound option in Settings to enable UI and hit SFX.");
            }
            fusion_audio_settings_reported = true;
        }
    }

    fusion_focus_phase += delta_time * 5.0f;
    if (fusion_focus_phase > 6.2831853f) {
        fusion_focus_phase = fmodf(fusion_focus_phase, 6.2831853f);
    }
    float pulse = 1.18f + 0.025f * sinf(fusion_focus_phase);
    bool show_focus = controls_using_controller();

    /*
     * Character Select's dormant native movement uses a sparse logical order,
     * so Down can jump diagonally or do nothing. Use the live Flash geometry
     * for every stick/D-pad direction and select the closest visible portrait
     * in that half-plane. Cross confirmation stays on Fusion's native path.
     */
    if (is_character) {
        clear_fusion_focus(module);
        if (direction != VITA_UI_DIRECTION_NONE &&
            fusion_character_select_portrait) {
            int current = fusion_character_slot_for_character(
                module, character_before);
            int target = move_fusion_character_portrait(
                module, current, direction);
            if (target >= 0) {
                int page = *(const int *)((const uint8_t *)module +
                    FUSION_CHARACTER_PAGE_OFFSET);
                int selected = fusion_character_select_portrait(
                    module, page, target);
                static const char *const names[] = {
                    "none", "up", "down", "left", "right"
                };
                l_info("Nearest-portrait navigation moved %s from slot %d "
                       "to slot %d on character page %d (result=%d).",
                       names[direction],
                       current, target, page, selected);
            }
        }
        if (fusion_character_select_handle_button) {
            int page = controls_take_character_page_press();
            if (page != 0) {
                uint32_t action = page < 0
                    ? FUSION_UI_CHARACTER_PAGE_PREVIOUS
                    : FUSION_UI_CHARACTER_PAGE_NEXT;
                fusion_character_select_handle_button(module, action);
                l_info("Vita %c changed the character-selection page through "
                       "Fusion action %u.", page < 0 ? 'L' : 'R', action);
            }
        }
        if (controls_take_cancel_press()) {
            ninjago_game_back();
            l_info("Circle sent native Android Back to Character Select.");
        }
        if (controls_take_confirm_press()) {
            l_info("Cross delegated to Character Select's native portrait "
                   "controller path.");
        }
        return;
    }

    /*
     * Level Select presents Dojo, Arena, and Temple Gallery as a native
     * three-item horizontal carousel. GotoNearestLevel recomputes the nearest
     * item from the current Flash geometry; calling it after Module_Update can
     * therefore rediscover the centered card and merely refresh its title.
     * Drive Level_SetAutoScroll with an explicit adjacent target instead. It
     * owns the native animation, image update, and final selected-level state.
     */
    if (is_level_select && fusion_level_select_move) {
        int page = controls_take_character_page_press();
        if (page != 0) {
            int selected = *(const int *)((const uint8_t *)module +
                FUSION_LEVEL_CAROUSEL_SELECTED_OFFSET);
            int pending = *(const int *)((const uint8_t *)module +
                FUSION_LEVEL_CAROUSEL_PENDING_OFFSET);
            bool scrolling = *((const uint8_t *)module +
                FUSION_LEVEL_CAROUSEL_SCROLLING_OFFSET) != 0;
            int base = scrolling && pending >= 0 &&
                       pending < FUSION_LEVEL_CAROUSEL_COUNT
                ? pending : selected;
            int target = base + page;
            if (target < 0) {
                target = 0;
            } else if (target >= FUSION_LEVEL_CAROUSEL_COUNT) {
                target = FUSION_LEVEL_CAROUSEL_COUNT - 1;
            }
            if (target != base) {
                fusion_level_select_move(module, target);
            }
            l_info("Vita %c requested level carousel %s: selected=%d, "
                   "pending=%d, scrolling=%d, target=%d.",
                   page < 0 ? 'L' : 'R', page < 0 ? "left" : "right",
                   selected, pending, scrolling ? 1 : 0, target);
        }
    }

    /* The daily draw has three real prize panels, not UI_Module buttons. */
    if (is_prize) {
        int state = *(const int *)((const uint8_t *)module +
                                  FUSION_PRIZE_STATE_OFFSET);
        if (state == FUSION_PRIZE_PICK_STATE) {
            int previous = fusion_prize_choice;
            if ((direction == VITA_UI_DIRECTION_LEFT ||
                 direction == VITA_UI_DIRECTION_UP) &&
                fusion_prize_choice > 0) {
                fusion_prize_choice--;
            } else if ((direction == VITA_UI_DIRECTION_RIGHT ||
                        direction == VITA_UI_DIRECTION_DOWN) &&
                       fusion_prize_choice + 1 <
                           FUSION_PRIZE_CHOICE_COUNT) {
                fusion_prize_choice++;
            }
            if (previous != fusion_prize_choice) {
                set_fusion_element_focus(
                    module, FUSION_UI_BUTTON_COUNT + (uint32_t)previous,
                    fusion_prize_choice_element(module, previous),
                    false, 1.0f);
                l_info("Daily-draw focus moved from prize %d to %d.",
                       previous + 1, fusion_prize_choice + 1);
            }
            set_fusion_element_focus(
                module,
                FUSION_UI_BUTTON_COUNT + (uint32_t)fusion_prize_choice,
                fusion_prize_choice_element(module, fusion_prize_choice),
                show_focus, pulse);
        } else {
            clear_fusion_focus(module);
        }

        if (controls_take_cancel_press()) {
            ninjago_game_back();
            l_info("Circle sent native Android Back to the daily draw.");
        }
        if (controls_take_confirm_press()) {
            if (state == FUSION_PRIZE_PICK_STATE) {
                *((int *)((uint8_t *)module +
                          FUSION_PRIZE_SELECTED_OFFSET)) =
                    fusion_prize_choice;
                bool free_draw = *((const uint8_t *)module +
                                   FUSION_PRIZE_FREE_DRAW_OFFSET) != 0;
                *((int *)((uint8_t *)module +
                          FUSION_PRIZE_STATE_OFFSET)) = free_draw
                    ? FUSION_PRIZE_FREE_REVEAL_STATE
                    : FUSION_PRIZE_PAID_REVEAL_STATE;
                clear_fusion_focus(module);
                l_info("Cross chose daily-draw prize %d through Fusion's "
                       "%s reveal state.", fusion_prize_choice + 1,
                       free_draw ? "free" : "token");
            } else if (fusion_button_visible(
                           module, FUSION_PRIZE_DRAW_BUTTON)) {
                uintptr_t *vtable = *(uintptr_t **)module;
                FusionUIHandleButton handle_button = vtable
                    ? (FusionUIHandleButton)vtable[15] : NULL;
                if (handle_button) {
                    handle_button(module, FUSION_PRIZE_DRAW_BUTTON);
                    l_info("Cross activated the daily-draw start button.");
                }
            } else {
                l_info("Cross ignored while the daily draw is animating.");
            }
        }
        return;
    }

    uint32_t selected_button = *((const uint8_t *)module +
                                 FUSION_UI_SELECTED_OFFSET);
    if (top_module_changed && is_redbrick && fusion_redbrick_select &&
        fusion_redbrick_set_up) {
        /*
         * Enter the actual three-slot loadout immediately. Requiring a
         * spatial Up edge from the bottom Play button left controller users
         * cycling Back/Buy/Play without ever reaching the Red Brick slots.
         */
        fusion_redbrick_slot_focus = 0;
        selected_button = FUSION_REDBRICK_PLAY_BUTTON;
        *((uint8_t *)module + FUSION_UI_SELECTED_OFFSET) =
            FUSION_REDBRICK_PLAY_BUTTON;
        l_info("Red Brick controller focus started on loadout slot 1; "
               "Cross opens its power grid.");
    }
    if (is_attract_module(module)) {
        int screen = fusion_settings_screen(module);
        int options_state = fusion_attract_options_state(module);
        bool options_active = screen == FUSION_ATTRACT_MAIN_SCREEN &&
            (options_state == 0 ||
             options_state == FUSION_ATTRACT_OPTIONS_OPEN);
        if (options_active) {
            fusion_settings_active = true;
        } else {
            fusion_settings_active = false;
            fusion_attract_option_focus = -1;
        }
        if (screen != fusion_last_settings_screen) {
            clear_fusion_focus(module);
            fusion_last_settings_screen = screen;
            l_info("Fusion attract-menu controller entered screen %d.",
                   screen);
        }
    } else if (is_frontend_module(module)) {
        int screen = fusion_settings_screen(module);
        if (screen == FUSION_FRONTEND_OPTIONS_SCREEN) {
            fusion_settings_active = true;
        } else if (fusion_last_settings_screen ==
                       FUSION_FRONTEND_OPTIONS_SCREEN) {
            fusion_settings_active = false;
        }
        if (screen != fusion_last_settings_screen) {
            clear_fusion_focus(module);
            fusion_last_settings_screen = screen;
            l_info("Fusion front-end controller entered screen %d.", screen);
        }
    }

    /*
     * Attract's Settings panel is an animated rail on screen 2, not screen 5.
     * Its four real button records are 6/4/2/1 from top to bottom. Keep a
     * private vertical focus so the profile, Play, and daily-draw buttons
     * cannot receive the same D-pad edge while this rail is open.
     */
    if (is_attract_module(module) && fusion_settings_active) {
        int previous = fusion_attract_option_focus;
        if (direction == VITA_UI_DIRECTION_UP) {
            if (fusion_attract_option_focus < 0) {
                fusion_attract_option_focus =
                    FUSION_ATTRACT_OPTION_COUNT - 1;
            } else if (fusion_attract_option_focus > 0) {
                fusion_attract_option_focus--;
            }
        } else if (direction == VITA_UI_DIRECTION_DOWN) {
            if (fusion_attract_option_focus < 0) {
                fusion_attract_option_focus = 0;
            } else if (fusion_attract_option_focus + 1 <
                       FUSION_ATTRACT_OPTION_COUNT) {
                fusion_attract_option_focus++;
            }
        }
        if (fusion_attract_option_focus >= 0) {
            selected_button = fusion_attract_option_buttons[
                fusion_attract_option_focus];
            *((uint8_t *)module + FUSION_UI_SELECTED_OFFSET) =
                (uint8_t)selected_button;
        }
        if (previous != fusion_attract_option_focus) {
            l_info("Settings focus moved to left-rail item %d (action %u).",
                   fusion_attract_option_focus + 1, selected_button);
        }
        /* Left/Right are intentionally contained by the modal rail too. */
        direction = VITA_UI_DIRECTION_NONE;
    }

    if (is_redbrick && fusion_redbrick_select && fusion_redbrick_set_up) {
        int index = fusion_redbrick_index(module);
        if (fusion_redbrick_grid_focused) {
            int previous = index;
            index = move_fusion_redbrick_grid_focus(
                module, index, direction);
            if (fusion_redbrick_grid_focused && previous != index) {
                set_fusion_redbrick_grid_highlight(
                    module, previous, false);
                fusion_redbrick_select(module,
                                       fusion_redbrick_value(module, index));
                set_fusion_redbrick_grid_highlight(
                    module, index, show_focus);
                float x;
                float y;
                if (fusion_redbrick_position(module, index, &x, &y)) {
                    l_info("Red-brick power focus moved from item %d to %d "
                           "at Flash position (%.1f, %.1f).",
                           previous + 1, index + 1, x, y);
                } else {
                    l_info("Red-brick power focus moved from item %d to %d "
                           "through the 3x3 fallback.",
                           previous + 1, index + 1);
                }
            }
            direction = VITA_UI_DIRECTION_NONE;
        } else if (fusion_redbrick_slot_focus >= 0) {
            int previous = fusion_redbrick_slot_focus;
            if ((direction == VITA_UI_DIRECTION_UP ||
                 direction == VITA_UI_DIRECTION_LEFT) &&
                fusion_redbrick_slot_focus > 0) {
                fusion_redbrick_slot_focus--;
            } else if ((direction == VITA_UI_DIRECTION_DOWN ||
                        direction == VITA_UI_DIRECTION_RIGHT) &&
                       fusion_redbrick_slot_focus + 1 <
                           FUSION_REDBRICK_SLOT_COUNT) {
                fusion_redbrick_slot_focus++;
            } else if (direction == VITA_UI_DIRECTION_LEFT &&
                       fusion_redbrick_slot_focus == 0) {
                set_fusion_redbrick_slot_highlight(
                    module, fusion_redbrick_slot_focus, false);
                fusion_redbrick_slot_focus = -1;
                selected_button = FUSION_REDBRICK_BACK_BUTTON;
                *((uint8_t *)module + FUSION_UI_SELECTED_OFFSET) =
                    (uint8_t)selected_button;
                l_info("Red Brick loadout focus moved from slot 1 to Back.");
            } else if (direction == VITA_UI_DIRECTION_RIGHT &&
                       fusion_redbrick_slot_focus + 1 ==
                           FUSION_REDBRICK_SLOT_COUNT) {
                set_fusion_redbrick_slot_highlight(
                    module, fusion_redbrick_slot_focus, false);
                fusion_redbrick_slot_focus = -1;
                selected_button = FUSION_REDBRICK_BUY_BUTTON;
                *((uint8_t *)module + FUSION_UI_SELECTED_OFFSET) =
                    (uint8_t)selected_button;
                l_info("Red Brick loadout focus moved from slot 3 to Buy.");
            } else if (direction == VITA_UI_DIRECTION_DOWN &&
                       fusion_redbrick_slot_focus + 1 ==
                           FUSION_REDBRICK_SLOT_COUNT) {
                set_fusion_redbrick_slot_highlight(
                    module, fusion_redbrick_slot_focus, false);
                fusion_redbrick_slot_focus = -1;
                selected_button = FUSION_REDBRICK_PLAY_BUTTON;
                *((uint8_t *)module + FUSION_UI_SELECTED_OFFSET) =
                    (uint8_t)selected_button;
                l_info("D-pad Down moved Red Brick loadout focus to Play.");
            }
            if (fusion_redbrick_slot_focus >= 0 &&
                previous != fusion_redbrick_slot_focus) {
                set_fusion_redbrick_slot_highlight(
                    module, previous, false);
                set_fusion_redbrick_slot_highlight(
                    module, fusion_redbrick_slot_focus, show_focus);
                l_info("Red Brick loadout focus moved from slot %d to %d.",
                       previous + 1, fusion_redbrick_slot_focus + 1);
            }
            direction = VITA_UI_DIRECTION_NONE;
        } else if (direction == VITA_UI_DIRECTION_RIGHT &&
                   selected_button == FUSION_REDBRICK_BACK_BUTTON) {
            fusion_redbrick_slot_focus = 0;
            set_fusion_redbrick_slot_highlight(module, 0, show_focus);
            l_info("D-pad Right entered Red Brick loadout slot 1 from Back.");
            direction = VITA_UI_DIRECTION_NONE;
        } else if (direction == VITA_UI_DIRECTION_LEFT &&
                   selected_button == FUSION_REDBRICK_BUY_BUTTON) {
            fusion_redbrick_slot_focus = FUSION_REDBRICK_SLOT_COUNT - 1;
            set_fusion_redbrick_slot_highlight(
                module, fusion_redbrick_slot_focus, show_focus);
            l_info("D-pad Left entered Red Brick loadout slot 3 from Buy.");
            direction = VITA_UI_DIRECTION_NONE;
        } else if (direction == VITA_UI_DIRECTION_RIGHT &&
                   selected_button == FUSION_REDBRICK_BUY_BUTTON) {
            selected_button = FUSION_REDBRICK_PLAY_BUTTON;
            *((uint8_t *)module + FUSION_UI_SELECTED_OFFSET) =
                (uint8_t)selected_button;
            l_info("D-pad Right moved Red Brick focus from Buy to Play.");
            direction = VITA_UI_DIRECTION_NONE;
        } else if (direction == VITA_UI_DIRECTION_LEFT &&
                   selected_button == FUSION_REDBRICK_PLAY_BUTTON) {
            selected_button = FUSION_REDBRICK_BUY_BUTTON;
            *((uint8_t *)module + FUSION_UI_SELECTED_OFFSET) =
                (uint8_t)selected_button;
            l_info("D-pad Left moved Red Brick focus from Play to Buy.");
            direction = VITA_UI_DIRECTION_NONE;
        } else if ((direction == VITA_UI_DIRECTION_LEFT &&
                    selected_button == FUSION_REDBRICK_BACK_BUTTON) ||
                   (direction == VITA_UI_DIRECTION_RIGHT &&
                    selected_button == FUSION_REDBRICK_PLAY_BUTTON)) {
            /* Do not let Fusion's generic focus search wrap the row. */
            direction = VITA_UI_DIRECTION_NONE;
        } else if (direction == VITA_UI_DIRECTION_UP &&
                    (selected_button == FUSION_REDBRICK_BACK_BUTTON ||
                     selected_button == FUSION_REDBRICK_BUY_BUTTON ||
                     selected_button == FUSION_REDBRICK_PLAY_BUTTON)) {
            fusion_redbrick_slot_focus = 0;
            set_fusion_redbrick_slot_highlight(module, 0, show_focus);
            l_info("D-pad entered Red Brick loadout slot 1 from button %d.",
                   selected_button);
            direction = VITA_UI_DIRECTION_NONE;
        }
    }

    if (is_level && fusion_character_level_select) {
        int level = *(const int *)((const uint8_t *)module +
                                  FUSION_LEVEL_SELECTED_OFFSET);
        if (level < 0 || level >= FUSION_LEVEL_ITEM_COUNT) {
            level = 0;
        }
        if (fusion_level_row_focused) {
            if (direction == VITA_UI_DIRECTION_LEFT && level > 0) {
                set_fusion_element_focus(
                    module, FUSION_UI_BUTTON_COUNT + (uint32_t)level,
                    fusion_level_element(module, level), false, 1.0f);
                fusion_character_level_select(module, level - 1);
                level--;
            } else if (direction == VITA_UI_DIRECTION_RIGHT &&
                       level + 1 < FUSION_LEVEL_ITEM_COUNT) {
                set_fusion_element_focus(
                    module, FUSION_UI_BUTTON_COUNT + (uint32_t)level,
                    fusion_level_element(module, level), false, 1.0f);
                fusion_character_level_select(module, level + 1);
                level++;
            } else if (direction == VITA_UI_DIRECTION_DOWN) {
                set_fusion_element_focus(
                    module, FUSION_UI_BUTTON_COUNT + (uint32_t)level,
                    fusion_level_element(module, level), false, 1.0f);
                fusion_level_row_focused = false;
                selected_button = FUSION_LEVEL_PLAY_BUTTON;
                *((uint8_t *)module + FUSION_UI_SELECTED_OFFSET) =
                    (uint8_t)selected_button;
            }
            direction = VITA_UI_DIRECTION_NONE;
        } else if (direction == VITA_UI_DIRECTION_UP) {
            fusion_level_row_focused = true;
            fusion_character_level_select(module, level);
            direction = VITA_UI_DIRECTION_NONE;
        }
    }
    sync_custom_confirm_route();

    if (!fusion_level_row_focused && !fusion_redbrick_grid_focused &&
        fusion_redbrick_slot_focus < 0 &&
        !(is_attract_module(module) && fusion_settings_active)) {
        selected_button = move_fusion_button_focus(
            module, selected_button, direction);
        *((uint8_t *)module + FUSION_UI_SELECTED_OFFSET) =
            (uint8_t)selected_button;
    }
    if (old_button != selected_button) {
        set_fusion_button_focus(module, old_button, false, 1.0f);
        if (direction != VITA_UI_DIRECTION_NONE) {
            l_info("Vita four-way UI focus moved from button %u to %u.",
                   old_button, selected_button);
        }
    }

    if (is_redbrick && fusion_redbrick_grid_focused) {
        set_fusion_button_focus(module, selected_button, false, 1.0f);
        clear_fusion_focus(module);
        int index = fusion_redbrick_index(module);
        set_fusion_redbrick_grid_highlight(module, index, show_focus);
        if (!fusion_redbrick_highlight(module, index) ||
            !fusion_flash_set_visibility || !fusion_flash_set_opacity) {
            set_fusion_element_focus(
                module, FUSION_UI_BUTTON_COUNT + (uint32_t)index,
                fusion_redbrick_element(module, index), show_focus, pulse);
        }
    } else if (is_redbrick && fusion_redbrick_slot_focus >= 0) {
        set_fusion_button_focus(module, selected_button, false, 1.0f);
        clear_fusion_focus(module);
        set_fusion_redbrick_slot_highlight(
            module, fusion_redbrick_slot_focus, show_focus);
        if (!fusion_redbrick_slot_highlight(
                module, fusion_redbrick_slot_focus) ||
            !fusion_flash_set_visibility || !fusion_flash_set_opacity) {
            set_fusion_element_focus(
                module,
                FUSION_UI_BUTTON_COUNT + FUSION_REDBRICK_COUNT +
                    (uint32_t)fusion_redbrick_slot_focus,
                fusion_redbrick_slot_element(
                    module, fusion_redbrick_slot_focus),
                show_focus, pulse);
        }
    } else if (is_level && fusion_level_row_focused) {
        set_fusion_button_focus(module, selected_button, false, 1.0f);
        int level = *(const int *)((const uint8_t *)module +
                                  FUSION_LEVEL_SELECTED_OFFSET);
        if (level >= 0 && level < FUSION_LEVEL_ITEM_COUNT) {
            set_fusion_element_focus(
                module, FUSION_UI_BUTTON_COUNT + (uint32_t)level,
                fusion_level_element(module, level), show_focus, pulse);
        }
    } else if (is_attract_module(module) && fusion_settings_active) {
        if (fusion_attract_option_focus >= 0) {
            set_fusion_button_focus(module, selected_button,
                                    show_focus, pulse);
        } else {
            clear_fusion_focus(module);
        }
    } else if (is_frontend_module(module) && fusion_settings_active &&
               selected_button == FUSION_FRONTEND_SETTINGS_VISUAL_BUTTON) {
        /*
         * On the options screen button 6 points at the panel container, so
         * scaling it enlarges every option. Do not pulse that container when
         * Language is selected; the other option records still receive their
         * individual focus pulse.
         */
        clear_fusion_focus(module);
    } else {
        set_fusion_button_focus(module, selected_button,
                                show_focus, pulse);
    }

    if (controls_take_cancel_press()) {
        if (is_redbrick && fusion_redbrick_grid_focused) {
            int index = fusion_redbrick_index(module);
            int slot = fusion_redbrick_grid_slot;
            if (slot < 0 || slot >= FUSION_REDBRICK_SLOT_COUNT) {
                slot = 0;
            }
            set_fusion_redbrick_grid_highlight(module, index, false);
            fusion_redbrick_grid_focused = false;
            fusion_redbrick_grid_slot = -1;
            fusion_redbrick_slot_focus = slot;
            set_fusion_redbrick_slot_highlight(module, slot, show_focus);
            l_info("Circle returned from the Red Brick power grid to "
                   "loadout slot %d.", slot + 1);
        } else if (is_redbrick && fusion_redbrick_slot_focus >= 0) {
            set_fusion_redbrick_slot_highlight(
                module, fusion_redbrick_slot_focus, false);
            fusion_redbrick_slot_focus = -1;
            *((uint8_t *)module + FUSION_UI_SELECTED_OFFSET) =
                FUSION_REDBRICK_PLAY_BUTTON;
            l_info("Circle returned from the Red Brick loadout to Play.");
        } else if (is_attract_module(module) && fusion_settings_active) {
            uintptr_t *vtable = *(uintptr_t **)module;
            FusionUIHandleButton handle_button =
                vtable ? (FusionUIHandleButton)vtable[15] : NULL;
            if (handle_button &&
                fusion_attract_options_state(module) ==
                    FUSION_ATTRACT_OPTIONS_OPEN) {
                handle_button(module, FUSION_ATTRACT_SETTINGS_BUTTON);
                clear_fusion_focus(module);
                fusion_settings_active = false;
                fusion_attract_option_focus = -1;
                l_info("Circle closed the modal Settings rail through "
                       "Attract action 0.");
            } else {
                l_info("Circle ignored while the Settings rail animation is "
                       "still opening.");
            }
        } else {
            if (is_settings_host) {
                fusion_settings_active = false;
            }
            if (is_redbrick) {
                leave_fusion_redbrick_focus(module);
            }
            ninjago_game_back();
            l_info("Circle sent native Android Back to the active Fusion UI.");
        }
        sync_custom_confirm_route();
    }

    /*
     * The Android build contains console-style directional focus code but its
     * base UI update never activates the focused item.  Dispatch the existing
     * virtual HandleButtonPress method on a one-shot Cross edge.  Offset 0x648
     * and vtable slot 15 are both validated properties of the pinned 1057 UI
     * base class; no touch coordinates or cursor emulation are involved.
     */
    if (controls_take_confirm_press()) {
        if (is_attract_module(module) && fusion_settings_active) {
            if (fusion_attract_option_focus < 0) {
                l_info("Cross ignored until D-pad Up/Down selects one of the "
                       "four Settings icons.");
                return;
            }
            uint32_t action = fusion_attract_option_buttons[
                fusion_attract_option_focus];
            uintptr_t *vtable = *(uintptr_t **)module;
            FusionUIHandleButton handle_button =
                vtable ? (FusionUIHandleButton)vtable[15] : NULL;
            if (handle_button && fusion_button_visible(module, action)) {
                handle_button(module, action);
                l_info("Cross activated Settings left-rail action %u.",
                       action);
            } else {
                l_warn("Ignored unavailable Settings left-rail action %u.",
                       action);
            }
            return;
        }
        if (is_attract_module(module) &&
            fusion_settings_screen(module) == FUSION_ATTRACT_MAIN_SCREEN &&
            fusion_attract_options_state(module) ==
                FUSION_ATTRACT_OPTIONS_CLOSED) {
            uint32_t button = *((const uint8_t *)module +
                                FUSION_UI_SELECTED_OFFSET);
            if (button == FUSION_ATTRACT_SETTINGS_BUTTON) {
                uintptr_t *vtable = *(uintptr_t **)module;
                FusionUIHandleButton handle_button =
                    vtable ? (FusionUIHandleButton)vtable[15] : NULL;
                if (handle_button) {
                    handle_button(module, FUSION_ATTRACT_SETTINGS_BUTTON);
                    clear_fusion_focus(module);
                    fusion_settings_active = true;
                    fusion_attract_option_focus = -1;
                    l_info("Cross opened the modal four-icon Settings rail "
                           "through Attract action 0.");
                }
                return;
            }
        }
        if (is_redbrick && fusion_redbrick_slot_focus >= 0 &&
            fusion_redbrick_select && fusion_redbrick_set_up) {
            int slot = fusion_redbrick_slot_focus;
            int index = fusion_redbrick_index_for_value(
                module, fusion_redbrick_slot_value(module, slot));
            if (index < 0) {
                index = fusion_redbrick_first_available_grid_index(
                    module, slot);
            }
            if (index < 0) {
                l_warn("No purchased, unused Red Brick is available for "
                       "loadout slot %d.", slot + 1);
                return;
            }
            set_fusion_redbrick_slot_highlight(module, slot, false);
            fusion_redbrick_slot_focus = -1;
            fusion_redbrick_grid_slot = slot;
            fusion_redbrick_grid_focused = true;
            fusion_redbrick_select(module,
                                   fusion_redbrick_value(module, index));
            set_fusion_redbrick_grid_highlight(module, index, show_focus);
            sync_custom_confirm_route();
            l_info("Cross opened the Red Brick power grid for loadout slot %d "
                   "at item %d.", slot + 1, index + 1);
            return;
        }
        if (is_redbrick && fusion_redbrick_grid_focused &&
            fusion_redbrick_select && fusion_redbrick_set_up) {
            int index = fusion_redbrick_index(module);
            int slot = fusion_redbrick_grid_slot;
            int red_brick = fusion_redbrick_value(module, index);
            if (slot < 0 || slot >= FUSION_REDBRICK_SLOT_COUNT ||
                !fusion_redbrick_assign_slot(module, slot, red_brick)) {
                l_warn("Red Brick item %d cannot be equipped in loadout "
                       "slot %d; it may be locked or already in use.",
                       index + 1, slot + 1);
                return;
            }
            set_fusion_redbrick_grid_highlight(module, index, false);
            fusion_redbrick_grid_focused = false;
            fusion_redbrick_grid_slot = -1;
            if (slot + 1 < FUSION_REDBRICK_SLOT_COUNT) {
                fusion_redbrick_slot_focus = slot + 1;
                set_fusion_redbrick_slot_highlight(
                    module, fusion_redbrick_slot_focus, show_focus);
                l_info("Cross equipped Red Brick %d in slot %d and advanced "
                       "focus to slot %d.",
                       red_brick, slot + 1,
                       fusion_redbrick_slot_focus + 1);
            } else {
                fusion_redbrick_slot_focus = -1;
                *((uint8_t *)module + FUSION_UI_SELECTED_OFFSET) =
                    FUSION_REDBRICK_PLAY_BUTTON;
                l_info("Cross equipped Red Brick %d in slot %d and returned "
                       "focus to Play.", red_brick, slot + 1);
            }
            sync_custom_confirm_route();
            return;
        }
        if (is_level &&
            fusion_level_row_focused && fusion_character_level_select) {
            int level = *(const int *)((const uint8_t *)module +
                                      FUSION_LEVEL_SELECTED_OFFSET);
            if (level >= 0 && level < FUSION_LEVEL_ITEM_COUNT) {
                set_fusion_element_focus(
                    module, FUSION_UI_BUTTON_COUNT + (uint32_t)level,
                    fusion_level_element(module, level), false, 1.0f);
            }
            fusion_character_level_select(module, level);
            fusion_level_row_focused = false;
            *((uint8_t *)module + FUSION_UI_SELECTED_OFFSET) =
                FUSION_LEVEL_PLAY_BUTTON;
            sync_custom_confirm_route();
            l_info("Cross confirmed power-up %d and focused Play.", level);
            return;
        }
        uint32_t button = *((const uint8_t *)module +
                            FUSION_UI_SELECTED_OFFSET);
        uint32_t action = button;
        bool options_screen = is_frontend_module(module) &&
            fusion_settings_screen(module) == FUSION_FRONTEND_OPTIONS_SCREEN;
        if (is_frontend_module(module) && !options_screen &&
            button == FUSION_FRONTEND_SETTINGS_VISUAL_BUTTON) {
            action = FUSION_FRONTEND_SETTINGS_ACTION;
            fusion_settings_active = true;
            clear_fusion_focus(module);
        } else if (is_frontend_module(module) &&
                   button == FUSION_FRONTEND_BACK_BUTTON) {
            fusion_settings_active = false;
        }
        uintptr_t *vtable = *(uintptr_t **)module;
        FusionUIHandleButton handle_button =
            vtable ? (FusionUIHandleButton)vtable[15] : NULL;
        if (button < FUSION_UI_BUTTON_COUNT &&
            fusion_button_visible(module, button) && handle_button) {
            if (is_redbrick) {
                /* Clear live Flash pointers before a bottom action can leave. */
                leave_fusion_redbrick_focus(module);
            }
            handle_button(module, action);
            l_info("Cross activated focused Fusion UI button %u through "
                   "action %u.", button, action);
        } else {
            l_warn("Ignored invalid focused Fusion UI button %u.", button);
        }
    }
}

static int hook_ui_module_update(uintptr_t address) {
    static const uint32_t expected[2] = {
        UINT32_C(0xe59f3064), /* ldr r3, [pc, #0x64] */
        UINT32_C(0xe3a02000), /* mov r2, #0 */
    };
    if (!address || (address & 3) != 0 ||
        *(const uint32_t *)address != expected[0] ||
        *(const uint32_t *)(address + 4) != expected[1]) {
        return -1;
    }

    uintptr_t trampoline = ALIGN_MEM(so_mod.patch_head, 4);
    if (trampoline + 20 > so_mod.patch_base + so_mod.patch_size) {
        return -2;
    }

    /*
     * The first overwritten instruction is PC-relative. Recreate its literal
     * load in the arena, preserve the second instruction, then continue at
     * the original third instruction. r0/r1 retain (module, delta_time).
     */
    uint32_t trampoline_code[5] = {
        UINT32_C(0xe59f3008),
        expected[1],
        UINT32_C(0xe51ff004),
        (uint32_t)(address + 8),
        *(const uint32_t *)(address + 0x6c),
    };
    kuKernelCpuUnrestrictedMemcpy((void *)trampoline, trampoline_code,
                                  sizeof(trampoline_code));
    kuKernelFlushCaches((void *)trampoline, sizeof(trampoline_code));
    so_mod.patch_head = trampoline + sizeof(trampoline_code);

    fusion_ui_module_update = (FusionUIModuleUpdate)trampoline;
    hook_addr(address, (uintptr_t)&vita_ui_module_update);
    return 0;
}

static void vita_hit_sfx(void *game_object) {
    void *hurt_bound = game_object
        ? *(void **)((uint8_t *)game_object + 0x7c) : NULL;
    uint32_t sound_id = hurt_bound
        ? *(const uint32_t *)((const uint8_t *)hurt_bound + 0x3c) : 0;
    unsigned int event = ++fusion_hit_sfx_count;
    if (event <= 12 || event % 128 == 0) {
        l_info("Combat hit-SFX event %u: object=%p, hurt-bound=%p, "
               "Fusion sound=%u.",
               event, game_object, hurt_bound, sound_id);
    }
    fusion_hit_sfx(game_object);
}

static unsigned int vita_edge_menu_button_pressed(void *edge_menu,
                                                   void *touch) {
    unsigned int forced = controls_take_edge_menu_button();
    if (forced != 0) {
        return forced;
    }
    return fusion_edge_menu_button_pressed(edge_menu, touch);
}

static bool vita_touch_up_inside_element(void *element) {
    if (controls_take_quit_confirmation(element)) {
        return true;
    }
    return fusion_touch_up_inside_element(element);
}

enum {
    HEART_CALL_RECORDS = 24,
    HEART_EVAL_BOSS = 0,
    HEART_EVAL_PLAYER = 1,
    HEART_EVAL_COUNT = 2,
};

typedef struct HeartAnimationCall {
    void *stream;
    unsigned int type;
    uint16_t start_frame;
    uint16_t end_frame;
    float speed;
    float blend;
    void *user_data;
    uint16_t flags;
    unsigned int transition;
    unsigned int replayed_transition;
} HeartAnimationCall;

typedef struct HeartAnimationEvaluation {
    void *stream;
    void *target;
    void *element;
    void *root;
    unsigned int transition;
    unsigned int frames_left;
    unsigned int draw_reports;
    const char *name;
} HeartAnimationEvaluation;

typedef struct HeartStreamIdentity {
    int recognized;
    int eval_slot;
    unsigned int player_slot;
    const char *name;
    void *target;
    void *element;
} HeartStreamIdentity;

static HeartAnimationCall fusion_heart_calls[HEART_CALL_RECORDS];
static HeartAnimationEvaluation fusion_heart_evals[HEART_EVAL_COUNT];
static unsigned int heart_texture_reports;

static HeartStreamIdentity heart_stream_identity(void *stream) {
    HeartStreamIdentity identity = {0};
    if (!stream) {
        return identity;
    }
    if (fusion_boss_hearts_item) {
        uint8_t *item = fusion_boss_hearts_item;
        if (stream == *(void **)(item + 0xbc)) {
            identity.recognized = 1;
            identity.eval_slot = HEART_EVAL_BOSS;
            identity.name = "boss-fill";
            identity.target = *(void **)(item + 0x48);
            identity.element = *(void **)(item + 0xe8);
        } else if (stream == *(void **)(item + 0xe0)) {
            identity.recognized = 1;
            identity.eval_slot = HEART_EVAL_BOSS;
            identity.name = "boss-damage";
            identity.target = *(void **)(item + 0x80);
        } else if (stream == *(void **)(item + 0xe4)) {
            identity.recognized = 1;
            identity.eval_slot = HEART_EVAL_BOSS;
            identity.name = "boss-heal";
        }
        if (identity.recognized) {
            return identity;
        }
    }
    if (fusion_player_bar) {
        for (unsigned int slot = 0; slot < 5; ++slot) {
            uint8_t *heart = fusion_player_bar + slot * 0x60;
            static const size_t offsets[] = {0x2a0, 0x2a4, 0x2a8, 0x2ac};
            static const char *names[] = {
                "player-fill", "player-empty", "player-damage",
                "player-partial",
            };
            for (unsigned int kind = 0; kind < 4; ++kind) {
                if (stream == *(void **)(heart + offsets[kind])) {
                    identity.recognized = 1;
                    identity.eval_slot = HEART_EVAL_PLAYER;
                    identity.player_slot = slot;
                    identity.name = names[kind];
                    identity.target = *(void **)(heart + 0x270);
                    identity.element = *(void **)(heart + 0x298);
                    return identity;
                }
            }
        }
    }
    return identity;
}

static HeartAnimationCall *heart_call_record(void *stream) {
    HeartAnimationCall *empty = NULL;
    for (unsigned int i = 0; i < HEART_CALL_RECORDS; ++i) {
        if (fusion_heart_calls[i].stream == stream) {
            return &fusion_heart_calls[i];
        }
        if (!fusion_heart_calls[i].stream && !empty) {
            empty = &fusion_heart_calls[i];
        }
    }
    return empty;
}

static void vita_play_anim_safe(void *stream, unsigned int type,
                                uint16_t start_frame, uint16_t end_frame,
                                float speed, float blend, void *user_data,
                                uint16_t flags) {
    HeartStreamIdentity identity = heart_stream_identity(stream);
    HeartAnimationCall *record = identity.recognized
        ? heart_call_record(stream) : NULL;
    if (record) {
        record->stream = stream;
        record->type = type;
        record->start_frame = start_frame;
        record->end_frame = end_frame;
        record->speed = speed;
        record->blend = blend;
        record->user_data = user_data;
        record->flags = flags;
        record->transition = fusion_active_heart_transition;
    }

    fusion_play_anim_safe(stream, type, start_frame, end_frame, speed, blend,
                          user_data, flags);

    if (identity.recognized && fusion_heart_anim_reports < 64) {
        ++fusion_heart_anim_reports;
        void *playing = fusion_animation_find_playing
            ? fusion_animation_find_playing(stream) : NULL;
        unsigned int status = fusion_animation_stream_status
            ? fusion_animation_stream_status(stream) : UINT32_MAX;
        float frame = playing && fusion_animation_next_frame
            ? fusion_animation_next_frame(stream, 0) : -1.0f;
        void *animation = stream ? *(void **)((uint8_t *)stream + 4) : NULL;
        l_info("Heart animation call: name=%s slot=%u transition=%u "
               "stream=%p type=%u range=%u..%u speed=%.3f blend=%.3f "
               "user=%p flags=%u playing=%p status=%u frame=%.2f "
               "animation=%p target=%p.",
               identity.name, identity.player_slot,
               fusion_active_heart_transition, stream, type, start_frame,
               end_frame, speed, blend, user_data, flags, playing, status,
               frame, animation, identity.target);
    }
}

static void heart_log_element_state(const HeartAnimationEvaluation *eval,
                                    const char *phase) {
    if (!eval || !eval->element) {
        return;
    }
    float opacity = fusion_flash_get_opacity
        ? fusion_flash_get_opacity(eval->element) : -1.0f;
    const float *matrix = fusion_flash_get_world_matrix
        ? fusion_flash_get_world_matrix(eval->element) : NULL;
    const float *uv = fusion_flash_get_uv
        ? fusion_flash_get_uv(eval->element) : NULL;
    bool visible = !fusion_flash_is_visible ||
        fusion_flash_is_visible(eval->element, true);
    void *root = eval->target
        ? *(void **)((uint8_t *)eval->target + 0xf8) : NULL;
    void *parent = root ? *(void **)((uint8_t *)root + 0x60) : NULL;
    float object_opacity = eval->target
        ? *(const float *)((const uint8_t *)eval->target + 0x100) : -1.0f;
    unsigned int attached = eval->target
        ? *((const uint8_t *)eval->target + 0x108) : 0;
    l_info("Heart Flash state %s: name=%s element=%p visible=%d "
           "opacity=%.3f matrix=[%.3f %.3f %.3f %.3f] "
           "uv=[%.3f %.3f %.3f %.3f] object=%p root=%p parent=%p "
           "object-opacity=%.3f attached=%u.",
           phase, eval->name, eval->element, visible, opacity,
           matrix ? matrix[0] : 0.0f, matrix ? matrix[5] : 0.0f,
           matrix ? matrix[12] : 0.0f, matrix ? matrix[13] : 0.0f,
           uv ? uv[0] : 0.0f, uv ? uv[1] : 0.0f, uv ? uv[2] : 0.0f,
           uv ? uv[3] : 0.0f, eval->target, root, parent, object_opacity,
           attached);
}

static NjFlashQuad heart_quad(void *element) {
    uint8_t *p = element;
    NjFlashQuad q = {
        .width = *(float *)(p + 0xc8), .height = *(float *)(p + 0xcc),
        .pivot_x = *(float *)(p + 0x54), .pivot_y = *(float *)(p + 0x58),
    };
    memcpy(q.uv, p + 0x94, sizeof(q.uv));
    memcpy(q.matrix, p + 0xd8, sizeof(q.matrix));
    return q;
}

static void heart_write_quad(void *element, const NjFlashQuad *q) {
    uint8_t *p = element;
    *(float *)(p + 0xc8) = q->width; *(float *)(p + 0xcc) = q->height;
    *(float *)(p + 0x54) = q->pivot_x; *(float *)(p + 0x58) = q->pivot_y;
    memcpy(p + 0x94, q->uv, sizeof(q->uv));
}

static void vita_flash_render_texture(void *element, const void *colour,
                                      float opacity, float scale, bool flag) {
    /* Once the bounded render evidence is collected, the rejected quad
     * workaround must not impose an ancestor walk on every UI texture. */
    if (!heart_quad_clip_enabled && heart_texture_reports >= 80) {
        fusion_flash_render_texture(element, colour, opacity, scale, flag);
        return;
    }
    HeartAnimationEvaluation *matched = NULL;
    void *clip = NULL;
    int heart_slot = -1;
    unsigned depth = 0;
    for (void *p = element; p && depth++ < 32;) {
        for (unsigned i = 0; i < HEART_EVAL_COUNT; ++i) {
            HeartAnimationEvaluation *e = &fusion_heart_evals[i];
            if (e->stream && (p == e->element || p == e->root)) { matched = e; break; }
        }
        for (unsigned i = 0; i < 6; ++i)
            if (p == heart_clip_roots[i]) heart_slot = (int)i;
        if (!clip) clip = *(void **)((uint8_t *)p + 0x8c);
        if (heart_slot >= 0) break;
        p = *(void **)((uint8_t *)p + 0x60);
    }
    bool trace = matched && matched->draw_reports < 8 && heart_texture_reports < 80;
    if (trace) {
        ++matched->draw_reports; ++heart_texture_reports;
        const float *m = clip ? (const float *)((uint8_t *)clip + 0xd8) : NULL;
        l_info("Heart actual texture: name=%s transition=%u element=%p "
               "clip=%p clip-matrix=[%.3f %.3f %.3f %.3f] "
               "clip-size=[%.3f %.3f] opacity=%.3f scale=%.3f.",
               matched->name, matched->transition, element, clip,
               m ? m[0] : 0, m ? m[5] : 0, m ? m[12] : 0, m ? m[13] : 0,
               clip ? *(float *)((uint8_t *)clip + 0xc8) : 0,
               clip ? *(float *)((uint8_t *)clip + 0xcc) : 0, opacity, scale);
        gl_heart_draw_trace_begin(matched->transition);
    }
    /* Device traces show authored streams, clip transforms and GL scissor submissions
     * advance, yet the reported pixels remain full. Crop the original quad to
     * that same authored mask, avoiding dependence on backend pixel-scissor
     * enforcement. This is not a new health meter: native art, UVs, animation,
     * opacity and damage/heal timing all remain authoritative.
     * Only live heart roots captured by the native setters are eligible.
     * Unsupported rotated/perspective transforms retain native rendering. */
    NjFlashQuad original, cropped;
    int crop = -1;
    if (heart_quad_clip_enabled && heart_slot >= 0 && clip) {
        original = heart_quad(element);
        NjFlashQuad mask = heart_quad(clip);
        crop = nj_flash_clip_quad(&original, &mask, &cropped);
        if (trace) {
            l_info("Heart authored quad clip: slot=%d result=%d "
                   "size=[%.3f %.3f]->[%.3f %.3f].", heart_slot, crop,
                   original.width, original.height,
                   crop > 0 ? cropped.width : 0,
                   crop > 0 ? cropped.height : 0);
        }
        if (crop == 2) heart_write_quad(element, &cropped);
    } else if (trace) {
        l_info("Heart authored quad clip: slot=%d clip=%p; native drawing "
               "retained (disabled, unrecognized, or no authored mask).",
               heart_slot, clip);
    }
    if (crop != 0)
        fusion_flash_render_texture(element, colour, opacity, scale, flag);
    if (crop == 2) heart_write_quad(element, &original);
    if (trace) gl_heart_draw_trace_end();
}

static void heart_apply_authored_frame(HeartAnimationEvaluation *eval) {
    if (!eval || !eval->stream || !eval->target) {
        return;
    }
    void *animation = *(void **)((uint8_t *)eval->stream + 4);
    if (!animation) {
        return;
    }
    if (fusion_animation_position_update) {
        fusion_animation_position_update(animation, eval->target);
    }
    if (fusion_animation_render_update) {
        fusion_animation_render_update(animation, eval->target);
    }

    /*
     * Heart Flash objects are attached to the parent HUD.  The public graph
     * update intentionally ignores attached objects (object+0x108 != 0),
     * which left the evaluated animation state out of the rendered graph.
     * Use Fusion's own recursive update with the exact parent-element world
     * matrix used by fnFlashElement_UpdateRecursive; retain the normal path
     * for standalone objects.
     */
    bool attached = *((const uint8_t *)eval->target + 0x108) != 0;
    if (attached && fusion_flash_update_internal_graph) {
        void *root = *(void **)((uint8_t *)eval->target + 0xf8);
        void *parent = root
            ? *(void **)((uint8_t *)root + 0x60) : NULL;
        const void *parent_matrix = parent
            ? (const uint8_t *)parent + 0xd8
            : (const uint8_t *)eval->target + 0x5c;
        fusion_flash_update_internal_graph(
            eval->target, parent_matrix);
    } else if (fusion_flash_update_graph) {
        fusion_flash_update_graph(eval->target);
    }
}

static void heart_start_evaluation(void *stream, unsigned int transition,
                                   const char *name) {
    if (!stream || !transition || !fusion_animation_find_playing) {
        return;
    }
    HeartStreamIdentity identity = heart_stream_identity(stream);
    if (!identity.recognized) {
        return;
    }
    void *playing = fusion_animation_find_playing(stream);
    HeartAnimationCall *record = heart_call_record(stream);
    if (!playing && record && record->transition == transition &&
        record->replayed_transition != transition && fusion_play_anim_safe) {
        record->replayed_transition = transition;
        fusion_play_anim_safe(record->stream, record->type,
                              record->start_frame, record->end_frame,
                              record->speed, record->blend,
                              record->user_data, record->flags);
        playing = fusion_animation_find_playing(stream);
        l_warn("Replayed captured Fusion %s call once for transition %u "
               "because the native setter created no playing instance "
               "(playing=%p).", name, transition, playing);
    }
    if (!playing) {
        l_warn("Fusion %s stream has no playing instance after transition "
               "%u; no frame forcing was applied.", name, transition);
        return;
    }

    HeartAnimationEvaluation *eval =
        &fusion_heart_evals[identity.eval_slot];
    eval->stream = stream;
    eval->target = identity.target;
    eval->element = identity.element;
    eval->root = identity.target
        ? *(void **)((uint8_t *)identity.target + 0xf8) : NULL;
    eval->transition = transition;
    eval->frames_left = 240;
    eval->draw_reports = 0;
    eval->name = name;
    heart_log_element_state(eval, "before-evaluation");
}

static void heart_evaluate_authored_streams(void) {
    for (unsigned int i = 0; i < HEART_EVAL_COUNT; ++i) {
        HeartAnimationEvaluation *eval = &fusion_heart_evals[i];
        if (!eval->stream) {
            continue;
        }
        void *playing = fusion_animation_find_playing
            ? fusion_animation_find_playing(eval->stream) : NULL;
        unsigned int status = fusion_animation_stream_status
            ? fusion_animation_stream_status(eval->stream) : UINT32_MAX;
        if (!playing || status == 6 || eval->frames_left == 0) {
            if (status == 6) {
                /* Commit Fusion's completed authored frame before clearing. */
                heart_apply_authored_frame(eval);
            }
            float frame = playing && fusion_animation_next_frame
                ? fusion_animation_next_frame(eval->stream, 0) : -1.0f;
            l_info("Heart authored stream finished: name=%s transition=%u "
                   "playing=%p status=%u frame=%.2f remaining=%u.",
                   eval->name, eval->transition, playing, status, frame,
                   eval->frames_left);
            heart_log_element_state(eval, "completed");
            memset(eval, 0, sizeof(*eval));
            continue;
        }

        heart_apply_authored_frame(eval);
        --eval->frames_left;
    }
}

static unsigned int vita_flash_render(void *object, const void *matrix,
                                      unsigned int render_pass,
                                      unsigned int flags) {
    unsigned int submitted = fusion_flash_render(
        object, matrix, render_pass, flags);
    for (unsigned int i = 0; i < HEART_EVAL_COUNT; ++i) {
        HeartAnimationEvaluation *eval = &fusion_heart_evals[i];
        if (eval->stream && eval->target == object &&
            eval->draw_reports < 2) {
            ++eval->draw_reports;
            l_info("Heart Flash draw submission: name=%s transition=%u "
                   "object=%p pass=%u flags=0x%x queued=%u.",
                   eval->name, eval->transition, object, render_pass, flags,
                   submitted);
        }
    }
    return submitted;
}

static void vita_hud_update(void *hud, void *world, float delta_time) {
    fusion_hud_update(hud, world, delta_time);
    controls_refresh_quit_confirmation_focus();
    heart_evaluate_authored_streams();

    /*
     * Fusion normally refreshes character-boss hearts through the boss UI
     * item's own callback. Keep the visible target synchronized here as well:
     * the Vita trace showed that damage and death progressed while the heart
     * animation stayed at its original frame.
     */
    void *boss = fusion_boss_hearts_go ? *fusion_boss_hearts_go : NULL;
    if (!boss || !fusion_is_character || !fusion_is_character(boss) ||
        !fusion_character_data || !fusion_set_boss_heart_count ||
        !fusion_boss_hearts_item) {
        fusion_last_boss = boss;
        fusion_last_boss_health = UINT32_MAX;
        return;
    }
    void *character_data = fusion_character_data(boss);
    if (!character_data) {
        return;
    }
    unsigned int health = *(const uint16_t *)((const uint8_t *)character_data +
                                              0xd0);
    unsigned int displayed = *(const uint16_t *)(
        (const uint8_t *)fusion_boss_hearts_item + 0x102);
    bool boss_changed = boss != fusion_last_boss ||
                        health != fusion_last_boss_health;
    if (boss_changed) {
        fusion_last_boss = boss;
        fusion_last_boss_health = health;
        if (displayed != health) {
            vita_set_boss_heart_count(fusion_boss_hearts_item, health);
        }
    }

    if (boss_changed && fusion_boss_heart_post_reports < 24) {
        ++fusion_boss_heart_post_reports;
        void *damage_stream = *(void **)(
            (uint8_t *)fusion_boss_hearts_item + 0xe0);
        void *heal_stream = *(void **)(
            (uint8_t *)fusion_boss_hearts_item + 0xe4);
        void *damage_playing = damage_stream && fusion_animation_find_playing
            ? fusion_animation_find_playing(damage_stream) : NULL;
        void *heal_playing = heal_stream && fusion_animation_find_playing
            ? fusion_animation_find_playing(heal_stream) : NULL;
        unsigned int damage_status = damage_stream &&
            fusion_animation_stream_status
            ? fusion_animation_stream_status(damage_stream) : UINT32_MAX;
        unsigned int heal_status = heal_stream &&
            fusion_animation_stream_status
            ? fusion_animation_stream_status(heal_stream) : UINT32_MAX;
        l_info("Boss hearts after HUD: health=%u displayed=%u "
               "damage=%p playing=%p status=%u heal=%p playing=%p "
               "status=%u.", health,
               *(const uint16_t *)((const uint8_t *)
                   fusion_boss_hearts_item + 0x102),
               damage_stream, damage_playing, damage_status,
               heal_stream, heal_playing, heal_status);
    }
}

static void vita_set_heart_count(unsigned int count) {
    if (!fusion_set_heart_count) {
        return;
    }
    unsigned int previous_health = fusion_last_player_health;
    unsigned int old_visible = fusion_player_bar
        ? fusion_player_bar[0x444] : UINT32_MAX;
    unsigned int transition = previous_health != UINT32_MAX &&
        previous_health != count ? ++fusion_heart_transition_serial : 0;
    fusion_active_heart_transition = transition;
    fusion_set_heart_count(count);
    fusion_active_heart_transition = 0;
    if (fusion_player_bar) {
        for (unsigned i = 0; i < 5; ++i) {
            void *target = *(void **)(fusion_player_bar + 0x270 + i * 0x60);
            heart_clip_roots[i] = target
                ? *(void **)((uint8_t *)target + 0xf8) : NULL;
        }
    }

    fusion_last_player_health = count;
    unsigned int visible = fusion_player_bar
        ? fusion_player_bar[0x444] : UINT32_MAX;
    unsigned int unit = fusion_percent_per_heart
        ? *fusion_percent_per_heart : 0;
    void *partial_stream = (fusion_player_bar && visible > 0 && visible <= 5)
        ? *(void **)(fusion_player_bar + 0x2ac + (visible - 1) * 0x60)
        : NULL;
    unsigned int status = (partial_stream && fusion_animation_stream_status)
        ? fusion_animation_stream_status(partial_stream) : UINT32_MAX;
    void *playing = partial_stream && fusion_animation_find_playing
        ? fusion_animation_find_playing(partial_stream) : NULL;
    float partial_frame = (fusion_player_bar && visible > 0 && visible <= 5)
        ? *(const float *)(fusion_player_bar + 0x2b8 +
                          (visible - 1) * 0x60) : -1.0f;
    if (transition && partial_stream) {
        heart_start_evaluation(partial_stream, transition,
                               "player-partial");
        playing = fusion_animation_find_playing
            ? fusion_animation_find_playing(partial_stream) : NULL;
        status = fusion_animation_stream_status
            ? fusion_animation_stream_status(partial_stream) : UINT32_MAX;
    }
    if ((transition || previous_health == UINT32_MAX) &&
        fusion_player_heart_reports < 32) {
        ++fusion_player_heart_reports;
        void *animation = partial_stream
            ? *(void **)((uint8_t *)partial_stream + 4) : NULL;
        void *target = (fusion_player_bar && visible > 0 && visible <= 5)
            ? *(void **)(fusion_player_bar + 0x270 +
                         (visible - 1) * 0x60) : NULL;
        float frame = playing && fusion_animation_next_frame
            ? fusion_animation_next_frame(partial_stream, 0) : partial_frame;
        l_info("Player hearts transition=%u: current=%u max=%u "
               "visible=%u->%u partial-stream=%p playing=%p status=%u "
               "frame=%.2f animation=%p target=%p.",
               transition, count, unit * 5, old_visible, visible,
               partial_stream, playing, status, frame, animation, target);
    }
}

static void vita_set_boss_heart_count(void *icon, unsigned int count) {
    count &= UINT32_C(0xffff);
    unsigned int old_count = icon
        ? *(const uint16_t *)((const uint8_t *)icon + 0x102) : UINT32_MAX;
    unsigned int transition = icon && old_count != count
        ? ++fusion_heart_transition_serial : 0;
    fusion_active_heart_transition = transition;
    fusion_set_boss_heart_count(icon, count);
    fusion_active_heart_transition = 0;
    if (icon) {
        void *target = *(void **)((uint8_t *)icon + 0x48);
        heart_clip_roots[5] = target
            ? *(void **)((uint8_t *)target + 0xf8) : NULL;
    }
    if (!icon || old_count == count) {
        return;
    }

    void *fill_stream = *(void **)((uint8_t *)icon + 0xbc);
    void *damage_stream = *(void **)((uint8_t *)icon + 0xe0);
    void *heal_stream = *(void **)((uint8_t *)icon + 0xe4);
    void *effect_stream = count < old_count ? damage_stream : heal_stream;
    const char *effect_name = count < old_count
        ? "boss-damage" : "boss-heal";
    /* +0xbc is both the frame-count reference and the visible heart fill. */
    heart_start_evaluation(fill_stream, transition, "boss-fill");

    if (old_count != count && fusion_boss_heart_reports < 32) {
        ++fusion_boss_heart_reports;
        unsigned int frames = fill_stream &&
            fusion_animation_frame_count
            ? fusion_animation_frame_count(fill_stream) : 0;
        void *fill_playing = fill_stream && fusion_animation_find_playing
            ? fusion_animation_find_playing(fill_stream) : NULL;
        unsigned int fill_status = fill_stream &&
            fusion_animation_stream_status
            ? fusion_animation_stream_status(fill_stream) : UINT32_MAX;
        float fill_frame = fill_playing && fusion_animation_next_frame
            ? fusion_animation_next_frame(fill_stream, 0) : -1.0f;
        void *fill_animation = fill_stream
            ? *(void **)((uint8_t *)fill_stream + 4) : NULL;
        void *effect_playing = effect_stream && fusion_animation_find_playing
            ? fusion_animation_find_playing(effect_stream) : NULL;
        unsigned int effect_status = effect_stream &&
            fusion_animation_stream_status
            ? fusion_animation_stream_status(effect_stream) : UINT32_MAX;
        void *target = *(void **)((uint8_t *)icon + 0x48);
        l_info("Boss hearts transition=%u: current=%u max=%u displayed=%u "
               "fill=%p playing=%p status=%u frame=%.2f animation=%p "
               "target=%p frames=%u effect=%s/%p playing=%p status=%u.",
               transition, count,
               *(const uint16_t *)((const uint8_t *)icon + 0x104),
               *(const uint16_t *)((const uint8_t *)icon + 0x102),
               fill_stream, fill_playing, fill_status, fill_frame,
               fill_animation, target, frames, effect_name, effect_stream,
               effect_playing, effect_status);
    }
}

static void vita_update_pad_controls(void *system, void *player,
                                     void *character_data,
                                     void *player_data_header) {
    fusion_update_pad_controls(system, player, character_data,
                               player_data_header);

    if (!system || !player || !player_data_header ||
        !fusion_character_orient || !fusion_character_get_turn_speed ||
        !controls_ranged_attack_held(player)) {
        return;
    }

    uint8_t *header = (uint8_t *)player_data_header;
    uint32_t *current_buttons = (uint32_t *)(header + 0x0c);
    if ((*current_buttons & UINT32_C(1)) == 0) {
        return;
    }

    /*
     * updatePadControls has already converted the left stick into Fusion's
     * camera-relative 16-bit world angle at header+6. Preserve that angle,
     * advance the current angle with Fusion's own positive turn step, then
     * remove only the analog-movement bit before updatePadData and the
     * character control subsystems consume it. The third Orient argument is a
     * signed turn step, not the desired angle; passing the unsigned target
     * there makes directions above 0x7fff rotate with a negative step. This
     * keeps held-Square aiming stationary without disturbing attacks, dodge,
     * block, D-pad events, or touch fallback.
     */
    uint16_t aim_angle = *(const uint16_t *)(header + 0x06);
    uint16_t previous_angle = *(const uint16_t *)(header + 0x08);
    int16_t turn_step = (int16_t)fusion_character_get_turn_speed();
    fusion_character_orient(player, player_data_header,
                            turn_step, false);
    uint16_t applied_angle = *(const uint16_t *)(header + 0x08);
    *current_buttons &= ~UINT32_C(1);
    *((uint8_t *)system + 0x2e) = 0;

    if (fusion_stationary_aim_reports < 8) {
        ++fusion_stationary_aim_reports;
        l_info("Held-Square ranged aim: target=%u previous=%u applied=%u "
               "turn_step=%d; analog movement suppressed.",
               aim_angle, previous_angle, applied_angle, turn_step);
    }
}

static bool vita_melee_tap(void *event, void *player,
                           const void *combat_data) {
    if (controls_use_ranged_attack_mode() && fusion_ranged_tap) {
        if (fusion_ranged_route_reports < 4) {
            ++fusion_ranged_route_reports;
            l_info("Redirecting Square melee tap to Fusion's native "
                   "ranged handler.");
        }
        bool handled = fusion_ranged_tap(event, player, combat_data);
        fusion_ranged_autofire_event = event;
        fusion_ranged_autofire_player = player;
        fusion_ranged_autofire_combat_data = combat_data;
        fusion_ranged_autofire_last_us = sceKernelGetProcessTimeWide();
        return handled;
    }
    fusion_ranged_autofire_event = NULL;
    fusion_ranged_autofire_player = NULL;
    fusion_ranged_autofire_combat_data = NULL;
    fusion_ranged_autofire_last_us = 0;
    bool force_melee = controls_using_controller();
    fusion_force_targetless_melee = force_melee ? 1u : 0u;
    if (force_melee && fusion_melee_route_reports < 4) {
        ++fusion_melee_route_reports;
        l_info("Routing controller Square through Fusion's strict melee "
               "path when no nearby target exists.");
    }
    bool handled = fusion_melee_tap(event, player, combat_data);
    fusion_force_targetless_melee = 0;
    return handled;
}

static bool vita_melee_held(void *event, void *player,
                            const void *combat_data) {
    if (controls_use_ranged_attack_mode() && fusion_ranged_held) {
        if (fusion_ranged_route_reports < 4) {
            ++fusion_ranged_route_reports;
            l_info("Redirecting Square melee hold to Fusion's native "
                   "ranged handler.");
        }
        fusion_ranged_autofire_event = event;
        fusion_ranged_autofire_player = player;
        fusion_ranged_autofire_combat_data = combat_data;
        return fusion_ranged_held(event, player, combat_data);
    }
    fusion_ranged_autofire_event = NULL;
    fusion_ranged_autofire_player = NULL;
    fusion_ranged_autofire_combat_data = NULL;
    fusion_ranged_autofire_last_us = 0;
    return fusion_melee_held(event, player, combat_data);
}

void ninjago_patch_poll(void) {
    if (ninjago_game_is_loading())
        memset(heart_clip_roots, 0, sizeof(heart_clip_roots));
    if (!fusion_ranged_tap || !fusion_ranged_autofire_event ||
        !controls_ranged_attack_held(fusion_ranged_autofire_player)) {
        fusion_ranged_autofire_event = NULL;
        fusion_ranged_autofire_player = NULL;
        fusion_ranged_autofire_combat_data = NULL;
        fusion_ranged_autofire_last_us = 0;
        return;
    }

    uint64_t now = sceKernelGetProcessTimeWide();
    if (fusion_ranged_autofire_last_us &&
        now - fusion_ranged_autofire_last_us < 125000) {
        return;
    }

    bool fired = fusion_ranged_tap(fusion_ranged_autofire_event,
                                   fusion_ranged_autofire_player,
                                   fusion_ranged_autofire_combat_data);
    fusion_ranged_autofire_last_us = now;
    if (fusion_ranged_autofire_reports < 8) {
        ++fusion_ranged_autofire_reports;
        l_info("Held Square repeated Fusion's ranged tap from the per-frame "
               "Vita controller path (accepted=%d); the left stick remained "
               "live.", fired);
    }
}

/* Preserve two ordinary ARM instructions before detouring a short function. */
static int hook_simple_arm_function(uintptr_t address,
                                    uint32_t expected_first,
                                    uint32_t expected_second,
                                    uintptr_t destination,
                                    uintptr_t *trampoline_out) {
    if (!address || (address & 3) != 0 || !trampoline_out ||
        *(const uint32_t *)address != expected_first ||
        *(const uint32_t *)(address + 4) != expected_second) {
        return -1;
    }

    uintptr_t trampoline = ALIGN_MEM(so_mod.patch_head, 4);
    if (trampoline + 16 > so_mod.patch_base + so_mod.patch_size) {
        return -2;
    }
    uint32_t trampoline_code[4] = {
        expected_first,
        expected_second,
        UINT32_C(0xe51ff004), /* ldr pc, [pc, #-4] */
        (uint32_t)(address + 8),
    };
    kuKernelCpuUnrestrictedMemcpy((void *)trampoline, trampoline_code,
                                  sizeof(trampoline_code));
    kuKernelFlushCaches((void *)trampoline, sizeof(trampoline_code));
    so_mod.patch_head = trampoline + sizeof(trampoline_code);
    *trampoline_out = trampoline;
    hook_addr(address, destination);
    return 0;
}

/*
 * GOCSPADMELEEEVENT::handleTap branches at +0xec after FindTarget returns
 * null. The stock targetless path may reject a ranged-capable character before
 * it reaches the ordinary melee combo states. Route only controller melee
 * calls to the function's own EnableWeapon(melee, true, true) block; touch and
 * all target-found calls retain the original instruction flow.
 */
static int patch_targetless_melee_branch(uintptr_t melee_tap_address) {
    if (!melee_tap_address || (melee_tap_address & 3) != 0) {
        return -1;
    }

    uintptr_t branch_address = melee_tap_address + 0xec;
    if (*(const uint32_t *)branch_address != UINT32_C(0x0a000018)) {
        return -2;
    }

    uintptr_t veneer = ALIGN_MEM(so_mod.patch_head, 4);
    if (veneer + 32 > so_mod.patch_base + so_mod.patch_size) {
        return -3;
    }
    intptr_t delta = (intptr_t)veneer - (intptr_t)(branch_address + 8);
    if ((delta & 3) != 0 || delta < -33554432 || delta > 33554428) {
        return -4;
    }

    uint32_t veneer_code[8] = {
        UINT32_C(0xe59fc00c), /* ldr ip, [pc, #12] -> flag address */
        UINT32_C(0xe59cc000), /* ldr ip, [ip] */
        UINT32_C(0xe35c0000), /* cmp ip, #0 */
        UINT32_C(0x159ff004), /* ldrne pc, [pc, #4] -> forced target */
        UINT32_C(0x059ff004), /* ldreq pc, [pc, #4] -> stock target */
        (uint32_t)(uintptr_t)&fusion_force_targetless_melee,
        (uint32_t)(melee_tap_address + 0x26c),
        (uint32_t)(melee_tap_address + 0x154),
    };
    uint32_t branch = UINT32_C(0x0a000000) |
                      ((uint32_t)(delta >> 2) & UINT32_C(0x00ffffff));
    kuKernelCpuUnrestrictedMemcpy((void *)veneer, veneer_code,
                                  sizeof(veneer_code));
    kuKernelCpuUnrestrictedMemcpy((void *)branch_address, &branch,
                                  sizeof(branch));
    kuKernelFlushCaches((void *)veneer, sizeof(veneer_code));
    kuKernelFlushCaches((void *)branch_address, sizeof(branch));
    so_mod.patch_head = veneer + sizeof(veneer_code);
    return 0;
}

/*
 * fnaController_Poll is only one ARM instruction long, so the normal
 * eight-byte hook_addr() would overwrite fnaController_HasMotor.  Branch to a
 * nearby two-word veneer in the loader's RX patch arena instead.
 */
static int hook_empty_arm_stub(uintptr_t address, uintptr_t destination) {
    if (!address || (address & 3) != 0 ||
        *(const uint32_t *)address != UINT32_C(0xe12fff1e)) {
        return -1;
    }

    uintptr_t veneer = ALIGN_MEM(so_mod.patch_head, 4);
    if (veneer + 8 > so_mod.patch_base + so_mod.patch_size) {
        return -2;
    }
    intptr_t delta = (intptr_t)veneer - (intptr_t)(address + 8);
    if ((delta & 3) != 0 || delta < -33554432 || delta > 33554428) {
        return -3;
    }

    uint32_t veneer_code[2] = {
        UINT32_C(0xe51ff004),
        (uint32_t)destination,
    };
    uint32_t branch = UINT32_C(0xea000000) |
                      ((uint32_t)(delta >> 2) & UINT32_C(0x00ffffff));
    kuKernelCpuUnrestrictedMemcpy((void *)veneer, veneer_code,
                                  sizeof(veneer_code));
    kuKernelCpuUnrestrictedMemcpy((void *)address, &branch, sizeof(branch));
    kuKernelFlushCaches((void *)veneer, sizeof(veneer_code));
    kuKernelFlushCaches((void *)address, sizeof(branch));
    so_mod.patch_head = veneer + sizeof(veneer_code);
    return 0;
}

void so_patch(void) {
    uintptr_t cmpxchg = (uintptr_t)&__atomic_cmpxchg;
    uintptr_t barrier = (uintptr_t)&bionic_memory_barrier;
    unsigned int cmpxchg_count = 0;
    unsigned int barrier_count = 0;

    for (size_t offset = 0; offset + sizeof(uint32_t) <= so_mod.exec_size;
         offset += sizeof(uint32_t)) {
        uintptr_t address = so_mod.exec_base + offset;
        uint32_t value = *(const uint32_t *)address;
        uintptr_t replacement = 0;
        if (value == ARM_KUSER_CMPXCHG) {
            replacement = cmpxchg;
            ++cmpxchg_count;
        } else if (value == ARM_KUSER_MEMORY_BARRIER) {
            replacement = barrier;
            ++barrier_count;
        }
        if (replacement) {
            kuKernelCpuUnrestrictedMemcpy((void *)address, &replacement,
                                          sizeof(replacement));
        }
    }
    l_info("Patched Android kuser literals: %u cmpxchg, %u barriers.",
           cmpxchg_count, barrier_count);

    fusion_ui_resolution = (uint32_t *)so_symbol(
        &so_mod, "fnaDevice_UIResolution");
    uintptr_t determine_ui = so_symbol(
        &so_mod, "_Z26Main_DetermineUIResolutionv");
    if (!fusion_ui_resolution || !determine_ui) {
        l_error("Could not locate Fusion's UI-resolution selector.");
    } else {
        hook_addr(determine_ui, (uintptr_t)&determine_vita_ui_resolution);
        l_success("Selected medium-density UI assets matched to 960x544.");
    }

    uintptr_t ui_module_update = so_symbol(
        &so_mod, "_ZN9UI_Module13Module_UpdateEf");
    /*
     * These exported pointer variables identify the exact live singleton
     * objects. The virtual update entries are a fallback for any equivalent
     * object constructed by a future flow in this pinned executable.
     */
    fusion_character_select_instance = (void **)so_symbol(
        &so_mod, "UI_CharacterSelect");
    fusion_character_level_instance = (void **)so_symbol(
        &so_mod, "UI_CharacterLevelUpScreen");
    fusion_level_select_instance = (void **)so_symbol(
        &so_mod, "UI_LevelSelect");
    fusion_frontend_instance = (void **)so_symbol(
        &so_mod, "UI_FrontEnd");
    fusion_attract_instance = (void **)so_symbol(
        &so_mod, "UI_AttractScreen");
    fusion_redbrick_instance = (void **)so_symbol(
        &so_mod, "UI_RedBrickScreen");
    fusion_prize_picker_instance = (void **)so_symbol(
        &so_mod, "UI_PrizePicker");
    fusion_character_select_update = so_symbol(
        &so_mod, "_ZN25UI_CharacterSelect_Module13Module_UpdateEf");
    fusion_character_level_update = so_symbol(
        &so_mod,
        "_ZN32UI_CharacterLevelUpScreen_Module13Module_UpdateEf");
    fusion_level_select_update = so_symbol(
        &so_mod, "_ZN21UI_LevelSelect_Module13Module_UpdateEf");
    fusion_frontend_update = so_symbol(
        &so_mod, "_ZN18UI_FrontEnd_Module13Module_UpdateEf");
    fusion_attract_update = so_symbol(
        &so_mod, "_ZN23UI_AttractScreen_Module13Module_UpdateEf");
    fusion_redbrick_update = so_symbol(
        &so_mod, "_ZN24UI_RedBrickScreen_Module13Module_UpdateEf");
    fusion_prize_picker_update = so_symbol(
        &so_mod, "_ZN21UI_PrizePicker_Module13Module_UpdateEf");
    fusion_character_select_handle_button = (FusionUIHandleButton)so_symbol(
        &so_mod,
        "_ZN25UI_CharacterSelect_Module12HandleButtonEN9UI_Module18UI_MAINMENU_BUTTONE");
    fusion_map_portrait_id = (FusionMapPortraitId)so_symbol(
        &so_mod, "_Z23mapPortraitIdToCharEnumi");
    fusion_character_select_portrait =
        (FusionCharacterSelectPortrait)so_symbol(
            &so_mod,
            "_ZN25UI_CharacterSelect_Module14SelectPortraitEii");
    fusion_get_current_module = (FusionGetCurrentModule)so_symbol(
        &so_mod, "_Z23geMain_GetCurrentModulev");
    fusion_flash_get_scale = (FusionFlashGetVec2)so_symbol(
        &so_mod, "_Z23fnFlashElement_GetScaleP14fnFLASHELEMENT");
    fusion_flash_get_world_matrix =
        (FusionFlashGetWorldMatrix)so_symbol(
            &so_mod,
            "_Z29fnFlashElement_GetWorldMatrixP14fnFLASHELEMENT");
    fusion_flash_set_scale = (FusionFlashSetVec2)so_symbol(
        &so_mod,
        "_Z23fnFlashElement_SetScaleP14fnFLASHELEMENTPK7f32vec2");
    fusion_flash_set_visibility = (FusionFlashSetVisibility)so_symbol(
        &so_mod,
        "_Z28fnFlashElement_SetVisibilityP14fnFLASHELEMENTb");
    fusion_flash_set_opacity = (FusionFlashSetOpacity)so_symbol(
        &so_mod, "_Z25fnFlashElement_SetOpacityP14fnFLASHELEMENTf");
    fusion_flash_is_visible = (FusionFlashIsVisible)so_symbol(
        &so_mod, "_Z24fnFlashElement_IsVisibleP14fnFLASHELEMENTb");
    fusion_flash_get_opacity = (FusionFlashGetOpacity)so_symbol(
        &so_mod, "_Z25fnFlashElement_GetOpacityP14fnFLASHELEMENT");
    fusion_flash_get_uv = (FusionFlashGetUV)so_symbol(
        &so_mod, "_Z20fnFlashElement_GetUVP14fnFLASHELEMENT");
    fusion_character_level_select = (FusionCharacterLevelSelect)so_symbol(
        &so_mod, "_ZN32UI_CharacterLevelUpScreen_Module11SelectLevelEi");
    fusion_level_select_move = (FusionLevelSelectMove)so_symbol(
        &so_mod, "_ZN21UI_LevelSelect_Module19Level_SetAutoScrollEi");
    fusion_redbrick_select = (FusionRedBrickSelect)so_symbol(
        &so_mod, "_ZN24UI_RedBrickScreen_Module14SelectRedBrickEi");
    fusion_redbrick_set_up = (FusionRedBrickSetUp)so_symbol(
        &so_mod, "_ZN24UI_RedBrickScreen_Module14SetRedBricksUpEv");
    fusion_redbrick_is_bought = (FusionRedBrickIsBought)so_symbol(
        &so_mod, "_Z25SaveGame_IsRedBrickBoughtjb");
    fusion_get_active_options = (FusionGetActiveOptions)so_symbol(
        &so_mod, "_Z23geSave_GetActiveOptionsv");
    fusion_runtime_sound_volume = (uint8_t *)so_symbol(
        &so_mod, "leMain_SoundVolume");
    if (!ui_module_update || !fusion_get_current_module ||
        !fusion_flash_is_visible) {
        l_error("Could not locate Fusion's spatial menu-navigation path.");
    } else {
        int ui_hook = hook_ui_module_update(ui_module_update);
        if (ui_hook < 0) {
            l_error("Could not install Fusion's spatial-menu update hook: "
                    "%d.", ui_hook);
        } else {
            l_success("Enabled four-way spatial Fusion menu navigation and "
                      "native Cross activation for Vita UI modules.");
        }
    }
    if (!fusion_character_select_instance ||
        !fusion_character_select_update ||
        !fusion_character_select_handle_button ||
        !fusion_map_portrait_id || !fusion_character_select_portrait) {
        l_error("Could not locate Fusion's character-selection controller "
                "path; L/R page switching is disabled.");
    } else {
        l_success("Delegated Character Select D-pad/Cross to its native "
                  "portrait state machine and mapped L/R page changes.");
    }
    if (!fusion_flash_get_scale || !fusion_flash_set_scale) {
        l_error("Could not locate Fusion's Flash scale helpers; controller "
                "focus pulsing is disabled.");
    } else {
        l_success("Enabled stable animated focus from each Flash element's "
                  "live scale multiplier.");
    }
    if (!fusion_character_level_instance ||
        !fusion_character_level_update ||
        !fusion_character_level_select) {
        l_error("Could not locate the five-item character power-up row; "
                "native row focus is disabled.");
    } else {
        l_success("Enabled native D-pad focus for all five character "
                  "power-up choices.");
    }
    if (!fusion_level_select_instance || !fusion_level_select_update ||
        !fusion_level_select_move) {
        l_error("Could not locate Fusion's three-item level carousel; "
                "L/R level switching is disabled.");
    } else {
        l_success("Mapped L/R to Fusion's native Dojo/Arena/Temple Gallery "
                  "level carousel.");
    }
    if (!fusion_frontend_instance || !fusion_frontend_update ||
        !fusion_attract_instance || !fusion_attract_update ||
        !fusion_prize_picker_instance || !fusion_prize_picker_update) {
        l_error("Could not locate the Front End, Attract, or Daily Draw "
                "module type; "
                "their controller-specific focus paths are disabled.");
    } else {
        l_success("Enabled the Attract menu's modal four-icon Settings rail, "
                  "screen-scoped Front End Settings, and three-choice Daily "
                  "Draw controller navigation.");
    }
    if (!fusion_redbrick_instance || !fusion_redbrick_update ||
        !fusion_redbrick_select || !fusion_redbrick_set_up ||
        !fusion_redbrick_is_bought) {
        l_error("Could not locate Fusion's Red Brick grid/loadout helpers; "
                "staged controller selection is disabled.");
    } else {
        l_success("Enabled staged controller focus for three Red Brick "
                  "loadout slots and all nine powers.");
        if (!fusion_flash_get_world_matrix) {
            l_warn("Flash world positions are unavailable; Red Brick D-pad "
                   "navigation is using the pinned 3x3 fallback.");
        } else {
            l_success("Red Brick D-pad navigation follows each power's live "
                      "Flash position instead of assuming record order.");
        }
        if (!fusion_flash_set_visibility || !fusion_flash_set_opacity) {
            l_warn("Native Red Brick highlight helpers are unavailable; "
                   "falling back to scale focus.");
        }
    }
    if (!fusion_get_active_options || !fusion_runtime_sound_volume) {
        l_warn("Could not resolve Fusion's SFX-volume diagnostics.");
    }

    uintptr_t hit_sfx_address = so_symbol(
        &so_mod, "_Z24leGOHurtBound_PlayHitSFXP12GEGAMEOBJECT");
    uintptr_t hit_sfx_trampoline = 0;
    int hit_sfx_hook = hook_simple_arm_function(
        hit_sfx_address,
        UINT32_C(0xe590307c), /* ldr r3, [r0, #0x7c] */
        UINT32_C(0xe1a01000), /* mov r1, r0 */
        (uintptr_t)&vita_hit_sfx, &hit_sfx_trampoline);
    if (hit_sfx_hook < 0) {
        l_warn("Could not install the combat hit-SFX diagnostic hook: %d.",
               hit_sfx_hook);
    } else {
        fusion_hit_sfx = (FusionHitSfx)hit_sfx_trampoline;
        l_success("Installed rate-limited combat hit-SFX event diagnostics.");
    }

    uintptr_t edge_menu_button_address = so_symbol(
        &so_mod,
        "_ZN14EDGEMENUSYSTEM13buttonPressedEP13fnaTOUCHPOINT");
    uintptr_t edge_menu_button_trampoline = 0;
    int edge_menu_button_hook = hook_simple_arm_function(
        edge_menu_button_address,
        UINT32_C(0xe92d40f8), /* push {r3-r7, lr} */
        UINT32_C(0xed2d8b02), /* vpush {d8} */
        (uintptr_t)&vita_edge_menu_button_pressed,
        &edge_menu_button_trampoline);
    if (edge_menu_button_hook < 0) {
        l_error("Could not install the native Edge Menu button hook: %d.",
                edge_menu_button_hook);
    } else {
        fusion_edge_menu_button_pressed =
            (FusionEdgeMenuButtonPressed)edge_menu_button_trampoline;
        l_success("Enabled native Cross activation for the five-button "
                  "in-fight pause rail.");
    }

    uintptr_t touch_up_inside_address = so_symbol(
        &so_mod, "_Z20TouchUpInsideElementP14fnFLASHELEMENT");
    uintptr_t touch_up_inside_trampoline = 0;
    int touch_up_inside_hook = hook_simple_arm_function(
        touch_up_inside_address,
        UINT32_C(0xe92d43f0), /* push {r4-r9, lr} */
        UINT32_C(0xed2d8b02), /* vpush {d8} */
        (uintptr_t)&vita_touch_up_inside_element,
        &touch_up_inside_trampoline);
    if (touch_up_inside_hook < 0) {
        l_error("Could not install the pause-quit confirmation hook: %d.",
                touch_up_inside_hook);
    } else {
        fusion_touch_up_inside_element =
            (FusionTouchUpInsideElement)touch_up_inside_trampoline;
        l_success("Enabled native Cross activation for the pause-quit "
                  "Yes/No confirmation.");
    }

    uintptr_t hud_update_address = so_symbol(
        &so_mod, "_ZN9HUDSYSTEM6updateEP12GEWORLDLEVELf");
    uintptr_t hud_update_trampoline = 0;
    int hud_update_hook = hook_simple_arm_function(
        hud_update_address,
        UINT32_C(0xe92d43f0), /* push {r4-r9, lr} */
        UINT32_C(0xe24dd02c), /* sub sp, sp, #0x2c */
        (uintptr_t)&vita_hud_update, &hud_update_trampoline);
    if (hud_update_hook < 0) {
        l_error("Could not install the post-HUD confirmation-focus hook: %d.",
                hud_update_hook);
    } else {
        fusion_hud_update = (FusionHudUpdate)hud_update_trampoline;
        l_success("Kept pause-quit D-pad focus visible after Fusion HUD "
                  "animation updates.");
    }

    fusion_boss_hearts_go = (void **)so_symbol(&so_mod,
                                                "Hud_BossHeartsGO");
    fusion_boss_hearts_item = (void *)so_symbol(&so_mod,
                                                "Hud_BossHeartsItem");
    fusion_is_character = (FusionIsCharacter)so_symbol(
        &so_mod, "_Z23GOCharacter_IsCharacterPK12GEGAMEOBJECT");
    fusion_character_data = (FusionCharacterData)so_symbol(
        &so_mod, "_Z15GOCharacterDataP12GEGAMEOBJECT");
    fusion_animation_frame_count = (FusionAnimationFrameCount)so_symbol(
        &so_mod, "_Z31fnAnimation_GetStreamFrameCountP17fnANIMATIONSTREAM");
    fusion_animation_stream_status = (FusionAnimationStreamStatus)so_symbol(
        &so_mod, "_Z27fnAnimation_GetStreamStatusP17fnANIMATIONSTREAM");
    fusion_animation_find_playing = (FusionAnimationFindPlaying)so_symbol(
        &so_mod, "_Z29fnAnimation_FindStreamPlayingP17fnANIMATIONSTREAM");
    fusion_animation_next_frame = (FusionAnimationGetStreamNextFrame)so_symbol(
        &so_mod, "_Z30fnAnimation_GetStreamNextFrameP17fnANIMATIONSTREAMi");
    fusion_animation_position_update =
        (FusionAnimationObjectUpdate)so_symbol(
            &so_mod,
            "_Z26fnAnimation_PositionUpdateP17fnANIMATIONOBJECTP8fnOBJECT");
    fusion_animation_render_update =
        (FusionAnimationObjectUpdate)so_symbol(
            &so_mod,
            "_Z24fnAnimation_RenderUpdateP17fnANIMATIONOBJECTPv");
    fusion_flash_update_graph = (FusionFlashUpdateGraph)so_symbol(
        &so_mod, "_Z19fnFlash_UpdateGraphP8fnOBJECT");
    fusion_flash_update_internal_graph =
        (FusionFlashUpdateInternalGraph)so_symbol(
            &so_mod,
            "_Z27fnFlash_UpdateInternalGraphP8fnOBJECTPK7f32mat4");
    fusion_player_bar = (uint8_t *)so_symbol(&so_mod, "HudPlayerBar");
    fusion_percent_per_heart = (const uint32_t *)so_symbol(
        &so_mod, "gdv_percentPerHeart");
    uintptr_t play_anim_safe_address = so_symbol(
        &so_mod,
        "_Z22geFlashUI_PlayAnimSafeP17fnANIMATIONSTREAM15fnANIMATIONTYPEttffPvt");
    uintptr_t play_anim_safe_trampoline = 0;
    int play_anim_safe_hook = hook_simple_arm_function(
        play_anim_safe_address,
        UINT32_C(0xe3500000), /* cmp r0, #0 */
        UINT32_C(0xe92d0070), /* push {r4-r6} */
        (uintptr_t)&vita_play_anim_safe, &play_anim_safe_trampoline);
    if (play_anim_safe_hook < 0) {
        l_warn("Could not capture Fusion heart-animation calls: %d.",
               play_anim_safe_hook);
    } else {
        fusion_play_anim_safe =
            (FusionPlayAnimSafe)play_anim_safe_trampoline;
        l_success("Captured exact Fusion heart-animation arguments for "
                  "bounded transition replay and authored HUD evaluation.");
    }
    if (!fusion_animation_next_frame || !fusion_animation_position_update ||
        !fusion_animation_render_update || !fusion_flash_update_graph ||
        !fusion_flash_update_internal_graph) {
        l_warn("Fusion heart evaluation helpers are incomplete; transition "
               "traces remain active without unsafe frame forcing.");
    }
    uintptr_t texture_address = so_symbol(&so_mod,
        "_Z28fnFlashElement_RenderTextureP14fnFLASHELEMENTPK7f32vec4ffb");
    /* Hardware rejected the inferred mask/pivot layout: full hearts
     * were cut to half width. Never enable that geometry workaround. */
    heart_quad_clip_enabled = false;
    l_info("Heart-quad workaround rejected by hardware and disabled; "
           "original Fusion geometry is retained.");
    uintptr_t texture_trampoline = 0;
    int texture_hook = hook_simple_arm_function(texture_address,
        UINT32_C(0xe92d4ff0), /* push {r4-r11,lr} */
        UINT32_C(0xed2d8b04), /* vpush {d8-d9} */
        (uintptr_t)&vita_flash_render_texture, &texture_trampoline);
    if (texture_hook >= 0) {
        fusion_flash_render_texture = (FusionFlashRenderTexture)texture_trampoline;
        l_success("Bounded heart tracing follows actual texture/clip draw "
                  "state, not only the parent heart container.");
    } else l_warn("Heart texture tracing hook unavailable: %d.", texture_hook);
    uintptr_t flash_render_address = so_symbol(
        &so_mod, "_Z14fnFlash_RenderP8fnOBJECTP7f32mat412fnRENDERPASSj");
    uintptr_t flash_render_trampoline = 0;
    int flash_render_hook = hook_simple_arm_function(
        flash_render_address,
        UINT32_C(0xe59030f8), /* ldr r3, [r0, #0xf8] */
        UINT32_C(0xe92d40f0), /* push {r4-r7, lr} */
        (uintptr_t)&vita_flash_render, &flash_render_trampoline);
    if (flash_render_hook < 0) {
        l_warn("Could not install bounded heart Flash draw tracing: %d.",
               flash_render_hook);
    } else {
        fusion_flash_render = (FusionFlashRender)flash_render_trampoline;
        l_success("Enabled bounded heart Flash draw-submission tracing.");
    }
    uintptr_t player_heart_count_address = so_symbol(
        &so_mod, "_Z17Hud_SetHeartCountj");
    uintptr_t player_heart_count_trampoline = 0;
    int player_heart_count_hook = hook_simple_arm_function(
        player_heart_count_address,
        UINT32_C(0xe92d4ff0), /* push {r4-r11, lr} */
        UINT32_C(0xed2d8b02), /* vpush {d8} */
        (uintptr_t)&vita_set_heart_count,
        &player_heart_count_trampoline);
    if (player_heart_count_hook < 0) {
        l_warn("Could not enable player-heart state trace: %d.",
               player_heart_count_hook);
    } else {
        fusion_set_heart_count =
            (FusionSetHeartCount)player_heart_count_trampoline;
        l_success("Enabled bounded player-heart state trace for damage tests "
                  "(bar=%d, unit=%d, stream-status=%d, playing=%d).",
                  fusion_player_bar != NULL,
                  fusion_percent_per_heart != NULL,
                  fusion_animation_stream_status != NULL,
                  fusion_animation_find_playing != NULL);
    }
    uintptr_t boss_heart_count_address = so_symbol(
        &so_mod, "_Z21Hud_SetBossHeartCountP17HUDBOSSHEARTSICONj");
    uintptr_t boss_heart_count_trampoline = 0;
    int boss_heart_count_hook = hook_simple_arm_function(
        boss_heart_count_address,
        UINT32_C(0xe3002102), /* movw r2, #0x102 */
        UINT32_C(0xe6ff1071), /* uxth r1, r1 */
        (uintptr_t)&vita_set_boss_heart_count,
        &boss_heart_count_trampoline);
    if (!fusion_boss_hearts_go || !fusion_boss_hearts_item ||
        !fusion_is_character || !fusion_character_data ||
        !fusion_animation_frame_count || !fusion_animation_stream_status ||
        !fusion_animation_find_playing ||
        boss_heart_count_hook < 0) {
        l_warn("Could not install immediate boss-heart synchronization: %d.",
               boss_heart_count_hook);
    } else {
        fusion_set_boss_heart_count =
            (FusionSetBossHeartCount)boss_heart_count_trampoline;
        l_success("Synchronized character-boss health through Fusion's "
                  "native setter and traced the authored animation streams.");
    }

    fusion_ranged_tap = (FusionCombatPadHandler)so_symbol(
        &so_mod,
        "_ZN18GOCSPADRANGEDEVENT9handleTapEP12GEGAMEOBJECTPK17GOCSCOMBATPADDATA");
    fusion_ranged_held = (FusionCombatPadHandler)so_symbol(
        &so_mod,
        "_ZN18GOCSPADRANGEDEVENT10handleHeldEP12GEGAMEOBJECTPK17GOCSCOMBATPADDATA");
    if (!fusion_ranged_tap || !fusion_ranged_held) {
        l_error("Could not resolve Fusion's native ranged attack handlers; "
                "Square retains its native melee behavior.");
    } else {
        uintptr_t melee_tap_address = so_symbol(
            &so_mod,
            "_ZN17GOCSPADMELEEEVENT9handleTapEP12GEGAMEOBJECTPK17GOCSCOMBATPADDATA");
        uintptr_t melee_tap_trampoline = 0;
        int melee_tap_hook = hook_simple_arm_function(
            melee_tap_address,
            UINT32_C(0xe92d41f0), /* push {r4-r8, lr} */
            UINT32_C(0xe1a00001), /* mov r0, r1 */
            (uintptr_t)&vita_melee_tap, &melee_tap_trampoline);

        uintptr_t melee_held_address = so_symbol(
            &so_mod,
            "_ZN17GOCSPADMELEEEVENT10handleHeldEP12GEGAMEOBJECTPK17GOCSCOMBATPADDATA");
        uintptr_t melee_held_trampoline = 0;
        int melee_held_hook = hook_simple_arm_function(
            melee_held_address,
            UINT32_C(0xe92d40f0), /* push {r4-r7, lr} */
            UINT32_C(0xe1a00001), /* mov r0, r1 */
            (uintptr_t)&vita_melee_held, &melee_held_trampoline);

        int targetless_melee_patch = -5;
        if (melee_tap_hook < 0) {
            l_error("Could not install the selected-weapon tap hook: %d.",
                    melee_tap_hook);
        } else {
            fusion_melee_tap =
                (FusionCombatPadHandler)melee_tap_trampoline;
            targetless_melee_patch = patch_targetless_melee_branch(
                melee_tap_address);
            if (targetless_melee_patch < 0) {
                l_error("Could not install the strict targetless-melee "
                        "branch: %d.", targetless_melee_patch);
            }
        }
        if (melee_held_hook < 0) {
            l_error("Could not install the selected-weapon hold hook: %d.",
                    melee_held_hook);
        } else {
            fusion_melee_held =
                (FusionCombatPadHandler)melee_held_trampoline;
        }
        if (melee_tap_hook >= 0 && melee_held_hook >= 0 &&
            targetless_melee_patch >= 0) {
            l_success("Square now routes Fusion's native melee tap/hold "
                      "handlers to their ranged counterparts only while "
                      "Select's ranged mode is active, repeats ranged taps "
                      "while held, and bypasses the targetless ranged "
                      "fallback in melee mode.");
        }
    }

    uintptr_t update_pad_controls_address = so_symbol(
        &so_mod,
        "_ZN21LEPLAYERCONTROLSYSTEM17updatePadControlsEP12GEGAMEOBJECTP15GOCHARACTERDATAP18GOPLAYERDATAHEADER");
    fusion_character_orient = (FusionCharacterOrient)so_symbol(
        &so_mod,
        "_Z20leGOCharacter_OrientP12GEGAMEOBJECTP15GOCHARACTERDATAsb");
    fusion_character_get_turn_speed =
        (FusionCharacterGetTurnSpeed)so_symbol(
            &so_mod, "_Z26leGOCharacter_GetTurnSpeedv");
    uintptr_t update_pad_controls_trampoline = 0;
    int update_pad_controls_hook = hook_simple_arm_function(
        update_pad_controls_address,
        UINT32_C(0xe92d47f0), /* push {r4-r10, lr} */
        UINT32_C(0xe1a04003), /* mov r4, r3 */
        (uintptr_t)&vita_update_pad_controls,
        &update_pad_controls_trampoline);
    if (!fusion_character_orient || !fusion_character_get_turn_speed ||
        update_pad_controls_hook < 0) {
        l_error("Could not install stationary held-Square ranged aiming: %d.",
                update_pad_controls_hook);
    } else {
        fusion_update_pad_controls =
            (FusionUpdatePadControls)update_pad_controls_trampoline;
        l_success("Held Square now uses the left stick for camera-relative "
                  "ranged aiming while suppressing character movement.");
    }

    uintptr_t controller_poll = so_symbol(
        &so_mod, "_Z18fnaController_PollP13fnINPUTDEVICE");
    int controller_hook = hook_empty_arm_stub(
        controller_poll, (uintptr_t)&fnaController_Poll_soloader);
    if (controller_hook < 0) {
        l_error("Could not install the native controller hook: %d.",
                controller_hook);
    } else {
        l_success("Installed native Fusion controller hook at %p.",
                  (void *)controller_poll);
    }
}
