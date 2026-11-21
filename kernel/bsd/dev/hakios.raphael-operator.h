/* QMI NAS Get Home Network (0x25), libqmi qmi-service-nas.json.
 * Home PLMN is separate from serving PLMN and does not imply registration.
 * No IMSI/ICCID reads. The caller checks endpoint and transaction first.
 */
struct raphael_home_network {
    uint16_t mcc,mnc;
    uint8_t mnc_digits; /* 0 unknown, 2 or 3 when TLV 0x12 says 3GPP */
    char name[64];
};
static bool
raphael_operator_text(const uint8_t *p,size_t n)
{
    /* Reject control characters and malformed UTF-8, never copy partial text. */
    for(size_t i=0;i<n;) {
        uint32_t c=p[i++];unsigned more;uint32_t minimum;
        if(c<0x80) {if(c<0x20 || c==0x7f)return false;continue;}
        if(c>=0xc2 && c<=0xdf){c&=31;more=1;minimum=0x80;}
        else if(c>=0xe0 && c<=0xef){c&=15;more=2;minimum=0x800;}
        else if(c>=0xf0 && c<=0xf4){c&=7;more=3;minimum=0x10000;}
        else return false;
        if(n-i<more)return false;
        for(unsigned j=0;j<more;j++){uint8_t b=p[i++];if((b&0xc0)!=0x80)return false;c=(c<<6)|(b&63);}
        if(c<minimum || c>0x10ffff || (c>=0xd800 && c<=0xdfff) || (c>=0x80 && c<=0x9f))return false;
    }
    return true;
}
/* Validate structure and operation result before inspecting success-only TLVs.
 * Failed QMI responses may contain empty/uninitialised optional data.
 * A rejected response is diagnosed, never silently called a transport timeout.
 */
static int
raphael_operator_result(const uint8_t *m,size_t n,uint16_t message,uint16_t *code)
{
    bool seen[256]={0};const uint8_t *result=NULL;size_t at=7;
    if(!m || n<7 || m[0]!=2 || raphael_sim_u16(m+3)!=message || raphael_sim_u16(m+5)!=n-7)return EPROTO;
    while(at<n) {
        if(n-at<3)return EPROTO;
        uint8_t type=m[at];size_t bytes=raphael_sim_u16(m+at+1);at+=3;
        if(bytes>n-at || seen[type])return EPROTO;seen[type]=true;
        if(type==2){if(bytes!=4)return EPROTO;result=m+at;}at+=bytes;
    }
    if(!result || raphael_sim_u16(result)>1)return EPROTO;
    if(code)*code=raphael_sim_u16(result+2);
    if(raphael_sim_u16(result))return EIO;
    return raphael_sim_u16(result+2)?EPROTO:0;
}
static void
raphael_operator_rejected(const uint8_t *m,size_t n,uint16_t message,int error)
{
    uint16_t code=0;int result=raphael_operator_result(m,n,message,&code);
    printf("raphael-sim: operator reply msg=%#x bytes=%u rejected=%d result-check=%d QMI-error=%u\n",
        message,(unsigned)n,error,result,code);
    /* Only TLV type and byte count, never subscriber data or a raw frame dump. */
    size_t at=7;
    for(unsigned i=0;i<6 && at+3<=n;i++) {
        uint8_t type=m[at];size_t bytes=raphael_sim_u16(m+at+1);at+=3;
        printf("raphael-sim: operator reply field=%#x bytes=%u\n",type,(unsigned)bytes);
        if(bytes>n-at)break;at+=bytes;
    }
}
static int
raphael_provider_decode(const uint8_t *m,size_t n,char out[64])
{
    int error=raphael_operator_result(m,n,0x39,NULL);if(error)return error;
    for(size_t at=7;at<n;) {
        uint8_t type=m[at];size_t bytes=raphael_sim_u16(m+at+1);at+=3;const uint8_t *v=m+at;at+=bytes;
        if(type!=0x10)continue;
        /* NAS Service Provider Name: display condition, byte-sized string. */
        if(bytes<2 || bytes!=(size_t)2+v[1])return EPROTO;
        if(!v[1])return ENOENT;
        if(v[1]>=64 || !raphael_operator_text(v+2,v[1]))return EPROTO;
        char next[64]={0};memcpy(next,v+2,v[1]);memcpy(out,next,sizeof(next));return 0;
    }
    return ENOENT;
}

