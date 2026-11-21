/* Original executable stays on System. One guarded COW instruction in RAM.
 * Only Internet's Apple postponement-ticket prerequisite is skipped; the
 * native context, SIM, APN and WDS checks after the branch remain in place. */
#include <mach/mach.h>
#include <mach-o/loader.h>
#include <mach-o/dyld.h>
#include <libkern/OSCacheControl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "commcenter-ticket-guard.h"

static int cc_main_seen;

static void cc_image_added(const struct mach_header *header,intptr_t slide)
{
    const struct mach_header_64 *h=(const void *)header;
    const unsigned char uuid[16]={0x5a,0x90,0x7e,0x0e,0xbf,0x03,0x37,0x6b,0x94,0xe6,0xe7,0x37,0xbd,0xd4,0x03,0x00};
    if(!h || h->magic!=MH_MAGIC_64 || h->cputype!=CPU_TYPE_ARM64 || h->filetype!=MH_EXECUTE || h->sizeofcmds>65536 || cc_main_seen)return;
    cc_main_seen=1;
    const unsigned char *at=(const void *)(h+1),*end=at+h->sizeofcmds;
    int matched=0,executable=0;
    unsigned char actual_uuid[16]={0};
    for(unsigned i=0;i<h->ncmds;i++) {
        if((size_t)(end-at)<sizeof(struct load_command))return;
        const struct load_command *lc=(const void *)at;
        if(lc->cmdsize<sizeof(*lc) || lc->cmdsize>(size_t)(end-at))return;
        if(lc->cmd==LC_UUID && lc->cmdsize>=sizeof(struct uuid_command)) {
            const struct uuid_command *u=(const void *)lc;
            memcpy(actual_uuid,u->uuid,sizeof actual_uuid);
            matched=!memcmp(u->uuid,uuid,sizeof uuid);
        }
        if(lc->cmd==LC_SEGMENT_64 && lc->cmdsize>=sizeof(struct segment_command_64)) {
            const struct segment_command_64 *s=(const void *)lc;
            if((s->initprot&VM_PROT_EXECUTE) && s->vmaddr<=CC_GUARD_ADDRESS &&
                s->vmsize>=sizeof(cc_guard) && CC_GUARD_ADDRESS-s->vmaddr<=s->vmsize-sizeof(cc_guard))executable=1;
        }
        at+=lc->cmdsize;
    }
    unsigned char *guard=(void *)(CC_GUARD_ADDRESS+slide);
    if(!matched || !executable) {
        ((void)0);
        for(unsigned i=0;i<sizeof actual_uuid;i++)((void)0);
        ((void)0);return;
    }
    if(memcmp(guard,cc_guard,sizeof cc_guard)) {
        for(unsigned i=0;i<sizeof cc_guard;i+=4) {
            uint32_t actual,expected;
            memcpy(&actual,guard+i,4);memcpy(&expected,cc_guard+i,4);
            if(actual!=expected)((void)0);
        }
        ((void)0);return;
    }
    uint32_t *word=(void *)(CC_BRANCH_ADDRESS+slide);
    vm_size_t size=(vm_size_t)getpagesize();
    vm_address_t page=(vm_address_t)word&~(size-1);
    kern_return_t result=vm_protect(mach_task_self(),page,size,FALSE,VM_PROT_READ|VM_PROT_WRITE|VM_PROT_COPY);
    if(result) {((void)0);return;}
    uint32_t original=*word;*word=CC_BRANCH_WORD;sys_icache_invalidate(word,4);
    result=vm_protect(mach_task_self(),page,size,FALSE,VM_PROT_READ|VM_PROT_EXECUTE);
    if(result) {
        *word=original;sys_icache_invalidate(word,4);
        kern_return_t restored=vm_protect(mach_task_self(),page,size,FALSE,VM_PROT_READ|VM_PROT_EXECUTE);
        ((void)0);return;
    }
    ((void)0);
}

__attribute__((constructor)) static void cc_compat_start(void)
{
    if(strcmp(getprogname(),"CommCenter"))return;
    /* Injection can precede the executable in dyld's image list. Registration
     * covers both existing images and an executable added after this library. */
    _dyld_register_func_for_add_image(cc_image_added);
    if(!cc_main_seen)((void)0);
}
