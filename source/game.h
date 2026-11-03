#ifndef NINJAGO_GAME_H
#define NINJAGO_GAME_H

#include <stdbool.h>
#include <stdint.h>

int ninjago_game_bind(void);
int ninjago_game_boot(void);
void ninjago_game_render(void);
void ninjago_game_pause(void);
void ninjago_game_resume(void);
void ninjago_game_back(void);
void ninjago_game_touch_down(int id, float x, float y, float pressure,
                             bool first_contact);
void ninjago_game_touch_move(int id, float x, float y, float pressure);
void ninjago_game_touch_up(int id, float x, float y, float pressure,
                           bool last_contact);
void ninjago_game_cancel_touches(void);
void ninjago_game_request_exit(void);
bool ninjago_game_exit_requested(void);
bool ninjago_game_paused(void);
bool ninjago_game_is_loading(void);

#endif
