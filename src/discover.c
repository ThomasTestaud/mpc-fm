/* Find the sample frames of a generator at run time. See discover.h. */
#define _GNU_SOURCE
#include "discover.h"

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/uio.h>
#include <unistd.h>

#include "fm/fm_carrier.h"

#define MAX_PATHS 4
#define QUEUE 16
#define NODE_BYTES 1024
#define MAX_NODES 50000
#define MAX_DEPTH 5
#define SCAN_OFFSETS 64 /* the frames may start a little after the pointer */

static disc_path_t g_paths[MAX_PATHS];
static _Atomic int g_npaths;
static disc_first_hop_fn g_first_hop;

void disc_set_first_hop(disc_first_hop_fn fn) { g_first_hop = fn; }

/* Max hops for a path whose first hop is at `off` (0: forbidden). */
static int first_hop_limit(int off)
{
    return g_first_hop ? g_first_hop(off) : MAX_DEPTH;
}

/* SPSC queue: audio thread -> logger. */
static void *g_queue[QUEUE];
static _Atomic unsigned g_qhead, g_qtail;

/* (generator, its sample object) pairs already searched without result
 * (logger only): generators are reused for other samples. They expire
 * after RETRY_POLLS polls (a sample may not be loaded yet). */
#define RETRY_POLLS 20
static unsigned g_search_every = 5; /* at most one search per 5 polls (about 5 s) */
static unsigned g_last_search;

void disc_set_throttle(unsigned polls) { g_search_every = polls; }

static unsigned g_searches;
unsigned disc_searches(void) { return g_searches; }
static struct {
    uintptr_t key;
    unsigned poll;
    int tries; /* after 2 failed searches: a normal sample, never again */
} g_tried[256];
static int g_ntried;
static unsigned g_polls;

int disc_read(void *dst, uintptr_t addr, size_t n)
{
    struct iovec local = {dst, n}, remote = {(void *)addr, n};
    ssize_t r = process_vm_readv(getpid(), &local, 1, &remote, 1, 0);
    return r == (ssize_t)n ? 0 : -1;
}

#define UNREADABLE (-2)

/* Header at the end of a path: 1 carrier, 0 frames there but no header,
 * -1 damaged header, UNREADABLE: the path does not lead to memory.
 * nib (may be NULL): low nibbles of the first 8 frames read. */
static int follow(void *gen, const disc_path_t *d, fm_patch *p, int *root, unsigned char *nib)
{
    uintptr_t q = (uintptr_t)gen;
    for (int i = 0; i < d->hops; i++) {
        uintptr_t next;
        if (disc_read(&next, q + (uintptr_t)d->off[i], sizeof next) != 0 || !next)
            return UNREADABLE;
        q = next;
    }
    unsigned char buf[FM_CARRIER_FRAMES * 4];
    int fb = fm_carrier_frame_bytes(d->fmt);
    if (disc_read(buf, q + (uintptr_t)d->data_off, 8 * (size_t)fb) != 0)
        return UNREADABLE;
    if (nib)
        for (int i = 0; i < 8; i++) {
            int v;
            if (d->fmt == FM_FMT_I16) {
                int16_t x;
                memcpy(&x, buf + 2 * i, 2);
                v = x;
            } else if (d->fmt == FM_FMT_I32) {
                int32_t x;
                memcpy(&x, buf + 4 * i, 4);
                v = x >> 16;
            } else {
                float x;
                memcpy(&x, buf + 4 * i, 4);
                v = (int)lrintf(x * 32768.0f);
            }
            nib[i] = (unsigned char)(v & 15);
        }
    if (!fm_carrier_has_magic_fmt(buf, d->fmt, 8))
        return 0;
    if (disc_read(buf, q + (uintptr_t)d->data_off, (size_t)FM_CARRIER_FRAMES * (size_t)fb) != 0)
        return -1;
    return fm_carrier_decode_fmt(buf, d->fmt, FM_CARRIER_FRAMES, p, root);
}

void disc_seed(const disc_path_t *d)
{
    int n = atomic_load_explicit(&g_npaths, memory_order_relaxed);
    for (int i = 0; i < n; i++)
        if (memcmp(&g_paths[i], d, sizeof *d) == 0)
            return;
    if (n < MAX_PATHS) {
        g_paths[n] = *d;
        atomic_store_explicit(&g_npaths, n + 1, memory_order_release);
    }
}

