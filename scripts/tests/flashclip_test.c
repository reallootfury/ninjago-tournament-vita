#include "utils/flashclip.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

static NjFlashQuad quad(float width, float height) {
    NjFlashQuad q = {.width=width, .height=height, .uv={0,0,1,1}};
    q.matrix[0]=q.matrix[5]=q.matrix[10]=q.matrix[15]=1;
    return q;
}
static void near(float a, float b) { assert(fabsf(a-b)<0.0001f); }
int main(void) {
    NjFlashQuad q=quad(40,40), mask=quad(40,30), out;
    mask.matrix[13]=10;
    assert(nj_flash_clip_quad(&q,&mask,&out)==2);
    near(out.height,30); near(out.pivot_y,-1.0f/3); near(out.uv[1],0.25f);
    near(out.matrix[13] - out.pivot_y*out.height,10);
    near(out.uv[3],1); near(q.height,40); /* Source is never changed. */
    q.pivot_x=q.pivot_y=0.5f; q.matrix[0]=q.matrix[5]=2;
    q.matrix[12]=100; q.matrix[13]=80;
    mask=quad(40,40); mask.matrix[12]=80; mask.matrix[13]=80;
    assert(nj_flash_clip_quad(&q,&mask,&out)==2);
    near(out.width,20); near(out.height,20); near(out.uv[0],0.25f);
    near(out.uv[2],0.75f); near(out.uv[1],0.5f); near(out.uv[3],1);
    q=quad(40,40); q.matrix[0]=-1; q.matrix[12]=40;
    mask=quad(20,40);
    assert(nj_flash_clip_quad(&q,&mask,&out)==2);
    near(out.uv[0],0.5f); near(out.uv[2],1);
    mask=quad(100,100); mask.matrix[12]=-10;
    assert(nj_flash_clip_quad(&q,&mask,&out)==1);
    mask.matrix[12]=200; assert(nj_flash_clip_quad(&q,&mask,&out)==0);
    mask.matrix[12]=0; mask.matrix[4]=0.1f;
    assert(nj_flash_clip_quad(&q,&mask,&out)==-1);
    mask=quad(40,40); q.width=NAN;
    assert(nj_flash_clip_quad(&q,&mask,&out)==-1);
    q=quad(40,40); q.matrix[0]=0;
    assert(nj_flash_clip_quad(&q,&mask,&out)==-1);
    /* Progressive authored movement exposes the same grey base underneath. */
    q=quad(40,40); mask=quad(40,40);
    for (int y=0;y<=40;++y) {
        mask.matrix[13]=(float)y;
        int result=nj_flash_clip_quad(&q,&mask,&out);
        if (!y) assert(result==1);
        else if (y==40) assert(result==0);
        else { assert(result==2); near(out.height,40-y); near(out.uv[1],y/40.0f); }
    }
    puts("PASS: original-quad/UV clipping, pivots, scale, mirroring, authored progression and safe fallback");
}
