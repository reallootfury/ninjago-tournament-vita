#include "utils/livearea.h"
#include "utils/logger.h"
#include "utils/sha256.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>

static int asset_hash(const char *path, uint8_t digest[32], int *error) {
    FILE *f = fopen(path,"rb");
    if (!f) { *error = errno; return 0; }
    NjSha256 ctx; nj_sha256_init(&ctx);
    uint8_t block[4096]; size_t n;
    while ((n=fread(block,1,sizeof(block),f))) nj_sha256_update(&ctx,block,n);
    int ok=!ferror(f);
    *error = ok ? 0 : errno;
    fclose(f); nj_sha256_final(&ctx,digest);
    return ok;
}
void livearea_log_assets(void) {
    static const char *names[]={"template.xml","bg0.png","startup.png"};
    for (unsigned i=0;i<3;++i) {
        char app[160], cached[160]; uint8_t a[32], b[32];
        int app_error=0, cache_error=0;
        snprintf(app,sizeof(app),"app0:/sce_sys/livearea/contents/%s",names[i]);
        snprintf(cached,sizeof(cached),"ur0:/appmeta/NINJA0001/livearea/contents/%s",names[i]);
        int have_app=asset_hash(app,a,&app_error);
        int have_cache=asset_hash(cached,b,&cache_error);
        if (have_app) {
            static const char digits[]="0123456789abcdef";
            char hex[65];
            for (unsigned j=0;j<32;++j) {
                hex[j*2]=digits[a[j]>>4]; hex[j*2+1]=digits[a[j]&15];
            }
            hex[64]=0;
            l_info("LiveArea packaged %s SHA-256=%s.",names[i],hex);
        }
        if (!have_app) {
            l_warn("Packaged LiveArea %s unreadable: errno=%d. "
                   "Game startup continues without a shell refresh.",names[i],app_error);
        } else if (!have_cache) {
            /* Access failure is not proof of stale or missing shell artwork. */
            l_info("LiveArea %s: packaged asset readable; shell cache "
                   "unavailable (errno=%d). Cache equality is unknown.",names[i],cache_error);
        } else {
            l_info("LiveArea %s: packaged/shell-cache SHA-256 equal=%d.",
                   names[i],!memcmp(a,b,32));
        }
    }
    l_info("LiveArea check is read-only; no promoter or shell database "
           "operations are performed.");
}
