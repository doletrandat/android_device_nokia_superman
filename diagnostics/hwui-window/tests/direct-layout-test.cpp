#include "../direct-buffer-layout.h"
#include <assert.h>
#include <stdio.h>
int main() {
    unsigned char source[48], dest[40];
    for(unsigned i=0;i<48;i++)source[i]=i;
    memset(dest,0xcd,sizeof(dest));
    assert(directPack(dest+4,32,source,48,4,2,6));
    assert(!memcmp(dest+4,source,16) && !memcmp(dest+20,source+24,16));
    for(unsigned i=0;i<4;i++)assert(dest[i]==0xcd && dest[i+36]==0xcd);
    unsigned char unchanged[40];memcpy(unchanged,dest,40);
    assert(!directPack(dest,31,source,48,4,2,6));
    assert(!directPack(dest,40,source,47,4,2,6));
    assert(!directPack(dest,40,source,48,4,2,3));
    assert(!directPack(dest,40,source,48,0,2,6));
    assert(!directPack(nullptr,40,source,48,4,2,6));
    assert(!directPack(dest,40,nullptr,48,4,2,6));
    assert(!memcmp(unchanged,dest,40));
    assert(directLayout(1080,2000,2048,16384000));
    assert(!directLayout(1081,2000,2048,16384000));
    assert(!directLayout(1080,2001,2048,16392192));
    assert(!directLayout(1080,2000,2049,16392000));
    assert(!directLayout(0xffffffff,0xffffffff,0xffffffff,0xffffffffffffffffULL));
    puts("PASS padded rows, output canaries, truncation/null/overflow/bounds rejection");
}
