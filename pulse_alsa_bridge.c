/*
 * pulse_alsa_bridge.c
 * Minimal PulseAudio native-protocol server that routes audio to ALSA.
 *
 * Creates a PA UNIX socket that pressure-vessel (Steam Runtime 3.0 "Sniper" and 4.0)
 * finds and forwards into the game container. Games connect via libpulse,
 * send PCM over the socket; this bridge writes it to per-stream ring buffers and
 * sums all streams into a stereo interleaved buffer that a playback thread
 * writes to the ALSA "default" device.
 *
 * Build:
 *   gcc -O2 -Wall -Wextra -o pa-alsa-bridge pulse_alsa_bridge.c \
 *       $(pkg-config --cflags --libs alsa) -lpthread -lm
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <math.h>
#include <alloca.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <poll.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <alsa/asoundlib.h>
#include "bridge_peak.h"

/* ======================================================================
 * PA protocol constants (PA 14.2 src/pulsecore/native-common.h)
 * ====================================================================== */

#define PA_PROTOCOL_VERSION  35u         /* server max; client negotiates down */
#define PA_CONTROL_CHANNEL   0xFFFFFFFFu /* control vs audio data channel */
#define PA_COOKIE_LEN        256

/* Command numbers from PA 14.2 enum (counted from 0) */
#define PA_CMD_ERROR                    0u
#define PA_CMD_REPLY                    2u
#define PA_CMD_CREATE_PLAYBACK_STREAM   3u
#define PA_CMD_DELETE_PLAYBACK_STREAM   4u
#define PA_CMD_CREATE_RECORD_STREAM     5u
#define PA_CMD_DELETE_RECORD_STREAM     6u
#define PA_CMD_AUTH                     8u
#define PA_CMD_SET_CLIENT_NAME          9u
#define PA_CMD_GET_PLAYBACK_LATENCY     14u
#define PA_CMD_DRAIN_PLAYBACK_STREAM    12u
#define PA_CMD_GET_SERVER_INFO          20u
#define PA_CMD_GET_SINK_INFO            21u
#define PA_CMD_GET_SINK_INFO_LIST       22u
#define PA_CMD_GET_CLIENT_INFO          27u
#define PA_CMD_GET_SOURCE_INFO          23u
#define PA_CMD_GET_SOURCE_INFO_LIST     24u
#define PA_CMD_GET_SINK_INPUT_INFO      29u
#define PA_CMD_GET_SINK_INPUT_INFO_LIST 30u
#define PA_CMD_GET_SOURCE_OUTPUT_INFO   31u
#define PA_CMD_GET_SOURCE_OUTPUT_INFO_LIST 32u
#define PA_CMD_SUBSCRIBE                35u
#define PA_CMD_SET_SINK_INPUT_VOLUME    37u
#define PA_CMD_CORK_PLAYBACK_STREAM     41u
#define PA_CMD_FLUSH_PLAYBACK_STREAM    42u
#define PA_CMD_TRIGGER_PLAYBACK_STREAM  43u   /* verified PA enum position 43 */
#define PA_CMD_SET_DEFAULT_SINK         44u   /* verified PA enum position 44 */
#define PA_CMD_SET_DEFAULT_SOURCE       45u
#define PA_CMD_KILL_SINK_INPUT          49u
#define PA_CMD_GET_RECORD_LATENCY       57u   /* verified PA enum position 57 */
#define PA_CMD_CORK_RECORD_STREAM       58u   /* verified PA enum position 58 */
#define PA_CMD_FLUSH_RECORD_STREAM      59u   /* verified PA enum position 59 */
#define PA_CMD_PREBUF_PLAYBACK_STREAM   60u   /* verified PA enum position 60 */
#define PA_CMD_REQUEST                  61u   /* server→client: request N bytes */
#define PA_CMD_MOVE_SINK_INPUT          67u
#define PA_CMD_SET_SINK_INPUT_MUTE      69u
#define PA_CMD_SUBSCRIBE_EVENT          66u   /* server→client: event notification */
#define PA_CMD_EXTENSION                87u   /* proto >= 14: generic extension mechanism */
#define PA_CMD_GET_CARD_INFO            88u   /* proto >= 15 */
#define PA_CMD_GET_CARD_INFO_LIST       89u   /* proto >= 15 */

/* Tag bytes (PA 14.2 src/pulsecore/tagstruct.h) */
#define PA_TAG_U32          'L'
#define PA_TAG_U8           'B'
#define PA_TAG_STRING       't'
#define PA_TAG_STRING_NULL  'N'
#define PA_TAG_BOOL_TRUE    '1'
#define PA_TAG_BOOL_FALSE   '0'
#define PA_TAG_USEC         'U'
#define PA_TAG_SAMPLE_SPEC  'a'
#define PA_TAG_ARBITRARY    'x'
#define PA_TAG_CHANNEL_MAP  'm'
#define PA_TAG_CVOLUME      'v'
#define PA_VOLUME_NORM      0x10000u  /* PA_VOLUME_NORM = 65536 (0 dB / 100%) */
#define PA_TAG_VOLUME       'V'
#define PA_TAG_TIMEVAL      'T'
#define PA_TAG_PROPLIST     'P'
#define PA_TAG_FORMAT_INFO  'f'   /* 0x66 — format_info type tag */

/* Sample format values (PA 14.2 src/pulse/sample.h) */
#define PA_SAMPLE_S16LE     3u
#define PA_SAMPLE_FLOAT32LE 5u
#define PA_SAMPLE_S32LE     7u
#define PA_SAMPLE_S24LE     9u    /* packed 24-bit, 3 bytes/sample */
#define PA_SAMPLE_S24_32LE  11u   /* 24-bit value in the low 3 bytes of a 32-bit word */
#define PA_SAMPLE_INVALID   255u  /* (uint8_t)PA_SAMPLE_INVALID == -1; sent by the extended format API */

/* Channel position values */
#define PA_CHAN_MONO  0u
#define PA_CHAN_FL    1u
#define PA_CHAN_FR    2u

/* Error codes */
#define PA_ERR_NOENTITY       5u
#define PA_ERR_NOTSUPPORTED   19u
#define PA_ERR_NOTIMPLEMENTED 23u
#define PA_ERR_INTERNAL       10u

/* ======================================================================
 * Configuration
 * ====================================================================== */

#define MAX_STREAMS   8
#define MAX_CLIENTS   8
#define MAX_RECORD_STREAMS 4 /* monitor-source capture clients (SSR, OBS, ...) */
#define HDR_LEN       20   /* 5 × uint32 packet header */
#define TS_BUF        4096 /* tagstruct write buffer */
#define CONV_FRAMES   8192 /* scratch buffer frames for audio conversion */
#define RB_BYTES      (512u * 1024u) /* 512 KB ring buffer per channel/stream */
#define REC_RB_BYTES  (256u * 1024u) /* monitor capture ring per record stream */
#define REC_TX_CHUNK  16384u         /* max payload bytes per record packet sent */
#define REC_FLUSH     2048u          /* resampler flush granularity (frames) */
#define RS_CHUNK      2048u          /* playback resampler flush granularity (frames) */
#define PLAY_MAX_FRAMES 8192 /* cap for playback-thread mix/write chunk */

/* Buffer sizes computed from ALSA parameters after connection */
static uint32_t BUF_MAXLENGTH;
static uint32_t BUF_TLENGTH;
static uint32_t BUF_PREBUF;
static uint32_t BUF_MINREQ;

/* ======================================================================
 * Minimal lock-free SPSC ring buffer
 *
 * Drop-in replacement for the libjack ringbuffer API used by the protocol
 * code, so the handlers below stay byte-for-byte identical to the JACK
 * variant.  One producer (main poll thread, handle_audio) and one consumer
 * (the ALSA playback thread) per ring.
 * ====================================================================== */

typedef struct {
    char           *buf;
    size_t          size;   /* power of two */
    size_t          mask;   /* size - 1 */
    volatile size_t w;      /* producer writes, consumer reads */
    volatile size_t r;      /* consumer writes, producer reads */
} ring_t;

static ring_t *ring_create(size_t sz) {
    size_t p = 1;
    while (p < sz) p <<= 1;
    ring_t *rb = calloc(1, sizeof(*rb));
    if (!rb) return NULL;
    rb->buf = calloc(1, p);
    if (!rb->buf) { free(rb); return NULL; }
    rb->size = p;
    rb->mask = p - 1;
    rb->w = rb->r = 0;
    return rb;
}

static void ring_free(ring_t *rb) {
    if (rb) { free(rb->buf); free(rb); }
}

static void ring_reset(ring_t *rb) {
    /* Store w first, then fence, then r. play_loop reads r before w
     * (ring_read_space = (w - r) & mask), so writing r last ensures the
     * consumer never sees a transiently large available-byte count. */
    rb->w = 0;
    __sync_synchronize();
    rb->r = 0;
}

static int ring_mlock(ring_t *rb) {
    return mlock(rb->buf, rb->size);
}

static size_t ring_read_space(const ring_t *rb) {
    return (rb->w - rb->r) & rb->mask;
}

static size_t ring_write_space(const ring_t *rb) {
    return (rb->size - 1) - ring_read_space(rb);
}

static size_t ring_write(ring_t *rb, const char *src, size_t n) {
    size_t space = ring_write_space(rb);
    if (n > space) n = space;
    size_t w = rb->w & rb->mask;
    size_t first = rb->size - w;
    if (first > n) first = n;
    memcpy(rb->buf + w, src, first);
    memcpy(rb->buf, src + first, n - first);
    __sync_synchronize();               /* publish data before advancing index */
    rb->w = (rb->w + n) & rb->mask;
    return n;
}

static size_t ring_read(ring_t *rb, char *dst, size_t n) {
    size_t avail = ring_read_space(rb);
    if (n > avail) n = avail;
    size_t r = rb->r & rb->mask;
    size_t first = rb->size - r;
    if (first > n) first = n;
    memcpy(dst, rb->buf + r, first);
    memcpy(dst + first, rb->buf, n - first);
    __sync_synchronize();
    rb->r = (rb->r + n) & rb->mask;
    return n;
}

/* ======================================================================
 * ALSA state
 * ====================================================================== */

static snd_pcm_t        *g_pcm;
static unsigned int      g_rate;     /* negotiated playback rate (Hz) */
static snd_pcm_uframes_t g_period;   /* negotiated period size (frames) */
static snd_pcm_uframes_t g_bufsz;    /* negotiated buffer size (frames) */
static pthread_t         g_play_thread;
static int               g_thread_started;

/* ======================================================================
 * Output peak meter (published to the GUI via POSIX shared memory)
 * ====================================================================== */

static struct BridgePeak *g_peak;       /* NULL if shm setup failed (non-fatal) */
static int                g_peak_fd = -1;

/* Create + map the peak page. Failure is non-fatal: audio still works, the GUI
 * meter just stays idle. */
static void peak_open(void) {
    g_peak_fd = shm_open(BRIDGE_PEAK_SHM, O_CREAT | O_RDWR, 0600);
    if (g_peak_fd < 0) return;
    if (ftruncate(g_peak_fd, sizeof(struct BridgePeak)) < 0) {
        close(g_peak_fd);
        g_peak_fd = -1;
        return;
    }
    void *p = mmap(NULL, sizeof(struct BridgePeak), PROT_READ | PROT_WRITE,
                   MAP_SHARED, g_peak_fd, 0);
    if (p == MAP_FAILED) {
        close(g_peak_fd);
        g_peak_fd = -1;
        return;
    }
    g_peak = p;
    memset(g_peak, 0, sizeof(*g_peak));
    g_peak->rate = g_rate;
    mlock(g_peak, sizeof(*g_peak));
}

static void peak_close(void) {
    if (g_peak) {
        munmap(g_peak, sizeof(*g_peak));
        g_peak = NULL;
    }
    if (g_peak_fd >= 0) {
        close(g_peak_fd);
        g_peak_fd = -1;
    }
    shm_unlink(BRIDGE_PEAK_SHM);
}

/* Publish one block's stereo peak (single writer, plain stores — RT-safe). */
static void peak_publish(float l, float r) {
    if (!g_peak) return;
    g_peak->peakL = l;
    g_peak->peakR = r;
    g_peak->seq++;
}

/* ======================================================================
 * Stream state  (shared: main thread writes, playback thread reads)
 * ====================================================================== */

typedef struct {
    volatile int active;  /* 1 = in use; set last on create, first on delete */
    int          client_idx;
    int          client_fd;
    uint8_t      format;      /* PA_SAMPLE_S16LE or PA_SAMPLE_FLOAT32LE */
    uint8_t      channels;    /* 1 or 2 */
    uint8_t      corked;      /* 1 = stream is corked (paused) */
    uint8_t      started;     /* 1 after PA_COMMAND_STARTED sent to client */
    uint8_t      muted;       /* per-stream mute (SET_SINK_INPUT_MUTE) */
    uint32_t     rate;        /* client-negotiated rate (Hz); resampled to g_rate */
    uint64_t     write_index; /* bytes accepted so far, returned in latency replies */

    /* per-stream software volume (SET_SINK_INPUT_VOLUME). volL/volR are the raw PA
     * cvolume values echoed back in introspection; gainL/gainR are the cached
     * linear multipliers (cubic mapping) applied by the playback-thread mix. */
    uint32_t     volL, volR;
    float        gainL, gainR;

    /* linear resampler state (rate -> g_rate), main thread only */
    double       rs_phase;    /* fractional position within the current segment */
    float        rs_prevL, rs_prevR;
} PaStream;

static PaStream  streams[MAX_STREAMS];
static ring_t   *rb_L[MAX_STREAMS];
static ring_t   *rb_R[MAX_STREAMS];

/* Main-thread-only scratch buffers for PCM conversion */
static float conv_L[CONV_FRAMES];
static float conv_R[CONV_FRAMES];

/* ----------------------------------------------------------------------
 * Record (monitor capture) streams
 *
 * The playback thread tees its mixed output into rb (producer); the main
 * thread drains rb and pushes PA memblocks to the client (consumer) — the
 * mirror image of the playback path. The ring is the only cross-thread
 * state; the resampler fields are playback-thread-only, the tx fields are
 * main-thread-only.
 * ---------------------------------------------------------------------- */
