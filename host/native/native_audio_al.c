/* OpenAL Soft backend under the browsing-of-isaac audio model.
 *
 * host_audio.c owns the OpenAL object model the guest sees (sources, queues,
 * the wall clock that retires buffers) and calls the weak
 * isaac_audio_backend_* hooks to make it audible; the web build answers them
 * with WebAudio (host_audio_web.c), this file with a real OpenAL device. The
 * semantics follow host_audio_web.c:
 *
 *   buffer(id, pcm)   keep a host copy of the PCM (the guest frees its
 *                     copy right after alBufferData)
 *   play static       the buffer from offset_sec, looping or not
 *   play stream       the source's queue from entry `head`, the first entry
 *                     from offset_sec
 *   queue/unqueue     entries join / the n oldest leave the guest queue
 *
 * Every stream entry gets its own AL buffer made from the host copy, so the
 * model retiring an entry by the clock never collides with AL still holding
 * it; AL buffers are deleted once AL reports them processed. A stream that
 * ran dry while the model still plays it is restarted when data arrives.
 *
 * Only linked into the gl profile. ISAAC_NATIVE_AUDIO=0 keeps it silent. */
#include <AL/al.h>
#include <AL/alc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static ALCdevice *g_dev;
static ALCcontext *g_ctx;
static int g_state;                 /* 0 untried, 1 ready, -1 off */
static unsigned g_plays, g_queued_al, g_restarts, g_al_errors;

static int ready(void) {
    if (g_state) return g_state > 0;
    g_state = -1;
    const char *e = getenv("ISAAC_NATIVE_AUDIO");
    if (e && *e == '0') return 0;
    g_dev = alcOpenDevice(NULL);
    if (!g_dev) { fprintf(stderr, "[native][al] no OpenAL device: silent\n"); return 0; }
    g_ctx = alcCreateContext(g_dev, NULL);
    if (!g_ctx || !alcMakeContextCurrent(g_ctx)) {
        fprintf(stderr, "[native][al] no OpenAL context: silent\n");
        return 0;
    }
    g_state = 1;
    fprintf(stderr, "[native][al] %s | %s\n", alGetString(AL_RENDERER),
            alcGetString(g_dev, ALC_DEVICE_SPECIFIER));
    return 1;
}

static void check(const char *what) {
    ALenum err = alGetError();
    if (err != AL_NO_ERROR && ++g_al_errors <= 20)
        fprintf(stderr, "[native][al] %s: AL error 0x%x\n", what, err);
}

/* ---- buffers: guest id -> host PCM copy + one AL buffer for static play --- */

typedef struct {
    uint32_t id;
    uint8_t *pcm;
    uint32_t bytes;
    ALenum format;
    ALsizei freq;
    ALuint al;                       /* static-play buffer, 0 until needed */
} buf_rec;

static buf_rec *g_bufs;
static size_t g_nbufs, g_capbufs;

static buf_rec *buf_find(uint32_t id, int create) {
    for (size_t i = 0; i < g_nbufs; ++i)
        if (g_bufs[i].id == id) return &g_bufs[i];
    if (!create) return NULL;
    if (g_nbufs == g_capbufs) {
        g_capbufs = g_capbufs ? g_capbufs * 2 : 256;
        g_bufs = realloc(g_bufs, g_capbufs * sizeof *g_bufs);
    }
    buf_rec *b = &g_bufs[g_nbufs++];
    memset(b, 0, sizeof *b);
    b->id = id;
    return b;
}

/* AL buffers that were still attached when retired: deleted when free. */
static ALuint *g_orphans;
static size_t g_norphans, g_caporphans;

static void orphan(ALuint al) {
    if (!al) return;
    alDeleteBuffers(1, &al);
    if (alGetError() == AL_NO_ERROR) return;
    if (g_norphans == g_caporphans) {
        g_caporphans = g_caporphans ? g_caporphans * 2 : 64;
        g_orphans = realloc(g_orphans, g_caporphans * sizeof *g_orphans);
    }
    g_orphans[g_norphans++] = al;
}

static void sweep_orphans(void) {
    for (size_t i = 0; i < g_norphans;) {
        alDeleteBuffers(1, &g_orphans[i]);
        if (alGetError() == AL_NO_ERROR) g_orphans[i] = g_orphans[--g_norphans];
        else ++i;
    }
}

