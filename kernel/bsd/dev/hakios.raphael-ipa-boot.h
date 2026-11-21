/* R8: exact-device MDT + Android use-case 1 bandwidth paths.
 * ipa_preboot=2 is read-only, 3 exercises/restores votes, 4 authenticates PAS15.
 * Mode 5 adds exact-device SMMU and IPA4.1 SRAM/resource setup.
 * IPA INIT_DRIVER uses the existing owner; no data-ready indication or RF.
 */
#include "hakios.raphael-ipa-wire.h"
#include <sys/sysctl.h>
#include <sys/hakios.ipa.h>
extern bool raphael_ipa_data_start(void);
extern void raphael_ipa_data_stop(void);
extern void raphael_modem_data_poll(void);
static int raphael_ipa_boot_state,raphael_ipa_stage,raphael_ipa_resources;
static bool raphael_ipa_preboot_tried;
SYSCTL_DECL(_kern_hakios);
SYSCTL_INT(_kern_hakios,OID_AUTO,ipa_boot_state,CTLFLAG_RD|CTLFLAG_LOCKED,&raphael_ipa_boot_state,0,
    "1 PAS authenticated, 3 preflight, 4 clock probe; no RF/data readiness");
SYSCTL_INT(_kern_hakios,OID_AUTO,ipa_stage,CTLFLAG_RD|CTLFLAG_LOCKED,&raphael_ipa_stage,0,
    "Last entered IPA hardware stage; 68 PAS success, negative means failure");
SYSCTL_INT(_kern_hakios,OID_AUTO,ipa_resource_count,CTLFLAG_RD|CTLFLAG_LOCKED,&raphael_ipa_resources,0,
    "Number of validated SM8150 IPA bandwidth resources");

struct raphael_ipa_bcm_info {uint32_t address,unit;uint16_t width;uint8_t domain;};
/* cmd-db layout is shared with the existing Wi-Fi owner. Validate all offsets
 * before forming an entry or auxiliary pointer; never infer a BCM vote from
 * another SoC's command database. */
static bool
raphael_ipa_bcm_info(const char *name,struct raphael_ipa_bcm_info *out)
{
	char id[8]={0};size_t length=strlen(name);
	if(!out || !length || length>sizeof(id) || !cmd_db_there())return false;
	memcpy(id,name,length);
	const uint32_t cap=CMD_DB_SIZE-__offsetof(struct cmd_db_header,data);
	for(unsigned s=0;s<8;s++) {
		const struct cmd_db_slave *slave=&cmd_db->slave[s];
		if(slave->id!=5 || slave->count>512 || (slave->header_offset&3) ||
		    slave->header_offset>cap || slave->count>(cap-slave->header_offset)/sizeof(struct cmd_db_entry))continue;
		const struct cmd_db_entry *first=(const struct cmd_db_entry*)(cmd_db->data+slave->header_offset);
		for(unsigned i=0;i<slave->count;i++) {
			const struct cmd_db_entry *entry=&first[i];
			if(memcmp(entry->id,id,sizeof(id)))continue;
			uint32_t at=(uint32_t)slave->data_offset+entry->offset;
			if(((entry->address>>16)&7)!=5 || (entry->address&3) || entry->length<8 ||
			    at>cap || entry->length>cap-at)return false;
			const uint8_t *p=cmd_db->data+at;
			struct raphael_ipa_bcm_info info={entry->address,raphael_ipa_u32(p),raphael_ipa_u16(p+4),p[6]};
			/* ACV is a bitmask (APPS=8), not a numeric bandwidth vote. */
			if(info.domain>=16 || (strcmp(name,"ACV") && (!info.unit || !info.width)))return false;
			*out=info;return true;
		}
	}
	return false;
}

#include "hakios.raphael-ipa-platform.h"