typedef struct {
    volatile int active;     /* set last on create, cleared first on delete */
    int          client_idx;
    int          client_fd;
    uint8_t      format;     /* PA_SAMPLE_S16LE or PA_SAMPLE_FLOAT32LE */
    uint8_t      channels;   /* 1 or 2 (client-negotiated) */
    uint8_t      corked;     /* 1 = paused: stop teeing audio */
    uint32_t     rate;       /* client-negotiated rate (Hz) */
    uint32_t     fragsize;   /* client's preferred fragment size (bytes) */
    ring_t      *rb;         /* client-format PCM bytes, playback→main */

    /* linear resampler state (g_rate -> rate), playback thread only */
    double       phase;      /* fractional position within the current segment */
    float        prevL, prevR;

    /* outbound packet assembly, main thread only */
    uint8_t     *tx;         /* HDR_LEN + REC_TX_CHUNK */
    uint32_t     tx_len;     /* bytes queued in tx (0 = idle) */
    uint32_t     tx_sent;    /* bytes already written to the socket */
    uint64_t     delivered;  /* total PCM bytes pushed to the client (read index) */
} RecordStream;

static RecordStream rstreams[MAX_RECORD_STREAMS];

/* ======================================================================
 * Client receive state
 * ====================================================================== */

typedef struct {
    int      fd;
    int      active;
    uint32_t proto;          /* negotiated protocol version */

    /* Header accumulation */
    uint8_t  hdr[HDR_LEN];
    int      hdr_pos;

    /* Payload accumulation */
    uint8_t  *payload;
    uint32_t  payload_cap;
    uint32_t  payload_len;   /* expected byte count */
    uint32_t  payload_pos;   /* received so far */
    uint32_t  channel;       /* parsed from header */
    int64_t   seek_offset;   /* parsed from header: signed byte offset for this packet */
    uint8_t   seek_mode;     /* parsed from header: pa_seek_mode_t (flags & 0xFF) */
} Client;

static Client clients[MAX_CLIENTS];

/* ======================================================================
 * Signal / shutdown
 * ====================================================================== */

static volatile int g_running = 1;
static char         g_socket_path[512];
static int          g_debug;   /* set once at startup from PULSE_BRIDGE_DEBUG env var */

/* Live output-device switch: the GUI writes the new ALSA device string to
 * g_ctl_path and sends SIGUSR1. The playback thread reopens ALSA in place, so
 * the PA socket and all client streams stay connected (no app restart). */
static char                  g_ctl_path[512];  /* from PULSE_BRIDGE_CTL_FILE env */
static volatile sig_atomic_t g_reopen = 0;

static void sighandler(int s) { (void)s; g_running = 0; }
static void reopenhandler(int s) { (void)s; g_reopen = 1; }

/* Defined in the ALSA section below; used by the playback thread above it. */
static void alsa_reopen(const char *dev);
static int  read_ctl_device(char *out, size_t outsz);

/* ======================================================================
 * Tagstruct write helpers
 * ====================================================================== */

typedef struct { uint8_t data[TS_BUF]; int pos; } TsW;

static void tw_u32(TsW *t, uint32_t v) {
    t->data[t->pos++] = PA_TAG_U32;
    t->data[t->pos++] = (v >> 24) & 0xFF;
    t->data[t->pos++] = (v >> 16) & 0xFF;
    t->data[t->pos++] = (v >>  8) & 0xFF;
    t->data[t->pos++] =  v        & 0xFF;
}

static void tw_str(TsW *t, const char *s) {
    if (!s) { t->data[t->pos++] = PA_TAG_STRING_NULL; return; }
    t->data[t->pos++] = PA_TAG_STRING;
    size_t n = strlen(s);
    memcpy(t->data + t->pos, s, n + 1);
    t->pos += (int)(n + 1);
}

static void tw_bool(TsW *t, int v) {
    t->data[t->pos++] = (uint8_t)(v ? PA_TAG_BOOL_TRUE : PA_TAG_BOOL_FALSE);
}

static void tw_usec(TsW *t, uint64_t v) {
    t->data[t->pos++] = PA_TAG_USEC;
    t->data[t->pos++] = (v >> 56) & 0xFF;
    t->data[t->pos++] = (v >> 48) & 0xFF;
    t->data[t->pos++] = (v >> 40) & 0xFF;
    t->data[t->pos++] = (v >> 32) & 0xFF;
    t->data[t->pos++] = (v >> 24) & 0xFF;
    t->data[t->pos++] = (v >> 16) & 0xFF;
    t->data[t->pos++] = (v >>  8) & 0xFF;
    t->data[t->pos++] =  v        & 0xFF;
}

static void tw_sample_spec(TsW *t, uint8_t fmt, uint8_t ch, uint32_t rate) {
    t->data[t->pos++] = PA_TAG_SAMPLE_SPEC;
    t->data[t->pos++] = fmt;
    t->data[t->pos++] = ch;
    t->data[t->pos++] = (rate >> 24) & 0xFF;
    t->data[t->pos++] = (rate >> 16) & 0xFF;
    t->data[t->pos++] = (rate >>  8) & 0xFF;
    t->data[t->pos++] =  rate        & 0xFF;
}

static void tw_channel_map(TsW *t, uint8_t ch) {
    t->data[t->pos++] = PA_TAG_CHANNEL_MAP;
    t->data[t->pos++] = ch;
    if (ch == 1) {
        t->data[t->pos++] = PA_CHAN_MONO;
    } else {
        t->data[t->pos++] = PA_CHAN_FL;
        t->data[t->pos++] = PA_CHAN_FR;
    }
}

/* ======================================================================
 * Tagstruct read helpers
 * ====================================================================== */

typedef struct { const uint8_t *data; uint32_t len; uint32_t pos; } TsR;

#define TR_NEED(t, n) if ((t)->pos + (uint32_t)(n) > (t)->len) return -1

static int tr_u32(TsR *t, uint32_t *v) {
    TR_NEED(t, 5);
    if (t->data[t->pos++] != PA_TAG_U32) return -1;
    *v  = (uint32_t)t->data[t->pos++] << 24;
    *v |= (uint32_t)t->data[t->pos++] << 16;
    *v |= (uint32_t)t->data[t->pos++] <<  8;
    *v |= (uint32_t)t->data[t->pos++];
    return 0;
}

static int tr_sample_spec(TsR *t, uint8_t *fmt, uint8_t *ch, uint32_t *rate) {
    TR_NEED(t, 7);
    if (t->data[t->pos++] != PA_TAG_SAMPLE_SPEC) return -1;
    *fmt  = t->data[t->pos++];
    *ch   = t->data[t->pos++];
    *rate  = (uint32_t)t->data[t->pos++] << 24;
    *rate |= (uint32_t)t->data[t->pos++] << 16;
    *rate |= (uint32_t)t->data[t->pos++] <<  8;
    *rate |= (uint32_t)t->data[t->pos++];
    return 0;
}

/* Read a channel map, returning the channel count. */
static int tr_channel_map(TsR *t, uint8_t *nch) {
    TR_NEED(t, 2);
    if (t->data[t->pos++] != PA_TAG_CHANNEL_MAP) return -1;
    uint8_t n = t->data[t->pos++];
    TR_NEED(t, n);
    t->pos += n;
    *nch = n;
    return 0;
}

/* Read a cvolume into per-channel left/right raw PA volume values (mono is
 * mirrored to both). Returns 0 on success. */
static int tr_cvolume(TsR *t, uint32_t *vL, uint32_t *vR) {
    TR_NEED(t, 2);
    if (t->data[t->pos++] != PA_TAG_CVOLUME) return -1;
    uint8_t n = t->data[t->pos++];
    if (n < 1) return -1;
    TR_NEED(t, (uint32_t)n * 4u);
    uint32_t v[2] = { PA_VOLUME_NORM, PA_VOLUME_NORM };
    for (uint8_t i = 0; i < n; i++) {
        uint32_t x = (uint32_t)t->data[t->pos]   << 24 |
                     (uint32_t)t->data[t->pos+1] << 16 |
                     (uint32_t)t->data[t->pos+2] <<  8 |
                     (uint32_t)t->data[t->pos+3];
        t->pos += 4;
        if (i < 2) v[i] = x;
    }
    *vL = v[0];
    *vR = (n >= 2) ? v[1] : v[0];
    return 0;
}

/* Map a raw PA volume to a linear gain. PulseAudio software volume is cubic:
 * gain = (v / PA_VOLUME_NORM)^3, so PA_VOLUME_NORM is unity. */
static float vol_to_gain(uint32_t v) {
    float f = (float)v / (float)PA_VOLUME_NORM;
    return f * f * f;
}

/* Skip one complete tagged value of any type */
static int tr_skip(TsR *t) __attribute__((unused));
static int tr_skip(TsR *t) {
    TR_NEED(t, 1);
    uint8_t tag = t->data[t->pos++];
    switch (tag) {
    case PA_TAG_U32:
        TR_NEED(t, 4); t->pos += 4; return 0;
    case PA_TAG_U8:
        TR_NEED(t, 1); t->pos++; return 0;
    case 'R': case 'r': case PA_TAG_USEC: case PA_TAG_TIMEVAL:
        TR_NEED(t, 8); t->pos += 8; return 0;
    case PA_TAG_BOOL_TRUE: case PA_TAG_BOOL_FALSE:
        return 0;
    case PA_TAG_STRING_NULL:
        return 0;
    case PA_TAG_STRING:
        while (t->pos < t->len && t->data[t->pos]) t->pos++;
        if (t->pos >= t->len) return -1;
        t->pos++;
        return 0;
    case PA_TAG_SAMPLE_SPEC:
        TR_NEED(t, 6); t->pos += 6; return 0;
    case PA_TAG_ARBITRARY: {
        TR_NEED(t, 4);
        uint32_t n = (uint32_t)t->data[t->pos  ] << 24 |
                     (uint32_t)t->data[t->pos+1] << 16 |
                     (uint32_t)t->data[t->pos+2] <<  8 |
                     (uint32_t)t->data[t->pos+3];
        t->pos += 4;
        TR_NEED(t, n); t->pos += n;
        return 0;
    }
    case PA_TAG_CHANNEL_MAP: {
        TR_NEED(t, 1);
        uint8_t n = t->data[t->pos++];
        TR_NEED(t, n); t->pos += n;
        return 0;
    }
    case PA_TAG_CVOLUME: {
        TR_NEED(t, 1);
        uint8_t n = t->data[t->pos++];
        TR_NEED(t, (uint32_t)n * 4u); t->pos += (uint32_t)n * 4u;
        return 0;
    }
    case PA_TAG_VOLUME:
        TR_NEED(t, 4); t->pos += 4; return 0;
    case PA_TAG_PROPLIST:
        /* Each entry is key(string) + value-length(u32) + value(arbitrary),
         * terminated by a string-null. Must match pa_tagstruct_put_proplist
         * exactly (three tags per entry) or the surrounding walk desyncs. */
        while (t->pos < t->len) {
            TR_NEED(t, 1);
            if (t->data[t->pos] == PA_TAG_STRING_NULL) { t->pos++; return 0; }
            if (tr_skip(t) < 0) return -1;  /* key (string)         */
            if (tr_skip(t) < 0) return -1;  /* value length (u32)   */
            if (tr_skip(t) < 0) return -1;  /* value (arbitrary)    */
        }
        return -1;
    default:
        return -1;
    }
}

/* Read a tagged string from a tagstruct; *s points into data (no copy).
 * Returns 0 on success, -1 on parse error. */
static int tr_str(TsR *t, const char **s) {
    TR_NEED(t, 1);
    uint8_t tag = t->data[t->pos];
    if (tag == PA_TAG_STRING_NULL) { t->pos++; *s = NULL; return 0; }
    if (tag != PA_TAG_STRING) return -1;
    t->pos++;
    *s = (const char *)(t->data + t->pos);
    while (t->pos < t->len && t->data[t->pos]) t->pos++;
    if (t->pos >= t->len) return -1;
    t->pos++;  /* consume null terminator */
    return 0;
}

/* Parse a PCM format_info at the cursor, updating sample format / channel
 * count / rate from its property list. Clients using the extended format API
 * (e.g. VLC) send an INVALID sample_spec and an empty channel map, carrying the
 * real format only here. Property values are JSON stored as arbitrary blobs:
 * strings are quoted ("s16le"), integers are bare ("48000"); each blob is
 * NUL-terminated. Returns 0 if a format_info was parsed. */
static int tr_format_info(TsR *t, uint8_t *fmt, uint8_t *ch, uint32_t *rate) {
    TR_NEED(t, 2);
    if (t->data[t->pos++] != PA_TAG_FORMAT_INFO) return -1;
    if (t->data[t->pos++] != PA_TAG_U8) return -1;
    TR_NEED(t, 1);
    t->pos++;                                          /* encoding (1 = PCM) */
    TR_NEED(t, 1);
    if (t->data[t->pos++] != PA_TAG_PROPLIST) return -1;
    for (;;) {
        TR_NEED(t, 1);
        if (t->data[t->pos] == PA_TAG_STRING_NULL) { t->pos++; break; }
        const char *key = NULL;
        if (tr_str(t, &key) < 0 || !key) return -1;
        uint32_t vlen = 0;
        if (tr_u32(t, &vlen) < 0) return -1;           /* value byte length */
        TR_NEED(t, 5);
        if (t->data[t->pos++] != PA_TAG_ARBITRARY) return -1;
        uint32_t alen = (uint32_t)t->data[t->pos]   << 24 |
                        (uint32_t)t->data[t->pos+1] << 16 |
                        (uint32_t)t->data[t->pos+2] <<  8 |
                        (uint32_t)t->data[t->pos+3];
        t->pos += 4;
        TR_NEED(t, alen);
        const char *val = (const char *)(t->data + t->pos);  /* NUL-terminated */
        t->pos += alen;
        if (g_debug)
            fprintf(stderr, "pulse-alsa-bridge:   format_info[%s] = %s\n", key, val);
        if (strcmp(key, "format.sample_format") == 0) {
            /* Check the longer/more specific names first so substrings don't
             * misfire (e.g. "s24-32le" must not match an "s24le" test). */
            if (strstr(val, "s16le"))          *fmt = (uint8_t)PA_SAMPLE_S16LE;
            else if (strstr(val, "s24-32le"))  *fmt = (uint8_t)PA_SAMPLE_S24_32LE;
            else if (strstr(val, "s32le"))     *fmt = (uint8_t)PA_SAMPLE_S32LE;
            else if (strstr(val, "s24le"))     *fmt = (uint8_t)PA_SAMPLE_S24LE;
            else if (strstr(val, "float32le")) *fmt = (uint8_t)PA_SAMPLE_FLOAT32LE;
        } else if (strcmp(key, "format.channels") == 0) {
            int c = atoi(val);
            if (c >= 1) *ch = (uint8_t)c;
        } else if (strcmp(key, "format.rate") == 0) {
            int rr = atoi(val);
            if (rr > 0) *rate = (uint32_t)rr;
        }
    }
    return 0;
}

