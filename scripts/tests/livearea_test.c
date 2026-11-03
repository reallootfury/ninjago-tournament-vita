/* Run the actual read-only startup check with missing/matching/stale assets. */
#include "utils/livearea.h"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static unsigned missing, unavailable, equal, unequal, completed;
void test_log(const char *format, ...) {
    char text[512]; va_list args;
    va_start(args,format); vsnprintf(text,sizeof(text),format,args); va_end(args);
    if (strstr(text,"Packaged LiveArea")) ++missing;
    if (strstr(text,"Cache equality is unknown")) ++unavailable;
    if (strstr(text,"SHA-256 equal=1")) ++equal;
    if (strstr(text,"SHA-256 equal=0")) ++unequal;
    if (strstr(text,"read-only")) ++completed;
}
static void write_asset(const char *base, const char *name, const char *data) {
    char path[256]; snprintf(path,sizeof(path),"%s/%s",base,name);
    FILE *f=fopen(path,"wb"); assert(f); assert(fputs(data,f)>=0); assert(!fclose(f));
}
static void reset(void) { missing=unavailable=equal=unequal=completed=0; }
int main(void) {
    char root[]="/tmp/ninjago-livearea-test-XXXXXX"; assert(mkdtemp(root)); assert(!chdir(root));
    livearea_log_assets(); assert(missing==3 && completed==1);
    const char *app="app0:/sce_sys/livearea/contents";
    const char *cached="ur0:/appmeta/NINJA0001/livearea/contents";
    const char *dirs[]={"app0:","app0:/sce_sys","app0:/sce_sys/livearea",app,
        "ur0:","ur0:/appmeta","ur0:/appmeta/NINJA0001","ur0:/appmeta/NINJA0001/livearea",cached};
    for (unsigned i=0;i<9;++i) assert(!mkdir(dirs[i],0700));
    const char *names[]={"template.xml","bg0.png","startup.png"};
    for (unsigned i=0;i<3;++i) write_asset(app,names[i],names[i]);
    reset(); livearea_log_assets(); assert(!missing && unavailable==3 && completed==1);
    for (unsigned i=0;i<3;++i) write_asset(cached,names[i],names[i]);
    reset(); livearea_log_assets(); assert(equal==3 && completed==1);
    write_asset(cached,"startup.png","OLD ARTWORK");
    reset(); livearea_log_assets(); assert(equal==2 && unequal==1 && completed==1);
    char path[256]; snprintf(path,sizeof(path),"%s/startup.png",cached);
    FILE *f=fopen(path,"rb"); assert(f); char old[32]={0}; assert(fread(old,1,11,f)==11); fclose(f);
    assert(!strcmp(old,"OLD ARTWORK")); /* Diagnostics never rewrite shell data. */
    for (unsigned i=0;i<3;++i) {
        snprintf(path,sizeof(path),"%s/%s",app,names[i]); assert(!remove(path));
        snprintf(path,sizeof(path),"%s/%s",cached,names[i]); assert(!remove(path));
    }
    for (int i=8;i>=0;--i) assert(!rmdir(dirs[i]));
    assert(!chdir("/tmp")); assert(!rmdir(root));
    puts("PASS: read-only LiveArea startup survives absent, unavailable-cache, matching and stale assets");
    return 0;
}
