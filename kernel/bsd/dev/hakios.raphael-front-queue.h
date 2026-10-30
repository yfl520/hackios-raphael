#ifndef RAPHAEL_CAMERA_QUEUE_INCLUDED
#define RAPHAEL_CAMERA_QUEUE_INCLUDED
/* Same SOF/WM_DONE protocol as HackiOS ios_session, with QUEUED reserved
 * explicitly. All calls hold the preview mutex; no MMIO/allocation here. */
#define RAPHAEL_SLOTS 3
enum { RQ_FREE, RQ_WRITING, RQ_QUEUED, RQ_READY, RQ_READING };
struct raphael_queue_slot {
    unsigned state;
    uint64_t sequence, time;
    uint32_t lines, gain, length;
};
struct raphael_queue {
    struct raphael_queue_slot slot[RAPHAEL_SLOTS];
    int writing, queued;
    uint64_t completed, given, overwritten;
};

static void
raphael_queue_init(struct raphael_queue *q)
{
    bzero(q, sizeof(*q));
    q->writing = 0; q->queued = 0;
    q->slot[0].state = RQ_QUEUED;
}

static bool
raphael_queue_done(struct raphael_queue *q, uint64_t time,
    uint32_t lines, uint32_t gain, uint32_t length, unsigned warmup)
{
    struct raphael_queue_slot *s = &q->slot[q->writing];
    q->completed++;
    /* No SOF seen or no next address: the writer may reuse this place.
     * Never let a reader see such a buffer. */
    if (q->writing == q->queued || s->state != RQ_WRITING) return false;
    s->sequence = q->completed; s->time = time;
    s->lines = lines; s->gain = gain; s->length = length;
    s->state = q->completed >= warmup ? RQ_READY : RQ_FREE;
    return s->state == RQ_READY;
}

/* Return next slot to program. A queued slot can never be read. */
static int
raphael_queue_sof(struct raphael_queue *q)
{
    if (q->queued < 0) return -1;
    if (q->slot[q->queued].state != RQ_QUEUED) return -1;
    q->writing = q->queued; q->queued = -1;
    q->slot[q->writing].state = RQ_WRITING;
    int next = -1;
    for (unsigned i = 0; i < RAPHAEL_SLOTS; i++) {
        if ((int)i == q->writing || q->slot[i].state == RQ_READING) continue;
        if (next < 0 || q->slot[i].sequence < q->slot[next].sequence) next = (int)i;
    }
    if (next < 0) return -1;
    if (q->slot[next].state == RQ_READY) q->overwritten++;
    q->slot[next].state = RQ_QUEUED;
    q->queued = next;
    return next;
}

static int
raphael_queue_take(struct raphael_queue *q)
{
    int take = -1;
    for (unsigned i = 0; i < RAPHAEL_SLOTS; i++) {
        if (q->slot[i].state != RQ_READY || q->slot[i].sequence <= q->given) continue;
        if (take < 0 || q->slot[i].sequence > q->slot[take].sequence) take = (int)i;
    }
    if (take >= 0) {
        q->slot[take].state = RQ_READING; q->given = q->slot[take].sequence;
    }
    return take;
}

static void
raphael_queue_release(struct raphael_queue *q, unsigned index)
{
    if (index < RAPHAEL_SLOTS && q->slot[index].state == RQ_READING)
        q->slot[index].state = RQ_FREE;
}

#endif