struct raphael_ipa_vote {
    const char *name;uint32_t unit,width,domain,x,y,address,saved,requested;bool touched;
};
static struct raphael_ipa_vote raphael_ipa_votes[]={
	{"MC0",2050000u,16u,0u,39u,1170u,0,0,0,false},
	{"SH0",3910000u,64u,1u,20u,613u,0,0,0,false},
	{"SH2",3910000u,8u,1u,10u,10u,0,0,0,false},
	{"SH5",3910000u,32u,1u,5u,10u,0,0,0,false},
	{"SN0",530000u,16u,3u,150u,1132u,0,0,0,false},
	{"SN1",530000u,8u,3u,150u,660u,0,0,0,false},
	{"SN11",530000u,16u,3u,301u,1132u,0,0,0,false},
	{"SN15",530000u,8u,3u,75u,75u,0,0,0,false},
	{"SN3",530000u,8u,3u,75u,75u,0,0,0,false},
	{"CN0",20000u,4u,5u,2000u,2000u,0,0,0,false},
	{"IP0",40u,8u,7u,0u,3125u,0,0,0,false},
	{"ACV",1u,0u,11u,0u,8u,0,0,0,false},
};
static bool raphael_ipa_pas_started;
extern void FlushPoC_DcacheRegion(vm_offset_t va,size_t length);
static bool
raphael_ipa_vote_restore(void)
{
    bool ok=true;
    lck_mtx_lock(&rpmh_lock);
    for(unsigned i=sizeof(raphael_ipa_votes)/sizeof(raphael_ipa_votes[0]);i-->0;) {
        struct raphael_ipa_vote *v=&raphael_ipa_votes[i];uint32_t current=0;
        if(!v->touched)continue;
        /* Never replace a request changed by another shared clock user. */
        if(!raphael_rpmh_read_locked(v->address,&current)) {ok=false;break;}
        if((current&0x2fffffffu)!=(v->requested&0x2fffffffu)) {
            ((void)0);
            v->touched=false;continue;
        }
        if(!raphael_rpmh_write_locked(v->address,(v->saved&0x2fffffffu)|0x40000000u)) {ok=false;break;}
        v->touched=false;
    }
    lck_mtx_unlock(&rpmh_lock);return ok;
}
static bool
raphael_ipa_votes_begin(void)
{
    const unsigned n=sizeof(raphael_ipa_votes)/sizeof(raphael_ipa_votes[0]);
    /* Validate every cmd-db record before the first RPMh transaction. */
    for(unsigned i=0;i<n;i++) {
        struct raphael_ipa_vote *v=&raphael_ipa_votes[i];struct raphael_ipa_bcm_info info={0};
        if(!raphael_ipa_bcm_info(v->name,&info) || info.unit!=v->unit || info.width!=v->width || info.domain!=v->domain) {
            ((void)0);return false;
        }
        v->address=info.address;raphael_ipa_resources++;
    }
    lck_mtx_lock(&rpmh_lock);need(&rsc);bool ok=rsc.base!=0;
    for(unsigned i=0;i<n && ok;i++) {
        struct raphael_ipa_vote *v=&raphael_ipa_votes[i];raphael_ipa_stage=20+(int)i;
        ((void)0);
        ok=raphael_rpmh_read_locked(v->address,&v->saved);
        uint32_t x=(v->saved>>14)&0x3fff,y=v->saved&0x3fff;
        if(!strcmp(v->name,"ACV"))y|=v->y;
        else if(y<v->y)y=v->y;
        if(x<v->x)x=v->x;
        v->requested=0x60000000u|(x<<14)|y;
    }
    /* Each domain is committed before the next. Never overwrite with a
     * universal max vote: rates come from actual paths and cmd-db units. */
    for(unsigned i=0;i<n && ok;i++) {
        struct raphael_ipa_vote *v=&raphael_ipa_votes[i];raphael_ipa_stage=40+(int)i;
        ((void)0);
        v->touched=true;ok=raphael_rpmh_write_locked(v->address,v->requested);
    }
    lck_mtx_unlock(&rpmh_lock);return ok;
}
static bool
raphael_ipa_preboot_stop(void)
{
    if(raphael_ipa_pas_started) {
        uint64_t answer=~0ull;
        int32_t result=scm("to stop Raphael IPA after MPSS",SCM_PAS_SHUTDOWN,SCM_ONE_VALUE,15,0,0,0,&answer);
        if(result || answer) {((void)0);return false;}
        raphael_ipa_pas_started=false;
    }
    /* Panic/quiesce has interrupts disabled: do not acquire rpmh_lock. */
    if(!ripa_space_off())return false;
    if(leaving)return true;
    return raphael_ipa_vote_restore();
}
static bool
raphael_ipa_preboot(void)
{
    uint32_t mode=0;
    if(!hakios_is_raphael() || !PE_parse_boot_argn("ipa_preboot",&mode,sizeof(mode)) || !mode)return true;
    if(raphael_ipa_preboot_tried)return raphael_ipa_boot_state>0;
    raphael_ipa_preboot_tried=true;raphael_ipa_boot_state=-1;
    if(mode<2 || mode>6) {((void)0);return false;}
    raphael_ipa_stage=1;((void)0);
    uint64_t header_size=0,sizes[8]={0};const uint8_t *files[8]={0};
    const uint8_t *header=image_file("ipa_fws.mdt",&header_size);struct raphael_ipa_image plan={0};
    if(!raphael_ipa_decode_mdt(header,header_size,&plan)) {raphael_ipa_stage=-1;return false;}
    for(unsigned i=0;i<plan.count;i++) {
        const struct raphael_ipa_segment *s=&plan.segment[i];char name[48];if(!s->file_bytes)continue;
        snprintf(name,sizeof(name),"ipa_fws.b%02u",s->index);files[i]=image_file(name,&sizes[i]);
        if(!files[i] || sizes[i]!=s->file_bytes) {raphael_ipa_stage=-1;return false;}
    }
    if(mode==2) {
        for(unsigned i=0;i<sizeof(raphael_ipa_votes)/sizeof(raphael_ipa_votes[0]);i++) {
            struct raphael_ipa_bcm_info info={0};
            if(!raphael_ipa_bcm_info(raphael_ipa_votes[i].name,&info)) {raphael_ipa_stage=-2;return false;}
            raphael_ipa_resources++;
        }
        raphael_ipa_stage=3;raphael_ipa_boot_state=3;return true;
    }
    vm_offset_t page=0;volatile uint8_t *memory=NULL;bool pas_initialized=false,ok=false;
    uint64_t answer=~0ull;int32_t result;
    if(header_size>PAGE_SIZE || kmem_alloc(kernel_map,&page,PAGE_SIZE,
        KMA_KOBJECT|KMA_ZERO|KMA_DATA,VM_KERN_MEMORY_IOKIT)!=KERN_SUCCESS) {raphael_ipa_stage=-1;return false;}
    memcpy((void*)page,header,header_size);FlushPoC_DcacheRegion(page,PAGE_SIZE);settle();
    if(!raphael_ipa_votes_begin())goto out;
    if(mode==3) {
        ok=raphael_ipa_vote_restore();raphael_ipa_boot_state=ok?4:-1;raphael_ipa_stage=ok?52:-52;goto out;
    }
    if(mode>=5) {
        raphael_ipa_stage=54;((void)0);
        if(!ripa_space_on())goto out;
    }
    raphael_ipa_stage=60;((void)0);
    result=scm("to validate Raphael IPA header",SCM_PAS_INIT_IMAGE,SCM_VALUE_AND_BUFFER,15,kvtophys(page),0,0,&answer);
    ((void)0);
    if(result || answer)goto out;
    pas_initialized=true;answer=~0ull;
    raphael_ipa_stage=62;((void)0);
    result=scm("where Raphael IPA firmware is staged",SCM_PAS_MEM_SETUP,SCM_THREE_VALUES,15,RAPHAEL_IPA_FW_BASE,plan.bytes,0,&answer);
    ((void)0);
    if(result || answer)goto out;
    raphael_ipa_stage=64;((void)0);
    memory=(volatile uint8_t*)ml_io_map_unmappable(RAPHAEL_IPA_FW_BASE,plan.bytes,VM_WIMG_WCOMB);
    if(!memory)goto out;
    for(unsigned i=0;i<plan.count;i++) {
        const struct raphael_ipa_segment *s=&plan.segment[i];
        for(uint32_t b=0;b<s->memory_bytes;b++)memory[s->offset+b]=b<s->file_bytes?files[i][b]:0;
    }
    settle();
    for(unsigned i=0;i<plan.count;i++) {
        const struct raphael_ipa_segment *s=&plan.segment[i];
        for(uint32_t b=0;b<s->memory_bytes;b++)if(memory[s->offset+b]!=(b<s->file_bytes?files[i][b]:0))goto out;
    }
    ml_io_unmap((vm_offset_t)memory,plan.bytes);memory=NULL;
    raphael_ipa_stage=66;((void)0);answer=~0ull;
    result=scm("to authenticate Raphael IPA",SCM_PAS_AUTH_AND_RESET,SCM_ONE_VALUE,15,0,0,0,&answer);
    ((void)0);
    if(result || answer)goto out;
    raphael_ipa_pas_started=true;raphael_ipa_boot_state=1;raphael_ipa_stage=68;ok=true;
    if(mode>=5 && !ripa_hardware_init()){ok=false;goto out;}
    ((void)0);
out:
    if(memory)ml_io_unmap((vm_offset_t)memory,plan.bytes);
    if(page)kmem_free(kernel_map,page,PAGE_SIZE);
    if(!ok) {
        bool stopped=!pas_initialized;
        if(pas_initialized) {
            uint64_t a=~0ull;int32_t r=scm("to stop failed Raphael IPA",SCM_PAS_SHUTDOWN,SCM_ONE_VALUE,15,0,0,0,&a);stopped=!r && !a;
            if(!stopped)((void)0);
        }
        if(stopped && !ripa_space_off())stopped=false;
        if(stopped && !raphael_ipa_vote_restore())((void)0);
        if(stopped)raphael_ipa_pas_started=false;
        raphael_ipa_boot_state=-1;if(raphael_ipa_stage>0)raphael_ipa_stage=-raphael_ipa_stage;
    }
    return ok;
}
