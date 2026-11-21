#include "hakios.raphael-gsi-regs.h"
/* Experimental Raphael IPA4.1 platform setup, opt-in mode 5 only.
 * Sources: Xiaomi raphael-p-oss 663db50e, ipa.c/ipa_utils.c/ipahal_reg.c;
 * SMMU uses this handset's validated camera helper mechanisms; IPA streams remain 0x520/0x522.
 * No RF enable, data rings or network registration in this module.
 */
#define RIPA_TABLES 32u
#define RIPA_ADDRESS 0x0000fffffffff000ull
#define RIPA_ATTR (1ull | 1ull<<6 | 2ull<<8 | 1ull<<10 | 1ull<<11 | 1ull<<53 | 1ull<<54)
static struct block ripa_smmu={"IPA apps SMMU",0x15000000,0x2000};
static struct block ripa_gr1={"IPA SMMU GR1",0,0x1000};
static struct block ripa_cb={"IPA SMMU context",0,0x1000};
static struct block ripa_regs={"IPA4.1 registers",RAPHAEL_IPA_REG_BASE,0x1000};
static struct block ripa_sram={"IPA4.1 SRAM direct access",RAPHAEL_IPA_SRAM_BASE,0x4000};
static struct block ripa_gsi={"GSI2.0 EE0",RAPHAEL_GSI_BASE+GSI_STATUS,0x1000};
static const uint32_t ripa_cb_offsets[]={0,0x10,0x20,0x24,0x28,0x2c,0x30,0x38,0x3c};
static struct {
    vm_offset_t tables;
    unsigned count,match[2],context,page,pages;
    uint32_t old_smr[2],old_s2cr[2],old_cb[9],old_cbar,old_cba2r;
    bool touched,on,hardware,retired,cbar_dirty,cba2r_dirty;
    uint16_t cb_dirty;uint8_t smr_dirty,s2cr_dirty;
    uint32_t restricted,shared_bytes;
} ripa;
extern void FlushPoC_DcacheRegion(vm_offset_t,size_t);

