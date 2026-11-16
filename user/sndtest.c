/* user/sndtest.c -- OSS sound on Kestrel: /dev/dsp and /dev/mixer
 *
 *   sndtest            checks, then 1.5 s of a 440 Hz tone (16-bit stereo,
 *                      48 kHz), a 660 Hz one (8-bit mono, 8 kHz) and 880 Hz
 *                      (32-bit stereo, 44.1 kHz), each played to the end
 *                      then two clients at once (mixed), and, when the
 *                      codec has an input, capture: timing, conversion,
 *                      non-blocking reads, poll, two readers, full duplex
 *   sndtest -t FREQ    the same, and the captured sound must be a tone of
 *                      FREQ Hz (QEMU fed a test tone to the input)
 *   sndtest FILE.wav   play a PCM WAV file
 * With QEMU's `-audiodev wav,id=a,path=out.wav -device intel-hda
 * -device hda-output,audiodev=a` the host can check what came out.
 * Exit status 0 = every check passed. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <time.h>
#include <sys/ioctl.h>

/* the OSS interface (<sys/soundcard.h>, defined here: musl-gcc has no
 * kernel headers) */
#define AFMT_U8      0x00000008
#define AFMT_S16_LE  0x00000010
#define AFMT_S32_LE  0x00001000
#define SNDCTL_DSP_RESET       0x5000
#define SNDCTL_DSP_SYNC        0x5001
#define SNDCTL_DSP_SPEED       0xC0045002
#define SNDCTL_DSP_SETFMT      0xC0045005
#define SNDCTL_DSP_CHANNELS    0xC0045006
#define SNDCTL_DSP_GETFMTS     0x8004500B
#define SNDCTL_DSP_GETOSPACE   0x8010500C
#define SNDCTL_DSP_GETOPTR     0x800C5012
#define SNDCTL_DSP_GETODELAY   0x80045017
#define SNDCTL_DSP_GETISPACE   0x8010500D
#define SNDCTL_DSP_GETIPTR     0x800C5011
#define SNDCTL_DSP_GETCAPS     0x8004500F
#define DSP_CAP_DUPLEX         0x00000100
#define SOUND_MIXER_READ_RECMASK  0x80044DFD
#define SOUND_MIXER_READ_VOLUME   0x80044D00
#define SOUND_MIXER_WRITE_VOLUME  0xC0044D00
#define SOUND_MIXER_READ_DEVMASK  0x80044DFE
#define SOUND_MIXER_INFO          0x805C4D65
#define SOUND_MASK_VOLUME         1
typedef struct { int fragments, fragstotal, fragsize, bytes; } audio_buf_info;
typedef struct { int bytes, blocks, ptr; } count_info;
typedef struct { char id[16], name[32]; int modify_counter; int fillers[10]; } mixer_info;

