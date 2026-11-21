/* Small bounded QuickTime/JPEG writer. One independent JPEG per sample. */
#ifndef HSR_MOV_H
#define HSR_MOV_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MOV_MAX_FRAMES 18000u
typedef struct {
 FILE *file; uint32_t width,height,fps,count;
 uint32_t *sizes,*offsets; long mdat; int failed;
} hsr_movie;
static int mov_write(FILE *f,const void *bytes,size_t n) { return fwrite(bytes,1,n,f)==n ? 0 : -1; }
static int mov_u32(FILE *f,uint32_t n) {
 uint8_t b[4]={(uint8_t)(n>>24),(uint8_t)(n>>16),(uint8_t)(n>>8),(uint8_t)n};return mov_write(f,b,4);
}
static int mov_begin(hsr_movie *m,FILE *file,uint32_t w,uint32_t h,uint32_t fps) {
 memset(m,0,sizeof(*m));
 if (!file || !w || w>65535 || !h || h>65535 || !fps || fps>30) return -1;
 m->sizes=(uint32_t *)calloc(MOV_MAX_FRAMES,4);m->offsets=(uint32_t *)calloc(MOV_MAX_FRAMES,4);
 if (!m->sizes || !m->offsets) {free(m->sizes);free(m->offsets);memset(m,0,sizeof(*m));return -1;}
 m->file=file;m->width=w;m->height=h;m->fps=fps;
 if (mov_u32(file,20) || mov_write(file,"ftypqt  ",8) || mov_u32(file,0) || mov_write(file,"qt  ",4)) m->failed=1;
 m->mdat=ftell(file);
 if (m->mdat<0 || mov_u32(file,0) || mov_write(file,"mdat",4)) m->failed=1;
 return m->failed ? -1 : 0;
}
static int mov_frame(hsr_movie *m,const void *jpeg,size_t bytes) {
 if (!m->file || m->failed || m->count>=MOV_MAX_FRAMES || bytes<4 || bytes>16*1024*1024) return -1;
 const uint8_t *j=(const uint8_t *)jpeg;
 if (j[0]!=0xff || j[1]!=0xd8 || j[bytes-2]!=0xff || j[bytes-1]!=0xd9) return -1;
 long at=ftell(m->file);
 if (at<0 || at+bytes>512u*1024*1024 || mov_write(m->file,jpeg,bytes)) {m->failed=1;return -1;}
 m->offsets[m->count]=(uint32_t)at;m->sizes[m->count]=(uint32_t)bytes;m->count++;
 return 0;
}
typedef struct {uint8_t *p;size_t n,cap;int bad;} mov_atoms;
static void ma_bytes(mov_atoms *a,const void *p,size_t n) {
 if (a->bad || n>a->cap-a->n) {a->bad=1;return;}memcpy(a->p+a->n,p,n);a->n+=n;
}
static void ma_zero(mov_atoms *a,size_t n) {
 if (a->bad || n>a->cap-a->n) {a->bad=1;return;}memset(a->p+a->n,0,n);a->n+=n;
}
static void ma16(mov_atoms *a,uint16_t n) {uint8_t b[2]={(uint8_t)(n>>8),(uint8_t)n};ma_bytes(a,b,2);}
static void ma32(mov_atoms *a,uint32_t n) {uint8_t b[4]={(uint8_t)(n>>24),(uint8_t)(n>>16),(uint8_t)(n>>8),(uint8_t)n};ma_bytes(a,b,4);}
static size_t ma_begin(mov_atoms *a,const char type[4]) {size_t at=a->n;ma32(a,0);ma_bytes(a,type,4);return at;}
static void ma_end(mov_atoms *a,size_t at) {
 if (a->bad || at+8>a->n || a->n-at>UINT32_MAX) {a->bad=1;return;}
 uint32_t n=(uint32_t)(a->n-at);a->p[at]=n>>24;a->p[at+1]=n>>16;a->p[at+2]=n>>8;a->p[at+3]=n;
}
static void ma_matrix(mov_atoms *a) {for (unsigned n=0;n<9;n++) ma32(a,n==0 || n==4 ? 0x10000 : n==8 ? 0x40000000 : 0);}
static int mov_finish(hsr_movie *m) {
 int result=-1;
 if (!m->file || m->failed || !m->count) goto done;
 long end=ftell(m->file);
 if (end<m->mdat+8 || fseek(m->file,m->mdat,SEEK_SET) || mov_u32(m->file,(uint32_t)(end-m->mdat)) || fseek(m->file,end,SEEK_SET)) goto done;
 mov_atoms a={0};a.cap=4096+(size_t)m->count*8;a.p=(uint8_t *)malloc(a.cap);
 if (!a.p) goto done;
 uint32_t scale=m->fps*1000,duration=m->count*1000;
 size_t moov=ma_begin(&a,"moov");
 size_t b=ma_begin(&a,"mvhd");ma32(&a,0);ma_zero(&a,8);ma32(&a,scale);ma32(&a,duration);
 ma32(&a,0x10000);ma16(&a,0x100);ma_zero(&a,10);ma_matrix(&a);ma_zero(&a,24);ma32(&a,2);ma_end(&a,b);
 size_t trak=ma_begin(&a,"trak");
 b=ma_begin(&a,"tkhd");ma32(&a,7);ma_zero(&a,8);ma32(&a,1);ma32(&a,0);ma32(&a,duration);
 ma_zero(&a,16);ma_matrix(&a);ma32(&a,m->width<<16);ma32(&a,m->height<<16);ma_end(&a,b);
 size_t mdia=ma_begin(&a,"mdia");
 b=ma_begin(&a,"mdhd");ma32(&a,0);ma_zero(&a,8);ma32(&a,scale);ma32(&a,duration);ma16(&a,0);ma16(&a,0);ma_end(&a,b);
 b=ma_begin(&a,"hdlr");ma32(&a,0);ma_bytes(&a,"mhlrvide",8);ma_zero(&a,12);ma_bytes(&a,"HakiOS Screen\0",14);ma_end(&a,b);
 size_t minf=ma_begin(&a,"minf");
 b=ma_begin(&a,"vmhd");ma32(&a,1);ma_zero(&a,8);ma_end(&a,b);
 size_t dinf=ma_begin(&a,"dinf");b=ma_begin(&a,"dref");ma32(&a,0);ma32(&a,1);
 size_t url=ma_begin(&a,"url ");ma32(&a,1);ma_end(&a,url);ma_end(&a,b);ma_end(&a,dinf);
 size_t stbl=ma_begin(&a,"stbl");
 b=ma_begin(&a,"stsd");ma32(&a,0);ma32(&a,1);
 size_t jpeg=ma_begin(&a,"jpeg");ma_zero(&a,6);ma16(&a,1);ma_zero(&a,16);
 ma16(&a,(uint16_t)m->width);ma16(&a,(uint16_t)m->height);ma32(&a,72<<16);ma32(&a,72<<16);
 ma32(&a,0);ma16(&a,1);ma_bytes(&a,"\x0dHakiOS Screen",14);ma_zero(&a,18);ma16(&a,24);ma16(&a,0xffff);
 ma_end(&a,jpeg);ma_end(&a,b);
 b=ma_begin(&a,"stts");ma32(&a,0);ma32(&a,1);ma32(&a,m->count);ma32(&a,1000);ma_end(&a,b);
 b=ma_begin(&a,"stsc");ma32(&a,0);ma32(&a,1);ma32(&a,1);ma32(&a,1);ma32(&a,1);ma_end(&a,b);
 b=ma_begin(&a,"stsz");ma32(&a,0);ma32(&a,0);ma32(&a,m->count);
 for (uint32_t n=0;n<m->count;n++) ma32(&a,m->sizes[n]);ma_end(&a,b);
 b=ma_begin(&a,"stco");ma32(&a,0);ma32(&a,m->count);
 for (uint32_t n=0;n<m->count;n++) ma32(&a,m->offsets[n]);ma_end(&a,b);
 ma_end(&a,stbl);ma_end(&a,minf);ma_end(&a,mdia);ma_end(&a,trak);ma_end(&a,moov);
 if (!a.bad && !mov_write(m->file,a.p,a.n) && !fflush(m->file)) result=0;
 free(a.p);
done:
 free(m->sizes);free(m->offsets);m->sizes=NULL;m->offsets=NULL;
 return result;
}
#endif