static int
raphael_home_decode(const uint8_t *m,size_t n,struct raphael_home_network *out)
{
    int checked=raphael_operator_result(m,n,0x25,NULL);if(checked)return checked;
    struct raphael_home_network next={0};
    bool result=false,home=false,digits=false;uint16_t error=0;size_t at=7;
    if(!m || !out || n<7 || m[0]!=2 || raphael_sim_u16(m+3)!=0x25 || raphael_sim_u16(m+5)!=n-7)return EPROTO;
    while(at<n) {
        if(n-at<3)return EPROTO;
        uint8_t t=m[at];size_t size=raphael_sim_u16(m+at+1);at+=3;
        if(size>n-at)return EPROTO;const uint8_t *v=m+at;at+=size;
        if(t==2){if(result || size!=4)return EPROTO;result=true;error=raphael_sim_u16(v);}
        else if(t==1){
            if(home || size<5 || size!=(size_t)5+v[4])return EPROTO;home=true;
            next.mcc=raphael_sim_u16(v);next.mnc=raphael_sim_u16(v+2);
            if(!next.mcc)return ENOENT;
            if(next.mcc>999 || next.mnc>999)return EPROTO;
            if(!raphael_operator_text(v+5,v[4]))return EPROTO;
            /* PLMN remains usable if an unusually long description cannot fit. */
            if(v[4]<sizeof(next.name))memcpy(next.name,v+5,v[4]);
        } else if(t==0x12){
            if(digits || size!=2 || v[0]>1 || v[1]>1)return EPROTO;digits=true;
            if(v[0])next.mnc_digits=v[1]?3:2;
        }
    }
    if(!result)return EPROTO;
    if(error)return EIO;
    if(!home || (next.mnc_digits==2 && next.mnc>99))return EPROTO;
    *out=next;return 0;
}

/* UIM Read Transparent (libqmi) and SIM alpha identifier decoding.
 * Only EF_SPN (6F46) is read. No IMSI/ICCID, raw EF or AID is logged.
 * EF_SPN is 17 bytes: display condition + 16-byte alpha identifier.
 * GSM default/extension and UCS2 80/81/82 are decoded into bounded UTF-8.
 */