/* ======================================================================
 * Packet send helpers
 * ====================================================================== */

static int send_packet(int fd, uint32_t channel, const uint8_t *data, uint32_t len) {
    uint8_t hdr[HDR_LEN];
    uint32_t *h = (uint32_t *)(void *)hdr;
    h[0] = htonl(len);
    h[1] = htonl(channel);
    h[2] = 0; h[3] = 0; h[4] = 0;
    if (write(fd, hdr, HDR_LEN) != HDR_LEN) return -1;
    if (len > 0 && write(fd, data, len) != (ssize_t)len) return -1;
    return 0;
}

static int send_cmd(int fd, TsW *ts) {
    return send_packet(fd, PA_CONTROL_CHANNEL, ts->data, (uint32_t)ts->pos);
}

static int send_reply_empty(int fd, uint32_t tag) {
    TsW ts = {0};
    tw_u32(&ts, PA_CMD_REPLY);
    tw_u32(&ts, tag);
    return send_cmd(fd, &ts);
}

static int send_error(int fd, uint32_t tag, uint32_t err) {
    TsW ts = {0};
    tw_u32(&ts, PA_CMD_ERROR);
    tw_u32(&ts, tag);
    tw_u32(&ts, err);
    return send_cmd(fd, &ts);
}

static int send_request(int fd, uint32_t stream_idx, uint32_t nbytes) {
    TsW ts = {0};
    tw_u32(&ts, PA_CMD_REQUEST);
    tw_u32(&ts, 0xFFFFFFFFu);  /* server-initiated: no sequence number */
    tw_u32(&ts, stream_idx);
    tw_u32(&ts, nbytes);
    return send_cmd(fd, &ts);
}

/* ======================================================================
 * Sample format helpers (all input is converted to float for mixing)
 * ====================================================================== */

/* Bytes per single-channel sample for a supported PA format. */
static uint32_t fmt_bytes(uint8_t fmt) {
    switch (fmt) {
    case PA_SAMPLE_S16LE:    return 2u;
    case PA_SAMPLE_S24LE:    return 3u;
    case PA_SAMPLE_S32LE:
    case PA_SAMPLE_S24_32LE:
    case PA_SAMPLE_FLOAT32LE: return 4u;
    default:                 return 4u;
    }
}

static int fmt_supported(uint8_t fmt) {
    return fmt == PA_SAMPLE_S16LE || fmt == PA_SAMPLE_FLOAT32LE ||
           fmt == PA_SAMPLE_S32LE || fmt == PA_SAMPLE_S24LE ||
           fmt == PA_SAMPLE_S24_32LE;
}

/* Decode one little-endian sample at p to a normalised float in [-1, 1]. */
static float decode_sample(uint8_t fmt, const uint8_t *p) {
    switch (fmt) {
    case PA_SAMPLE_S16LE: {
        int16_t v = 0;
        memcpy(&v, p, 2);
        return v / 32768.0f;
    }
    case PA_SAMPLE_S24LE:
    case PA_SAMPLE_S24_32LE: {
        /* 24-bit value in the low 3 bytes; sign-extend from bit 23. */
        int32_t v = (int32_t)((uint32_t)p[0] | (uint32_t)p[1] << 8 |
                              (uint32_t)p[2] << 16);
        if (v & 0x00800000) v |= (int32_t)0xFF000000;
        return v / 8388608.0f;
    }
    case PA_SAMPLE_S32LE: {
        int32_t v = 0;
        memcpy(&v, p, 4);
        return v / 2147483648.0f;
    }
    default: {  /* PA_SAMPLE_FLOAT32LE */
        float v = 0.0f;
        memcpy(&v, p, 4);
        return v;
    }
    }
}

/* ======================================================================
 * Stream allocation
 * ====================================================================== */

static int alloc_stream(void) {
    for (int i = 0; i < MAX_STREAMS; i++)
        if (!streams[i].active) return i;
    return -1;
}

static int alloc_record_stream(void) {
    for (int i = 0; i < MAX_RECORD_STREAMS; i++)
        if (!rstreams[i].active) return i;
    return -1;
}

/* ----------------------------------------------------------------------
 * Shared tagstruct writers for the introspection descriptors below.
 * ---------------------------------------------------------------------- */

/* cvolume at PA_VOLUME_NORM (0x00010000) for every channel. */
static void tw_cvolume_norm(TsW *t, uint8_t ch) {
    t->data[t->pos++] = PA_TAG_CVOLUME;
    t->data[t->pos++] = ch;
    for (uint8_t i = 0; i < ch; i++) {
        t->data[t->pos++] = 0x00; t->data[t->pos++] = 0x01;
        t->data[t->pos++] = 0x00; t->data[t->pos++] = 0x00;
    }
}

/* A single PCM format_info (encoding=1) with an empty proplist. */
static void tw_format_info_pcm(TsW *t) {
    t->data[t->pos++] = PA_TAG_FORMAT_INFO;   /* 'f' */
    t->data[t->pos++] = PA_TAG_U8;            /* 'B' */
    t->data[t->pos++] = 1u;                   /* PA_ENCODING_PCM = 1 */
    t->data[t->pos++] = PA_TAG_PROPLIST;      /* 'P' */
    t->data[t->pos++] = PA_TAG_STRING_NULL;   /* 'N' empty proplist */
}

/* An empty proplist ('P' immediately followed by the string-null terminator). */
static void tw_proplist_empty(TsW *t) {
    t->data[t->pos++] = PA_TAG_PROPLIST;
    t->data[t->pos++] = PA_TAG_STRING_NULL;
}

/* ======================================================================
 * Command handlers
 * ====================================================================== */

static void cmd_auth(Client *cl, TsR *ts, uint32_t tag) {
    uint32_t version = 8u;
    tr_u32(ts, &version);  /* client protocol version */
    if (version > PA_PROTOCOL_VERSION) version = PA_PROTOCOL_VERSION;
    cl->proto = version;

    TsW rep = {0};
    tw_u32(&rep, PA_CMD_REPLY);
    tw_u32(&rep, tag);
    tw_u32(&rep, version);
    send_cmd(cl->fd, &rep);
}

static void cmd_set_client_name(Client *cl, TsR *ts, uint32_t tag) {
    (void)ts;
    TsW rep = {0};
    tw_u32(&rep, PA_CMD_REPLY);
    tw_u32(&rep, tag);
    if (cl->proto >= 13u)
        tw_u32(&rep, 0u);  /* client index */
    send_cmd(cl->fd, &rep);
}

static void cmd_get_server_info(Client *cl, TsR *ts, uint32_t tag) {
    (void)ts;
    uint32_t rate = g_rate;
    TsW rep = {0};
    tw_u32(&rep, PA_CMD_REPLY);
    tw_u32(&rep, tag);
    tw_str(&rep, "pulse-alsa-bridge");
    tw_str(&rep, "17.0.0");       /* PA version string — matches proto=35 */
    tw_str(&rep, "alsa");
    tw_str(&rep, "localhost");
    tw_sample_spec(&rep, (uint8_t)PA_SAMPLE_S16LE, 2, rate);
    tw_str(&rep, "alsa_sink");
    tw_str(&rep, "alsa_source");
    tw_u32(&rep, 0xDEADBEEFu);    /* cookie (any value) */
    if (cl->proto >= 15u)
        tw_channel_map(&rep, 2);
    send_cmd(cl->fd, &rep);
}

static void cmd_create_playback_stream(Client *cl, TsR *ts, uint32_t tag) {
    uint8_t  fmt = (uint8_t)PA_SAMPLE_S16LE;
    uint8_t  ch  = 2;
    uint32_t rate = g_rate;

    if (tr_sample_spec(ts, &fmt, &ch, &rate) < 0) {
        send_error(cl->fd, tag, PA_ERR_NOTSUPPORTED);
        return;
    }
    /* Extended format API (e.g. VLC's vlcpulse): the request carries an INVALID
     * sample_spec and an empty channel map, with the real PCM format only in a
     * format_info list at the tail. We recover it below. A valid but unsupported
     * format is rejected so the client can renegotiate (e.g. FIX_FORMAT). */
    int fmt_api = (fmt == PA_SAMPLE_INVALID);
    if (!fmt_api && !fmt_supported(fmt)) {
        send_error(cl->fd, tag, PA_ERR_NOTSUPPORTED);
        return;
    }

    /* Read the PA_STREAM_START_CORKED flag from the request tagstruct.
     * Layout after sample_spec: channel_map, sink_index(u32), sink_name(str),
     * maxlength(u32), corked(bool).  At proto<13 there is a name string first,
     * but we only negotiate proto>=13 so that case is not reached. */
    int start_corked = 0;
    uint8_t map_ch = ch;
    if (cl->proto < 13u) tr_skip(ts);  /* name string — never reached at proto>=13 */
    if (tr_channel_map(ts, &map_ch) == 0 &&
        tr_skip(ts) == 0 &&   /* sink_index u32 */
        tr_skip(ts) == 0 &&   /* sink_name str */
        tr_skip(ts) == 0 &&   /* maxlength u32 */
        ts->pos < ts->len) {
        uint8_t b = ts->data[ts->pos++];
        start_corked = (b != PA_TAG_BOOL_FALSE);
    }

    /* Recover the real PCM format from the trailing format_info list (the
     * extended API). Walk the remaining buffer_attr and version-gated flag
     * fields to reach the n_formats count, then parse the first format. If
     * anything is unparseable, fall back to our canonical stereo format. */
    if (fmt_api) {
        fmt = (uint8_t)PA_SAMPLE_FLOAT32LE;
        ch  = 2;
        tr_skip(ts);                       /* tlength  */
        tr_skip(ts);                       /* prebuf   */
        tr_skip(ts);                       /* minreq   */
        tr_skip(ts);                       /* syncid   */
        tr_skip(ts);                       /* cvolume  */
        /* proto>=12: no_remap, no_remix, fix_format, fix_rate, fix_channels, no_move, variable_rate */
        if (cl->proto >= 12u) { for (int i = 0; i < 7; i++) tr_skip(ts); }
        if (cl->proto >= 13u) { tr_skip(ts); tr_skip(ts); tr_skip(ts); }   /* muted, adjust_latency, proplist */
        if (cl->proto >= 14u) { tr_skip(ts); tr_skip(ts); }                /* volume_set, early_requests */
        if (cl->proto >= 15u) { tr_skip(ts); tr_skip(ts); tr_skip(ts); }   /* muted_set, dont_inhibit, fail_on_suspend */
        if (cl->proto >= 17u) tr_skip(ts);                                 /* relative_volume */
        if (cl->proto >= 18u) tr_skip(ts);                                 /* passthrough */
        if (cl->proto >= 21u && ts->pos + 1 < ts->len &&
            ts->data[ts->pos] == PA_TAG_U8) {
            uint8_t n_formats = ts->data[ts->pos + 1];
            ts->pos += 2;
            if (n_formats >= 1)
                tr_format_info(ts, &fmt, &ch, &rate);
        }
        if (!fmt_supported(fmt)) {
            if (g_debug)
                fprintf(stderr, "pulse-alsa-bridge: create: UNSUPPORTED format %u → FLOAT32LE fallback (likely wrong)\n", fmt);
            fmt = (uint8_t)PA_SAMPLE_FLOAT32LE;
        }
    }
    if (ch < 1 || ch > 2) ch = 2;
    /* Honour the client's rate; the bridge resamples it to the device rate
     * (play_feed). Guard against an absent/absurd value from the format API. */
    if (rate < 4000u || rate > 192000u) rate = g_rate;

    int slot = alloc_stream();
    if (slot < 0) {
        send_error(cl->fd, tag, PA_ERR_INTERNAL);
        return;
    }

    /* Reset ring buffers before activating the slot */
    ring_reset(rb_L[slot]);
    ring_reset(rb_R[slot]);

    streams[slot].client_idx  = (int)(cl - clients);
    streams[slot].client_fd   = cl->fd;
    streams[slot].format      = fmt;
    streams[slot].channels    = ch;
    streams[slot].corked      = (uint8_t)start_corked;
    streams[slot].started     = 0;
    streams[slot].muted       = 0;
    streams[slot].rate        = rate;
    streams[slot].write_index = 0;
    streams[slot].volL        = PA_VOLUME_NORM;
    streams[slot].volR        = PA_VOLUME_NORM;
    streams[slot].gainL       = 1.0f;
    streams[slot].gainR       = 1.0f;
    streams[slot].rs_phase    = 0.0;
    streams[slot].rs_prevL    = 0.0f;
    streams[slot].rs_prevR    = 0.0f;
    __sync_synchronize();           /* publish slot fields before active=1 */
    streams[slot].active      = 1;  /* last: makes slot visible to playback thread */

    uint8_t  reply_ch  = (ch < 2) ? 1 : 2;

    TsW rep = {0};
    tw_u32(&rep, PA_CMD_REPLY);
    tw_u32(&rep, tag);
    tw_u32(&rep, (uint32_t)slot);  /* stream index */
    tw_u32(&rep, 0u);              /* sink input index */
    tw_u32(&rep, BUF_TLENGTH);     /* missing: client may send this many bytes now */
    if (cl->proto >= 9u) {
        tw_u32(&rep, BUF_MAXLENGTH);
        tw_u32(&rep, BUF_TLENGTH);
        tw_u32(&rep, BUF_PREBUF);
        tw_u32(&rep, BUF_MINREQ);
    }
    if (cl->proto >= 12u) {
        tw_sample_spec(&rep, fmt, reply_ch, rate);
        tw_channel_map(&rep, reply_ch);
        tw_u32(&rep, 0u);           /* sink index */
        tw_str(&rep, "alsa_sink");
        tw_bool(&rep, 0);           /* not suspended */
    }
    if (cl->proto >= 13u) {
        tw_usec(&rep, 0u);          /* configured_sink_latency */
    }
    if (cl->proto >= 21u) {
        /* Single format_info (no count) — verified: protocol-native.c line 2113-2121.
         * Extended-API games (n_formats > 0) call pa_context_fail(PA_ERR_PROTOCOL) if absent. */
        rep.data[rep.pos++] = PA_TAG_FORMAT_INFO;   /* 'f' */
        rep.data[rep.pos++] = PA_TAG_U8;            /* 'B' */
        rep.data[rep.pos++] = 1u;                   /* PA_ENCODING_PCM = 1 */
        rep.data[rep.pos++] = PA_TAG_PROPLIST;      /* 'P' */
        rep.data[rep.pos++] = PA_TAG_STRING_NULL;   /* 'N' empty proplist */
    }
    send_cmd(cl->fd, &rep);

    if (g_debug)
        fprintf(stderr, "pulse-alsa-bridge: stream %d created: fmt=%u ch=%u rate=%u proto=%u corked=%d\n",
                slot, fmt, ch, rate, cl->proto, start_corked);
    /* The 'missing' field in the CREATE reply IS the initial write grant (= BUF_TLENGTH).
     * Real PA sends no separate REQUEST after CREATE — protocol-native.c line 2081. */
}

