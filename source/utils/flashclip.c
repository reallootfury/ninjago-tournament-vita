#include "utils/flashclip.h"
#include <math.h>

static int valid(const NjFlashQuad *q) {
    if (!q || !isfinite(q->width) || !isfinite(q->height) ||
        q->width <= 0 || q->height <= 0 ||
        !isfinite(q->pivot_x) || !isfinite(q->pivot_y)) return 0;
    for (unsigned i = 0; i < 16; ++i)
        if (!isfinite(q->matrix[i])) return 0;
    for (unsigned i = 0; i < 4; ++i)
        if (!isfinite(q->uv[i])) return 0;
    const float *m = q->matrix;
    return fabsf(m[0]) > 0.000001f && fabsf(m[5]) > 0.000001f &&
        fabsf(m[1]) < 0.000001f && fabsf(m[4]) < 0.000001f &&
        fabsf(m[3]) < 0.000001f && fabsf(m[7]) < 0.000001f &&
        fabsf(m[15] - 1.0f) < 0.000001f;
}

static void bounds(const NjFlashQuad *q, float box[4]) {
    float x0 = -q->pivot_x * q->width;
    float y0 = -q->pivot_y * q->height;
    float x1 = q->matrix[0] * x0 + q->matrix[12];
    float y1 = q->matrix[5] * y0 + q->matrix[13];
    float x2 = q->matrix[0] * (x0 + q->width) + q->matrix[12];
    float y2 = q->matrix[5] * (y0 + q->height) + q->matrix[13];
    box[0] = fminf(x1, x2); box[1] = fminf(y1, y2);
    box[2] = fmaxf(x1, x2); box[3] = fmaxf(y1, y2);
}

int nj_flash_clip_quad(const NjFlashQuad *q, const NjFlashQuad *mask,
                       NjFlashQuad *out) {
    if (!out || !valid(q) || !valid(mask)) return -1;
    float b[4], c[4]; bounds(q, b); bounds(mask, c);
    float left = fmaxf(b[0], c[0]), top = fmaxf(b[1], c[1]);
    float right = fminf(b[2], c[2]), bottom = fminf(b[3], c[3]);
    for (unsigned i = 0; i < 4; ++i)
        if (!isfinite(b[i]) || !isfinite(c[i])) return -1;
    if (right <= left || bottom <= top) return 0;
    *out = *q;
    if (left == b[0] && top == b[1] && right == b[2] && bottom == b[3]) return 1;
    float ax = (left - q->matrix[12]) / q->matrix[0];
    float bx = (right - q->matrix[12]) / q->matrix[0];
    float ay = (top - q->matrix[13]) / q->matrix[5];
    float by = (bottom - q->matrix[13]) / q->matrix[5];
    float x0 = fminf(ax, bx), x1 = fmaxf(ax, bx);
    float y0 = fminf(ay, by), y1 = fmaxf(ay, by);
    float original_x0 = -q->pivot_x * q->width;
    float original_y0 = -q->pivot_y * q->height;
    out->width = x1 - x0; out->height = y1 - y0;
    out->pivot_x = -x0 / out->width; out->pivot_y = -y0 / out->height;
    float du = q->uv[2] - q->uv[0], dv = q->uv[3] - q->uv[1];
    out->uv[0] = q->uv[0] + du * ((x0 - original_x0) / q->width);
    out->uv[2] = q->uv[0] + du * ((x1 - original_x0) / q->width);
    out->uv[1] = q->uv[1] + dv * ((y0 - original_y0) / q->height);
    out->uv[3] = q->uv[1] + dv * ((y1 - original_y0) / q->height);
    return valid(out) ? 2 : -1;
}