int disc_lookup(void *gen, fm_patch *p, int *root, int allow_search, unsigned char nib[8])
{
    int n = atomic_load_explicit(&g_npaths, memory_order_acquire);
    int readable = 0;
    memset(nib, 0xff, 8);
    for (int i = 0; i < n; i++) {
        int r = follow(gen, &g_paths[i], p, root, readable ? NULL : nib);
        if (r == 1)
            return 1;
        readable |= r != UNREADABLE;
    }
    /* A known path led to frames without a header: a normal sample, no
     * search, unless the caller says this kind of sample may be out of
     * the paths' reach. (v0.1.2 searched for every normal sample: tens of
     * thousands of reads that made the audio drop out.) */
    if (readable && !allow_search)
        return 0;
    /* No path leads anywhere for this one: let the logger look. */
    unsigned h = atomic_load_explicit(&g_qhead, memory_order_relaxed);
    unsigned t = atomic_load_explicit(&g_qtail, memory_order_acquire);
    if (h - t < QUEUE) {
        g_queue[h % QUEUE] = gen;
        atomic_store_explicit(&g_qhead, h + 1, memory_order_release);
    }
    return 0;
}

/* ---- the search (logger thread) ---- */

typedef struct {
    uintptr_t addr;
    int parent, off, depth;
    int limit; /* max depth allowed on this branch (from its first hop) */
} node_t;

static node_t g_nodes[MAX_NODES];
#define SEEN 65536
static uintptr_t g_seen[SEEN];

static int seen(uintptr_t a)
{
    unsigned h = (unsigned)((a >> 3) * 2654435761u) & (SEEN - 1);
    for (int k = 0; k < SEEN; k++, h = (h + 1) & (SEEN - 1)) {
        if (g_seen[h] == a)
            return 1;
        if (!g_seen[h]) {
            g_seen[h] = a;
            return 0;
        }
    }
    return 1; /* full: treat as seen */
}

static int plausible(uintptr_t v, uintptr_t lo, uintptr_t hi)
{
    if (v & 7)
        return 0;
    if (v < 0x10000 || v >= (1ull << 48))
        return 0;
    return !(v >= lo && v < hi); /* not into the app image (code, vtables) */
}

/* How many bytes of the object at `a` to scan for pointers. Heap blocks
 * (glibc malloc) have their size just before them: stay inside it, so a
 * path never relies on two blocks that just happen to be neighbours. */
static size_t object_span(uintptr_t a)
{
    uint64_t raw;
    if (a < 8 || disc_read(&raw, a - 8, sizeof raw) != 0)
        return NODE_BYTES;
    uint64_t size = raw & ~(uint64_t)7;
    int mmapped = (raw & 2) != 0;
    int plausible = mmapped ? (size >= 4096 && !(size & 4095) && size <= (1ull << 30))
                            : (size >= 32 && !(size & 15) && size <= (64ull << 20));
    if (!plausible)
        return NODE_BYTES; /* not the start of a heap block: inner object, static data */
    uint64_t usable = size - (mmapped ? 16 : 8);
    return usable < NODE_BYTES ? (size_t)usable : NODE_BYTES;
}

int disc_search(void *gen, uintptr_t image_lo, uintptr_t image_hi, disc_path_t *found, int *reads)
{
    memset(g_seen, 0, sizeof g_seen);
    int n = 0, head = 0;
    *reads = 0;
    g_nodes[n++] = (node_t){(uintptr_t)gen, -1, 0, 0, MAX_DEPTH};
    seen((uintptr_t)gen);
    static unsigned char buf[NODE_BYTES + SCAN_OFFSETS + FM_CARRIER_FRAMES * 4];
    while (head < n) {
        node_t nd = g_nodes[head];
        int me = head++;
        /* Is this a buffer that starts with a carrier header? */
        size_t want = NODE_BYTES > SCAN_OFFSETS + (size_t)FM_CARRIER_FRAMES * 4
                          ? NODE_BYTES : SCAN_OFFSETS + (size_t)FM_CARRIER_FRAMES * 4;
        int have_all = disc_read(buf, nd.addr, want) == 0;
        if (have_all)
            (*reads)++;
        if (nd.depth > 0 && have_all) {
            for (int fmt = 0; fmt < FM_FMT_COUNT; fmt++)
                for (int o = 0; o < SCAN_OFFSETS; o += 4) {
                    if (!fm_carrier_has_magic_fmt(buf + o, fmt, 8))
                        continue;
                    fm_patch p;
                    int root;
                    int fb = fm_carrier_frame_bytes(fmt);
                    if ((size_t)o + (size_t)FM_CARRIER_FRAMES * (size_t)fb > want ||
                        fm_carrier_decode_fmt(buf + o, fmt, FM_CARRIER_FRAMES, &p, &root) != 1)
                        continue;
                    /* Path: the offsets from the generator down to this node. */
                    int chain[MAX_DEPTH + 1], depth = 0;
                    for (int k = me; g_nodes[k].parent >= 0 && depth <= MAX_DEPTH; k = g_nodes[k].parent)
                        chain[depth++] = g_nodes[k].off;
                    found->hops = depth;
                    for (int k = 0; k < depth; k++)
                        found->off[k] = chain[depth - 1 - k];
                    found->data_off = o;
                    found->fmt = fmt;
                    return 1;
                }
        }
        if (nd.depth >= MAX_DEPTH || nd.depth >= nd.limit)
            continue;
        /* Follow its pointers. */
        if (!have_all) {
            if (disc_read(buf, nd.addr, NODE_BYTES) != 0)
                continue;
            (*reads)++;
        }
        /* The generator itself is not a heap block (it sits inside the
         * voice): what precedes it means nothing, scan it in full. */
        size_t span = nd.depth == 0 ? NODE_BYTES : object_span(nd.addr);
        for (int o = 0; o + 8 <= (int)span && n < MAX_NODES; o += 8) {
            uintptr_t v;
            memcpy(&v, buf + o, 8);
            int limit = nd.limit;
            if (nd.depth == 0) { /* first hop: only fields tied to this pad */
                limit = first_hop_limit(o);
                if (limit <= 0)
                    continue;
            }
            if (!plausible(v, image_lo, image_hi) || seen(v))
                continue;
            g_nodes[n++] = (node_t){v, me, o, nd.depth + 1, limit};
        }
    }
    return 0;
}

