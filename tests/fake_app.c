/* Host test: an "audio app" loop like the stock one (read, process, write). */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <time.h>
#include <unistd.h>

int snd_pcm_open(void **pcm, const char *name, int stream, int mode);
int snd_pcm_close(void *pcm);
long snd_pcm_readi(void *pcm, void *buf, unsigned long n);
long snd_pcm_writei(void *pcm, const void *buf, unsigned long n);
int snd_pcm_recover(void *pcm, int err, int silent);

static void burn_us(long us) /* "DSP work": busy, not sleeping */
{
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    do
        clock_gettime(CLOCK_MONOTONIC, &b);
    while ((b.tv_sec - a.tv_sec) * 1000000L + (b.tv_nsec - a.tv_nsec) / 1000 < us);
}

int main(void)
{
    void *play, *cap;
    int buf[256] = {0};
    if (snd_pcm_open(&play, "hw:CARD=I2S,DEV=0", 0, 0) || snd_pcm_open(&cap, "hw:CARD=I2S,DEV=0", 1, 0))
        return 1;
    for (int i = 0; i < 1300; i++) { /* about 3.5 s */
        snd_pcm_readi(cap, buf, 128);
        burn_us(800);
        long r = snd_pcm_writei(play, buf, 128);
        if (r == -EPIPE)
            snd_pcm_recover(play, (int)r, 1);
    }
    snd_pcm_close(play);
    snd_pcm_close(cap);
    sleep(3); /* the logger writes its status */
    return 0;
}
