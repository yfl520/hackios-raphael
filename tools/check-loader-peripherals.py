#!/usr/bin/env python3
"""Check actual read-only native FG publication and display request validation."""
from pathlib import Path
import re
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
source=(root/'xnuloader/loader.c').read_text()
def func(name):
    match=re.search(r'static void\n'+name+r'\(.*?\n\}',source,re.S)
    if not match: raise RuntimeError(name)
    return re.sub(r'__asm__ volatile \("dmb ish" ::: "memory"\);',
                  '__asm__ volatile ("" ::: "memory");',match[0])
struct=re.search(r'struct loader_battery \{.*?\n\};',source,re.S)[0]
flags='\n'.join(line for line in source.splitlines() if line.startswith('#define LOADER_BATTERY_'))
code=f'#include "{root}/xnuloader/efi.h"\n#include "{root}/xnu/files/bsd/sys/hakios.loader-display.h"\n'+r'''
#include <assert.h>
#include <stdio.h>
#include <stdarg.h>
static void print(const char *s,...) { (void)s; }
static void clean_dcache(uint64_t a,uint64_t b) { assert(b>a); }
static void flush_dcache(uint64_t a,uint64_t b) { assert(b>a); }
'''+flags+'\n'+struct+r'''
static struct loader_battery charger_shared;
static struct efi_fg_status sample={90,-212,4412,34};
static uint8_t plugged=1, present=1;
static efi_status fg_error, presence_error, cable_error;
static struct efi_fg_pmic_info fg_pmic={1,2};
static int fg_pmic_valid=1;
static unsigned fg_calls;
static efi_status fg_get(uint32_t n,struct efi_fg_status *out) { assert(n==1);fg_calls++;*out=sample;return fg_error; }
static efi_status bp(uint8_t *out) {*out=present;return presence_error;}
static efi_status cp(uint8_t *out) {*out=plugged;return cable_error;}
static efi_status vp(uint32_t *out) {*out=4412;return 0;}
static efi_pmic_fg fg={.get_battery_status=fg_get}, *PMIC_FG=&fg;
static efi_charger_ex ex={.get_battery_presence=bp,.get_charger_presence=cp,.get_battery_voltage=vp}, *CHARGER_EX=&ex;
static int display_enabled=1;
static struct loader_display display_shared;
static unsigned calls;
static uint32_t actual=60;
static efi_status set_error,get_error;
static efi_status set_level(uint32_t d,uint32_t p) {assert(d==0&&p>=1&&p<=100);calls++;if(!set_error)actual=p;return set_error;}
static efi_status get_level(uint32_t d,uint32_t *p) {assert(d==0);*p=actual;return get_error;}
static efi_display_power_control ctrl={.set_brightness=set_level,.get_brightness=get_level}, *DISPLAY_CONTROL=&ctrl;
'''+func('charger_poll_raphael')+'\n'+func('display_poll')+r'''
int main(void) {
    _Static_assert(sizeof(struct efi_fg_status)==16,"FG ABI");
    _Static_assert(offsetof(efi_pmic_fg,get_battery_status)==24,"FG protocol ABI");
    _Static_assert(offsetof(efi_pmic_fg,get_pmic_info)==104,"native raphael metadata ABI");
    _Static_assert(offsetof(efi_display_power_control,set_brightness)==24,"display protocol ABI");
    _Static_assert(offsetof(struct loader_display,request)==64,"request cache line");
    _Static_assert(offsetof(struct loader_display,answer)==128,"answer cache line");
    charger_poll_raphael();
    assert(charger_shared.soc_percent==90 && charger_shared.current_ma==-212);
    assert(charger_shared.flags&LOADER_BATTERY_VALID);
    assert(charger_shared.flags&LOADER_BATTERY_CHARGING);
    /* Raphael's FG current may change polarity while USB remains attached. */
    sample.current_ma=200; charger_poll_raphael();
    assert(charger_shared.flags&LOADER_BATTERY_CHARGING);
    sample.percent=100; charger_poll_raphael();
    assert(!(charger_shared.flags&LOADER_BATTERY_CHARGING));
    sample.current_ma=-200; plugged=0; charger_poll_raphael();
    assert(!(charger_shared.flags&(LOADER_BATTERY_CHARGING|LOADER_BATTERY_CHARGER_PRESENT)));
    sample.percent=1; charger_poll_raphael(); assert(charger_shared.soc_percent==1);
    assert(!(charger_shared.flags&LOADER_BATTERY_SOC_ESTIMATED));
    sample.percent=101; charger_poll_raphael(); assert(charger_shared.soc_percent==-1);
    sample.percent=50; fg_error=EFI_ERROR_BIT|6; charger_poll_raphael();
    assert(!(charger_shared.flags&LOADER_BATTERY_VALID));
    presence_error=0;sample.temperature_c=34;fg_pmic_valid=0;
    unsigned previous_calls=fg_calls;charger_poll_raphael();
    assert(fg_calls==previous_calls && !(charger_shared.flags&LOADER_BATTERY_VALID));
    fg_pmic_valid=1;
    fg_error=0; presence_error=EFI_ERROR_BIT|7; charger_poll_raphael();
    assert(!(charger_shared.flags&LOADER_BATTERY_VALID));
    presence_error=0; sample.temperature_c=200; charger_poll_raphael();
    assert(!(charger_shared.flags&LOADER_BATTERY_VALID));
    display_poll(); assert(!calls);
    display_shared.request.sequence=1;display_shared.request.percent=20;
    display_poll(); assert(calls==1 && actual==20 && display_shared.answer.sequence==1 && !display_shared.answer.status);
    display_poll(); assert(calls==1);
    display_shared.request.sequence=2;display_shared.request.percent=101;
    display_poll(); assert(calls==1 && display_shared.answer.sequence==2 && EFI_ERROR(display_shared.answer.status));
    display_shared.request.sequence=3;display_shared.request.percent=80;set_error=EFI_ERROR_BIT|7;
    display_poll(); assert(calls==2 && actual==20 && EFI_ERROR(display_shared.answer.status));
    set_error=0;get_error=EFI_ERROR_BIT|7;
    display_shared.request.sequence=4;display_poll();
    assert(calls==3 && EFI_ERROR(display_shared.answer.status));
    puts("PASS: real FG values, no voltage SOC estimate, cable/current distinction, bad sample rejection; display bounds and failure acknowledgement");
}
'''
with tempfile.TemporaryDirectory(prefix='loader-peripherals-') as tmp:
    path=Path(tmp)
    (path/'test.c').write_text(code)
    subprocess.run(['cc','-std=gnu11','-O1','-g','-fsanitize=address,undefined',
                    '-fno-sanitize-recover=all',str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