void disc_describe(const disc_path_t *d, char *out, size_t cap)
{
    static const char *const FMT[] = {"float32", "int16", "int32"};
    size_t len = (size_t)snprintf(out, cap, "gen");
    for (int i = 0; i < d->hops && len < cap; i++)
        len += (size_t)snprintf(out + len, cap - len, " ->+%d", d->off[i]);
    if (len < cap)
        snprintf(out + len, cap - len, " +%d: %s frames", d->data_off, FMT[d->fmt % 3]);
}

int disc_paths(disc_path_t *out, int max)
{
    int n = atomic_load_explicit(&g_npaths, memory_order_acquire);
    if (n > max)
        n = max;
    memcpy(out, g_paths, (size_t)n * sizeof *out);
    return n;
}

int disc_poll(uintptr_t image_lo, uintptr_t image_hi, int key_off, char *msg, size_t cap)
{
    unsigned t = atomic_load_explicit(&g_qtail, memory_order_relaxed);
    unsigned h = atomic_load_explicit(&g_qhead, memory_order_acquire);
    int learned = 0;
    g_polls++;
    for (int i = 0; i < g_ntried; i++) /* one retry after a while (not loaded yet?) */
        if (g_tried[i].tries == 1 && g_polls - g_tried[i].poll > RETRY_POLLS)
            g_tried[i].poll = 0, g_tried[i].tries = -g_tried[i].tries; /* marked: may retry */
    if (g_last_search && g_polls - g_last_search < g_search_every)
        return 0; /* the queue waits */
    while (t != h && !learned) {
        void *gen = g_queue[t % QUEUE];
        atomic_store_explicit(&g_qtail, ++t, memory_order_release);
        uintptr_t sample = 0;
        disc_read(&sample, (uintptr_t)gen + (uintptr_t)key_off, sizeof sample);
        uintptr_t key = sample ? sample : (uintptr_t)gen; /* a sample's frames do not change */
        int tried = -1;
        for (int i = 0; i < g_ntried; i++)
            if (g_tried[i].key == key)
                tried = i;
        if (tried >= 0 && g_tried[tried].tries > 0)
            continue; /* searched, not due for a retry */
        disc_path_t d;
        int reads = 0;
        g_last_search = g_polls;
        g_searches++;
        int found = disc_search(gen, image_lo, image_hi, &d, &reads);
        if (found) {
            int n = atomic_load_explicit(&g_npaths, memory_order_relaxed);
            if (n < MAX_PATHS) {
                g_paths[n] = d;
                atomic_store_explicit(&g_npaths, n + 1, memory_order_release);
            }
            char where[160];
            disc_describe(&d, where, sizeof where);
            snprintf(msg, cap, "FM carrier frames found: %s (%d reads)", where, reads);
            learned = 1;
            g_ntried = 0; /* new knowledge: generators may be retried */
        } else if (tried >= 0) {
            g_tried[tried].tries = 1 - g_tried[tried].tries; /* -1 -> 2: final */
            g_tried[tried].poll = g_polls;
        } else if (g_ntried < (int)(sizeof g_tried / sizeof *g_tried)) {
            g_tried[g_ntried].key = key; /* a normal sample, or not loaded yet */
            g_tried[g_ntried].poll = g_polls;
            g_tried[g_ntried++].tries = 1;
        }
        break; /* one search per turn */
    }
    return learned;
}

void disc_reset(void)
{
    atomic_store(&g_npaths, 0);
    atomic_store(&g_qhead, 0);
    atomic_store(&g_qtail, 0);
    g_ntried = 0;
    g_last_search = 0;
}
