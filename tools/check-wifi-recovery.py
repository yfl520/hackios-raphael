#!/usr/bin/env python3
"""Exercise the actual ICNSS power-vote and raw BDF recovery boundaries."""
from pathlib import Path
import re
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / 'xnu/files/bsd/dev/hakios.wifi.c').read_text()
def function(name):
    match = re.search(r'(?:static )?(?:(?:bool|void)\s+|const uint8_t \*\s*)' + name + r'\(.*?\n\}', source, re.S)
    if not match:
        raise RuntimeError(name)
    return match[0]

code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static bool raphael=true, dsp_started=true, wlfw_ready=true, raphael_wlan_crashed;
static bool icnss_rails_touched, icnss_qdss_touched, icnss_power_voted;
static uint32_t icnss_rail_address[5];
static bool resource_ok=true, rails_ok=true, clock_ok=true;
struct cmd_db_entry { uint32_t address; };
static const struct cmd_db_entry *rpmh_resource(const char *s, unsigned n) {
    static const struct cmd_db_entry entry={0x40000};
    assert(n==0 && s); return resource_ok ? &entry : NULL;
}
static bool wifi_rpmh_write(const uint32_t *a, const uint32_t *v, unsigned n) {
    assert(a && v && (n==5 || n==15)); return rails_ok;
}
static bool mailbox_send(const char *s) { assert(strstr(s,"qdss")); return clock_ok; }
static bool hakios_is_raphael(void) { return raphael; }
static const char *selected="bd_f11cn.bin";
static bool parse_ok=true, file_ok=true;
static uint64_t file_size=26328;
static uint8_t blob[128*1024];
static bool PE_parse_boot_arg_str(const char *key,char *out,size_t size) {
    assert(!strcmp(key,"wifi_bdf") && size==32);
    if (!parse_ok) return false;
    snprintf(out,size,"%s",selected); return true;
}
static const uint8_t *image_file(const char *name,uint64_t *size) {
    assert(!strcmp(name,"bd_f11cn.bin") || !strcmp(name,"bd_f11gl.bin") ||
           !strcmp(name,"firmware-5.bin") || !strcmp(name,"bd_m20gl.elf"));
    *size=file_size; return file_ok ? blob : NULL;
}
''' + '\n'.join(function(name) for name in
    ('icnss_power', 'hakios_wifi_icnss_ready', 'hakios_wifi_icnss_powered',
     'hakios_wifi_icnss_firmware_state', 'icnss_board_file', 'hakios_wifi_driver_file')) + r'''
int main(void) {
    assert(!hakios_wifi_icnss_powered() && !hakios_wifi_icnss_ready());
    assert(icnss_power(true) && hakios_wifi_icnss_ready());
    wlfw_ready=false; raphael_wlan_crashed=true;
    assert(hakios_wifi_icnss_powered() && !hakios_wifi_icnss_ready());
    hakios_wifi_icnss_firmware_state(true);
    assert(!raphael_wlan_crashed && hakios_wifi_icnss_ready());
    hakios_wifi_icnss_firmware_state(false);
    assert(raphael_wlan_crashed && !wlfw_ready && hakios_wifi_icnss_powered());
    hakios_wifi_icnss_firmware_state(true);
    assert(hakios_wifi_icnss_ready());
    dsp_started=false; assert(!hakios_wifi_icnss_powered()); dsp_started=true;
    assert(icnss_power(false) && !hakios_wifi_icnss_powered());
    resource_ok=false; assert(!icnss_power(true) && !hakios_wifi_icnss_powered());
    resource_ok=true; rails_ok=false;
    assert(!icnss_power(true) && icnss_rails_touched && !hakios_wifi_icnss_powered());
    rails_ok=true; clock_ok=false;
    assert(!icnss_power(true) && icnss_qdss_touched && !hakios_wifi_icnss_powered());
    clock_ok=true; assert(icnss_power(true) && hakios_wifi_icnss_powered());
    clock_ok=false; assert(!icnss_power(false) && !hakios_wifi_icnss_powered());
    clock_ok=true; assert(icnss_power(false));
    uint64_t size=0;
    const char *raw="ath10k/WCN3990/hw1.0/board.bin";
    assert(hakios_wifi_driver_file(raw,&size)==blob && size==26328);
    selected="bd_f11gl.bin"; assert(hakios_wifi_driver_file(raw,&size)==blob);
    assert(!hakios_wifi_driver_file("ath10k/WCN3990/hw1.0/board-2.bin",&size));
    assert(!hakios_wifi_driver_file("ath10k/WCN3990/hw1.0/board-snoc-.bin",&size));
    assert(!hakios_wifi_driver_file("other/board.bin",&size));
    selected="../bd_f11cn.bin"; assert(!hakios_wifi_driver_file(raw,&size) && !size);
    selected="bd_f11cn.bin"; parse_ok=false;
    assert(!hakios_wifi_driver_file(raw,&size) && !size); parse_ok=true;
    file_ok=false; assert(!hakios_wifi_driver_file(raw,&size) && !size); file_ok=true;
    file_size=0; assert(!hakios_wifi_driver_file(raw,&size) && !size);
    file_size=128*1024+1; assert(!hakios_wifi_driver_file(raw,&size) && !size);
    file_size=128*1024; assert(hakios_wifi_driver_file(raw,&size)==blob);
    raphael=false; assert(!hakios_wifi_icnss_powered());
    assert(hakios_wifi_driver_file("ath11k/board.bin",&size)==blob);
    puts("PASS: successful power votes survive firmware restart; failures stay off; native API1 CN/GL BDF mapping");
}
'''
with tempfile.TemporaryDirectory(prefix='wifi-recovery-') as tmp:
    path=Path(tmp)
    (path/'test.c').write_text(code)
    subprocess.run(['cc','-std=gnu11','-O1','-g','-fsanitize=address,undefined',
                    '-fno-sanitize-recover=all',str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
