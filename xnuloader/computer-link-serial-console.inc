/* Qualcomm's extracted USB driver resolves "SerialPort Lib" and dereferences
 * Write at offset 16 for each write (UsbfnDwc3Dxe RVA fc30). That table comes
 * from the firmware's GUID HOB, not ST->con_out. Only isolate this framebuffer
 * writer while XNU owns the display. Keep bounded, text-free counters. */
typedef uint64_t (EFIAPI *cl_sio_write_fn)(uint8_t *,uint64_t);
struct cl_sio {uint32_t version,pad;void *read;cl_sio_write_fn write;void *rest[5];};
struct cl_shim {uint32_t version,pad;void *install;efi_status (EFIAPI *load)(char *,uint32_t,void **);};
struct cl_hob {uint16_t type,length;uint32_t reserved;};
struct cl_phit {struct cl_hob header;uint32_t version,bootmode;uint64_t memory[4],end;};
_Static_assert(offsetof(struct cl_sio,write)==16,"Qualcomm serial library ABI");
_Static_assert(offsetof(struct cl_shim,load)==16,"Qualcomm shim loader ABI");
_Static_assert(sizeof(struct cl_phit)==56,"PI handoff HOB ABI");
static struct cl_sio *cl_sio_table;
static cl_sio_write_fn cl_sio_original;
static uint64_t cl_sio_calls,cl_sio_bytes,cl_sio_usb,cl_sio_power;
static unsigned cl_sio_installed,cl_sio_lookup;
static const efi_guid cl_hob_guid={0x7739f24c,0x93d7,0x11d4,{0x9a,0x3a,0,0x90,0x27,0x3f,0xc1,0x4d}};
static const efi_guid cl_shim_hob_guid={0xbedaeabc,0x5e70,0x4d66,{0x97,0x33,0x21,0x3d,7,0x2b,0x9d,4}};

/* Pure, bounded HOB traversal, shared with the host sanitizer regression. */
static struct cl_shim *cl_sio_find(const void *data,size_t bytes) {
    const uint8_t *base=data;
    if(!data || bytes<sizeof(struct cl_phit) || bytes>0x100000) return NULL;
    const struct cl_phit *phit=data;
    if(phit->header.type!=1 || phit->header.length!=sizeof(*phit) ||
        phit->header.reserved || phit->version!=9) return NULL;
    for(size_t at=0;at<=bytes-sizeof(struct cl_hob);) {
        struct cl_hob h;memcpy(&h,base+at,sizeof(h));
        if(h.reserved || h.length<sizeof(h) || (h.length&7) || h.length>bytes-at) return NULL;
        if(h.type==0xffff) return NULL;
        if(h.type==4 && h.length==32 &&
            !memcmp(base+at+8,&cl_shim_hob_guid,sizeof(efi_guid))) {
            uintptr_t pointer;memcpy(&pointer,base+at+24,sizeof(pointer));
            if(!pointer || (pointer&7))return NULL;
            return (struct cl_shim *)pointer;
        }
        at+=h.length;
    }
    return NULL;
}
static int cl_sio_has(const uint8_t *text,uint64_t length,const char *word) {
    if(!text)return 0;
    if(length>512)length=512;
    for(uint64_t i=0;i<length;i++) {
        unsigned j=0;
        while(word[j] && i+j<length && text[i+j]==(uint8_t)word[j])j++;
        if(!word[j])return 1;
    }
    return 0;
}
static uint64_t EFIAPI cl_sio_capture(uint8_t *text,uint64_t length) {
    cl_sio_calls++;cl_sio_bytes+=length;
    if(cl_sio_has(text,length,"Dwc") || cl_sio_has(text,length,"Usb") ||
        cl_sio_has(text,length,"URB") || cl_sio_has(text,length,"TRB"))cl_sio_usb++;
    if(cl_sio_has(text,length,"SPMI") || cl_sio_has(text,length,"Pwr") ||
        cl_sio_has(text,length,"Gpio"))cl_sio_power++;
    return length;
}
static int cl_sio_install_table(void *interface) {
    if(cl_sio_table)return 1;
    if(!interface || ((uintptr_t)interface&7))return 0;
    struct cl_sio *table=interface;
    if(table->version!=0x10001 || !table->write || table->write==cl_sio_capture)return 0;
    cl_sio_table=table;cl_sio_original=table->write;
    table->write=cl_sio_capture;cl_sio_installed=1;return 1;
}
static int cl_sio_install(struct cl_shim *shim) {
    if(cl_sio_table)return 1;
    if(!shim || shim->version!=0x10001 || !shim->load)return 0;
    void *interface=NULL;
    efi_status status=shim->load("SerialPort Lib",0x10001,&interface);
    return !EFI_ERROR(status) && cl_sio_install_table(interface);
}
/* The driver may retain the shim after its HOB is no longer discoverable through
 * the current system table. Follow its cached interface only after matching the
 * exact known driver image size, string and instructions; never patch code. */
