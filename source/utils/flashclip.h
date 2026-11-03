#ifndef NINJAGO_FLASHCLIP_H
#define NINJAGO_FLASHCLIP_H

/* Fusion's original textured quad, including its authored pivot and UVs. */
typedef struct NjFlashQuad {
    float width, height, pivot_x, pivot_y;
    float uv[4];
    float matrix[16];
} NjFlashQuad;

/* -1=unsupported/invalid (leave native rendering alone), 0=fully clipped,
 * 1=unchanged, 2=cropped. Only axis-aligned affine UI transforms are supported.
 * No health values or authored animation frames are synthesized here. */
int nj_flash_clip_quad(const NjFlashQuad *quad, const NjFlashQuad *mask,
                       NjFlashQuad *out);

#endif
