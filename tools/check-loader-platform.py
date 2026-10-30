#!/usr/bin/env python3
"""Exercise the actual EFI display/platform selection without accessing MMIO."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source', type=Path, default=root / 'xnuloader/loader.c')
parser.add_argument('--cc', nargs=argparse.REMAINDER, default=['cc'])
args = parser.parse_args()
source = args.source.read_text(encoding='utf-8')
entry = source[source.index('efi_main(efi_handle image, efi_system_table *st)'):]
selection = re.search(r'\n\tPLAT = &platform_qemu_virt;.*?(?=\n\tprint\("platform %s)', entry, re.S)
if selection is None:
    raise SystemExit('Cannot locate EFI platform selection')
test = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint64_t efi_status;
typedef struct { unsigned dummy; } efi_guid;
struct platform { const char *name; };
static const struct platform platform_qemu_virt = {"qemu"};
static const struct platform platform_raphael = {"raphael"};
static const struct platform platform_redwood = {"redwood"};
static const struct platform *PLAT;
static struct { uint64_t base; unsigned width, height, stride; } SCREEN;
struct info { unsigned width, height, pixels_per_scanline; };
struct mode { struct info *info; uint64_t fb_base; };
struct gop { struct mode *mode; };
static struct gop *GOP, mock_gop;
static struct mode mock_mode;
static struct info mock_info;
static const efi_guid gop_guid = {0};
static unsigned resized;
static int missing;
#define EFI_ERROR(s) ((s) != 0)
static efi_status locate(const efi_guid *guid, void *registration, void **result) {
    (void)guid; (void)registration;
    if (missing) return 1;
    *result = &mock_gop;
    return 0;
}
static struct { efi_status (*locate_protocol)(const efi_guid *, void *, void **); } services = {locate};
static typeof(services) *BS = &services;
static void print(const char *format, ...) { (void)format; }
static void ramfb_phone_screen(void) { resized++; }
static void select_platform(void) {
''' + selection[0] + r'''
}
static int check(uint64_t fb, int unavailable, const struct platform *want,
                 unsigned want_resize, unsigned null_mode, unsigned null_info) {
    memset(&SCREEN, 0, sizeof(SCREEN));
    mock_info = (struct info){1080, 2340, 1088};
    mock_mode = (struct mode){null_info ? NULL : &mock_info, fb};
    mock_gop.mode = null_mode ? NULL : &mock_mode;
    missing = unavailable;
    resized = 0;
    PLAT = NULL;
    select_platform();
    if (PLAT != want || resized != want_resize) {
        fprintf(stderr, "FAIL fb=%llx: platform=%s, QEMU MMIO initializations=%u; expected=%s/%u\n",
                (unsigned long long)fb, PLAT ? PLAT->name : "null", resized, want->name, want_resize);
        return 1;
    }
    if (!unavailable && !null_mode && !null_info && fb) {
        assert(SCREEN.base == fb);
        assert(SCREEN.width == 1080 && SCREEN.height == 2340 && SCREEN.stride == 4352);
    } else {
        assert(GOP == NULL && SCREEN.base == 0);
    }
    return 0;
}
int main(void) {
    int failures = 0;
    failures += check(0x9c000000ULL, 0, &platform_raphael, 0, 0, 0);
    failures += check(0xe1000000ULL, 0, &platform_redwood, 0, 0, 0);
    failures += check(0x78000000ULL, 0, &platform_qemu_virt, 1, 0, 0);
    failures += check(0x9c000000ULL, 1, &platform_qemu_virt, 0, 0, 0);
    failures += check(0x9c000000ULL, 0, &platform_qemu_virt, 0, 1, 0);
    failures += check(0x9c000000ULL, 0, &platform_qemu_virt, 0, 0, 1);
    failures += check(0, 0, &platform_qemu_virt, 0, 0, 0);
    return failures ? 1 : 0;
}
'''
with tempfile.TemporaryDirectory(prefix='hackios-platform-') as temporary:
    path = Path(temporary)
    (path / 'test.c').write_text(test, encoding='utf-8')
    binary = path / 'test.exe'
    subprocess.run([*args.cc, '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    str(path / 'test.c'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('PASS: raphael/redwood platform selection, QEMU fallback, absent GOP/mode/info, framebuffer stride (7 cases)')