static void cmd_delete_playback_stream(Client *cl, TsR *ts, uint32_t tag) {
    uint32_t idx = 0;
    if (tr_u32(ts, &idx) == 0 && idx < MAX_STREAMS) {
        int ci = (int)(cl - clients);
        if (streams[idx].active && streams[idx].client_idx == ci) {
            size_t rb_l = ring_read_space(rb_L[idx]);
            size_t rb_r = ring_read_space(rb_R[idx]);
            if (rb_l > 0 || rb_r > 0)
                streams[idx].client_fd = -1;  /* drain: let ALSA play the buffered audio */
            else
                streams[idx].active = 0;
        }
    }
    send_reply_empty(cl->fd, tag);
}

/* ======================================================================
 * Playback resampler (client rate -> device rate)
 * ====================================================================== */

/* Linear-resample a stream's float L/R block from the client's rate to the
 * device rate (g_rate) and append it to the stream's ring buffers. When the
 * rates already match this is a straight copy. Runs on the main thread only;
 * the resampler phase/prev carry across calls for cross-block continuity.
 * (Mirror of rec_feed on the capture side.) */
static void play_feed(PaStream *s, int slot, const float *inL, const float *inR, uint32_t N) {
    if (s->rate == g_rate) {
        ring_write(rb_L[slot], (const char *)inL, N * sizeof(float));
        ring_write(rb_R[slot], (const char *)inR, N * sizeof(float));
        return;
    }

    float    oL[RS_CHUNK], oR[RS_CHUNK];
    uint32_t o = 0;
    const double step = (double)s->rate / (double)g_rate; /* input frames / output frame */
    double ph = s->rs_phase;
    float  pL = s->rs_prevL, pR = s->rs_prevR;

    for (uint32_t n = 0; n < N; n++) {
        float cL = inL[n], cR = inR[n];
        while (ph < 1.0) {
            float f = (float)ph;
            oL[o] = pL + (cL - pL) * f;
            oR[o] = pR + (cR - pR) * f;
            o++;
            if (o == RS_CHUNK) {
                ring_write(rb_L[slot], (const char *)oL, o * sizeof(float));
                ring_write(rb_R[slot], (const char *)oR, o * sizeof(float));
                o = 0;
            }
            ph += step;
        }
        ph -= 1.0;
        pL = cL; pR = cR;
    }
    if (o) {
        ring_write(rb_L[slot], (const char *)oL, o * sizeof(float));
        ring_write(rb_R[slot], (const char *)oR, o * sizeof(float));
    }
    s->rs_phase = ph;
    s->rs_prevL = pL;
    s->rs_prevR = pR;
}

/* ======================================================================
 * Audio data handler (called when channel != PA_CONTROL_CHANNEL)
 * ====================================================================== */

/* Client-byte read index = write_index minus what is still buffered in the ring.
 * The ring holds device-rate (g_rate) float frames, so scale back to the client's
 * rate and format. Shared by handle_audio (seek reconciliation) and
 * cmd_get_playback_latency (timing reply). */
static uint64_t stream_read_index(const PaStream *s, uint32_t channel) {
    uint32_t bps = fmt_bytes(s->format);
    size_t rb_floats = ring_read_space(rb_L[channel]) / sizeof(float);
    uint64_t client_frames = (g_rate != 0)
        ? (uint64_t)rb_floats * s->rate / g_rate
        : (uint64_t)rb_floats;
    uint64_t unread = client_frames * bps * (uint32_t)s->channels;
    return (s->write_index > unread) ? s->write_index - unread : 0;
}

static void handle_audio(uint32_t channel, const uint8_t *data, uint32_t len,
                         int64_t seek_offset, uint8_t seek_mode) {
    if (channel >= MAX_STREAMS || !streams[channel].active) return;
    PaStream *s = &streams[channel];

    uint32_t bps         = fmt_bytes(s->format);
    uint32_t frame_bytes = bps * s->channels;
    if (frame_bytes == 0) return;

    /* ---- Timeline seek reconciliation ----------------------------------------
     * A media player seeking the timeline repositions its write index via the
     * packet's seek mode + offset and debits its write-credit accordingly. We must
     * mirror that here so our write_index stays in lockstep with the client (else
     * REQUEST credit desyncs and the client stops writing → stall). Resolve the
     * absolute client-byte position this packet targets; if it is not the current
     * queue end, it is a real seek. */
    uint64_t queue_end = s->write_index;
    int64_t  target;
    switch (seek_mode) {
    case 1:  /* PA_SEEK_ABSOLUTE: absolute byte index */
        target = seek_offset; break;
    case 2:  /* PA_SEEK_RELATIVE_ON_READ: relative to read index */
    case 3:  /* PA_SEEK_RELATIVE_END: we have no queue end distinct from read+buffered,
              *  so anchor on read index (rare from VLC/Parole; safe either way) */
        target = (int64_t)stream_read_index(s, channel) + seek_offset; break;
    case 0:  /* PA_SEEK_RELATIVE: relative to queue end (offset 0 = normal append) */
    default:
        target = (int64_t)queue_end + seek_offset; break;
    }
    if (target < 0) target = 0;

    int repositioned = ((uint64_t)target != queue_end);
    if (repositioned) {
        /* The ring is strictly SPSC; we cannot rewind over data the playback thread
         * may already have consumed. The only safe reposition is to discard the
         * buffered-but-unplayed audio and re-anchor write_index to the seek target —
         * exactly what FLUSH does. Forward gaps are dropped (no silence fill): the
         * player's clock is driven by write_index, which we set exactly. */
        if (g_debug)
            fprintf(stderr,
                "pulse-alsa-bridge: stream %u: SEEK mode=%u off=%lld queue_end=%llu "
                "read_idx=%llu → reanchor write_index=%llu\n",
                channel, seek_mode, (long long)seek_offset,
                (unsigned long long)queue_end,
                (unsigned long long)stream_read_index(s, channel),
                (unsigned long long)target);
        ring_reset(rb_L[channel]);
        ring_reset(rb_R[channel]);
        s->rs_phase = 0.0;
        s->rs_prevL = 0.0f;
        s->rs_prevR = 0.0f;
        s->write_index = (uint64_t)target;
        s->started     = 0;   /* re-arm STARTED for the new position */
    }
    /* ------------------------------------------------------------------------- */

    uint32_t nframes = len / frame_bytes;
    if (nframes > CONV_FRAMES) nframes = CONV_FRAMES;
    if (nframes == 0) return;

    for (uint32_t i = 0; i < nframes; i++) {
        const uint8_t *f = data + i * frame_bytes;
        float l = decode_sample(s->format, f);
        float r = (s->channels >= 2) ? decode_sample(s->format, f + bps) : l;
        conv_L[i] = l;
        conv_R[i] = r;
    }

    play_feed(s, (int)channel, conv_L, conv_R, nframes);

    streams[channel].write_index += len;

    /* Send PA_COMMAND_STARTED once the prebuffer is full — but ONLY for uncorked streams.
     * For corked streams (PA_STREAM_START_CORKED), STARTED must be deferred until UNCORK:
     * sending it during silent prebuffer fill causes the client to enter an unexpected state
     * and disconnect instead of sending real audio. */
    if (!s->started && !s->corked &&
        s->write_index >= (uint64_t)BUF_PREBUF && s->client_fd != -1) {
        s->started = 1;
        if (g_debug)
            fprintf(stderr, "pulse-alsa-bridge: stream %u: prebuf satisfied → sending STARTED\n", channel);
        TsW ev = {0};
        tw_u32(&ev, 86u);           /* PA_COMMAND_STARTED (server→client) */
        tw_u32(&ev, 0xFFFFFFFFu);   /* unsolicited: no sequence number */
        tw_u32(&ev, channel);
        send_cmd(s->client_fd, &ev);
    }

    /* Request more audio when the ring is below the target fill level, OR
     * unconditionally right after a reposition. ring_target: 2 BUF_TLENGTH chunks
     * in ring-float-bytes per channel (~42 ms). The post-seek grant is the anti-stall
     * step: it pulls the client's (possibly negative) requested_bytes back above 0 so
     * its write callback fires again from the new anchor. */
    uint32_t bps_req = (s->format == PA_SAMPLE_S16LE) ? 2u : 4u;
    uint32_t ring_chunk = BUF_TLENGTH / (bps_req * (uint32_t)s->channels) * (uint32_t)sizeof(float);
    if (s->client_fd != -1 && !s->corked &&
        (repositioned || ring_read_space(rb_L[channel]) < ring_chunk * 2u))
        send_request(s->client_fd, channel, BUF_TLENGTH);
}

static void cmd_cork_playback_stream(Client *cl, TsR *ts, uint32_t tag) {
    uint32_t idx = 0;
    tr_u32(ts, &idx);
    int is_corked = 1;
    if (ts->pos < ts->len) {
        uint8_t t = ts->data[ts->pos++];
        is_corked = (t != PA_TAG_BOOL_FALSE);
    }
    if (idx < MAX_STREAMS && streams[idx].active)
        streams[idx].corked = (uint8_t)is_corked;
    send_reply_empty(cl->fd, tag);
    if (!is_corked && idx < MAX_STREAMS && streams[idx].active) {
        /* On uncork: send STARTED if not yet sent (corked streams defer STARTED to here),
         * then REQUEST so the client starts writing real audio. */
        if (!streams[idx].started) {
            streams[idx].started = 1;
            if (g_debug)
                fprintf(stderr, "pulse-alsa-bridge: stream %u: uncorked → sending STARTED\n", idx);
            TsW ev = {0};
            tw_u32(&ev, 86u);           /* PA_COMMAND_STARTED */
            tw_u32(&ev, 0xFFFFFFFFu);
            tw_u32(&ev, idx);
            send_cmd(cl->fd, &ev);
        }
        send_request(cl->fd, idx, BUF_TLENGTH);
    }
}

static void tw_u64(TsW *t, uint8_t tag, uint64_t v) {
    t->data[t->pos++] = tag;
    t->data[t->pos++] = (v >> 56) & 0xFF;
    t->data[t->pos++] = (v >> 48) & 0xFF;
    t->data[t->pos++] = (v >> 40) & 0xFF;
    t->data[t->pos++] = (v >> 32) & 0xFF;
    t->data[t->pos++] = (v >> 24) & 0xFF;
    t->data[t->pos++] = (v >> 16) & 0xFF;
    t->data[t->pos++] = (v >>  8) & 0xFF;
    t->data[t->pos++] =  v        & 0xFF;
}

static void tw_timeval(TsW *t, struct timeval *tv) {
    uint32_t sec  = (uint32_t)tv->tv_sec;
    uint32_t usec = (uint32_t)tv->tv_usec;
    t->data[t->pos++] = PA_TAG_TIMEVAL;
    t->data[t->pos++] = (sec  >> 24) & 0xFF;
    t->data[t->pos++] = (sec  >> 16) & 0xFF;
    t->data[t->pos++] = (sec  >>  8) & 0xFF;
    t->data[t->pos++] =  sec         & 0xFF;
    t->data[t->pos++] = (usec >> 24) & 0xFF;
    t->data[t->pos++] = (usec >> 16) & 0xFF;
    t->data[t->pos++] = (usec >>  8) & 0xFF;
    t->data[t->pos++] =  usec        & 0xFF;
}

static void cmd_get_playback_latency(Client *cl, TsR *ts, uint32_t tag) {
    /* Parse stream index; skip the local-time TIMEVAL the client sends */
    uint32_t idx = 0;
    tr_u32(ts, &idx);

    struct timeval tv = {0, 0};
    gettimeofday(&tv, NULL);

    uint64_t write_idx = 0, read_idx = 0;
    if (idx < MAX_STREAMS && streams[idx].active) {
        PaStream *s = &streams[idx];
        write_idx = s->write_index;
        read_idx  = stream_read_index(s, idx);
    }

    TsW rep = {0};
    tw_u32(&rep, PA_CMD_REPLY);
    tw_u32(&rep, tag);
    tw_usec(&rep, 0u);      /* sink latency */
    tw_usec(&rep, 0u);      /* source latency */
    tw_bool(&rep, !(idx < MAX_STREAMS && streams[idx].active && streams[idx].corked));
    tw_timeval(&rep, &tv);  /* remote_time (server's current time) */
    tw_timeval(&rep, &tv);  /* local_time  (echo of client's timestamp; we use now) */
    tw_u64(&rep, 'r', write_idx);  /* write_index: PA_TAG_S64 'r' — verified stream.c:gets64 */
    tw_u64(&rep, 'r', read_idx);   /* read_index:  PA_TAG_S64 'r' */
    /* proto >= 13 (playback only): underrun_for + playing_for, both PA_TAG_U64 'R'.
     * Verified: protocol-native.c:2916-2917, stream.c:1864-1865 getu64. */
    if (cl->proto >= 13u) {
        tw_u64(&rep, 'R', 0u);   /* underrun_for: frames spent underrunning (0 = none) */
        tw_u64(&rep, 'R', 0u);   /* playing_for: frames spent playing (0 = just started) */
    }
    send_cmd(cl->fd, &rep);
}

/* Send a sink descriptor for our ALSA sink, with all fields required by the
 * negotiated protocol version.  Fields past the base set are version-gated
 * exactly as in PA's protocol-native.c pa_sink_info serialisation.
 *
 * Base fields (all proto):
 *   index, name, description, sample_spec, channel_map, owner_module,
 *   volume (cvolume), mute, monitor_source, monitor_source_name,
 *   latency, driver, flags
 * Proto >= 13: proplist, configured_latency
 * Proto >= 15: base_volume (PA_TAG_VOLUME), state, n_volume_steps, card
 * Proto >= 16: n_ports, active_port_name
 *
 * Used for both GET_SINK_INFO (single) and GET_SINK_INFO_LIST. */