static uint64_t *ripa_table(unsigned n) {return (uint64_t*)(ripa.tables+4096*n);}
static uint64_t *
ripa_next(uint64_t *entry)
{
    if(*entry&1) {
        if((*entry&3)!=3)return NULL;
        for(unsigned n=0;n<ripa.count;n++)
            if(kvtophys((vm_offset_t)ripa_table(n))==(*entry&RIPA_ADDRESS))return ripa_table(n);
        return NULL;
    }
    if(ripa.count==RIPA_TABLES)return NULL;
    uint64_t *next=ripa_table(ripa.count++);
    *entry=kvtophys((vm_offset_t)next)|3;return next;
}
/* Exact identity mappings; 4K edges keep DDR holes and adjacent IMEM unmapped. */
static bool
ripa_map(uint64_t address,uint64_t bytes,bool device)
{
    if(!bytes || (address|bytes)&4095 || address>=(1ull<<36) || bytes>(1ull<<36)-address)return false;
    const uint64_t attr=RIPA_ATTR|(device?2ull<<2:0);
    while(bytes) {
        uint64_t *l2=ripa_next(&ripa_table(0)[address>>30]);
        if(!l2)return false;
        uint64_t *entry=&l2[(address>>21)&511];uint64_t step=0x200000;
        if((address&(step-1)) || bytes<step || device) {
            uint64_t *l3=ripa_next(entry);if(!l3)return false;
            entry=&l3[(address>>12)&511];step=4096;
        }
        uint64_t descriptor=address|attr|(step==4096?2:0);
        if(*entry && *entry!=descriptor)return false;
        *entry=descriptor;address+=step;bytes-=step;
    }
    return true;
}
static bool
ripa_tables_prepare(void)
{
    if(!ripa.tables && kmem_alloc(kernel_map,&ripa.tables,round_page(RIPA_TABLES*4096),
        KMA_KOBJECT|KMA_ZERO|KMA_DATA,VM_KERN_MEMORY_IOKIT)!=KERN_SUCCESS)return false;
    bzero((void*)ripa.tables,RIPA_TABLES*4096);ripa.count=1;
    /* Replaced by prepare-ipa-r8.py from the actual /memory reg property. */
    static const uint64_t memory[][2]={
        {0x80000000ull,0x39900000ull},
        {0xc0000000ull,0xc0000000ull},
        {0x180000000ull,0x200000000ull},
    };
    for(unsigned i=0;i<sizeof(memory)/sizeof(memory[0]);i++)
        if(!ripa_map(memory[i][0],memory[i][1],false))return false;
    /* qcom,additional-mapping from ipa_smmu_ap: exactly two IMEM pages. */
    if(!ripa_map(0x146bd000,0x2000,true))return false;
    FlushPoC_DcacheRegion(ripa.tables,RIPA_TABLES*4096);settle();return true;
}
static void
ripa_smmu_store(vm_offset_t at,uint32_t value)
{
#if defined(__aarch64__)
    /* Reuse the real-register store verified by Raphael's camera helper. */
    __asm__ volatile ("" : "+r" (value));
    __asm__ volatile ("str %w0, [%1]" : : "r"(value),"r"(at) : "memory");
#else
    wr(at,value);
#endif
}
static bool
ripa_smmu_put(vm_offset_t at,uint32_t value)
{
    ripa_smmu_store(at,value);settle();uint32_t got=rd(at);
    if(got!=value) {
        uint64_t physical=0;
        struct block *blocks[]={&ripa_cb,&ripa_gr1,&ripa_smmu};
        for(unsigned i=0;i<3;i++)if(at>=blocks[i]->base && at-blocks[i]->base<blocks[i]->size){
            physical=blocks[i]->phys+at-blocks[i]->base;break;
        }
        ((void)0);
    }
    return got==value;
}
static bool
ripa_cb_put(unsigned i,uint32_t value)
{
    ripa.cb_dirty|=1u<<i;
    if(i==1)value=(rd(ripa_cb.base+0x10)&~0x38017u)|(value&0x38017u);
    return ripa_smmu_put(ripa_cb.base+ripa_cb_offsets[i],value);
}
static bool
ripa_tlb_sync(void)
{
    ripa_smmu_store(ripa_cb.base+0x610,ripa.context);
    ripa_smmu_store(ripa_cb.base+0x610,ripa.old_cb[3]>>16);settle();
    ripa_smmu_store(ripa_cb.base+0x7f0,~0u);settle();
    for(unsigned ms=0;ms<100;ms++) {
        if(!(rd(ripa_cb.base+0x7f4)&1))return true;
        sleep_ms(1);
    }
    ((void)0);return false;
}
static bool
ripa_space_retire_locked(void)
{
    /* Only an originally unreferenced disabled bank and EXACT FAULT routes.
     * Removing both stream routes and all table pointers is mandatory.
     * This is retirement, never represented as byte-exact restoration. */
    if(ripa.smr_dirty || ripa.s2cr_dirty || (ripa.old_cb[0]&1))return false;
    for(unsigned i=0;i<2;i++)if((ripa.old_smr[i]&0x80000000u) ||
        (ripa.old_s2cr[i]&0x30000u)!=0x20000u ||
        rd(ripa_smmu.base+0x800+4*ripa.match[i])!=ripa.old_smr[i] ||
        rd(ripa_smmu.base+0xc00+4*ripa.match[i])!=ripa.old_s2cr[i])return false;
    if(!ripa_smmu_put(ripa_cb.base,0) ||
        !ripa_smmu_put(ripa_gr1.base+0x800+4*ripa.context,1) ||
        !ripa_smmu_put(ripa_gr1.base+4*ripa.context,0x1f300))return false;
    for(unsigned i=2;i<6;i++)if(!ripa_smmu_put(ripa_cb.base+ripa_cb_offsets[i],0))return false;
    if(!ripa_tlb_sync() || rd(ripa_cb.base) || (rd(ripa_cb.base+0x58)&0xc00001feu))return false;
    ripa.cb_dirty=0;ripa.cbar_dirty=ripa.cba2r_dirty=false;
    ripa.retired=true;ripa.touched=ripa.on=ripa.hardware=false;
    ((void)0);
    return true;
}
/* Call only after both MPSS and PAS15 have stopped, or before PAS15 starts. */
static bool
ripa_space_restore_locked(void)
{
    if(!ripa.touched)return true;
    ripa.hardware=ripa.on=false;
    /* Invalidate BOTH SMRs before restoring either route or context. */
    for(unsigned i=0;i<2;i++)if(ripa.smr_dirty&(1u<<i)) {
        if(!ripa_smmu_put(ripa_smmu.base+0x800+4*ripa.match[i],ripa.old_smr[i]))return false;
        ripa.smr_dirty&=~(1u<<i);
    }
    for(unsigned i=0;i<2;i++)if(ripa.s2cr_dirty&(1u<<i)) {
        vm_offset_t reg=ripa_smmu.base+0xc00+4*ripa.match[i];
        if(!ripa_smmu_put(reg,ripa.old_s2cr[i])) {
            if((ripa.old_smr[i]&0x80000000u) || (ripa.old_s2cr[i]&0x30000u)!=0x20000u ||
                rd(ripa_smmu.base+0x800+4*ripa.match[i])!=ripa.old_smr[i])return false;
            ripa_smmu_store(reg,(ripa.old_s2cr[i]&~0x30000u)|0x10000u);settle();
            if(rd(reg)!=ripa.old_s2cr[i])return false;
            ((void)0);
        }
        ripa.s2cr_dirty&=~(1u<<i);
    }
    if(ripa.cb_dirty && (!ripa_cb_put(0,0) || !ripa_tlb_sync()))return false;
    /* CBAR/CBA2R determine interpretation of all saved CB registers. */
    if(ripa.cba2r_dirty) {
        if(!ripa_smmu_put(ripa_gr1.base+0x800+4*ripa.context,ripa.old_cba2r))return false;
        ripa.cba2r_dirty=false;
    }
    if(ripa.cbar_dirty) {
        if(!ripa_smmu_put(ripa_gr1.base+4*ripa.context,ripa.old_cbar)) {
            if(!ripa.old_cbar && !ripa.old_cba2r)return ripa_space_retire_locked();
            return false;
        }
        ripa.cbar_dirty=false;
    }
    for(unsigned i=1;i<9;i++)if(ripa.cb_dirty&(1u<<i)) {
        if(!ripa_smmu_put(ripa_cb.base+ripa_cb_offsets[i],ripa.old_cb[i]))return false;
        ripa.cb_dirty&=~(1u<<i);
    }
    if(ripa.cb_dirty&1) {
        if(!ripa_smmu_put(ripa_cb.base,ripa.old_cb[0]))return false;
        ripa.cb_dirty&=~1u;
    }
    ripa.touched=false;return true;
}
static bool
ripa_space_off(void)
{
    if(!ripa.touched)return true;
    if(leaving)return ripa_space_restore_locked();
    lck_mtx_lock(&hakios_smmu_lock);bool ok=ripa_space_restore_locked();
    lck_mtx_unlock(&hakios_smmu_lock);return ok;
}
static bool
ripa_space_on(void)
{
    if(ripa.on)return true;
    if(ripa.touched)return false;
    if(!ripa_tables_prepare())return false;
    uint64_t root=kvtophys(ripa.tables);
    if(!root || (root&4095) || root>=(1ull<<36))return false;
    lck_mtx_lock(&hakios_smmu_lock);need(&ripa_smmu);
    bool ok=false,used[256]={0};const uint32_t streams[]={0x520,0x522};
    if(!ripa_smmu.base)goto out;
    uint32_t scr0=rd(ripa_smmu.base),idr0=rd(ripa_smmu.base+0x20),idr1=rd(ripa_smmu.base+0x24);
    unsigned matches=idr0&255,contexts=idr1&255;
    ripa.page=idr1>>31?0x10000:0x1000;ripa.pages=2u<<((idr1>>28)&7);
    if((scr0&(1u<<3)) || !matches || !contexts || contexts>ripa.pages || 2*ripa.pages*ripa.page!=0x100000)goto out;
    ripa.match[0]=ripa.match[1]=~0u;
    for(unsigned n=0;n<matches;n++) {
        uint32_t smr=rd(ripa_smmu.base+0x800+4*n),s2cr=rd(ripa_smmu.base+0xc00+4*n);
        if(smr>>31) {
            used[s2cr&255]=true;
            for(unsigned i=0;i<2;i++)if(!(((smr&0x7fff)^streams[i])&~((smr>>16)&0x7fff)))goto out;
        } else {
            unsigned i=ripa.match[0]==~0u?0:1;
            if(ripa.match[i]==~0u){ripa.match[i]=n;ripa.old_smr[i]=smr;ripa.old_s2cr[i]=s2cr;}
        }
    }
    if(ripa.match[1]==~0u)goto out;
    for(ripa.context=0;ripa.context<contexts;ripa.context++) {
        if(used[ripa.context])continue;
        ripa_cb.phys=ripa_smmu.phys+(ripa.pages+ripa.context)*ripa.page;
        need(&ripa_cb);if(!ripa_cb.base)goto out;
        if(!(rd(ripa_cb.base)&1) && !(rd(ripa_cb.base+0x58)&0xc00001feu) && !(rd(ripa_cb.base+0x7f4)&1))break;
        ml_io_unmap(ripa_cb.base,ripa_cb.size);ripa_cb.base=0;
    }
    if(ripa.context==contexts)goto out;
    ripa_gr1.phys=ripa_smmu.phys+ripa.page;need(&ripa_gr1);
    if(!ripa_gr1.base || !ripa_cb.base)goto out;
    for(unsigned i=0;i<9;i++)ripa.old_cb[i]=rd(ripa_cb.base+ripa_cb_offsets[i]);
    ripa.old_cbar=rd(ripa_gr1.base+4*ripa.context);ripa.old_cba2r=rd(ripa_gr1.base+0x800+4*ripa.context);
    ((void)0);
    ripa.retired=false;ripa.touched=true;ripa.cba2r_dirty=true;
    if(!ripa_smmu_put(ripa_gr1.base+0x800+4*ripa.context,1))goto failed;
    ripa.cbar_dirty=true;
    if(!ripa_smmu_put(ripa_gr1.base+4*ripa.context,0x1f300))goto failed;
    uint64_t ttbr=root|((uint64_t)ripa.context<<48);
    const uint32_t values[]={0,1u|1u<<4|7u<<15,(uint32_t)ttbr,(uint32_t)(ttbr>>32),0,0,
        25u|1u<<8|1u<<10|3u<<12|1u<<23,0xf404ff44,0};
    for(unsigned i=0;i<9;i++)if(!ripa_cb_put(i,values[i]))goto failed;
    /* Optional ASIDPNE is read-as-zero on this handset. Explicit TLB sync. */
    if(!ripa_tlb_sync() || !ripa_cb_put(0,0x127))goto failed;
    for(unsigned i=0;i<2;i++) {
        ripa.s2cr_dirty|=1u<<i;
        if(!ripa_smmu_put(ripa_smmu.base+0xc00+4*ripa.match[i],ripa.context))goto failed;
        ripa.smr_dirty|=1u<<i;
        if(!ripa_smmu_put(ripa_smmu.base+0x800+4*ripa.match[i],(1u<<31)|streams[i]))goto failed;
    }
    ripa.on=true;ok=true;goto out;
failed:
    if(!ripa_space_restore_locked())((void)0);
out:
    lck_mtx_unlock(&hakios_smmu_lock);
    ((void)0);
    return ok;
}