static int
raphael_alpha_put(char out[64],size_t *at,uint32_t c)
{
    if(c<0x20 || (c>=0x7f && c<=0x9f) || c>0xffff ||
        (c>=0xd800 && c<=0xdfff) || c==0xfffe || c==0xffff)return EPROTO;
    size_t need=c<0x80?1:c<0x800?2:3;
    if(*at+need>=64)return EOVERFLOW;
    if(need==1)out[(*at)++]=(char)c;
    else if(need==2){out[(*at)++]=(char)(0xc0|(c>>6));out[(*at)++]=(char)(0x80|(c&63));}
    else {out[(*at)++]=(char)(0xe0|(c>>12));out[(*at)++]=(char)(0x80|((c>>6)&63));out[(*at)++]=(char)(0x80|(c&63));}
    return 0;
}
static uint32_t
raphael_gsm_character(uint8_t b,bool extension)
{
    if(extension) {
        switch(b) {
        case 0x14:return '^';case 0x28:return '{';case 0x29:return '}';
        case 0x2f:return '\\';case 0x3c:return '[';case 0x3d:return '~';
        case 0x3e:return ']';case 0x40:return '|';case 0x65:return 0x20ac;
        default:return 0;
        }
    }
    static const uint16_t special[32]={
        '@',0xa3,'$',0xa5,0xe8,0xe9,0xf9,0xec,0xf2,0xc7,0x0a,0xd8,0xf8,0x0d,0xc5,0xe5,
        0x394,'_',0x3a6,0x393,0x39b,0x3a9,0x3a0,0x3a8,0x3a3,0x398,0x39e,0,0xc6,0xe6,0xdf,0xc9
    };
    if(b<32)return special[b];
    switch(b) {
    case 0x24:return 0xa4;case 0x40:return 0xa1;case 0x5b:return 0xc4;
    case 0x5c:return 0xd6;case 0x5d:return 0xd1;case 0x5e:return 0xdc;case 0x5f:return 0xa7;
    case 0x60:return 0xbf;case 0x7b:return 0xe4;case 0x7c:return 0xf6;
    case 0x7d:return 0xf1;case 0x7e:return 0xfc;case 0x7f:return 0xe0;
    default:return b<128?b:0;
    }
}
static bool
raphael_alpha_padding(const uint8_t *p,size_t n)
{
    for(size_t i=0;i<n;i++)if(p[i]!=0xff)return false;
    return true;
}
static int
raphael_alpha_decode(const uint8_t *p,size_t n,char out[64])
{
    if(!p || !out || !n || n>16)return EPROTO;
    char next[64]={0};size_t at=0,pos=0,end=n;uint32_t base=0;bool compressed=false;
    if(raphael_alpha_padding(p,n))return ENOENT;
    if(p[0]==0x80) {
        pos=1;
        while(pos+1<n) {
            uint16_t c=(uint16_t)p[pos]<<8|p[pos+1];
            if(c==0xffff)break;
            int error=raphael_alpha_put(next,&at,c);if(error)return error;pos+=2;
        }
        if(!raphael_alpha_padding(p+pos,n-pos))return EPROTO;
        pos=n;
    } else {
        if(p[0]==0x81 || p[0]==0x82) {
            size_t header=p[0]==0x81?3:4;
            if(n<header || p[1]>n-header)return EPROTO;
            base=p[0]==0x81?(uint32_t)p[2]<<7:(uint32_t)p[2]<<8|p[3];
            pos=header;end=pos+p[1];compressed=true;
            if(!raphael_alpha_padding(p+end,n-end))return EPROTO;
        }
        while(pos<end) {
            uint8_t b=p[pos++];uint32_t c;
            if(!compressed && b==0xff) {
                if(!raphael_alpha_padding(p+pos,end-pos))return EPROTO;
                break;
            }
            if(compressed && (b&0x80))c=base+(b&0x7f);
            else if(b==0x1b) {
                if(pos>=end || p[pos]>=0x80)return EPROTO;
                c=raphael_gsm_character(p[pos++],true);
            } else c=raphael_gsm_character(b,false);
            int error=raphael_alpha_put(next,&at,c);if(error)return error;
        }
    }
    while(at && next[at-1]==' ')next[--at]=0;
    if(!at)return ENOENT;
    if(!raphael_operator_text((const uint8_t*)next,at))return EPROTO;
    memcpy(out,next,sizeof(next));return 0;
}
static int
raphael_spn_decode(const uint8_t *m,size_t n,char out[64])
{
    int error=raphael_operator_result(m,n,0x20,NULL);if(error)return error;
    const uint8_t *content=NULL;size_t size=0;
    for(size_t at=7;at<n;) {
        uint8_t type=m[at];size_t bytes=raphael_sim_u16(m+at+1);at+=3;
        const uint8_t *v=m+at;at+=bytes;
        if(type==0x10) {
            if(bytes!=2)return EPROTO;
            /* GSM 11.11/AOSP IccIoResult success classes. Full payload is
             * still required; 91 may announce a proactive command. */
            if(!((v[0]==0x90 && v[1]==0) || v[0]==0x91 || v[0]==0x9e || v[0]==0x9f))return EIO;
        } else if(type==0x11) {
            if(bytes!=19 || raphael_sim_u16(v)!=17)return EPROTO;
            content=v+2;size=17;
        } else if(type==0x13) {
            if(bytes!=1 || v[0])return EPROTO;
        } else if(type==0x12) return EPROTO; /* async token was never requested */
    }
    if(!content || size!=17)return EPROTO;
    return raphael_alpha_decode(content+1,16,out);
}
/* Both primary-GW session and little-endian path match qmicli-uim.c.
 * The caller proves that the provisioned primary points to this READY app.
 */
static size_t
raphael_spn_request_body(uint8_t app_type,uint8_t body[22])
{
    if(app_type!=1 && app_type!=2)return 0;
    const uint8_t request[]={1,2,0,0,0,2,7,0,0x46,0x6f,4,0,0x3f,0xff,0x7f,3,4,0,0,0,17,0};
    memcpy(body,request,sizeof(request));
    if(app_type==1){body[13]=0x20;body[14]=0x7f;} /* GSM SIM DF_GSM; USIM ADF=7FFF */
    return sizeof(request);
}