static void send_sink_info(int fd, uint32_t tag, uint32_t proto) {
    uint32_t rate = g_rate;
    TsW rep = {0};
    tw_u32(&rep, PA_CMD_REPLY);
    tw_u32(&rep, tag);

    /* ---- base fields ---- */
    tw_u32(&rep, 0u);                        /* sink index */
    tw_str(&rep, "alsa_sink");               /* name */
    tw_str(&rep, "ALSA Audio");              /* description */
    tw_sample_spec(&rep, (uint8_t)PA_SAMPLE_FLOAT32LE, 2, rate);
    tw_channel_map(&rep, 2);
    tw_u32(&rep, 0xFFFFFFFFu);               /* owner module: none */
    /* cvolume: 2 channels at PA_VOLUME_NORM = 0x00010000 */
    rep.data[rep.pos++] = PA_TAG_CVOLUME;
    rep.data[rep.pos++] = 2;
    for (int _i = 0; _i < 2; _i++) {
        rep.data[rep.pos++] = 0x00; rep.data[rep.pos++] = 0x01;
        rep.data[rep.pos++] = 0x00; rep.data[rep.pos++] = 0x00;
    }
    tw_bool(&rep, 0);                        /* mute = false */
    tw_u32(&rep, 0u);                        /* monitor source index: alsa_sink.monitor */
    tw_str(&rep, "alsa_sink.monitor");       /* monitor source name */
    tw_usec(&rep, 0u);                       /* latency */
    tw_str(&rep, "alsa");                    /* driver */
    tw_u32(&rep, 0u);                        /* flags */

    /* ---- proto >= 13 ---- */
    if (proto >= 13u) {
        rep.data[rep.pos++] = PA_TAG_PROPLIST;     /* 'P': proplist type tag */
        rep.data[rep.pos++] = PA_TAG_STRING_NULL;  /* 'N': empty proplist body */
        tw_usec(&rep, 0u);                         /* configured_latency */
    }
    /* ---- proto >= 15 ---- */
    if (proto >= 15u) {
        /* base_volume = PA_VOLUME_NORM */
        rep.data[rep.pos++] = PA_TAG_VOLUME;
        rep.data[rep.pos++] = 0x00; rep.data[rep.pos++] = 0x01;
        rep.data[rep.pos++] = 0x00; rep.data[rep.pos++] = 0x00;
        tw_u32(&rep, 0u);                          /* state: PA_SINK_RUNNING = 0 */
        tw_u32(&rep, 65537u);                      /* n_volume_steps = PA_VOLUME_NORM+1 */
        tw_u32(&rep, 0xFFFFFFFFu);                 /* card: none */
    }
    /* ---- proto >= 16 ---- */
    if (proto >= 16u) {
        tw_u32(&rep, 0u);                          /* n_ports = 0 */
        tw_str(&rep, NULL);                        /* active_port = NULL */
    }
    /* ---- proto >= 21 ---- */
    if (proto >= 21u) {
        /* Count + format_info list — verified: protocol-native.c line 3225-3236. */
        rep.data[rep.pos++] = PA_TAG_U8;            /* 'B' */
        rep.data[rep.pos++] = 1u;                   /* n_formats = 1 */
        rep.data[rep.pos++] = PA_TAG_FORMAT_INFO;   /* 'f' */
        rep.data[rep.pos++] = PA_TAG_U8;            /* 'B' */
        rep.data[rep.pos++] = 1u;                   /* PA_ENCODING_PCM = 1 */
        rep.data[rep.pos++] = PA_TAG_PROPLIST;      /* 'P' */
        rep.data[rep.pos++] = PA_TAG_STRING_NULL;   /* 'N' empty proplist */
    }
    send_cmd(fd, &rep);
}

/* Append one sink-input descriptor (our playback stream `slot`) to `rep`, with
 * the version-gated layout of PA's sink_input_fill_tagstruct (protocol-native.c
 * 3402-3443). Some clients (e.g. VLC) create a playback stream and then call
 * GET_SINK_INPUT_INFO on its index to track it — returning a malformed/empty
 * reply tears the connection down, which is why those apps "don't work". */
static void put_sink_input_info(TsW *rep, uint32_t proto, int slot) {
    PaStream *s = &streams[slot];
    uint8_t ch = (s->channels < 2) ? 1u : 2u;
    tw_u32(rep, (uint32_t)slot);             /* sink input index */
    tw_str(rep, "Playback Stream");          /* media name */
    tw_u32(rep, 0xFFFFFFFFu);                /* owner module: none */
    tw_u32(rep, 0xFFFFFFFFu);                /* client: none */
    tw_u32(rep, 0u);                         /* sink index */
    tw_sample_spec(rep, s->format, ch, s->rate);
    tw_channel_map(rep, ch);
    /* current per-stream cvolume (echo the raw values the client set) */
    rep->data[rep->pos++] = PA_TAG_CVOLUME;
    rep->data[rep->pos++] = ch;
    for (uint8_t i = 0; i < ch; i++) {
        uint32_t v = (i == 0) ? s->volL : s->volR;
        rep->data[rep->pos++] = (v >> 24) & 0xFF;
        rep->data[rep->pos++] = (v >> 16) & 0xFF;
        rep->data[rep->pos++] = (v >>  8) & 0xFF;
        rep->data[rep->pos++] =  v        & 0xFF;
    }
    tw_usec(rep, 0u);                         /* latency */
    tw_usec(rep, 0u);                         /* sink latency */
    tw_str(rep, "copy");                      /* resample method */
    tw_str(rep, "alsa");                      /* driver */
    if (proto >= 11u) tw_bool(rep, s->muted ? 1 : 0);        /* muted */
    if (proto >= 13u) tw_proplist_empty(rep);                /* proplist */
    if (proto >= 19u) tw_bool(rep, s->corked ? 1 : 0);       /* corked */
    if (proto >= 20u) { tw_bool(rep, 1); tw_bool(rep, 1); }  /* has_volume, volume_writable */
    if (proto >= 21u) tw_format_info_pcm(rep);               /* format */
}

/* GET_SINK_INPUT_INFO: one descriptor for the requested stream, or NOENTITY. */
static void send_sink_input_info(int fd, uint32_t tag, uint32_t proto, uint32_t idx) {
    if (idx >= MAX_STREAMS || !streams[idx].active) {
        send_error(fd, tag, PA_ERR_NOENTITY);
        return;
    }
    TsW rep = {0};
    tw_u32(&rep, PA_CMD_REPLY);
    tw_u32(&rep, tag);
    put_sink_input_info(&rep, proto, (int)idx);
    send_cmd(fd, &rep);
}

/* GET_SINK_INPUT_INFO_LIST: every active stream, then an empty reply as EOL. */
static void send_sink_input_info_list(int fd, uint32_t tag, uint32_t proto) {
    for (int i = 0; i < MAX_STREAMS; i++) {
        if (!streams[i].active) continue;
        TsW rep = {0};
        tw_u32(&rep, PA_CMD_REPLY);
        tw_u32(&rep, tag);
        put_sink_input_info(&rep, proto, i);
        send_cmd(fd, &rep);
    }
    send_reply_empty(fd, tag);
}

/* A minimal client descriptor (protocol-native.c client_fill_tagstruct). */
static void put_client_info(TsW *rep, uint32_t proto) {
    tw_u32(rep, 0u);                         /* client index */
    tw_str(rep, "pulse-alsa-bridge client"); /* application name */
    tw_u32(rep, 0xFFFFFFFFu);                /* owner module: none */
    tw_str(rep, "alsa");                     /* driver */
    if (proto >= 13u) tw_proplist_empty(rep);
}

/* Send our monitor source descriptor (PA's source_fill_tagstruct,
 * protocol-native.c 3239-3312). Note the format list is gated on proto >= 22
 * for sources (vs >= 21 for sinks). */
static void send_source_info(int fd, uint32_t tag, uint32_t proto) {
    TsW rep = {0};
    tw_u32(&rep, PA_CMD_REPLY);
    tw_u32(&rep, tag);

    tw_u32(&rep, 0u);                         /* source index */
    tw_str(&rep, "alsa_sink.monitor");        /* name */
    tw_str(&rep, "Monitor of ALSA Audio");    /* description */
    tw_sample_spec(&rep, (uint8_t)PA_SAMPLE_S16LE, 2, g_rate);
    tw_channel_map(&rep, 2);
    tw_u32(&rep, 0xFFFFFFFFu);                /* owner module: none */
    tw_cvolume_norm(&rep, 2);
    tw_bool(&rep, 0);                         /* mute = false */
    tw_u32(&rep, 0u);                         /* monitor_of sink index */
    tw_str(&rep, "alsa_sink");                /* monitor_of sink name */
    tw_usec(&rep, 0u);                        /* latency */
    tw_str(&rep, "alsa");                     /* driver */
    tw_u32(&rep, 0u);                         /* flags */

    if (proto >= 13u) { tw_proplist_empty(&rep); tw_usec(&rep, 0u); }
    if (proto >= 15u) {
        rep.data[rep.pos++] = PA_TAG_VOLUME;
        rep.data[rep.pos++] = 0x00; rep.data[rep.pos++] = 0x01;
        rep.data[rep.pos++] = 0x00; rep.data[rep.pos++] = 0x00;  /* base_volume = NORM */
        tw_u32(&rep, 0u);                     /* state: PA_SOURCE_RUNNING = 0 */
        tw_u32(&rep, 65537u);                 /* n_volume_steps */
        tw_u32(&rep, 0xFFFFFFFFu);            /* card: none */
    }
    if (proto >= 16u) { tw_u32(&rep, 0u); tw_str(&rep, NULL); }  /* n_ports, active_port */
    if (proto >= 22u) {
        rep.data[rep.pos++] = PA_TAG_U8;
        rep.data[rep.pos++] = 1u;            /* n_formats = 1 */
        tw_format_info_pcm(&rep);
    }
    send_cmd(fd, &rep);
}

/* Append one source-output descriptor (our record stream `slot`) to `rep`
 * (PA's source_output_fill_tagstruct, protocol-native.c 3445-3484). */
static void put_source_output_info(TsW *rep, uint32_t proto, int slot) {
    RecordStream *r = &rstreams[slot];
    uint8_t ch = (r->channels < 2) ? 1u : 2u;
    tw_u32(rep, (uint32_t)slot);             /* source output index */
    tw_str(rep, "Record Stream");            /* media name */
    tw_u32(rep, 0xFFFFFFFFu);                /* owner module: none */
    tw_u32(rep, 0xFFFFFFFFu);                /* client: none */
    tw_u32(rep, 0u);                         /* source index */
    tw_sample_spec(rep, r->format, ch, r->rate);
    tw_channel_map(rep, ch);
    tw_usec(rep, 0u);                         /* latency */
    tw_usec(rep, 0u);                         /* source latency */
    tw_str(rep, "copy");                      /* resample method */
    tw_str(rep, "alsa");                      /* driver */
    if (proto >= 13u) tw_proplist_empty(rep);
    if (proto >= 19u) tw_bool(rep, r->corked ? 1 : 0);
    if (proto >= 22u) {
        tw_cvolume_norm(rep, ch);
        tw_bool(rep, 0);                      /* muted */
        tw_bool(rep, 1);                      /* has_volume */
        tw_bool(rep, 1);                      /* volume_writable */
        tw_format_info_pcm(rep);
    }
}

/* ======================================================================
 * Extension command handler (PA_COMMAND_EXTENSION = 87)
 *
 * Payload after cmd+tag: module_index (uint32), module_name (string),
 * subcommand (uint32), [subcommand-specific data].
 *
 * We implement just enough for Steam to stay connected:
 *   module-stream-restore: TEST → version 1; READ → empty list; others → ack
 *   module-device-manager: TEST → version 1; others → ack
 *   unknown module        → PA_ERR_NOENTITY
 * ====================================================================== */

static void cmd_extension(Client *cl, TsR *ts, uint32_t tag) {
    uint32_t    module_idx = 0;
    const char *module_name = NULL;
    uint32_t    subcmd = 0;

    if (tr_u32(ts, &module_idx) < 0 ||
        tr_str(ts, &module_name) < 0 ||
        tr_u32(ts, &subcmd) < 0) {
        send_error(cl->fd, tag, PA_ERR_NOTSUPPORTED);
        return;
    }

    if (!module_name ||
        (strcmp(module_name, "module-stream-restore") != 0 &&
         strcmp(module_name, "module-device-manager") != 0)) {
        send_error(cl->fd, tag, PA_ERR_NOENTITY);
        return;
    }

    if (subcmd == 0) { /* SUBCOMMAND_TEST */
        TsW rep = {0};
        tw_u32(&rep, PA_CMD_REPLY);
        tw_u32(&rep, tag);
        tw_u32(&rep, 1u);  /* EXT_VERSION = 1 */
        send_cmd(cl->fd, &rep);
    } else {
        /* READ (1): empty list in one packet; SUBSCRIBE (4), WRITE (2), etc.: ack */
        send_reply_empty(cl->fd, tag);
    }
}

/* ======================================================================
 * Record streams (monitor capture)
 *
 * A client records the sink's monitor by creating a record stream; the
 * playback thread tees its mix into the stream's ring (rec_feed), and the
 * poll loop pushes that PCM back to the client as memblocks.
 * ====================================================================== */