struct cl_loaded_image {
    uint32_t revision,pad;void *parent,*system,*device,*path,*reserved;
    uint32_t options_size,pad2;void *options,*base;uint64_t size;
    uint32_t code_type,data_type;void *unload;
};
_Static_assert(offsetof(struct cl_loaded_image,base)==64,"LoadedImage base ABI");
_Static_assert(offsetof(struct cl_loaded_image,size)==72,"LoadedImage size ABI");
static const efi_guid cl_image_guid={0x5b1b31a1,0x9562,0x11d2,{0x8e,0x3f,0,0xa0,0xc9,0x69,0x72,0x3b}};
static const uint32_t cl_usb_signature[]={0xf81d0ff5,0xa9014ff4,0xa9027bfd,0x910083fd,
    0x90000055,0xaa0103f3,0xaa0003f4,0xf943faa8,0xb5000088,0x97ffffe7,0xf943faa8,0xb4000108,
    0xf9400902,0xaa1403e0,0xaa1303e1,0xa9427bfd,0xa9414ff4,0xf84307f5,0xd61f0040};
static void *cl_sio_cached_table(const void *image,uint64_t size) {
    if(!image || size!=0x19000)return NULL;
    const uint8_t *base=image;
    if(memcmp(base+0xfc00,cl_usb_signature,sizeof(cl_usb_signature)) ||
        memcmp(base+0x169d5,"SerialPort Lib",15))return NULL;
    uintptr_t pointer;memcpy(&pointer,base+0x177f0,sizeof(pointer));
    return pointer && !(pointer&7) ? (void *)pointer:NULL;
}
static void cl_sio_driver_acquire(void) {
    cl_sio_lookup=5;
    if(!BS || !BS->locate_handle_buffer || !BS->handle_protocol || !BS->free_pool)return;
    efi_handle *handles=NULL;uint64_t count=0;
    efi_status status=BS->locate_handle_buffer(ByProtocol,&cl_image_guid,NULL,&count,&handles);
    if(EFI_ERROR(status) || !handles){cl_sio_lookup=9;return;}
    if(count<=256)for(uint64_t i=0;i<count;i++) {
        struct cl_loaded_image *image=NULL;
        status=BS->handle_protocol(handles[i],&cl_image_guid,(void **)&image);
        if(EFI_ERROR(status) || !image || image->revision!=0x1000)continue;
        void *table=cl_sio_cached_table(image->base,image->size);
        if(!table)continue;
        cl_sio_lookup=7;
        if(cl_sio_install_table(table)){cl_sio_lookup=8;break;}
    }
    BS->free_pool(handles);
}
static void cl_sio_acquire(void) {
    if(cl_sio_table)return;
    cl_sio_lookup=1;
    for(uint64_t i=0;ST->tables && i<ST->nr_tables && i<64;i++) {
        if(memcmp(&ST->tables[i].guid,&cl_hob_guid,sizeof(efi_guid)))continue;
        const struct cl_phit *phit=ST->tables[i].table;
        uintptr_t start=(uintptr_t)phit;
        if(!start || (start&7))continue;
        if(phit->header.type!=1 || phit->header.length!=sizeof(*phit) || phit->version!=9)continue;
        if(phit->end<start || phit->end-start>0x100000-sizeof(struct cl_hob))continue;
        size_t bytes=(size_t)(phit->end-start)+sizeof(struct cl_hob);
        cl_sio_lookup=2;
        struct cl_shim *shim=cl_sio_find(phit,bytes);
        if(shim)cl_sio_lookup=3;
        if(cl_sio_install(shim)){cl_sio_lookup=4;return;}
    }
    cl_sio_driver_acquire();
}
static void cl_sio_release(void) {
    if(cl_sio_table && cl_sio_table->write==cl_sio_capture)cl_sio_table->write=cl_sio_original;
    cl_sio_table=NULL;cl_sio_original=NULL;cl_sio_installed=0;
}