static bool
ripa_write_checked(uint32_t offset,uint32_t value,uint32_t mask)
{
    uint32_t old=rd(ripa_regs.base+offset),wanted=(old&~mask)|(value&mask);
    wr(ripa_regs.base+offset,wanted);settle();return (rd(ripa_regs.base+offset)&mask)==(wanted&mask);
}
#include "hakios.raphael-gsi2-start.h"
static bool
ripa_hardware_init(void)
{
    if(!ripa.on)return false;
    raphael_ipa_stage=70;((void)0);
    need(&ripa_regs);uint32_t version=rd(ripa_regs.base+0x34),shared=rd(ripa_regs.base+0x54);
    ripa.restricted=(shared>>16)*8;ripa.shared_bytes=(shared&0xffff)*8;
    ((void)0);
    if(!version || version==~0u || ripa.shared_bytes<RAPHAEL_IPA_END ||
        ripa.restricted>ripa_sram.size-RAPHAEL_IPA_END)return false;
    /* IPA4.1 has legacy aggregation counters: do not touch 4.5+ Qtime regs. */
    if(!ripa_write_checked(0x1d0,0x39,~0u) || !ripa_write_checked(0x44,0x30000000,~0u) ||
        !ripa_write_checked(0x1fc,0,0x1000) || !ripa_write_checked(0x74,0xcc,~0u) ||
        !ripa_write_checked(0x78,0x48,~0u) || !ripa_write_checked(0x1f0,15u<<4,~0u) ||
        !ripa_write_checked(0x3c,(1u<<7)|(1u<<8)|(1u<<15),(1u<<7)|(1u<<8)|(1u<<15)) ||
        !ripa_write_checked(0x220,256,~0u))return false;
    /* ipa3_rsrc_{src,dst}_grp_config[IPA_4_1]; four valid resource groups. */
    static const uint8_t source[5][4][2]={
        {{1,63},{1,63},{0,0},{1,63}},{{10,10},{10,10},{0,0},{8,8}},
        {{12,12},{14,14},{0,0},{8,8}},{{0,63},{0,63},{0,63},{0,63}},
        {{14,14},{20,20},{0,0},{14,14}},
    };
    static const uint8_t dest[2][4][2]={
        {{4,4},{4,4},{3,3},{2,2}},{{2,63},{1,63},{1,2},{0,2}},
    };
    for(unsigned side=0;side<2;side++)for(unsigned n=0;n<(side?2:5);n++)for(unsigned g=0;g<4;g+=2) {
        const uint8_t (*v)[2]=side?dest[n]:source[n];
        uint32_t word=v[g][0]|(uint32_t)v[g][1]<<8|(uint32_t)v[g+1][0]<<16|(uint32_t)v[g+1][1]<<24;
        if(!ripa_write_checked((side?0x500:0x400)+0x20*n+2*g,word,0x3f3f3f3f))return false;
    }
    need(&ripa_sram);vm_offset_t sram=ripa_sram.base+ripa.restricted;
    /* Preserve the uC mailbox/info and descriptor RAM. No 4.11 offsets. */
    for(uint32_t at=0x280;at<0x2400;at+=4)wr(sram+at,0);
    static const uint16_t paired[]={0x288,0x308,0x388,0x408,0x488,0x508,0x588,0x608,0x688,0x7d0,0xbd8,0xc30};
    for(unsigned i=0;i<sizeof(paired)/sizeof(paired[0]);i++) {
        wr(sram+paired[i]-8,0xdeadbeef);wr(sram+paired[i]-4,0xdeadbeef);
    }
    wr(sram+0x23fc,0xdeadbeef);settle();
    for(unsigned i=0;i<sizeof(paired)/sizeof(paired[0]);i++)
        if(rd(sram+paired[i]-8)!=0xdeadbeef || rd(sram+paired[i]-4)!=0xdeadbeef)return false;
    if(rd(sram+0x23fc)!=0xdeadbeef || !ripa_write_checked(0x1e8,ripa.restricted+0x7d0,~0u))return false;
    (void)ripa_gsi_ready();
    need(&ripa_gsi);uint32_t status=rd(ripa_gsi.base);
    ((void)0);
    if(rd(ripa_cb.base+0x58)&0xc00001fe)return false; /* Rings still require actual EE0 readiness. */
    ripa.hardware=true;raphael_ipa_stage=78;
    ((void)0);return true;
}

/* PAS/platform preparation precedes MPSS. GSI may still be disabled here;
 * retain the real enabled-bit check before allowing any channel setup. */
static bool ripa_data_gsi_ready(void)
{
    return ripa.hardware && (rd(ripa_gsi.base)&1) &&
        !(rd(ripa_cb.base+0x58)&0xc00001feu);
}