static void cmd_create_record_stream(Client *cl, TsR *ts, uint32_t tag) {
    uint8_t  fmt = (uint8_t)PA_SAMPLE_S16LE;
    uint8_t  ch  = 2;
    uint32_t rate = g_rate;

    /* Request layout: [name(str) if proto<13], sample_spec, channel_map,
     * source_index(u32), source_name(str), maxlength(u32), corked(bool),
     * fragsize(u32), then version-gated flags we don't need.  We only read up
     * to fragsize — everything we negotiate comes from the sample_spec. */
    const char *name = NULL;
    if (cl->proto < 13u) tr_str(ts, &name);  /* name (unused) */
    if (tr_sample_spec(ts, &fmt, &ch, &rate) < 0) {
        send_error(cl->fd, tag, PA_ERR_NOTSUPPORTED);
        return;
    }
    if (fmt != PA_SAMPLE_S16LE && fmt != PA_SAMPLE_FLOAT32LE) {
        send_error(cl->fd, tag, PA_ERR_NOTSUPPORTED);
        return;
    }
    if (ch < 1 || ch > 2) ch = 2;
    if (rate < 4000u || rate > 192000u) {
        send_error(cl->fd, tag, PA_ERR_NOTSUPPORTED);
        return;
    }

    uint32_t maxlength = 0, fragsize = 0;
    const char *src_name = NULL;
    int start_corked = 0;
    tr_skip(ts);                 /* channel_map */
    tr_skip(ts);                 /* source_index (u32) */
    tr_str(ts, &src_name);       /* source_name */
    tr_u32(ts, &maxlength);
    if (ts->pos < ts->len)       /* corked (bool) */
        start_corked = (ts->data[ts->pos++] != PA_TAG_BOOL_FALSE);
    tr_u32(ts, &fragsize);

    int slot = alloc_record_stream();
    if (slot < 0) {
        send_error(cl->fd, tag, PA_ERR_INTERNAL);
        return;
    }
    RecordStream *r = &rstreams[slot];
    ring_reset(r->rb);
    r->client_idx = (int)(cl - clients);
    r->client_fd  = cl->fd;
    r->format     = fmt;
    r->channels   = ch;
    r->corked     = (uint8_t)start_corked;
    r->rate       = rate;
    r->phase      = 0.0;
    r->prevL = r->prevR = 0.0f;
    r->tx_len = r->tx_sent = 0;
    r->delivered  = 0;
    /* fragsize drives how much we send per packet; clamp to a sane window. */
    if (fragsize == 0u || fragsize == 0xFFFFFFFFu || fragsize > REC_TX_CHUNK)
        fragsize = REC_TX_CHUNK;
    r->fragsize = fragsize;
    __sync_synchronize();        /* publish fields before active = 1 */
    r->active = 1;

    /* Reply (protocol-native.c command_create_record_stream, 2400-2430). The
     * client validates the echoed sample_spec against its request, so echo it
     * back exactly; rec_feed converts the mix to this spec. */
    uint32_t ring_max = REC_RB_BYTES;
    TsW rep = {0};
    tw_u32(&rep, PA_CMD_REPLY);
    tw_u32(&rep, tag);
    tw_u32(&rep, (uint32_t)slot);    /* stream index (channel) */
    tw_u32(&rep, (uint32_t)slot);    /* source_output index */
    if (cl->proto >= 9u) {
        tw_u32(&rep, ring_max);      /* maxlength */
        tw_u32(&rep, fragsize);      /* fragsize */
    }
    if (cl->proto >= 12u) {
        tw_sample_spec(&rep, fmt, ch, rate);
        tw_channel_map(&rep, ch);
        tw_u32(&rep, 0u);            /* source index */
        tw_str(&rep, "alsa_sink.monitor");
        tw_bool(&rep, 0);            /* not suspended */
    }
    if (cl->proto >= 13u)
        tw_usec(&rep, 0u);           /* configured_source_latency */
    if (cl->proto >= 22u)
        tw_format_info_pcm(&rep);
    send_cmd(cl->fd, &rep);

    if (g_debug)
        fprintf(stderr, "pulse-alsa-bridge: record stream %d created: fmt=%u ch=%u rate=%u corked=%d\n",
                slot, fmt, ch, rate, start_corked);
}

static void cmd_delete_record_stream(Client *cl, TsR *ts, uint32_t tag) {
    uint32_t idx = 0;
    if (tr_u32(ts, &idx) == 0 && idx < MAX_RECORD_STREAMS) {
        if (rstreams[idx].active && rstreams[idx].client_idx == (int)(cl - clients)) {
            rstreams[idx].active = 0;   /* playback thread stops teeing */
            rstreams[idx].tx_len = rstreams[idx].tx_sent = 0;
        }
    }
    send_reply_empty(cl->fd, tag);
}

/* GET_RECORD_LATENCY: mirror cmd_get_playback_latency for capture streams
 * (protocol-native.c command_get_record_latency). Clients poll this for timing. */
static void cmd_get_record_latency(Client *cl, TsR *ts, uint32_t tag) {
    uint32_t idx = 0;
    tr_u32(ts, &idx);   /* the client's timeval follows; we don't need it */

    struct timeval now;
    gettimeofday(&now, NULL);

    uint64_t read_idx = 0, write_idx = 0;
    if (idx < MAX_RECORD_STREAMS && rstreams[idx].active) {
        RecordStream *r = &rstreams[idx];
        read_idx  = r->delivered;                          /* bytes handed to client */
        write_idx = r->delivered + ring_read_space(r->rb); /* + still buffered */
    }

    TsW rep = {0};
    tw_u32(&rep, PA_CMD_REPLY);
    tw_u32(&rep, tag);
    tw_usec(&rep, 0u);      /* monitor latency */
    tw_usec(&rep, 0u);      /* source latency */
    tw_bool(&rep, idx < MAX_RECORD_STREAMS && rstreams[idx].active && !rstreams[idx].corked);
    tw_timeval(&rep, &now); /* remote_time */
    tw_timeval(&rep, &now); /* local_time */
    tw_u64(&rep, 'r', write_idx);  /* write_index (PA_TAG_S64) */
    tw_u64(&rep, 'r', read_idx);   /* read_index  (PA_TAG_S64) */
    send_cmd(cl->fd, &rep);
}

/* ======================================================================
 * Command dispatcher
 * ====================================================================== */

static void dispatch(Client *cl, uint32_t channel,
                     const uint8_t *data, uint32_t len) {
    if (channel != PA_CONTROL_CHANNEL) {
        handle_audio(channel, data, len, cl->seek_offset, cl->seek_mode);
        return;
    }

    TsR ts = { data, len, 0 };
    uint32_t cmd = 0, tag = 0;
    if (tr_u32(&ts, &cmd) < 0 || tr_u32(&ts, &tag) < 0) return;

    if (g_debug)
        fprintf(stderr, "pulse-alsa-bridge: [fd=%d] cmd=%u\n", cl->fd, cmd);

    switch (cmd) {
    case PA_CMD_AUTH:
        cmd_auth(cl, &ts, tag);
        break;
    case PA_CMD_SET_CLIENT_NAME:
        cmd_set_client_name(cl, &ts, tag);
        break;
    case PA_CMD_GET_SERVER_INFO:
        cmd_get_server_info(cl, &ts, tag);
        break;
    case PA_CMD_GET_PLAYBACK_LATENCY:
        cmd_get_playback_latency(cl, &ts, tag);
        break;
    case PA_CMD_CREATE_PLAYBACK_STREAM:
        cmd_create_playback_stream(cl, &ts, tag);
        break;
    case PA_CMD_DELETE_PLAYBACK_STREAM:
        cmd_delete_playback_stream(cl, &ts, tag);
        break;
    case PA_CMD_CORK_PLAYBACK_STREAM:
        cmd_cork_playback_stream(cl, &ts, tag);
        break;
    case PA_CMD_TRIGGER_PLAYBACK_STREAM: {
        uint32_t idx = 0;
        tr_u32(&ts, &idx);
        send_reply_empty(cl->fd, tag);
        if (idx < MAX_STREAMS && streams[idx].active)
            send_request(cl->fd, idx, BUF_TLENGTH);
        break;
    }

    /* Record streams: capture the sink monitor (parecord, SSR, OBS, ...) */
    case PA_CMD_CREATE_RECORD_STREAM:
        cmd_create_record_stream(cl, &ts, tag);
        break;
    case PA_CMD_DELETE_RECORD_STREAM:
        cmd_delete_record_stream(cl, &ts, tag);
        break;
    case PA_CMD_GET_RECORD_LATENCY:
        cmd_get_record_latency(cl, &ts, tag);
        break;
    case PA_CMD_CORK_RECORD_STREAM: {
        uint32_t idx = 0;
        tr_u32(&ts, &idx);
        int corked = 1;
        if (ts.pos < ts.len)
            corked = (ts.data[ts.pos++] != PA_TAG_BOOL_FALSE);
        if (idx < MAX_RECORD_STREAMS && rstreams[idx].active)
            rstreams[idx].corked = (uint8_t)corked;
        send_reply_empty(cl->fd, tag);
        break;
    }
    case PA_CMD_FLUSH_RECORD_STREAM: {
        uint32_t idx = 0;
        tr_u32(&ts, &idx);
        if (idx < MAX_RECORD_STREAMS && rstreams[idx].active) {
            ring_reset(rstreams[idx].rb);
            rstreams[idx].tx_len = rstreams[idx].tx_sent = 0;
        }
        send_reply_empty(cl->fd, tag);
        break;
    }

    /* Source info: our one monitor source (alsa_sink.monitor) */
    case PA_CMD_GET_SOURCE_INFO:
        send_source_info(cl->fd, tag, cl->proto);
        break;
    case PA_CMD_GET_SOURCE_INFO_LIST:
        send_source_info(cl->fd, tag, cl->proto);
        send_reply_empty(cl->fd, tag);  /* EOL */
        break;
    case PA_CMD_GET_SOURCE_OUTPUT_INFO: {
        uint32_t idx = 0;
        tr_u32(&ts, &idx);
        if (idx < MAX_RECORD_STREAMS && rstreams[idx].active) {
            TsW rep = {0};
            tw_u32(&rep, PA_CMD_REPLY);
            tw_u32(&rep, tag);
            put_source_output_info(&rep, cl->proto, (int)idx);
            send_cmd(cl->fd, &rep);
        } else {
            send_error(cl->fd, tag, PA_ERR_NOENTITY);
        }
        break;
    }
    case PA_CMD_GET_SOURCE_OUTPUT_INFO_LIST:
        for (int i = 0; i < MAX_RECORD_STREAMS; i++) {
            if (!rstreams[i].active) continue;
            TsW rep = {0};
            tw_u32(&rep, PA_CMD_REPLY);
            tw_u32(&rep, tag);
            put_source_output_info(&rep, cl->proto, i);
            send_cmd(cl->fd, &rep);
        }
        send_reply_empty(cl->fd, tag);  /* EOL */
        break;

    /* Sink info: return a real descriptor so clients can create streams */
    case PA_CMD_GET_SINK_INFO:
        send_sink_info(cl->fd, tag, cl->proto);
        break;
    case PA_CMD_GET_SINK_INFO_LIST:
        send_sink_info(cl->fd, tag, cl->proto);
        send_reply_empty(cl->fd, tag);  /* EOL: signals end of list to libpulse */
        break;

    case PA_CMD_SUBSCRIBE:
        send_reply_empty(cl->fd, tag);
        break;

    /* Extension commands: module-stream-restore and module-device-manager */
    case PA_CMD_EXTENSION:
        cmd_extension(cl, &ts, tag);
        break;

    /* No cards/hardware to report */
    case PA_CMD_GET_CARD_INFO:
        send_error(cl->fd, tag, PA_ERR_NOENTITY);
        break;
    case PA_CMD_GET_CARD_INFO_LIST:
        send_reply_empty(cl->fd, tag);  /* empty list = no cards */
        break;

    /* FLUSH: reply must include write_index + read_index (s64 each), then REQUEST */
    case PA_CMD_FLUSH_PLAYBACK_STREAM: {
        uint32_t idx = 0;
        tr_u32(&ts, &idx);
        if (idx < MAX_STREAMS && streams[idx].active) {
            ring_reset(rb_L[idx]);
            ring_reset(rb_R[idx]);
        }
        /* FLUSH replies with a PLAIN ack — NOT write_index/read_index. libpulse's
         * pa_stream_flush registers pa_stream_simple_ack_callback, which fails the
         * whole context with PA_ERR_PROTOCOL if the reply tagstruct is not empty
         * (stream.c:2292), disconnecting the client. Verified: real PA sends only
         * pa_pstream_send_simple_ack (protocol-native.c:4016). Players flush on every
         * timeline seek, so a malformed reply here froze playback on seek. */
        send_reply_empty(cl->fd, tag);
        /* Re-grant credit so the client resumes writing from the new position after
         * the flush (REQUEST is a separate server→client command, not part of the
         * ack, so it does not corrupt the reply). Skip while corked — uncork sends
         * its own REQUEST. */
        if (idx < MAX_STREAMS && streams[idx].active && !streams[idx].corked &&
            streams[idx].client_fd != -1)
            send_request(cl->fd, idx, BUF_TLENGTH);
        break;
    }

    /* Sink-input info: a real descriptor per playback stream (VLC and other
     * players introspect their own stream after creating it). */
    case PA_CMD_GET_SINK_INPUT_INFO: {
        uint32_t idx = 0;
        tr_u32(&ts, &idx);
        send_sink_input_info(cl->fd, tag, cl->proto, idx);
        break;
    }
    case PA_CMD_GET_SINK_INPUT_INFO_LIST:
        send_sink_input_info_list(cl->fd, tag, cl->proto);
        break;

    /* Client info: a single minimal client descriptor */
    case PA_CMD_GET_CLIENT_INFO: {
        TsW rep = {0};
        tw_u32(&rep, PA_CMD_REPLY);
        tw_u32(&rep, tag);
        put_client_info(&rep, cl->proto);
        send_cmd(cl->fd, &rep);
        break;
    }

    /* Per-stream software volume: apply it in the mix so player volume sliders
     * (e.g. VLC) actually attenuate that stream. */
    case PA_CMD_SET_SINK_INPUT_VOLUME: {
        uint32_t idx = 0, vL = PA_VOLUME_NORM, vR = PA_VOLUME_NORM;
        if (tr_u32(&ts, &idx) == 0 && tr_cvolume(&ts, &vL, &vR) == 0 &&
            idx < MAX_STREAMS && streams[idx].active) {
            streams[idx].volL  = vL;
            streams[idx].volR  = vR;
            streams[idx].gainL = vol_to_gain(vL);
            streams[idx].gainR = vol_to_gain(vR);
        }
        send_reply_empty(cl->fd, tag);
        break;
    }
    case PA_CMD_SET_SINK_INPUT_MUTE: {
        uint32_t idx = 0;
        if (tr_u32(&ts, &idx) == 0 && idx < MAX_STREAMS && streams[idx].active &&
            ts.pos < ts.len)
            streams[idx].muted = (ts.data[ts.pos++] != PA_TAG_BOOL_FALSE);
        send_reply_empty(cl->fd, tag);
        break;
    }

    /* Acknowledge-only commands */
    case PA_CMD_DRAIN_PLAYBACK_STREAM:
    case PA_CMD_PREBUF_PLAYBACK_STREAM:
    case PA_CMD_SET_DEFAULT_SINK:
    case PA_CMD_SET_DEFAULT_SOURCE:
    case 39u:  /* PA_COMMAND_SET_SINK_MUTE */
    case 40u:  /* PA_COMMAND_SET_SOURCE_MUTE */
    case PA_CMD_MOVE_SINK_INPUT:
    case PA_CMD_KILL_SINK_INPUT:
        send_reply_empty(cl->fd, tag);
        break;

    default:
        if (g_debug)
            fprintf(stderr, "pulse-alsa-bridge: [fd=%d] unhandled cmd=%u → error\n",
                    cl->fd, cmd);
        send_error(cl->fd, tag, PA_ERR_NOTSUPPORTED);
        break;
    }
}

