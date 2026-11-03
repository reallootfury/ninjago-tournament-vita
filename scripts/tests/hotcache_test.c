/* Host-only validation of the exact portable cache/SHA code linked on Vita. */
#include "utils/hotcache.h"
#include "utils/sha256.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void check_hash(const char *input, const char *expected) {
    NjSha256 h; uint8_t out[32]; char hex[65];
    nj_sha256_init(&h);
    for (size_t i=0;i<strlen(input);++i) nj_sha256_update(&h,input+i,1);
    nj_sha256_final(&h,out);
    for (unsigned i=0;i<32;++i) sprintf(hex+2*i,"%02x",out[i]);
    assert(!strcmp(hex,expected));
}
static void write_profile(const HotcacheConfig *c, const uint32_t *pages, uint32_t count) {
    ObbPageFileHeader h={.magic={'N','H','P','1'}, .version=1,
        .page_size=c->page_size, .obb_size=c->source_size, .entry_count=count};
    memcpy(h.obb_sha256,c->source_sha256,32);
    FILE *f=fopen(c->profile,"wb"); assert(f);
    assert(fwrite(&h,sizeof(h),1,f)==1);
    assert(fwrite(pages,4,count,f)==count); assert(!fclose(f));
}
static int cancel(int stage, uint64_t done, uint64_t total, void *user) {
    (void)done; (void)total; return stage==*(int *)user;
}
int main(void) {
    check_hash("","e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    check_hash("abc","ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    check_hash("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    check_hash("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");
    NjSha256 million; uint8_t million_digest[32]; char million_hex[65];
    char a[1000]; memset(a,'a',sizeof(a)); nj_sha256_init(&million);
    for (unsigned i=0;i<1000;++i) nj_sha256_update(&million,a,sizeof(a));
    nj_sha256_final(&million,million_digest);
    for (unsigned i=0;i<32;++i) sprintf(million_hex+2*i,"%02x",million_digest[i]);
    assert(!strcmp(million_hex,"cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
    char root[]="/tmp/ninjago-hotcache-test-XXXXXX"; assert(mkdtemp(root));
    char source[256],profile[256],data[256],index[256],stamp[300];
    snprintf(source,sizeof(source),"%s/source",root);
    snprintf(profile,sizeof(profile),"%s/profile",root);
    snprintf(data,sizeof(data),"%s/cache",root);
    snprintf(index,sizeof(index),"%s/index",root);
    snprintf(stamp,sizeof(stamp),"%s.source",data);
    uint8_t bytes[101],digest[32];
    for (unsigned i=0;i<sizeof(bytes);++i) bytes[i]=(uint8_t)i;
    NjSha256 h; nj_sha256_init(&h); nj_sha256_update(&h,bytes,sizeof(bytes)); nj_sha256_final(&h,digest);
    FILE *f=fopen(source,"wb"); assert(f); assert(fwrite(bytes,1,sizeof(bytes),f)==sizeof(bytes)); fclose(f);
    HotcacheConfig c={.source=source,.profile=profile,.data=data,.index=index,
        .source_size=sizeof(bytes),.page_size=32,.maximum_pages=2,.source_sha256=digest};
    assert(hotcache_prepare(&c,NULL,NULL)==0);
    uint32_t pages[]={3,1,0,2}; write_profile(&c,pages,4);
    assert(hotcache_prepare(&c,NULL,NULL)==1);
    assert(hotcache_source_matches(&c));
    uint8_t cached[64]; f=fopen(data,"rb"); assert(f);
    assert(fread(cached,1,64,f)==64 && fgetc(f)==EOF); fclose(f);
    assert(!memcmp(cached,bytes+96,5));
    for (unsigned i=5;i<32;++i) assert(cached[i]==0);
    assert(!memcmp(cached+32,bytes+32,32));
    ObbPageFileHeader header; ObbHotcacheEntry entries[2];
    f=fopen(index,"rb"); assert(f); assert(fread(&header,sizeof(header),1,f)==1);
    assert(header.entry_count==2 && !memcmp(header.magic,"NHC1",4));
    assert(fread(entries,sizeof(*entries),2,f)==2 && fgetc(f)==EOF); fclose(f);
    assert(entries[0].page==1 && entries[0].cache_slot==1);
    assert(entries[1].page==3 && entries[1].cache_slot==0);
    for (int stage=0;stage<2;++stage) assert(hotcache_prepare(&c,cancel,&stage)==-2);
    f=fopen(data,"rb"); assert(f); uint8_t preserved[64];
    assert(fread(preserved,1,64,f)==64); fclose(f); assert(!memcmp(preserved,cached,64));
    uint32_t duplicate[]={0,0}; write_profile(&c,duplicate,2); assert(hotcache_prepare(&c,NULL,NULL)==-1);
    uint32_t invalid[]={4}; write_profile(&c,invalid,1); assert(hotcache_prepare(&c,NULL,NULL)==-1);
    write_profile(&c,pages,4); f=fopen(profile,"ab"); fputc(1,f); fclose(f);
    assert(hotcache_prepare(&c,NULL,NULL)==-1);
    write_profile(&c,pages,4); digest[0]^=1; assert(hotcache_prepare(&c,NULL,NULL)==-1); digest[0]^=1;
    f=fopen(source,"ab"); fputc(9,f); fclose(f);
    assert(!hotcache_source_matches(&c)); assert(hotcache_prepare(&c,NULL,NULL)==-1);
    remove(source); remove(profile); remove(data); remove(index); remove(stamp); assert(!rmdir(root));
    puts("PASS: SHA vectors, first-touch mapping, bounded cache, final-page padding, cancellation, malformed profiles, source mismatch");
    return 0;
}