static ALuint al_from(const buf_rec *b) {
    ALuint al = 0;
    alGenBuffers(1, &al);
    alBufferData(al, b->format, b->pcm, (ALsizei)b->bytes, b->freq);
    check("alBufferData");
    return al;
}

/* ---- sources: guest id -> AL source + the guest queue's entries ---------- */

typedef struct {
    uint32_t id;
    ALuint al;
    uint32_t *entries;               /* guest buffer ids still in the guest queue */
    size_t nentries, capentries;
    int streaming, playing;
} src_rec;

static src_rec *g_srcs;
static size_t g_nsrcs, g_capsrcs;

static src_rec *src_find(uint32_t id, int create) {
    for (size_t i = 0; i < g_nsrcs; ++i)
        if (g_srcs[i].id == id) return &g_srcs[i];
    if (!create || !ready()) return NULL;
    if (g_nsrcs == g_capsrcs) {
        g_capsrcs = g_capsrcs ? g_capsrcs * 2 : 64;
        g_srcs = realloc(g_srcs, g_capsrcs * sizeof *g_srcs);
    }
    src_rec *s = &g_srcs[g_nsrcs++];
    memset(s, 0, sizeof *s);
    s->id = id;
    alGenSources(1, &s->al);
    check("alGenSources");
    return s;
}

/* Delete the AL buffers this source has finished with. */
static void reclaim(src_rec *s) {
    ALint done = 0;
    alGetSourcei(s->al, AL_BUFFERS_PROCESSED, &done);
    while (done-- > 0) {
        ALuint al = 0;
        alSourceUnqueueBuffers(s->al, 1, &al);
        if (alGetError() != AL_NO_ERROR) break;
        alDeleteBuffers(1, &al);
    }
}

/* Stop and detach everything; the stream's AL buffers go with it. */
static void silence(src_rec *s) {
    alSourceStop(s->al);
    reclaim(s);
    alSourcei(s->al, AL_BUFFER, 0);
    alGetError();
    s->playing = 0;
}

static void queue_al(src_rec *s, uint32_t id, float offset_sec) {
    buf_rec *b = buf_find(id, 0);
    if (!b || !b->pcm) return;
    ALuint al = 0;
    if (offset_sec > 0.0f) {
        /* The first entry from an offset: AL_SEC_OFFSET is per source, so
         * trim the copy instead. */
        int frame = (b->format == AL_FORMAT_STEREO16) ? 4 : (b->format == AL_FORMAT_MONO16 ||
                    b->format == AL_FORMAT_STEREO8) ? 2 : 1;
        uint32_t skip = (uint32_t)(offset_sec * (float)b->freq) * (uint32_t)frame;
        if (skip >= b->bytes) return;
        alGenBuffers(1, &al);
        alBufferData(al, b->format, b->pcm + skip, (ALsizei)(b->bytes - skip), b->freq);
        check("alBufferData(offset)");
    } else {
        al = al_from(b);
    }
    alSourceQueueBuffers(s->al, 1, &al);
    check("alSourceQueueBuffers");
    ++g_queued_al;
}

/* ---- the hooks ------------------------------------------------------------ */

void isaac_audio_backend_buffer(uint32_t id, const void *pcm, uint32_t bytes,
                                int channels, int bits, int freq) {
    if (!ready() || !pcm || !bytes || !freq) return;
    buf_rec *b = buf_find(id, 1);
    uint8_t *copy = malloc(bytes);
    if (!copy) return;
    memcpy(copy, pcm, bytes);
    free(b->pcm);
    b->pcm = copy;
    b->bytes = bytes;
    b->freq = freq;
    b->format = channels == 2 ? (bits == 16 ? AL_FORMAT_STEREO16 : AL_FORMAT_STEREO8)
                              : (bits == 16 ? AL_FORMAT_MONO16 : AL_FORMAT_MONO8);
    orphan(b->al);                    /* a refill: the old static copy is stale */
    b->al = 0;
}

