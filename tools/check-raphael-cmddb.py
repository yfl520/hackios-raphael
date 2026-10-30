#!/usr/bin/env python3
"""Run the actual lookup against eight-byte resource IDs and bad headers."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'xnu/files/bsd/dev/hakios.wifi.c').read_text()
start = source.index('struct cmd_db_slave {')
end = source.index('static const struct cmd_db_header *cmd_db;', start)
types = source[start:end]
start = source.index('static const struct cmd_db_entry *\nrpmh_resource(')
end = source.index('\nstatic void\nshow_rpmh_resource(', start)
lookup = source[start:end]
code = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#define CMD_DB_SIZE 0x20000
#define __offsetof offsetof
''' + types + r'''
static union { uint64_t align; uint8_t bytes[CMD_DB_SIZE]; } storage;
static const struct cmd_db_header *cmd_db=(const void *)storage.bytes;
static bool cmd_db_there(void){return true;}
''' + lookup + r'''
int main(void) {
    struct cmd_db_header *db=(void *)storage.bytes;
    db->slave[0].id=3;db->slave[0].count=3;
    struct cmd_db_entry *entries=(void *)db->data;
    memcpy(entries[0].id,"mmcx.lvl",8);entries[0].address=0x30040;
    memcpy(entries[1].id,"mx.lvl",6);entries[1].address=0x30010;
    memcpy(entries[2].id,"bobc1",5);entries[2].address=0x40100;
    assert(rpmh_resource("mmcx.lvl",0)==entries);
    assert(rpmh_resource("mx.lvl",0)==entries+1);
    assert(rpmh_resource("bobc1",0)==entries+2);
    assert(!rpmh_resource("mmcx.lv",0));assert(!rpmh_resource("mmcx.lvll",0));
    assert(!rpmh_resource("",0));assert(!rpmh_resource("nope",0));
    assert(rpmh_resource(NULL,0x40108)==entries+2);
    assert(!rpmh_resource(NULL,0x40110));
    db->slave[0].count=513;assert(!rpmh_resource("mmcx.lvl",0));
    db->slave[0].count=512;db->slave[0].header_offset=65532;
    /* This offset is in range but carries no valid matching entries. */
    assert(!rpmh_resource("mmcx.lvl",0));
    puts("cmd-db: full eight-byte mmcx.lvl lookup, short/long rejection and bounds PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='camera-cmddb-') as tmp:
    src, exe = Path(tmp) / 'check.c', Path(tmp) / 'check'
    src.write_text(code)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g',
                    '-fsanitize=address,undefined', str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
