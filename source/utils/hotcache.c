/* Portable, private-cache builder. Never modifies the owned source OBB. */
#include "utils/hotcache.h"
#include "utils/sha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>
#include <sys/stat.h>

static int by_page(const void *a, const void *b) {
    uint32_t x=((const ObbHotcacheEntry *)a)->page;
    uint32_t y=((const ObbHotcacheEntry *)b)->page;
    return (x>y)-(x<y);
}
static int finish(FILE **file) {
    FILE *f=*file;
    *file=NULL;
    int ok=fflush(f)==0;
    if (ok) ok=fsync(fileno(f))==0;
    if (fclose(f)!=0) ok=0;
    return ok;
}
typedef struct __attribute__((packed)) SourceStamp {
    char magic[4];
    uint64_t size;
    int64_t mtime, ctime;
    uint8_t hash[32];
} SourceStamp;
static int stamp_path(const HotcacheConfig *c, char path[512]) {
    int n=snprintf(path,512,"%s.source",c->data);
    return n>=0 && n<512;
}
int hotcache_source_matches(const HotcacheConfig *c) {
    char path[512]; struct stat st; SourceStamp stamp;
    if (!stamp_path(c,path) || stat(c->source,&st)!=0 ||
        (uint64_t)st.st_size!=c->source_size) return 0;
    /* Legacy desktop caches were SHA-verified by their builder. */
    if (access(path,F_OK)!=0) return 1;
    FILE *f=fopen(path,"rb");
    if (!f) return 0;
    int valid=fread(&stamp,sizeof(stamp),1,f)==1 && fgetc(f)==EOF && !ferror(f);
    fclose(f);
    return valid && !memcmp(stamp.magic,"NHS1",4) &&
        !memcmp(stamp.hash,c->source_sha256,32) &&
        stamp.size==c->source_size && stamp.mtime==st.st_mtime && stamp.ctime==st.st_ctime;
}
int hotcache_prepare(const HotcacheConfig *c, HotcacheProgress progress, void *user) {
    if (!c || !c->page_size || c->page_size>65536 || !c->maximum_pages ||
        c->source_size>LONG_MAX || !c->source_size) return -1;
    FILE *profile=fopen(c->profile,"rb");
    if (!profile) return 0;
    FILE *source=NULL, *data=NULL, *index=NULL;
    uint8_t *buffer=NULL;
    ObbHotcacheEntry *entries=NULL;
    char data_temp[512], index_temp[512];
    int result=-1, own_temps=0;
    ObbPageFileHeader h;
    uint64_t pages=(c->source_size+c->page_size-1)/c->page_size;
    if (pages>UINT32_MAX || fread(&h,sizeof(h),1,profile)!=1 ||
        memcmp(h.magic,"NHP1",4) || h.version!=1 || h.reserved ||
        h.page_size!=c->page_size || h.obb_size!=c->source_size ||
        memcmp(h.obb_sha256,c->source_sha256,32) || !h.entry_count ||
        h.entry_count>pages || h.entry_count>65536) goto done;
    entries=malloc(h.entry_count*sizeof(*entries));
    buffer=malloc(c->page_size);
    if (!entries || !buffer) goto done;
    for (uint32_t i=0;i<h.entry_count;++i) {
        entries[i].cache_slot=i;
        if (fread(&entries[i].page,4,1,profile)!=1 || entries[i].page>=pages) goto done;
    }
    if (fgetc(profile)!=EOF || ferror(profile)) goto done;
    /* Validate the entire profile, not only the bounded prefix we will copy. */
    qsort(entries,h.entry_count,sizeof(*entries),by_page);
    for (uint32_t i=1;i<h.entry_count;++i)
        if (entries[i-1].page==entries[i].page) goto done;
    uint32_t count=h.entry_count<c->maximum_pages?h.entry_count:c->maximum_pages;
    uint32_t kept=0;
    for (uint32_t i=0;i<h.entry_count;++i)
        if (entries[i].cache_slot<count) entries[kept++]=entries[i];
    /* Sorted index still maps to first-touch slots. */
    struct stat before, after;
    source=fopen(c->source,"rb");
    if (!source || fstat(fileno(source),&before) ||
        (uint64_t)before.st_size!=c->source_size) goto done;
    NjSha256 hash; uint8_t digest[32];
    nj_sha256_init(&hash);
    for (uint64_t done=0;done<c->source_size;) {
        size_t n=(size_t)(c->source_size-done);
        if (n>c->page_size) n=c->page_size;
        if (fread(buffer,1,n,source)!=n) goto done;
        nj_sha256_update(&hash,buffer,n); done+=n;
        if (progress && progress(0,done,c->source_size,user)) { result=-2; goto done; }
    }
    nj_sha256_final(&hash,digest);
    if (memcmp(digest,c->source_sha256,32)) goto done;
    int n=snprintf(data_temp,sizeof(data_temp),"%s.tmp",c->data);
    int m=snprintf(index_temp,sizeof(index_temp),"%s.tmp",c->index);
    if (n<0 || n>=(int)sizeof(data_temp) || m<0 || m>=(int)sizeof(index_temp)) goto done;
    own_temps=1;
    data=fopen(data_temp,"wb");
    if (!data) goto done;
    /* Order reads and writes by first touch, not sorted page number. */
    uint32_t *ordered=malloc(count*sizeof(*ordered));
    if (!ordered) goto done;
    for (uint32_t i=0;i<count;++i) ordered[entries[i].cache_slot]=entries[i].page;
    for (uint32_t slot=0;slot<count;++slot) {
        uint64_t offset=(uint64_t)ordered[slot]*c->page_size;
        size_t bytes=(size_t)(c->source_size-offset);
        if (bytes>c->page_size) bytes=c->page_size;
        memset(buffer,0,c->page_size);
        if (fseek(source,(long)offset,SEEK_SET) || fread(buffer,1,bytes,source)!=bytes ||
            fwrite(buffer,1,c->page_size,data)!=c->page_size) { free(ordered); goto done; }
        if (progress && progress(1,slot+1,count,user)) { free(ordered); result=-2; goto done; }
    }
    free(ordered);
    if (fstat(fileno(source),&after) || before.st_size!=after.st_size ||
        before.st_mtime!=after.st_mtime || before.st_ctime!=after.st_ctime) goto done;
    if (!finish(&data)) goto done;
    index=fopen(index_temp,"wb");
    if (!index) goto done;
    memcpy(h.magic,"NHC1",4); h.entry_count=count;
    if (fwrite(&h,sizeof(h),1,index)!=1 ||
        fwrite(entries,sizeof(*entries),count,index)!=count || !finish(&index)) goto done;
    /* Invalidate the old index first: interruption can only disable caching,
     * never pair new data with an old mapping. All game I/O starts afterward. */
    if (access(c->index,F_OK)==0 && remove(c->index)!=0) goto done;
    if (access(c->data,F_OK)==0 && remove(c->data)!=0) goto done;
    if (rename(data_temp,c->data) || rename(index_temp,c->index)) goto done;
    char identity[512];
    SourceStamp stamp={.magic={'N','H','S','1'}, .size=c->source_size,
                       .mtime=after.st_mtime, .ctime=after.st_ctime};
    memcpy(stamp.hash,c->source_sha256,32);
    if (!stamp_path(c,identity)) { remove(c->index); goto done; }
    FILE *stamp_file=fopen(identity,"wb");
    if (!stamp_file) { remove(c->index); goto done; }
    if (fwrite(&stamp,sizeof(stamp),1,stamp_file)!=1 || !finish(&stamp_file)) {
        if (stamp_file) fclose(stamp_file);
        remove(c->index); goto done;
    }
    result=1;
done:
    if (profile) fclose(profile);
    if (source) fclose(source);
    if (data) fclose(data);
    if (index) fclose(index);
    if (own_temps) { remove(data_temp); remove(index_temp); }
    free(entries); free(buffer);
    return result;
}
