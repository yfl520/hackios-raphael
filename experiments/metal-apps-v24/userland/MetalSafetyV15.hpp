/* SPDX-License-Identifier: MIT
 * Shared by the iOS frontend and the host sanitizer tests. No Apple layout guess.
 */
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <limits>

namespace raphael_metal_v15 {
inline bool mul(uint64_t a,uint64_t b,uint64_t &out) {
    if(b && a>UINT64_MAX/b)return false;
    out=a*b;return true;
}
inline uint64_t texelBytes(uint64_t f) {
    switch(f) {
    case 1: case 10: case 11: case 12: case 13: case 14:return 1;
    case 20: case 22: case 23: case 24: case 25:
    case 30: case 31: case 32: case 33: case 34:
    case 40: case 41: case 42: case 43:return 2;
    case 53: case 54: case 55: case 60: case 62: case 63: case 64: case 65:
    case 70: case 71: case 72: case 73: case 74: case 80: case 81:
    case 90: case 91: case 92: case 93: case 94: case 554: case 555:return 4;
    case 103: case 104: case 105: case 110: case 112: case 113: case 114: case 115:
    case 552: case 553:return 8;
    case 123: case 124: case 125:return 16;
    default:return 0;
    }
}
struct Linear {
    uint64_t width=0,height=0,texel=0,row=0,bytes=0,size=0,page=0;
    bool init(uint64_t w,uint64_t h,uint64_t t,uint64_t pitch,uint64_t alignment) {
        *this={};uint64_t tight=0,total=0;
        if(!w||!h||!t||!alignment||(alignment&(alignment-1))||!mul(w,t,tight)||pitch<tight||
           !mul(pitch,h,total)||total>UINT64_MAX-(alignment-1))return false;
        width=w;height=h;texel=t;row=pitch;bytes=total;page=alignment;
        size=(total+alignment-1)&~(alignment-1);return true;
    }
    // Strict, whole-pixel region; never clip a malformed request silently.
    bool copy(void *memory,void *linear,uint64_t offset,uint64_t stride,
              uint64_t x,uint64_t y,uint64_t w,uint64_t h,bool toMemory) const {
        uint64_t count=0,last=0;
        if(!memory||!linear||!size||!w||!h||x>width||w>width-x||y>height||h>height-y||
           !mul(w,texel,count)||stride<count||!mul(h-1,stride,last)||
           offset>UINT64_MAX-last||offset+last>UINT64_MAX-count)return false;
        const uintptr_t mem=(uintptr_t)memory,lin=(uintptr_t)linear;
        if(mem>UINTPTR_MAX-size||lin>UINTPTR_MAX-(offset+last+count))return false;
        for(uint64_t i=0;i<h;i++) {
            auto *m=(uint8_t *)memory+(y+i)*row+x*texel;
            auto *l=(uint8_t *)linear+offset+i*stride;
            if(toMemory)memmove(m,l,(size_t)count);else memmove(l,m,(size_t)count);
        }
        return true;
    }
};

constexpr uint64_t HealthMagic=0x524d544c56313501ULL;
struct BootRecord {uint64_t magic,seconds,micros,state;};
inline bool sameBoot(const BootRecord &r,uint64_t s,uint64_t us) {
    return r.magic==HealthMagic && r.seconds==s && r.micros==us;
}
struct Progress {
    uint64_t started=0,lastProgress=0,active=0,completed=0;
    bool failed=false,healthy=false;
    void commit(uint64_t now) {if(!active)lastProgress=now;++active;}
    void finish(uint64_t now,bool success) {
        if(active)--active;
        lastProgress=now;
        if(!success)failed=true;else ++completed;
    }
    bool stalled(uint64_t now) const {return active && now>=lastProgress && now-lastProgress>=20000000;}
    bool ready(uint64_t now) const {return !failed && completed>=3 && now>=started && now-started>=10000000;}
};
}
