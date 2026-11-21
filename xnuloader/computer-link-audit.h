/* Streaming plist diagnostics: retains Request/Error tokens only. Other keys,
 * values, certificate data and identifiers are neither captured nor logged. */
#ifndef HAKIOS_LINK_AUDIT_H
#define HAKIOS_LINK_AUDIT_H
struct ca_reader {
    uint8_t prefix[4]; unsigned prefix_length, remaining, disabled;
    unsigned in_tag, tag_length, text_length, capture, wanted;
    char tag[16], text[96];
};
typedef void (*ca_callback)(void *,unsigned,const char *);
static int ca_equal(const char *a,const char *b) {
    while (*a && *a==*b) { a++; b++; } return *a==*b;
}
static void ca_body(struct ca_reader *r, uint8_t byte, ca_callback callback, void *ctx) {
    if (byte=='<') { r->in_tag=1; r->tag_length=0; return; }
    if (r->in_tag) {
        if (byte!='>') {
            if (r->tag_length<sizeof(r->tag)-1) r->tag[r->tag_length++]=(char)byte;
            return;
        }
        r->in_tag=0; r->tag[r->tag_length]=0;
        if (ca_equal(r->tag,"key")) { r->capture=1; r->text_length=0; }
        else if (ca_equal(r->tag,"/key") && r->capture==1) {
            r->text[r->text_length]=0;
            r->wanted=ca_equal(r->text,"Request") ? 1:ca_equal(r->text,"Error") ? 2:0;
            r->capture=0;
        } else if (ca_equal(r->tag,"string")) { r->capture=r->wanted ? 2:0; r->text_length=0; }
        else if (ca_equal(r->tag,"/string") && r->capture==2) {
            r->text[r->text_length]=0;
            unsigned valid=r->text_length>0;
            for (unsigned i=0;i<r->text_length;i++) {
                char c=r->text[i]; if (!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_')) valid=0;
            }
            if (valid) callback(ctx,r->wanted,r->text);
            r->capture=r->wanted=0;
        } else if (r->tag[0]!='/' && r->wanted && !ca_equal(r->tag,"string")) r->wanted=0;
        return;
    }
    if (r->capture && r->text_length<sizeof(r->text)-1) r->text[r->text_length++]=(char)byte;
}
static void ca_feed(struct ca_reader *r,const uint8_t *data,unsigned n,ca_callback callback,void *ctx) {
    for (unsigned i=0;i<n && !r->disabled;i++) {
        if (!r->remaining) {
            r->prefix[r->prefix_length++]=data[i];
            if (r->prefix_length==4) {
                r->remaining=((uint32_t)r->prefix[0]<<24)|((uint32_t)r->prefix[1]<<16)|((uint32_t)r->prefix[2]<<8)|r->prefix[3];
                r->prefix_length=0; r->in_tag=r->capture=r->wanted=r->text_length=0;
                if (!r->remaining || r->remaining>1024*1024) r->disabled=1; /* TLS is opaque */
            }
        } else { ca_body(r,data[i],callback,ctx); r->remaining--; }
    }
}
#endif
