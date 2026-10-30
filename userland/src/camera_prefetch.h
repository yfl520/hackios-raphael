/* Two complete RAW buffers, one reader and one ISP consumer. Only Raphael's
 * complete-frame cdev protocol uses this; other boards keep their streaming ABI.
 * No buffer is reused until take() releases the consumer's previous lease.
 * finish() joins the bounded cdev read before buffers or the fd can be freed. */
#ifndef HAKIOS_CAMERA_PREFETCH_H
#define HAKIOS_CAMERA_PREFETCH_H
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

struct camera_prefetch_frame {
    uint8_t *data;
    ssize_t length;
    int error;
    uint64_t began, ended;
};

struct camera_prefetch {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    pthread_t thread;
    struct camera_prefetch_frame frame[2];
    size_t bytes;
    ssize_t (*read_frame)(void *, void *, size_t);
    uint64_t (*now)(void);
    void *context;
    bool stop;
    int ready, held;
};

static void *
camera_prefetch_reader(void *context)
{
    struct camera_prefetch *p = context;
    int slot = 0;
    pthread_mutex_lock(&p->lock);
    while (!p->stop) {
        /* One queued frame at most, even when the consumer has no lease yet. */
        while (!p->stop && (p->ready >= 0 || p->held == slot))
            pthread_cond_wait(&p->changed, &p->lock);
        if (p->stop) break;
        pthread_mutex_unlock(&p->lock);
        struct camera_prefetch_frame *f = &p->frame[slot];
        f->began = p->now();
        f->length = p->read_frame(p->context, f->data, p->bytes);
        f->error = f->length < 0 ? errno : 0;
        f->ended = p->now();
        pthread_mutex_lock(&p->lock);
        if (p->stop) break;
        p->ready = slot;
        slot ^= 1;
        pthread_cond_broadcast(&p->changed);
    }
    pthread_mutex_unlock(&p->lock);
    return NULL;
}

/* Caller only uses take/finish after successful initialization. */
static int
camera_prefetch_start(struct camera_prefetch *p, size_t bytes,
    ssize_t (*read_frame)(void *, void *, size_t), void *context, uint64_t (*now)(void))
{
    memset(p, 0, sizeof(*p));
    p->ready = p->held = -1;
    p->bytes = bytes; p->read_frame = read_frame; p->context = context; p->now = now;
    p->frame[0].data = malloc(bytes);
    p->frame[1].data = malloc(bytes);
    int error = ENOMEM;
    if (!p->frame[0].data || !p->frame[1].data) goto buffers;
    if ((error = pthread_mutex_init(&p->lock, NULL))) goto buffers;
    if ((error = pthread_cond_init(&p->changed, NULL))) goto mutex;
    if ((error = pthread_create(&p->thread, NULL, camera_prefetch_reader, p))) goto cond;
    return 0;
cond:
    pthread_cond_destroy(&p->changed);
mutex:
    pthread_mutex_destroy(&p->lock);
buffers:
    free(p->frame[0].data); free(p->frame[1].data);
    return error;
}

/* Single consumer: invalidates its previous lease, then waits for the next
 * result (including errors/short reads). Header and body stay in one buffer. */
static const struct camera_prefetch_frame *
camera_prefetch_take(struct camera_prefetch *p)
{
    pthread_mutex_lock(&p->lock);
    p->held = -1;
    pthread_cond_broadcast(&p->changed);
    while (p->ready < 0 && !p->stop) pthread_cond_wait(&p->changed, &p->lock);
    const struct camera_prefetch_frame *f = NULL;
    if (!p->stop) {
        p->held = p->ready;
        p->ready = -1;
        f = &p->frame[p->held];
        pthread_cond_broadcast(&p->changed);
    }
    pthread_mutex_unlock(&p->lock);
    return f;
}

static void
camera_prefetch_finish(struct camera_prefetch *p)
{
    pthread_mutex_lock(&p->lock);
    p->stop = true;
    pthread_cond_broadcast(&p->changed);
    pthread_mutex_unlock(&p->lock);
    pthread_join(p->thread, NULL);
    pthread_cond_destroy(&p->changed);
    pthread_mutex_destroy(&p->lock);
    free(p->frame[0].data); free(p->frame[1].data);
}
#endif
