#include <assert.h>
#include <stdio.h>
#include "hakios.raphael-charge-policy.h"

static unsigned classify(unsigned apsd, unsigned result, unsigned snk, unsigned misc)
{
    return raphael_charge_class(0x10,apsd,result,0,1,0,0xd4,1,35,3800,0,0,0x18,1,snk,misc,2);
}

int main(void)
{
    /* Enumerated USB hosts must reject for every APSD result byte. */
    for (unsigned r=0;r<256;r++) assert(!classify(0x11,r,8,9));
    for (unsigned r=0;r<256;r++) {
        unsigned want = r==2||r==4||r==8||r==0x28||r==0x48 ? 1 : r==0x10 ? 2 : 0;
        assert(classify(0x21,r,8,9)==want);
    }
    assert(!classify(1,0x10,8,9)); /* FLOAT without timeout */
    for (unsigned rp=0;rp<128;rp++)
        assert(classify(0x21,0x10,rp,9)==((rp==8||rp==4||rp==2)?2:0));
    for (unsigned misc=0;misc<256;misc++) {
        int want=(misc&1)&&(misc&8)&&!(misc&0xd0);
        assert(classify(0x21,0x10,8,misc)==(want?2:0));
    }
    for (unsigned fault=0;fault<7;fault++) {
        unsigned thermal=fault==0?1:0, pause=fault==1?1:0, suspend=fault==2?1:0;
        unsigned otg=fault==3?1:0, aicl=fault==4?0:4;
        int fresh=fault==5?0:1, temp=fault==6?45:35;
        assert(!raphael_charge_class(0x10,0x21,0x10,thermal,1,pause,aicl,fresh,temp,3800,
            suspend,otg,0x18,1,8,9,2));
    }
    assert(!raphael_charge_class(0x11,0x21,0x10,0,1,0,4,1,35,3800,0,0,0x18,1,8,9,2));
    assert(!raphael_charge_class(0x10,0x21,0x10,0,1,0,4,1,35,3800,0,0,0x18,0,8,9,2));
    assert(!raphael_charge_class(0x10,0x21,0x10,0,1,0,4,1,35,3800,0,0,0x18,1,8,9,3));
    for (int temp=-40; temp<=100; temp++) {
        int accepted=temp>=15 && temp<45;
        assert(raphael_charge_temperature_ok(temp)==accepted);
        assert((raphael_charge_class(0x10,1,2,0,1,0,4,1,temp,3800,
            0,0,0x18,1,2,9,0)!=0)==accepted);
        assert(!raphael_charge_class(0x10,1,2,1,1,0,4,1,temp,3800,
            0,0,0x18,1,2,9,0)); /* hardware thermal faults still veto */
    }
    struct raphael_charge_gate g={0};
    assert(!raphael_charge_settled(&g,2,1000000000ULL));
    assert(!raphael_charge_settled(&g,2,60999999999ULL));
    assert(raphael_charge_settled(&g,2,61000000000ULL));
    assert(!raphael_charge_settled(&g,0,62000000000ULL));
    assert(!raphael_charge_settled(&g,2,63000000000ULL));
    assert(!raphael_charge_settled(&g,2,64000000000ULL));
    assert(!raphael_charge_settled(&g,1,65000000000ULL));
    assert(!raphael_charge_settled(&g,1,79999999999ULL));
    assert(raphael_charge_settled(&g,1,80000000000ULL));
    assert(!raphael_charge_settled(&g,1,10)); /* clock regression */
    puts("PASS: 256 host results; 256 source results; Rp/misc/guard faults; settling, detach and source changes.");
    return 0;
}
