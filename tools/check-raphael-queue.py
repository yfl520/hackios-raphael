#!/usr/bin/env python3
"""Exercise actual shared SOF/completion/read queue, including long readers."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
source=(root/'xnu/files/bsd/dev/hakios.raphael-queue.h').read_text()
code=r'''
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#define bzero(p,n) memset(p,0,n)
''' + source + r'''
static void invariant(const struct raphael_queue *q, int reader){
    assert(q->queued>=0&&q->queued<RAPHAEL_SLOTS);
    assert(q->slot[q->queued].state==RQ_QUEUED);
    assert(reader!=q->queued);
    if(reader>=0)assert(q->slot[reader].state==RQ_READING);
    unsigned reading=0,queued=0;
    for(unsigned i=0;i<RAPHAEL_SLOTS;i++){
        reading+=q->slot[i].state==RQ_READING;
        queued+=q->slot[i].state==RQ_QUEUED;
    }
    assert(reading==(reader>=0)&&queued==1);
}
int main(void){
    struct raphael_queue q;
    for(unsigned period=1;period<=37;period++){
        raphael_queue_init(&q);int reader=-1;uint64_t before=0;
        for(unsigned frame=0;frame<10000;frame++){
            /* Completion and next SOF may be observed together, as upstream. */
            if(frame)raphael_queue_done(&q,frame*33,100+frame,288,3064,4);
            int queued=raphael_queue_sof(&q);assert(queued>=0);
            invariant(&q,reader);
            /* A slow consumer holds one slot across many writer cycles. */
            if(reader>=0){
                assert(q.slot[reader].sequence==before);
                if(frame%period==0){raphael_queue_release(&q,reader);reader=-1;}
            }
            if(reader<0){
                reader=raphael_queue_take(&q);
                if(reader>=0){
                    assert(frame>=4&&q.slot[reader].sequence>before);
                    assert(q.slot[reader].lines==100+frame);
                    before=q.slot[reader].sequence;
                }
            }
            invariant(&q,reader);
        }
        if(reader>=0)raphael_queue_release(&q,reader);
    }
    /* Completion before first SOF and repeated completion cannot expose a
     * buffer that remains the DMA target, or steal one from a reader. */
    raphael_queue_init(&q);
    assert(!raphael_queue_done(&q,1,100,288,3064,0));
    assert(raphael_queue_take(&q)<0);
    assert(raphael_queue_sof(&q)==1);
    assert(raphael_queue_done(&q,2,100,288,3064,0));
    int r=raphael_queue_take(&q);assert(r==0);
    assert(!raphael_queue_done(&q,3,100,288,3064,0));
    assert(q.slot[0].state==RQ_READING);
    assert(raphael_queue_sof(&q)==2);
    invariant(&q,r);
    puts("Actual rear session queue: 370000 frame boundaries, queued-slot exclusion, slow readers, warmup and duplicate completion PASS");
}
'''
with tempfile.TemporaryDirectory() as td:
    src,exe=Path(td)/'queue.c',Path(td)/'queue'
    src.write_text(code)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(src),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
