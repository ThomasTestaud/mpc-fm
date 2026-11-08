/* A tiny stand-in for libasound, for host tests of the probe. */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdlib.h>
#include <time.h>

typedef struct { int stream, writes; } pcm_t;

static void sleep_us(long us)
{
    struct timespec t = {0, us * 1000};
    nanosleep(&t, NULL);
}

int snd_pcm_open(void **pcm, const char *name, int stream, int mode)
{
    (void)name; (void)mode;
    pcm_t *p = calloc(1, sizeof *p);
    if (!p)
        return -ENOMEM;
    p->stream = stream;
    *pcm = p;
    return 0;
}

int snd_pcm_close(void *pcm) { free(pcm); return 0; }

/* Capture blocks like a real device until the period is there. */
long snd_pcm_readi(void *pcm, void *buf, unsigned long n)
{
    (void)pcm; (void)buf;
    sleep_us(1500);
    return (long)n;
}

/* The 50th write reports an underrun. */
long snd_pcm_writei(void *pcm, const void *buf, unsigned long n)
{
    pcm_t *p = pcm;
    (void)buf;
    sleep_us(300);
    return ++p->writes == 50 ? -EPIPE : (long)n;
}

int snd_pcm_recover(void *pcm, int err, int silent) { (void)pcm; (void)err; (void)silent; return 0; }
