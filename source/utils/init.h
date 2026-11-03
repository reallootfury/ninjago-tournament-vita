/*
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See licenses/upstream-MIT.txt for details.
 */

/**
 * @file  init.h
 * @brief so-loader initialization routines.
 */

#ifndef SOLOADER_INIT_H
#define SOLOADER_INIT_H

#include <so_util/so_util.h>

#ifdef __cplusplus
extern "C" {
#endif

void resolve_imports(so_module *mod);

void so_patch();

/* Run controller actions whose native Fusion event is not repeated itself. */
void ninjago_patch_poll(void);

void soloader_init_all();

#ifdef __cplusplus
};
#endif

#endif // SOLOADER_INIT_H
