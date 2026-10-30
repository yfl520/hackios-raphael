#!/usr/bin/env python3
"""Exercise the actual complete-frame prefetch with a delayed/erroring cdev.

Checks leases across concurrent reads, full-frame identity/metadata, ordered
errors, bounded queue depth, teardown during reads/backpressure, and failed
initialization. Host concurrency evidence only, not a Camera.app fps test.
"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
code = r'''
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
static unsigned allocations, fail_allocation;
static int fail_thread;
static void *test_malloc(size_t bytes) {
    if (++allocations == fail_allocation) return NULL;
    return malloc(bytes);
}
static int test_create(pthread_t *t, const pthread_attr_t *a, void *(*fn)(void *), void *ctx) {
    return fail_thread ? EAGAIN : pthread_create(t, a, fn, ctx);
}
#define malloc test_malloc
#define pthread_create test_create
#include "camera_prefetch.h"
#undef malloc
#undef pthread_create

#define WORDS 4096
struct mock {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    unsigned reads, active, block_on;
    bool released;
};
static uint64_t now(void) {
    struct timespec t; assert(!clock_gettime(CLOCK_MONOTONIC, &t));
    return (uint64_t)t.tv_sec * 1000000000 + (uint64_t)t.tv_nsec;
}
static void pause_ns(long ns) {
    struct timespec t = {0,ns}; nanosleep(&t,NULL);
}
static void mock_init(struct mock *m) {
    memset(m,0,sizeof(*m));
    assert(!pthread_mutex_init(&m->lock,NULL)); assert(!pthread_cond_init(&m->changed,NULL));
}
static void mock_done(struct mock *m) {
    assert(!m->active); pthread_cond_destroy(&m->changed); pthread_mutex_destroy(&m->lock);
}
static ssize_t mock_read(void *ctx, void *buffer, size_t bytes) {
    struct mock *m = ctx;
    assert(bytes == WORDS*sizeof(uint64_t));
    pthread_mutex_lock(&m->lock);
    assert(++m->active == 1); unsigned n = ++m->reads;
    pthread_cond_broadcast(&m->changed);
    while (m->block_on == n && !m->released) pthread_cond_wait(&m->changed,&m->lock);
    pthread_mutex_unlock(&m->lock);
    uint64_t *data = buffer;
    for (unsigned i=0;i<WORDS;i++) data[i] = ((uint64_t)n<<32)|i;
    pause_ns(100000);
    pthread_mutex_lock(&m->lock); m->active--; pthread_mutex_unlock(&m->lock);
    if (n%17==0) {errno=EIO;return -1;}
    if (n%13==0) {errno=EAGAIN;return -1;}
    if (n%11==0) {errno=EINTR;return -1;}
    return n%7==0 ? (ssize_t)bytes-1 : (ssize_t)bytes;
}
static unsigned reads(struct mock *m) {
    pthread_mutex_lock(&m->lock); unsigned n=m->reads; pthread_mutex_unlock(&m->lock); return n;
}
static void wait_reads(struct mock *m,unsigned n) {
    uint64_t deadline=now()+2000000000;
    while(reads(m)<n) {assert(now()<deadline);pause_ns(100000);}
}
static void verify(const struct camera_prefetch_frame *f,unsigned n) {
    assert(f && f->ended >= f->began);
    int error = n%17==0?EIO:n%13==0?EAGAIN:n%11==0?EINTR:0;
    assert(f->error==error);
    assert(f->length==(error?-1:(ssize_t)(WORDS*sizeof(uint64_t))-(n%7==0)));
    const uint64_t *data=(const void *)f->data;
    for(unsigned i=0;i<WORDS;i++)assert(data[i]==(((uint64_t)n<<32)|i));
}
static void *finish(void *ctx) {camera_prefetch_finish(ctx);return NULL;}
int main(void) {
    for(unsigned pass=0;pass<12;pass++) {
        struct mock m;mock_init(&m);struct camera_prefetch p;
        assert(!camera_prefetch_start(&p,WORDS*sizeof(uint64_t),mock_read,&m,now));
        for(unsigned n=1;n<=80;n++) {
            const struct camera_prefetch_frame *f=camera_prefetch_take(&p);
            verify(f,n);
            if(n%4==0)pause_ns(500000); /* a slow ISP holds its buffer */
            verify(f,n);
            assert(reads(&m)<=n+1); /* at most one frame ahead */
        }
        camera_prefetch_finish(&p);mock_done(&m);
    }
    /* Stop while the producer waits on a full queue, with no consumer. */
    for(unsigned n=0;n<20;n++) {
        struct mock m;mock_init(&m);struct camera_prefetch p;
        assert(!camera_prefetch_start(&p,WORDS*sizeof(uint64_t),mock_read,&m,now));
        wait_reads(&m,1);pause_ns(1000000);assert(reads(&m)==1);
        camera_prefetch_finish(&p);mock_done(&m);
    }
    /* Stop/join during the bounded cdev read: don't free its writing buffer. */
    for(unsigned n=0;n<20;n++) {
        struct mock m;mock_init(&m);m.block_on=1;struct camera_prefetch p;
        assert(!camera_prefetch_start(&p,WORDS*sizeof(uint64_t),mock_read,&m,now));
        wait_reads(&m,1);pthread_t t;assert(!pthread_create(&t,NULL,finish,&p));
        pause_ns(500000);
        pthread_mutex_lock(&m.lock);m.released=true;pthread_cond_broadcast(&m.changed);pthread_mutex_unlock(&m.lock);
        assert(!pthread_join(t,NULL));mock_done(&m);
    }
    /* Initialization failure must release allocations and never leave a reader. */
    for(unsigned n=1;n<=2;n++) {
        allocations=0;fail_allocation=n;struct camera_prefetch p;
        assert(camera_prefetch_start(&p,WORDS*sizeof(uint64_t),mock_read,NULL,now)==ENOMEM);
    }
    fail_allocation=0;fail_thread=1;struct camera_prefetch p;
    assert(camera_prefetch_start(&p,WORDS*sizeof(uint64_t),mock_read,NULL,now)==EAGAIN);
    puts("prefetch: 960 leased frames; ordered EIO/EAGAIN/EINTR/short reads; 40 teardown races; allocation/thread failures passed");
}
'''
with tempfile.TemporaryDirectory() as td:
    src, exe = Path(td) / 'prefetch.c', Path(td) / 'prefetch'
    src.write_text(code)
    subprocess.run(['clang', '-std=c11', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-pthread',
                    '-I' + str(root / 'userland/src'), str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=30)
