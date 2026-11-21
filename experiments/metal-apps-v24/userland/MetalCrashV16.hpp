// Bounded, allocation-free formatting for fatal-signal observation.
#pragma once
#include <cstddef>
#include <cstdint>
namespace raphael_metal_v16 {
struct CrashLine {
    char bytes[256]{};
    size_t used=0;
    void text(const char *s) {if(s)while(*s && used<sizeof(bytes)-2)bytes[used++]=*s++;}
    void hex(uint64_t n) {for(int i=15;i>=0;--i)if(used<sizeof(bytes)-2)bytes[used++]="0123456789abcdef"[(n>>(i*4))&15];}
    void end() {if(used<sizeof(bytes))bytes[used++]='\n';}
};
inline bool nextFrame(uint64_t fp,uint64_t next) {
    return fp && !(fp&15) && next>fp && !(next&15) && next-fp<=1024*1024;
}
}