void isaac_audio_backend_play(uint32_t src, uint32_t buffer, float gain, float pitch,
                              int looping, int streaming, uint32_t head, float offset_sec) {
    src_rec *s = src_find(src, 1);
    if (!s) return;
    sweep_orphans();
    silence(s);
    alSourcef(s->al, AL_GAIN, gain);
    alSourcef(s->al, AL_PITCH, pitch > 0.01f ? pitch : 1.0f);
    s->streaming = streaming;
    if (streaming) {
        alSourcei(s->al, AL_LOOPING, AL_FALSE);
        for (size_t i = head; i < s->nentries; ++i)
            queue_al(s, s->entries[i], i == head ? offset_sec : 0.0f);
    } else {
        buf_rec *b = buf_find(buffer, 0);
        if (!b || !b->pcm) return;
        if (!b->al) b->al = al_from(b);
        alSourcei(s->al, AL_BUFFER, (ALint)b->al);
        alSourcei(s->al, AL_LOOPING, looping ? AL_TRUE : AL_FALSE);
        if (offset_sec > 0.0f) alSourcef(s->al, AL_SEC_OFFSET, offset_sec);
    }
    alSourcePlay(s->al);
    check("alSourcePlay");
    s->playing = 1;
    ++g_plays;
}

void isaac_audio_backend_queue(uint32_t src, uint32_t buffer) {
    src_rec *s = src_find(src, 1);
    if (!s) return;
    if (s->nentries == s->capentries) {
        s->capentries = s->capentries ? s->capentries * 2 : 8;
        s->entries = realloc(s->entries, s->capentries * sizeof *s->entries);
    }
    s->entries[s->nentries++] = buffer;
    s->streaming = 1;
    if (!s->playing) return;
    reclaim(s);
    queue_al(s, buffer, 0.0f);
    ALint st = 0;
    alGetSourcei(s->al, AL_SOURCE_STATE, &st);
    if (st != AL_PLAYING) {           /* ran dry ahead of the model's clock */
        alSourcePlay(s->al);
        ++g_restarts;
    }
}

void isaac_audio_backend_unqueue(uint32_t src, uint32_t n) {
    src_rec *s = src_find(src, 0);
    if (!s) return;
    if (n > s->nentries) n = (uint32_t)s->nentries;
    memmove(s->entries, s->entries + n, (s->nentries - n) * sizeof *s->entries);
    s->nentries -= n;
    reclaim(s);
}

void isaac_audio_backend_clear(uint32_t src) {
    src_rec *s = src_find(src, 0);
    if (!s) return;
    silence(s);
    s->nentries = 0;
}

void isaac_audio_backend_delete(uint32_t src) {
    for (size_t i = 0; i < g_nsrcs; ++i) {
        if (g_srcs[i].id != src) continue;
        silence(&g_srcs[i]);
        alDeleteSources(1, &g_srcs[i].al);
        free(g_srcs[i].entries);
        g_srcs[i] = g_srcs[--g_nsrcs];
        return;
    }
}

void isaac_audio_backend_stop(uint32_t src) {
    src_rec *s = src_find(src, 0);
    if (s) silence(s);
}

void isaac_audio_backend_pause(uint32_t src) {
    src_rec *s = src_find(src, 0);
    if (!s) return;
    alSourcePause(s->al);
    s->playing = 0;
}

void isaac_audio_backend_gain(uint32_t src, float gain) {
    src_rec *s = src_find(src, 0);
    if (s) alSourcef(s->al, AL_GAIN, gain);
}

void isaac_audio_backend_pitch(uint32_t src, float pitch) {
    src_rec *s = src_find(src, 0);
    if (s) alSourcef(s->al, AL_PITCH, pitch > 0.01f ? pitch : 1.0f);
}

void isaac_audio_backend_drop_buffer(uint32_t id) {
    for (size_t i = 0; i < g_nbufs; ++i) {
        if (g_bufs[i].id != id) continue;
        orphan(g_bufs[i].al);
        free(g_bufs[i].pcm);
        g_bufs[i] = g_bufs[--g_nbufs];
        return;
    }
}

void isaac_native_audio_report(void) {
    if (g_state <= 0) return;
    fprintf(stderr, "[native][al] %u plays, %u stream buffers queued to AL, %u stream restarts, "
            "%zu sources, %zu buffers kept, %zu orphaned, %u AL errors\n",
            g_plays, g_queued_al, g_restarts, g_nsrcs, g_nbufs, g_norphans, g_al_errors);
}

int isaac_native_audio_state(void) { return g_state; }

/* Close the device before the process returns to the loader. */
void isaac_native_audio_shutdown(void) {
    if (g_state <= 0) return;
    for (size_t i = 0; i < g_nsrcs; ++i) { alSourceStop(g_srcs[i].al); alDeleteSources(1, &g_srcs[i].al); }
    alcMakeContextCurrent(NULL);
    alcDestroyContext(g_ctx);
    alcCloseDevice(g_dev);
    g_state = 0;
}
