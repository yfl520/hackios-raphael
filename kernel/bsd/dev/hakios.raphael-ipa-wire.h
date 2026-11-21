/* Raphael IPA 4.1, GSI 2.0. Pure wire/MDT helpers, shared by kernel and ASan tests.
 * Register base = DT wrapper 0x1e00000 + ipahal_get_reg_base() 0x40000.
 * Xiaomi raphael-p-oss 663db50e: ipa_utils.c (ipa_4_1_mem_part),
 * ipa_qmi_service.c and ipa_qmi_service_v01.c. No RF or MMIO in this file.
 */
#ifndef HAKIOS_RAPHAEL_IPA_WIRE_H
#define HAKIOS_RAPHAEL_IPA_WIRE_H
#define RAPHAEL_IPA_FW_BASE 0x98f00000u
#define RAPHAEL_IPA_FW_BYTES 0x10000u
#define RAPHAEL_IPA_REG_BASE 0x01e40000u
#define RAPHAEL_IPA_SRAM_BASE 0x01e47000u
#define RAPHAEL_GSI_BASE 0x01e04000u
#define RAPHAEL_IPA_END 0x2800u
#define RAPHAEL_IPA_INIT_BYTES 154u
struct raphael_ipa_segment {
	uint32_t index, offset, target, file_bytes, memory_bytes, virtual_address;
};
struct raphael_ipa_image {
	uint32_t count, bytes;
	struct raphael_ipa_segment segment[8];
};
static uint16_t raphael_ipa_u16(const uint8_t *p) { return p[0] | (uint16_t)p[1]<<8; }
static uint32_t raphael_ipa_u32(const uint8_t *p) {
	return p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}
static void raphael_ipa_put32(uint8_t *p,uint32_t v) {
	for(unsigned i=0;i<4;i++) p[i]=(uint8_t)(v>>(8*i));
}
/* Validate all segments before publishing any load plan. Never relocate hash entries.
 * Only the three known IPA/GSI IRAM destinations are accepted on this board.
 * Returns false without modifying out for malformed/incompatible firmware.
 */
static bool
raphael_ipa_decode_mdt(const uint8_t *p,size_t n,struct raphael_ipa_image *out)
{
	struct raphael_ipa_image plan={0}; uint32_t low=~0u,high=0; bool reloc=false;
	if(!p || !out || n<52 || n>0x10000 || memcmp(p,"\177ELF\1\1\1",7) ||
	    raphael_ipa_u16(p+40)!=52 || raphael_ipa_u16(p+42)!=32) return false;
	uint32_t phoff=raphael_ipa_u32(p+28),count=raphael_ipa_u16(p+44);
	if(phoff<52 || phoff>n || !count || count>8 || count>(n-phoff)/32) return false;
	for(unsigned i=0;i<count;i++) {
		const uint8_t *s=p+phoff+32*i;uint32_t flags=raphael_ipa_u32(s+24),bytes=raphael_ipa_u32(s+20);
		if(raphael_ipa_u32(s)!=1 || ((flags>>24)&7)==2 || !bytes) continue;
		uint32_t pa=raphael_ipa_u32(s+12),file=raphael_ipa_u32(s+16),va=raphael_ipa_u32(s+8);
		/* GSI 2.0: 0x6000 bytes; DPS: 0x80; HPS: 0x140 on this board. */
		uint32_t limit=va==0x01e08000?0x6000:va==0x01e5e000?0x80:va==0x01e5e080?0x140:0;
		if(!limit || bytes>limit || file>bytes || ((file|bytes|pa)&3) || (uint64_t)pa+bytes>0x100000000ull) return false;
		if(plan.count && reloc!=!!(flags&(1u<<27))) return false;
		reloc=!!(flags&(1u<<27));if(pa<low)low=pa;
		plan.segment[plan.count++]=(struct raphael_ipa_segment){i,0,pa,file,bytes,va};
	}
	if(plan.count!=3) return false;
	for(unsigned i=0;i<plan.count;i++) {
		struct raphael_ipa_segment *s=&plan.segment[i];
		uint64_t offset=reloc?(uint64_t)s->target-low:(uint64_t)s->target-RAPHAEL_IPA_FW_BASE;
		if(offset>=RAPHAEL_IPA_FW_BYTES || s->memory_bytes>RAPHAEL_IPA_FW_BYTES-offset) return false;
		s->offset=(uint32_t)offset;s->target=RAPHAEL_IPA_FW_BASE+s->offset;
		for(unsigned j=0;j<i;j++) {
			const struct raphael_ipa_segment *other=&plan.segment[j];
			if(s->virtual_address==other->virtual_address ||
			    (s->offset<other->offset+other->memory_bytes && other->offset<s->offset+s->memory_bytes)) return false;
		}
		if(high<s->offset+s->memory_bytes)high=s->offset+s->memory_bytes;
	}
	plan.bytes=(high+0xfff)&~0xfffu;if(!plan.bytes || plan.bytes>RAPHAEL_IPA_FW_BYTES)return false;
	*out=plan;return true;
}
/* INIT_MODEM_DRIVER body for IPA4.1: actual AP WAN consumer is endpoint 11.
 * smem_restricted_bytes is learned from hardware, not a hard-coded zero.
 * Return zero without touching output if capacity/offset/transaction is invalid.
 */
static size_t __attribute__((unused))
raphael_ipa_init_packet(uint8_t *p,size_t capacity,uint16_t transaction,uint32_t restricted)
{
	static const struct { uint8_t type,words;uint32_t a,b;bool offset; } fields[]={
		{0x10,1,3,0,false}, {0x11,2,0x688,0x7c7,true},
		{0x12,2,0x508,7,true}, {0x13,2,0x608,7,true},
		{0x14,1,0x308,0,true}, {0x15,1,0x408,0,true},
		{0x16,2,0x13f0,0x100c,true}, {0x17,1,11,0,false},
		{0x19,2,0x7d0,0x9cf,true}, {0x1b,2,0x488,7,true},
		{0x1c,2,0x588,7,true}, {0x1d,1,0x288,0,true},
		{0x1e,1,0x388,0,true}, {0x1f,1,0xc30,0,true},
		{0x20,1,0x60,0,false}, {0x21,1,0x13d0,0,true}, {0x22,1,0x20,0,false},
	};
	if(!p || capacity<RAPHAEL_IPA_INIT_BYTES || !transaction || restricted>0xffff || (restricted&7))return 0;
	size_t at=7;p[0]=0;p[1]=(uint8_t)transaction;p[2]=(uint8_t)(transaction>>8);p[3]=0x21;p[4]=0;
	for(unsigned i=0;i<sizeof(fields)/sizeof(fields[0]);i++) {
		p[at++]=fields[i].type;p[at++]=4*fields[i].words;p[at++]=0;
		raphael_ipa_put32(p+at,fields[i].a+(fields[i].offset?restricted:0));at+=4;
		if(fields[i].words==2) {
			bool end=fields[i].type==0x11 || fields[i].type==0x19;
			raphael_ipa_put32(p+at,fields[i].b+(end?restricted:0));at+=4;
		}
	}
	p[5]=(uint8_t)(at-7);p[6]=(uint8_t)((at-7)>>8);return at;
}
#endif