/* ======================================================================
 * Client lifecycle
 * ====================================================================== */

static Client *alloc_client(int fd) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!clients[i].active) {
            memset(&clients[i], 0, sizeof(clients[i]));
            clients[i].fd     = fd;
            clients[i].active = 1;
            clients[i].proto  = PA_PROTOCOL_VERSION;
            return &clients[i];
        }
    }
    return NULL;
}

static void close_client(Client *cl) {
    int idx = (int)(cl - clients);
    for (int i = 0; i < MAX_STREAMS; i++) {
        if (!streams[i].active || streams[i].client_idx != idx) continue;
        size_t rb_l = ring_read_space(rb_L[i]);
        size_t rb_r = ring_read_space(rb_R[i]);
        /* Keep stream alive if audio is buffered so the playback thread can drain it.
         * client_fd=-1 signals "draining: no client to request more data from". */
        if (rb_l > 0 || rb_r > 0) {
            streams[i].client_fd = -1;
            if (g_debug)
                fprintf(stderr, "pulse-alsa-bridge: stream %d: rb_L=%zu rb_R=%zu bytes → drain mode\n",
                        i, rb_l, rb_r);
        } else {
            streams[i].active = 0;
            if (g_debug)
                fprintf(stderr, "pulse-alsa-bridge: stream %d: ring buffer empty → immediate deactivate\n", i);
        }
    }
    /* Record streams have no audio to drain to ALSA — drop them at once. */
    for (int i = 0; i < MAX_RECORD_STREAMS; i++) {
        if (rstreams[i].active && rstreams[i].client_idx == idx) {
            rstreams[i].active = 0;
            rstreams[i].tx_len = rstreams[i].tx_sent = 0;
        }
    }
    close(cl->fd);
    free(cl->payload);
    cl->payload = NULL;
    cl->active  = 0;
}

/* Read and process all available data for one client.
 * Returns 0 on success / EAGAIN; -1 on disconnect or error. */
static int read_client(Client *cl) {
    for (;;) {
        /* Phase 1: accumulate 20-byte packet header */
        if (cl->hdr_pos < HDR_LEN) {
            ssize_t n = read(cl->fd, cl->hdr + cl->hdr_pos,
                             (size_t)(HDR_LEN - cl->hdr_pos));
            if (n == 0) return -1;
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
                return -1;
            }
            cl->hdr_pos += (int)n;
            if (cl->hdr_pos < HDR_LEN) return 0;

            /* Parse header */
            uint32_t h[5];
            memcpy(h, cl->hdr, HDR_LEN);
            cl->payload_len = ntohl(h[0]);
            cl->channel     = ntohl(h[1]);
            /* h[2]/h[3] = 64-bit signed seek offset, h[4] low 8 bits = seek mode.
             * Audio (non-control) packets carry these for timeline seeks; control
             * packets always send offset 0 / SEEK_RELATIVE, so this is harmless there. */
            cl->seek_offset = (int64_t)(((uint64_t)ntohl(h[2]) << 32) | ntohl(h[3]));
            cl->seek_mode   = (uint8_t)(ntohl(h[4]) & 0xFFu);
            cl->payload_pos = 0;

            /* Grow payload buffer if needed */
            if (cl->payload_len > cl->payload_cap) {
                free(cl->payload);
                cl->payload = malloc(cl->payload_len + 1);
                if (!cl->payload) return -1;
                cl->payload_cap = cl->payload_len;
            }
        }

        /* Phase 2: accumulate payload */
        if (cl->payload_pos < cl->payload_len) {
            ssize_t n = read(cl->fd, cl->payload + cl->payload_pos,
                             (size_t)(cl->payload_len - cl->payload_pos));
            if (n == 0) return -1;
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
                return -1;
            }
            cl->payload_pos += (uint32_t)n;
            if (cl->payload_pos < cl->payload_len) return 0;
        }

        /* Packet complete */
        dispatch(cl, cl->channel, cl->payload, cl->payload_len);

        /* Reset for next packet */
        cl->hdr_pos     = 0;
        cl->payload_pos = 0;
        cl->payload_len = 0;
    }
}

/* Push buffered monitor audio to each record client. Consumer side of the
 * record SPSC ring; runs on the main thread only. A packet (header + PCM) is
 * built into the stream's tx buffer and written non-blocking; a partial write
 * resumes on the next call, so the byte stream never desyncs. On a full socket
 * the write yields EAGAIN and we retry later (the ring drops oldest if it
 * overflows, so a slow reader can never stall playback). */
static void pump_record_streams(void) {
    for (int i = 0; i < MAX_RECORD_STREAMS; i++) {
        RecordStream *r = &rstreams[i];
        if (!r->active) continue;

        /* Start a new packet only when the previous one is fully flushed. */
        if (r->tx_len == r->tx_sent) {
            size_t avail = ring_read_space(r->rb);
            uint32_t bps = (r->format == PA_SAMPLE_S16LE) ? 2u : 4u;
            uint32_t framebytes = bps * r->channels;
            uint32_t cap = (r->fragsize < REC_TX_CHUNK) ? r->fragsize : REC_TX_CHUNK;
            uint32_t want = (avail < cap) ? (uint32_t)avail : cap;
            want -= want % framebytes;            /* whole frames only */
            if (want == 0) continue;

            uint32_t *h = (uint32_t *)(void *)r->tx;
            h[0] = htonl(want);
            h[1] = htonl((uint32_t)i);            /* channel = record stream index */
            h[2] = 0; h[3] = 0; h[4] = 0;         /* offset 0, seek PA_SEEK_RELATIVE */
            ring_read(r->rb, (char *)(r->tx + HDR_LEN), want);
            r->tx_len  = HDR_LEN + want;
            r->tx_sent = 0;
            r->delivered += want;
        }

        while (r->tx_sent < r->tx_len) {
            ssize_t n = write(r->client_fd, r->tx + r->tx_sent, r->tx_len - r->tx_sent);
            if (n > 0) {
                r->tx_sent += (uint32_t)n;
            } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                break;                            /* socket full: finish next time */
            } else {
                r->active = 0;                    /* write error: drop the stream */
                r->tx_len = r->tx_sent = 0;
                break;
            }
        }
        if (r->tx_sent == r->tx_len)
            r->tx_len = r->tx_sent = 0;
    }
}

/* ======================================================================
 * Monitor capture feed (playback thread → record ring)
 * ====================================================================== */

/* Write one [-1,1] sample to *p in the record stream's format. */
static inline void put_sample(uint8_t *p, uint8_t fmt, float v) {
    if (v >  1.0f) v =  1.0f;
    if (v < -1.0f) v = -1.0f;
    if (fmt == PA_SAMPLE_S16LE) {
        int16_t s = (int16_t)lrintf(v * 32767.0f);
        memcpy(p, &s, 2);
    } else {
        memcpy(p, &v, 4);
    }
}

/* Tee N frames of the stereo float mix (at g_rate) into one record stream,
 * linear-resampling to the stream's rate, down/keeping channels, converting to
 * its format.  The resampler phase/prev carry across blocks for continuity.
 * Producer side of the SPSC ring; runs on the playback thread only. */
static void rec_feed(RecordStream *r, const float *inL, const float *inR, snd_pcm_uframes_t N) {
    const uint32_t bps        = (r->format == PA_SAMPLE_S16LE) ? 2u : 4u;
    const uint32_t framebytes = bps * r->channels;
    const double   step       = (double)g_rate / (double)r->rate; /* input frames / output frame */

    uint8_t  obuf[REC_FLUSH * 2u * 4u];
    uint32_t ob = 0;
    double   ph = r->phase;
    float    pL = r->prevL, pR = r->prevR;

    for (snd_pcm_uframes_t n = 0; n < N; n++) {
        float cL = inL[n], cR = inR[n];
        while (ph < 1.0) {
            float f = (float)ph;
            float l = pL + (cL - pL) * f;
            float r2 = pR + (cR - pR) * f;
            if (r->channels == 1) {
                put_sample(obuf + ob, r->format, 0.5f * (l + r2));
            } else {
                put_sample(obuf + ob,       r->format, l);
                put_sample(obuf + ob + bps, r->format, r2);
            }
            ob += framebytes;
            if (ob + framebytes > sizeof(obuf)) {
                ring_write(r->rb, (char *)obuf, ob);
                ob = 0;
            }
            ph += step;
        }
        ph -= 1.0;
        pL = cL; pR = cR;
    }
    if (ob) ring_write(r->rb, (char *)obuf, ob);
    r->phase = ph;
    r->prevL = pL;
    r->prevR = pR;
}

/* ======================================================================
 * ALSA playback thread
 *
 * Mirrors the JACK process callback: sum all active, uncorked streams into a
 * stereo float mix, soft-clip, convert to interleaved S16, and write to ALSA.
 * snd_pcm_writei() blocks, which paces this loop at real time.  When no stream
 * has audio we write a period of silence to keep the PCM running.
 * ====================================================================== */

static void *play_loop(void *arg) {
    (void)arg;

    static float   outL[PLAY_MAX_FRAMES], outR[PLAY_MAX_FRAMES];
    static float   tmpL[PLAY_MAX_FRAMES], tmpR[PLAY_MAX_FRAMES];
    static int16_t inter[PLAY_MAX_FRAMES * 2];

    snd_pcm_uframes_t N = g_period;
    if (N > PLAY_MAX_FRAMES) N = PLAY_MAX_FRAMES;

    while (g_running) {
        /* Live output-device switch requested via SIGUSR1: reopen ALSA in place,
         * keeping the PA socket and all client streams connected. */
        if (g_reopen) {
            g_reopen = 0;
            char dev[512];
            if (read_ctl_device(dev, sizeof(dev)) == 0) {
                alsa_reopen(dev);
                N = g_period;
                if (N > PLAY_MAX_FRAMES) N = PLAY_MAX_FRAMES;
            }
        }

        memset(outL, 0, N * sizeof(float));
        memset(outR, 0, N * sizeof(float));

        for (int i = 0; i < MAX_STREAMS; i++) {
            if (!streams[i].active || streams[i].corked) continue;
            size_t avL = ring_read_space(rb_L[i]);
            size_t avR = ring_read_space(rb_R[i]);
            size_t av  = (avL < avR) ? avL : avR;
            snd_pcm_uframes_t fr = (snd_pcm_uframes_t)(av / sizeof(float));
            if (fr > N) fr = N;
            if (fr == 0) {
                if (streams[i].client_fd == -1)
                    streams[i].active = 0;
                continue;
            }
            ring_read(rb_L[i], (char *)tmpL, fr * sizeof(float));
            ring_read(rb_R[i], (char *)tmpR, fr * sizeof(float));
            float gl = streams[i].muted ? 0.0f : streams[i].gainL;
            float gr = streams[i].muted ? 0.0f : streams[i].gainR;
            for (snd_pcm_uframes_t j = 0; j < fr; j++) {
                outL[j] += tmpL[j] * gl;
                outR[j] += tmpR[j] * gr;
            }
        }

        /* Soft clip + float→S16 interleave, tracking the block peak for the meter.
         * The clipped values are written back to outL/outR so the monitor feed
         * sees exactly what is sent to the hardware. */
        float peakL = 0.0f, peakR = 0.0f;
        for (snd_pcm_uframes_t i = 0; i < N; i++) {
            float l = outL[i], r = outR[i];
            if (l >  1.0f) l =  1.0f;
            if (l < -1.0f) l = -1.0f;
            if (r >  1.0f) r =  1.0f;
            if (r < -1.0f) r = -1.0f;
            float al = (l < 0.0f) ? -l : l;
            float ar = (r < 0.0f) ? -r : r;
            if (al > peakL) peakL = al;
            if (ar > peakR) peakR = ar;
            inter[2 * i]     = (int16_t)lrintf(l * 32767.0f);
            inter[2 * i + 1] = (int16_t)lrintf(r * 32767.0f);
            outL[i] = l;
            outR[i] = r;
        }
        peak_publish(peakL, peakR);

        /* Tee the mix to any monitor-capture clients (parecord, SSR, OBS, ...). */
        for (int i = 0; i < MAX_RECORD_STREAMS; i++) {
            if (rstreams[i].active && !rstreams[i].corked)
                rec_feed(&rstreams[i], outL, outR, N);
        }

        snd_pcm_sframes_t w = snd_pcm_writei(g_pcm, inter, N);
        if (w < 0) {
            w = snd_pcm_recover(g_pcm, (int)w, 1);   /* EPIPE (xrun) / ESTRPIPE */
            if (w < 0) snd_pcm_prepare(g_pcm);
        }
    }
    return NULL;
}

/* ======================================================================
 * ALSA device setup
 * ====================================================================== */

/* Open and configure a playback handle into *out without touching the globals,
 * so a live switch can validate the new device before swapping. want_rate is the
 * requested rate (Hz); the negotiated rate/period/buffer are returned. */
static int alsa_open_handle(snd_pcm_t **out, const char *dev, unsigned want_rate,
                           unsigned *neg_rate, snd_pcm_uframes_t *neg_period,
                           snd_pcm_uframes_t *neg_bufsz) {
    snd_pcm_t *pcm = NULL;
    int err;
    if ((err = snd_pcm_open(&pcm, dev, SND_PCM_STREAM_PLAYBACK, 0)) < 0) {
        fprintf(stderr,
            "pulse-alsa-bridge: cannot open ALSA device '%s': %s\n"
            "  Check that a sound card is present and your ALSA config (~/.asoundrc) is valid.\n",
            dev, snd_strerror(err));
        return -1;
    }

    snd_pcm_hw_params_t *hw;
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(pcm, hw);

    if ((err = snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0 ||
        (err = snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16_LE)) < 0 ||
        (err = snd_pcm_hw_params_set_channels(pcm, hw, 2)) < 0) {
        fprintf(stderr, "pulse-alsa-bridge: hw params (access/format/channels): %s\n",
                snd_strerror(err));
        goto fail;
    }

    unsigned int rate = want_rate;
    if ((err = snd_pcm_hw_params_set_rate_near(pcm, hw, &rate, 0)) < 0) {
        fprintf(stderr, "pulse-alsa-bridge: set rate: %s\n", snd_strerror(err));
        goto fail;
    }

    snd_pcm_uframes_t period = 1024;   /* default period/buffer; adapts to the device */
    snd_pcm_uframes_t bufsz  = 4096;
    snd_pcm_hw_params_set_period_size_near(pcm, hw, &period, 0);
    snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &bufsz);

    if ((err = snd_pcm_hw_params(pcm, hw)) < 0) {
        fprintf(stderr, "pulse-alsa-bridge: hw params commit: %s\n", snd_strerror(err));
        goto fail;
    }

    snd_pcm_hw_params_get_period_size(hw, &period, NULL);
    snd_pcm_hw_params_get_buffer_size(hw, &bufsz);

    /* sw params: start once a buffer is queued, wake per period */
    snd_pcm_sw_params_t *sw;
    snd_pcm_sw_params_alloca(&sw);
    snd_pcm_sw_params_current(pcm, sw);
    snd_pcm_sw_params_set_start_threshold(pcm, sw, period);
    snd_pcm_sw_params_set_avail_min(pcm, sw, period);
    snd_pcm_sw_params(pcm, sw);

    snd_pcm_prepare(pcm);

    *out        = pcm;
    *neg_rate   = rate;
    *neg_period = period;
    *neg_bufsz  = bufsz;
    return 0;