static int failures;
#define CHECK(c, what) do { int ok_ = (c); printf("sndtest:  %s  %s\n", ok_ ? "ok  " : "FAIL", what); \
                            if (!ok_) { failures++; printf("sndtest:        (errno %d: %s)\n", errno, strerror(errno)); } } while (0)

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static int write_all(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    while (len) {
        ssize_t n = write(fd, p, len);
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

/* frames of a tone at freq in the given format */
static void *tone(int fmt, int ch, int rate, double freq, double secs, size_t *bytes)
{
    int bps = fmt == AFMT_U8 ? 1 : fmt == AFMT_S16_LE ? 2 : 4;
    size_t frames = (size_t)(rate * secs);
    uint8_t *b = malloc(frames * ch * bps), *p = b;
    for (size_t i = 0; i < frames; i++) {
        double v = 0.5 * sin(2 * M_PI * freq * i / rate);
        for (int c = 0; c < ch; c++) {
            if (fmt == AFMT_U8) *p++ = (uint8_t)(128 + v * 127);
            else if (fmt == AFMT_S16_LE) { int16_t s = (int16_t)(v * 32767); memcpy(p, &s, 2); p += 2; }
            else { int32_t s = (int32_t)(v * 2147483647.0); memcpy(p, &s, 4); p += 4; }
        }
    }
    *bytes = frames * ch * bps;
    return b;
}

static int setup(int fd, int fmt, int ch, int rate, int *got_rate)
{
    int f = fmt, c = ch, r = rate;
    if (ioctl(fd, SNDCTL_DSP_SETFMT, &f) < 0 || f != fmt) return -1;
    if (ioctl(fd, SNDCTL_DSP_CHANNELS, &c) < 0 || c != ch) return -1;
    if (ioctl(fd, SNDCTL_DSP_SPEED, &r) < 0) return -1;
    *got_rate = r;
    return 0;
}

static int read_all(int fd, void *buf, size_t len)
{
    char *p = buf;
    while (len) {
        ssize_t n = read(fd, p, len);
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        if (n == 0) return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

/* The frequency of a captured tone from its zero crossings (rising), and
 * its level (RMS, 0..1), from the first channel of S16 or U8 frames. */
static double tone_freq(const void *buf, size_t frames, int fmt, int ch, int rate, double *rms)
{
    int crossings = 0;
    double sum = 0, prev = 0;
    for (size_t i = 0; i < frames; i++) {
        double v = fmt == AFMT_U8 ? (((const uint8_t *)buf)[i * ch] - 128) / 128.0 : ((const int16_t *)buf)[i * ch] / 32768.0;
        if (i && prev < 0 && v >= 0) crossings++;
        prev = v;
        sum += v * v;
    }
    *rms = frames ? sqrt(sum / frames) : 0;
    return frames ? crossings * (double)rate / frames : 0;
}

/* Two clients at once, at different rates and formats: they mix, so the
 * two seconds of sound take one second. */
static void mixing_checks(void)
{
    int a = open("/dev/dsp", O_WRONLY), b = open("/dev/dsp", O_WRONLY), ga = 0, gb = 0;
    CHECK(a >= 0 && b >= 0, "two clients open /dev/dsp at once");
    if (a < 0 || b < 0) { close(a); close(b); return; }
    CHECK(setup(a, AFMT_S16_LE, 2, 48000, &ga) == 0 && setup(b, AFMT_U8, 1, 22050, &gb) == 0 && gb == 22050,
          "each its own format: S16 stereo 48000 and U8 mono 22050 (converted)");
    size_t la, lb;
    uint8_t *ta = tone(AFMT_S16_LE, 2, 48000, 440, 1.0, &la), *tb = tone(AFMT_U8, 1, 22050, 660, 1.0, &lb);
    fcntl(a, F_SETFL, O_NONBLOCK);
    fcntl(b, F_SETFL, O_NONBLOCK);
    size_t da = 0, db = 0;
    double t0 = now();
    while ((da < la || db < lb) && now() - t0 < 5) {    /* both fed as they drain */
        ssize_t n;
        if (da < la && (n = write(a, ta + da, la - da)) > 0) da += (size_t)n;
        if (db < lb && (n = write(b, tb + db, lb - db)) > 0) db += (size_t)n;
        usleep(5000);
    }
    fcntl(a, F_SETFL, 0);
    fcntl(b, F_SETFL, 0);
    CHECK(da == la && db == lb && ioctl(a, SNDCTL_DSP_SYNC, 0) == 0 && ioctl(b, SNDCTL_DSP_SYNC, 0) == 0, "both written and played");
    double el = now() - t0;
    printf("sndtest:        two 1 s sounds together took %.2f s\n", el);
    CHECK(el > 0.8 && el < 1.7, "they played at the same time (mixed), not one after the other");
    int d = -1;
    CHECK(ioctl(a, SNDCTL_DSP_GETODELAY, &d) == 0 && d == 0, "each client's delay is its own: 0 after its SYNC");
    free(ta);
    free(tb);
    close(a);
    close(b);
}

/* Capture, when the codec has an input. want_freq: the tone QEMU feeds
 * in (0: only timing and plumbing are checked). */
static void capture_checks(double want_freq)
{
    int fd = open("/dev/dsp", O_RDONLY), got = 0;
    CHECK(fd >= 0, "capture: open /dev/dsp O_RDONLY");
    if (fd < 0) return;
    int caps = 0;
    CHECK(ioctl(fd, SNDCTL_DSP_GETCAPS, &caps) == 0 && (caps & DSP_CAP_DUPLEX), "GETCAPS: duplex");
    CHECK(setup(fd, AFMT_S16_LE, 2, 48000, &got) == 0 && got == 48000, "capture: S16_LE stereo 48000 Hz");
    size_t len = 48000 * 4;
    int16_t *buf = malloc(len);
    double t0 = now();
    CHECK(read_all(fd, buf, len) == 0, "read 1 s of sound");
    double el = now() - t0, rms, f = tone_freq(buf, len / 4, AFMT_S16_LE, 2, 48000, &rms);
    printf("sndtest:        1 s took %.2f s; %.0f Hz, level %.3f\n", el, f, rms);
    CHECK(el > 0.8 && el < 1.5, "it took about a second: the input runs in real time");
    if (want_freq > 0) CHECK(rms > 0.05 && f > want_freq * 0.95 && f < want_freq * 1.05, "the captured sound is the test tone");
    audio_buf_info bi;
    usleep(200000);
    CHECK(ioctl(fd, SNDCTL_DSP_GETISPACE, &bi) == 0 && bi.bytes >= 4 * 48000 / 10 && bi.fragsize > 0,
          "GETISPACE: what came in meanwhile (200 ms)");
    count_info ci;
    CHECK(ioctl(fd, SNDCTL_DSP_GETIPTR, &ci) == 0 && ci.bytes >= (int)len, "GETIPTR: the bytes delivered");
    CHECK(ioctl(fd, SNDCTL_DSP_RESET, 0) == 0, "RESET drops what is waiting");
    fcntl(fd, F_SETFL, O_NONBLOCK);
    char c4[4];
    errno = 0;
    CHECK(read(fd, c4, 4) < 0 && errno == EAGAIN, "non-blocking read right after: EAGAIN");
    struct pollfd p = { fd, POLLIN, 0 };
    CHECK(poll(&p, 1, 2000) == 1 && (p.revents & POLLIN), "poll: readable as sound comes in");
    fcntl(fd, F_SETFL, 0);

    /* 8 kHz mono 8-bit: converted from the hardware rate */
    CHECK(setup(fd, AFMT_U8, 1, 8000, &got) == 0 && got == 8000, "capture: U8 mono 8000 Hz");
    uint8_t *b8 = malloc(4000);
    t0 = now();
    CHECK(read_all(fd, b8, 4000) == 0, "read 0.5 s");
    el = now() - t0;
    f = tone_freq(b8, 4000, AFMT_U8, 1, 8000, &rms);
    printf("sndtest:        0.5 s took %.2f s; %.0f Hz, level %.3f\n", el, f, rms);
    CHECK(el > 0.35 && el < 0.9, "at the right pace");
    if (want_freq > 0) CHECK(rms > 0.05 && f > want_freq * 0.95 && f < want_freq * 1.05, "still the test tone after conversion");

    /* a second reader alongside */
    int fd2 = open("/dev/dsp", O_RDONLY), g2;
    CHECK(fd2 >= 0 && setup(fd2, AFMT_S16_LE, 1, 16000, &g2) == 0 && g2 == 16000, "a second reader, S16 mono 16000 Hz");
    int16_t *b2 = malloc(16000);
    t0 = now();
    CHECK(read_all(fd, b8, 2000) == 0 && read_all(fd2, b2, 16000) == 0, "both read 0.25 s and 0.5 s");
    el = now() - t0;
    CHECK(el > 0.35 && el < 0.9, "...each from its own position, in real time");
    if (want_freq > 0) {
        f = tone_freq(b2, 8000, AFMT_S16_LE, 1, 16000, &rms);
        CHECK(rms > 0.05 && f > want_freq * 0.95 && f < want_freq * 1.05, "the second reader hears the tone too");
    }
    close(fd2);
    close(fd);

    /* full duplex: one descriptor plays and records */
    fd = open("/dev/dsp", O_RDWR);
    CHECK(fd >= 0 && setup(fd, AFMT_S16_LE, 2, 48000, &got) == 0, "full duplex: O_RDWR");
    size_t tl;
    void *t = tone(AFMT_S16_LE, 2, 48000, 550, 0.5, &tl);
    CHECK(fd >= 0 && write_all(fd, t, tl) == 0 && read_all(fd, buf, 48000 * 2) == 0 && ioctl(fd, SNDCTL_DSP_SYNC, 0) == 0,
          "write 0.5 s, read 0.5 s, SYNC");
    free(t);
    close(fd);
    free(b2);
    free(b8);
    free(buf);
}

static int play_wav(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return 1; }
    uint8_t h[44];
    if (fread(h, 1, 44, f) != 44 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) { fprintf(stderr, "%s: not a WAV file\n", path); return 1; }
    int ch = h[22] | h[23] << 8, rate = h[24] | h[25] << 8 | h[26] << 16 | h[27] << 24, bits = h[34] | h[35] << 8;
    int fd = open("/dev/dsp", O_WRONLY);
    if (fd < 0) { perror("/dev/dsp"); return 1; }
    int got;
    if (setup(fd, bits == 8 ? AFMT_U8 : bits == 32 ? AFMT_S32_LE : AFMT_S16_LE, ch, rate, &got) < 0) { perror("setup"); return 1; }
    printf("sndtest: %s: %d Hz (playing at %d), %d channels, %d bits\n", path, rate, got, ch, bits);
    char buf[16384];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) if (write_all(fd, buf, n) < 0) { perror("write"); return 1; }
    ioctl(fd, SNDCTL_DSP_SYNC, 0);
    close(fd);
    return 0;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    double want_freq = 0;
    if (argc > 2 && !strcmp(argv[1], "-t")) want_freq = atof(argv[2]);
    else if (argc > 1) return play_wav(argv[1]);

    int mx = open("/dev/mixer", O_RDWR);
    CHECK(mx >= 0, "open /dev/mixer");
    int devmask = 0, vol = 0;
    CHECK(ioctl(mx, SOUND_MIXER_READ_DEVMASK, &devmask) == 0 && (devmask & SOUND_MASK_VOLUME), "mixer has a volume control");
    vol = 75 | 75 << 8;
    CHECK(ioctl(mx, SOUND_MIXER_WRITE_VOLUME, &vol) == 0 && (vol & 0xff) == 75, "set volume 75%");
    vol = 0;
    CHECK(ioctl(mx, SOUND_MIXER_READ_VOLUME, &vol) == 0 && (vol & 0xff) == 75, "read volume back");
    vol = 100 | 100 << 8;
    ioctl(mx, SOUND_MIXER_WRITE_VOLUME, &vol);
    mixer_info mi;
    CHECK(ioctl(mx, SOUND_MIXER_INFO, &mi) == 0, "mixer info");
    printf("sndtest:        mixer: %s\n", mi.name);
    close(mx);

    int fd = open("/dev/dsp", O_WRONLY);
    CHECK(fd >= 0, "open /dev/dsp");
    if (fd < 0) return 1;
    int caps = 0, recmask = -1;
    ioctl(fd, SNDCTL_DSP_GETCAPS, &caps);
    int duplex = caps & DSP_CAP_DUPLEX;
    printf("sndtest:        the codec %s\n", duplex ? "has an input" : "has no input");
    if (!duplex) CHECK(open("/dev/dsp", O_RDONLY) < 0 && errno == ENODEV, "no input: O_RDONLY refused");
    mx = open("/dev/mixer", O_RDWR);
    CHECK(ioctl(mx, SOUND_MIXER_READ_RECMASK, &recmask) == 0 && (recmask != 0) == (duplex != 0), "mixer: recording sources");
    close(mx);
    int second = open("/dev/dsp", O_WRONLY);
    CHECK(second >= 0, "a second open works (clients mix)");
    close(second);
    int fmts = 0;
    CHECK(ioctl(fd, SNDCTL_DSP_GETFMTS, &fmts) == 0 && (fmts & AFMT_S16_LE) && (fmts & AFMT_U8), "formats: S16_LE and U8");
    int got = 0;
    CHECK(setup(fd, AFMT_S16_LE, 2, 48000, &got) == 0 && got == 48000, "S16_LE stereo 48000 Hz");
    audio_buf_info bi;
    CHECK(ioctl(fd, SNDCTL_DSP_GETOSPACE, &bi) == 0 && bi.bytes > 16384 && bi.fragsize > 0, "GETOSPACE: room in the buffer");
    printf("sndtest:        %d fragments of %d bytes, %d bytes free\n", bi.fragstotal, bi.fragsize, bi.bytes);

    size_t len;
    void *t = tone(AFMT_S16_LE, 2, 48000, 440, 1.5, &len);
    double t0 = now();
    CHECK(write_all(fd, t, len) == 0, "write 1.5 s of 440 Hz");
    int delay = -1;
    CHECK(ioctl(fd, SNDCTL_DSP_GETODELAY, &delay) == 0 && delay > 0, "GETODELAY: sound still queued");
    CHECK(ioctl(fd, SNDCTL_DSP_SYNC, 0) == 0, "SYNC: played to the end");
    double el = now() - t0;
    printf("sndtest:        1.5 s of sound took %.2f s\n", el);
    CHECK(el > 1.3 && el < 2.2, "it took about as long as the sound lasts");
    delay = -1;
    CHECK(ioctl(fd, SNDCTL_DSP_GETODELAY, &delay) == 0 && delay == 0, "GETODELAY 0 after SYNC");
    count_info ci;
    CHECK(ioctl(fd, SNDCTL_DSP_GETOPTR, &ci) == 0 && ci.bytes >= (int)len, "GETOPTR: all bytes played");
    free(t);

    /* nonblocking: fill until EAGAIN */
    int fl = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    t = tone(AFMT_S16_LE, 2, 48000, 440, 2.0, &len);
    ssize_t n = write(fd, t, len);
    errno = 0;
    ssize_t n2 = write(fd, t, len);                     /* right away: the buffer is still full */
    int e2 = errno;
    CHECK(n > 0 && (size_t)n < len, "nonblocking write takes what fits");
    errno = e2;
    CHECK(n2 < 0 && e2 == EAGAIN, "then EAGAIN");
    struct pollfd p = { fd, POLLOUT, 0 };
    CHECK(poll(&p, 1, 2000) == 1 && (p.revents & POLLOUT), "poll: writable again as it plays");
    CHECK(ioctl(fd, SNDCTL_DSP_RESET, 0) == 0, "RESET drops the rest");
    fcntl(fd, F_SETFL, fl);
    free(t);

    /* 8-bit mono at 8 kHz, converted by the driver */
    CHECK(setup(fd, AFMT_U8, 1, 8000, &got) == 0 && got == 8000, "U8 mono 8000 Hz (converted if the codec lacks it)");
    printf("sndtest:        8000 Hz asked, %d Hz granted\n", got);
    t = tone(AFMT_U8, 1, got, 660, 1.0, &len);
    CHECK(write_all(fd, t, len) == 0 && ioctl(fd, SNDCTL_DSP_SYNC, 0) == 0, "play 1 s of 660 Hz (8-bit mono)");
    free(t);

    CHECK(setup(fd, AFMT_S32_LE, 2, 44100, &got) == 0 && got == 44100, "S32_LE stereo 44100 Hz");
    printf("sndtest:        44100 Hz asked, %d Hz granted\n", got);
    t = tone(AFMT_S32_LE, 2, got, 880, 1.0, &len);
    CHECK(write_all(fd, t, 0) == 0 && write_all(fd, t, 3) == 0 && write_all(fd, (char *)t + 3, len - 3) == 0,
          "play 1 s of 880 Hz (32-bit, written in odd pieces)");
    CHECK(ioctl(fd, SNDCTL_DSP_SYNC, 0) == 0, "SYNC");
    free(t);
    close(fd);

    int f2 = open("/dev/dsp", O_WRONLY);
    CHECK(f2 >= 0, "open again after close");
    close(f2);

    mixing_checks();
    if (duplex) capture_checks(want_freq);

    FILE *ps = fopen("/proc/sound", "r");
    char line[200];
    while (ps && fgets(line, sizeof line, ps)) printf("sndtest:        %s", line);
    if (ps) fclose(ps);

    if (failures) printf("sndtest: %d checks failed\n", failures);
    else printf("sndtest: all checks passed\n");
    return failures ? 1 : 0;
}