fail:
    snd_pcm_close(pcm);
    return -1;
}

static int alsa_open(const char *dev) {
    snd_pcm_t        *pcm = NULL;
    unsigned          rate = 0;
    snd_pcm_uframes_t period = 0, bufsz = 0;
    /* Request 44100; set_rate_near adapts to whatever the device offers. */
    if (alsa_open_handle(&pcm, dev, 44100, &rate, &period, &bufsz) < 0)
        return -1;
    g_pcm    = pcm;
    g_rate   = rate;
    g_period = period;
    g_bufsz  = bufsz;
    if (g_period > PLAY_MAX_FRAMES) {
        fprintf(stderr, "pulse-alsa-bridge: period %lu > %d frames, capping write chunk\n",
                (unsigned long)g_period, PLAY_MAX_FRAMES);
    }
    return 0;
}

/* Live device switch (playback thread only). Reopens at the established g_rate so
 * client streams — negotiated at g_rate — stay valid; plughw/plug converts to the
 * device's native rate. On failure the current device is kept (audio continues). */
static void alsa_reopen(const char *dev) {
    snd_pcm_t        *pcm = NULL;
    unsigned          rate = 0;
    snd_pcm_uframes_t period = 0, bufsz = 0;
    if (alsa_open_handle(&pcm, dev, g_rate, &rate, &period, &bufsz) < 0) {
        fprintf(stderr, "pulse-alsa-bridge: live switch to '%s' failed; keeping current device\n", dev);
        return;
    }
    if (rate != g_rate)
        fprintf(stderr, "pulse-alsa-bridge: warning: '%s' gave %u Hz (wanted %u); pitch may be off\n",
                dev, rate, g_rate);

    snd_pcm_t *old = g_pcm;
    g_pcm    = pcm;        /* the playback thread is the only g_pcm user: safe swap */
    g_period = period;
    g_bufsz  = bufsz;
    snd_pcm_drop(old);
    snd_pcm_close(old);
    fprintf(stderr, "pulse-alsa-bridge: switched output to '%s' (%u Hz)\n", dev, g_rate);
}

/* Read the device string the GUI left in the control file (one line). Empty or
 * missing -> "default". Returns 0 on success, -1 if no control file is set. */
static int read_ctl_device(char *out, size_t outsz) {
    if (!g_ctl_path[0])
        return -1;
    FILE *f = fopen(g_ctl_path, "r");
    if (!f)
        return -1;
    if (!fgets(out, (int)outsz, f)) {
        fclose(f);
        return -1;
    }
    fclose(f);
    char *nl = strchr(out, '\n');
    if (nl) *nl = '\0';
    if (!out[0]) {
        strncpy(out, "default", outsz - 1);
        out[outsz - 1] = '\0';
    }
    return 0;
}

/* ======================================================================
 * Socket path discovery
 * (mirrors flatpak_run_get_pulse_runtime_dir() in pressure-vessel)
 * ====================================================================== */

static void mkdir_p(const char *path) {
    char tmp[512];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0700);
            *p = '/';
        }
    }
    mkdir(tmp, 0700);
}

static void find_socket_path(char *out, size_t outsz) {
    const char *xdg = getenv("XDG_RUNTIME_DIR");
    if (xdg && *xdg) {
        char dir[512];
        snprintf(dir, sizeof(dir), "%s/pulse", xdg);
        mkdir_p(dir);
        snprintf(out, outsz, "%s/pulse/native", xdg);
        return;
    }

    /* Devuan sysvinit without elogind: try /run/user/$UID */
    char try_run[128];
    snprintf(try_run, sizeof(try_run), "/run/user/%u", (unsigned)getuid());
    struct stat st;
    if (stat(try_run, &st) == 0 && S_ISDIR(st.st_mode)) {
        char dir[512];
        snprintf(dir, sizeof(dir), "%s/pulse", try_run);
        mkdir_p(dir);
        snprintf(out, outsz, "%s/pulse/native", try_run);
        return;
    }

    /* Last resort: ~/.config/pulse/{machine_id}-runtime/native
     * Must export PULSE_RUNTIME_PATH so pressure-vessel finds the dir */
    const char *home = getenv("HOME");
    if (!home || !*home) home = "/tmp";

    char machine_id[64] = "unknown";
    FILE *f = fopen("/etc/machine-id", "r");
    if (!f) f = fopen("/var/lib/dbus/machine-id", "r");
    if (f) {
        if (fgets(machine_id, sizeof(machine_id), f)) {
            char *nl = strchr(machine_id, '\n');
            if (nl) *nl = '\0';
        }
        fclose(f);
    }

    char dir[504];  /* leave 8 bytes for "/native\0" in the 512-byte out buffer */
    snprintf(dir, sizeof(dir), "%s/.config/pulse/%s-runtime", home, machine_id);
    mkdir_p(dir);
    snprintf(out, outsz, "%s/native", dir);

    fprintf(stderr,
        "pulse-alsa-bridge: XDG_RUNTIME_DIR not set.\n"
        "  pressure-vessel needs PULSE_RUNTIME_PATH. Run before Steam:\n"
        "  export PULSE_RUNTIME_PATH=%s\n", dir);
}

/* ======================================================================
 * main()
 * ====================================================================== */

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    g_debug = !!getenv("PULSE_BRIDGE_DEBUG");

    /* Control file for live device switches (optional; GUI sets it). */
    const char *ctl = getenv("PULSE_BRIDGE_CTL_FILE");
    if (ctl && *ctl) {
        strncpy(g_ctl_path, ctl, sizeof(g_ctl_path) - 1);
        g_ctl_path[sizeof(g_ctl_path) - 1] = '\0';
    }

    signal(SIGTERM, sighandler);
    signal(SIGINT,  sighandler);
    signal(SIGPIPE, SIG_IGN);

    /* SIGUSR1 = live output-device switch. Use sigaction so the handler stays
     * installed across repeated switches (signal() may reset to SIG_DFL under
     * _POSIX_C_SOURCE, which would kill the bridge on the second switch). */
    struct sigaction sa_usr1;
    memset(&sa_usr1, 0, sizeof(sa_usr1));
    sa_usr1.sa_handler = reopenhandler;
    sa_usr1.sa_flags = SA_RESTART;
    sigaction(SIGUSR1, &sa_usr1, NULL);

    /* Open ALSA (optional device override via env) */
    const char *dev = getenv("PULSE_BRIDGE_ALSA_DEV");
    if (!dev || !*dev) dev = "default";
    if (alsa_open(dev) < 0)
        return 1;

    /* Buffer sizing relative to ALSA period. Cap the period used here at 1024
     * frames so that on devices with large hw periods (e.g. sof-hda-dsp reports
     * 4096 frames through dmix) the PA flow-control window stays sane (~93 ms
     * tlength) rather than ballooning to 742 ms and starving the ring. */
    uint32_t buf_period = (uint32_t)g_period;
    if (buf_period > 1024u) buf_period = 1024u;
    uint32_t period_bytes = buf_period * 2u * (uint32_t)sizeof(float);
    BUF_MAXLENGTH = period_bytes * 16u;
    BUF_TLENGTH   = period_bytes * 4u;
    BUF_PREBUF    = period_bytes * 2u;
    BUF_MINREQ    = period_bytes;

    /* Allocate ring buffers for all stream slots up front */
    for (int i = 0; i < MAX_STREAMS; i++) {
        rb_L[i] = ring_create(RB_BYTES);
        rb_R[i] = ring_create(RB_BYTES);
        if (!rb_L[i] || !rb_R[i]) {
            fprintf(stderr, "pulse-alsa-bridge: ring buffer alloc failed\n");
            goto err_alsa;
        }
        ring_mlock(rb_L[i]);
        ring_mlock(rb_R[i]);
    }

    /* Allocate monitor-capture rings and packet buffers up front, so create/
     * delete just toggle the slot (no alloc/free races with the playback thread). */
    for (int i = 0; i < MAX_RECORD_STREAMS; i++) {
        rstreams[i].rb = ring_create(REC_RB_BYTES);
        rstreams[i].tx = malloc(HDR_LEN + REC_TX_CHUNK);
        if (!rstreams[i].rb || !rstreams[i].tx) {
            fprintf(stderr, "pulse-alsa-bridge: record buffer alloc failed\n");
            goto err_alsa;
        }
        ring_mlock(rstreams[i].rb);
    }

    /* Discover PA socket path */
    find_socket_path(g_socket_path, sizeof(g_socket_path));

    /* Remove stale socket from a previous crash */
    unlink(g_socket_path);

    /* Create UNIX listen socket */
    int srv_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (srv_fd < 0) { perror("pulse-alsa-bridge: socket"); goto err_alsa; }

    {
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        /* sun_path is 108 bytes on Linux; g_socket_path is bounded to fit */
        memcpy(addr.sun_path, g_socket_path,
               strnlen(g_socket_path, sizeof(addr.sun_path) - 1) + 1);

        if (bind(srv_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            perror("pulse-alsa-bridge: bind");
            close(srv_fd);
            goto err_alsa;
        }
    }

    if (listen(srv_fd, 8) < 0) {
        perror("pulse-alsa-bridge: listen");
        close(srv_fd); unlink(g_socket_path);
        goto err_alsa;
    }

    fprintf(stderr,
        "pulse-alsa-bridge: socket %s\n"
        "pulse-alsa-bridge: ALSA '%s' %u Hz, %lu-frame period, %lu-frame buffer\n",
        g_socket_path, dev, g_rate,
        (unsigned long)g_period, (unsigned long)g_bufsz);

    /* Publish output levels to the GUI meter (non-fatal if it fails) */
    peak_open();

    /* Start playback thread last, so the error paths above need no join */
    if (pthread_create(&g_play_thread, NULL, play_loop, NULL) != 0) {
        fprintf(stderr, "pulse-alsa-bridge: failed to start playback thread\n");
        close(srv_fd); unlink(g_socket_path);
        goto err_alsa;
    }
    g_thread_started = 1;

    /* ---- Poll loop ---- */
    struct pollfd pfds[1 + MAX_CLIENTS];
    int           client_map[1 + MAX_CLIENTS];

    while (g_running) {
        int nfds = 0;
        pfds[nfds].fd     = srv_fd;
        pfds[nfds].events = POLLIN;
        client_map[nfds]  = -1;
        nfds++;

        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].active) {
                pfds[nfds].fd     = clients[i].fd;
                pfds[nfds].events = POLLIN;
                client_map[nfds]  = i;
                nfds++;
            }
        }

        int ret = poll(pfds, (unsigned)nfds, 10);
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }

        /* Proactive REQUEST: restart flow when ring drains below target but client
         * has no budget left (handle_audio won't fire until it writes more). */
        for (int i = 0; i < MAX_STREAMS; i++) {
            if (!streams[i].active || streams[i].corked || streams[i].client_fd < 0) continue;
            uint32_t bps = (streams[i].format == PA_SAMPLE_S16LE) ? 2u : 4u;
            uint32_t rchunk = BUF_TLENGTH / (bps * (uint32_t)streams[i].channels) * (uint32_t)sizeof(float);
            if (ring_read_space(rb_L[i]) < rchunk * 2u)
                send_request(streams[i].client_fd, i, BUF_TLENGTH);
        }

        /* Push captured monitor audio to any record clients. */
        pump_record_streams();

        /* New connections */
        if (pfds[0].revents & POLLIN) {
            int cfd = accept(srv_fd, NULL, NULL);
            if (cfd >= 0) {
                Client *cl = alloc_client(cfd);
                if (!cl) {
                    close(cfd);
                } else {
                    int flags = fcntl(cfd, F_GETFL, 0);
                    if (flags >= 0) fcntl(cfd, F_SETFL, flags | O_NONBLOCK);
                    fprintf(stderr, "pulse-alsa-bridge: client connected (fd=%d)\n", cfd);
                }
            }
        }

        /* Service existing clients */
        for (int pi = 1; pi < nfds; pi++) {
            if (!(pfds[pi].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            int ci = client_map[pi];
            if (ci < 0) continue;
            if (read_client(&clients[ci]) < 0) {
                fprintf(stderr, "pulse-alsa-bridge: client disconnected (fd=%d)\n",
                        clients[ci].fd);
                close_client(&clients[ci]);
            }
        }
    }

    /* ---- Cleanup ---- */
    fprintf(stderr, "pulse-alsa-bridge: shutting down\n");
    g_running = 0;
    pthread_join(g_play_thread, NULL);
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].active) close_client(&clients[i]);
    close(srv_fd);
    unlink(g_socket_path);
    peak_close();
    snd_pcm_drain(g_pcm);
    snd_pcm_close(g_pcm);
    for (int i = 0; i < MAX_STREAMS; i++) {
        ring_free(rb_L[i]);
        ring_free(rb_R[i]);
    }
    for (int i = 0; i < MAX_RECORD_STREAMS; i++) {
        ring_free(rstreams[i].rb);
        free(rstreams[i].tx);
    }
    return 0;

err_alsa:
    if (g_thread_started) {
        g_running = 0;
        pthread_join(g_play_thread, NULL);
    }
    peak_close();
    if (g_pcm) snd_pcm_close(g_pcm);
    for (int i = 0; i < MAX_STREAMS; i++) {
        ring_free(rb_L[i]);
        ring_free(rb_R[i]);
    }
    for (int i = 0; i < MAX_RECORD_STREAMS; i++) {
        ring_free(rstreams[i].rb);
        free(rstreams[i].tx);
    }
    return 1;
}
