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
 * No PulseAudio binary, no PipeWire, no JACK. Only libasound + pthread.
 * Opening the ALSA "default" device picks up the dmix (software mixing, so the
 * bridge coexists with other ALSA apps), plug (rate/format conversion) and the
 * equalizer chain that AlsaTune's .asoundrc configures.
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
#define PA_TAG_VOLUME       'V'
#define PA_TAG_TIMEVAL      'T'
#define PA_TAG_PROPLIST     'P'
#define PA_TAG_FORMAT_INFO  'f'   /* 0x66 — format_info type tag */

/* Sample format values (PA 14.2 src/pulse/sample.h) */
#define PA_SAMPLE_S16LE     3u
#define PA_SAMPLE_FLOAT32LE 5u

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
#define HDR_LEN       20   /* 5 × uint32 packet header */
#define TS_BUF        4096 /* tagstruct write buffer */
#define CONV_FRAMES   8192 /* scratch buffer frames for audio conversion */
#define RB_BYTES      (512u * 1024u) /* 512 KB ring buffer per channel/stream */
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
    rb->buf = malloc(p);
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
    rb->r = rb->w = 0;
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
    uint64_t     write_index; /* bytes accepted so far, returned in latency replies */
} PaStream;

static PaStream  streams[MAX_STREAMS];
static ring_t   *rb_L[MAX_STREAMS];
static ring_t   *rb_R[MAX_STREAMS];

/* Main-thread-only scratch buffers for PCM conversion */
static float conv_L[CONV_FRAMES];
static float conv_R[CONV_FRAMES];

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
} Client;

static Client clients[MAX_CLIENTS];

/* ======================================================================
 * Signal / shutdown
 * ====================================================================== */

static volatile int g_running = 1;
static char         g_socket_path[512];
static int          g_debug;   /* set once at startup from PULSE_BRIDGE_DEBUG env var */

static void sighandler(int s) { (void)s; g_running = 0; }

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
        /* key/value pairs (string + arbitrary) terminated by string-null */
        while (t->pos < t->len) {
            TR_NEED(t, 1);
            if (t->data[t->pos] == PA_TAG_STRING_NULL) { t->pos++; return 0; }
            if (tr_skip(t) < 0) return -1;  /* key */
            if (tr_skip(t) < 0) return -1;  /* value */
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
 * Stream allocation
 * ====================================================================== */

static int alloc_stream(void) {
    for (int i = 0; i < MAX_STREAMS; i++)
        if (!streams[i].active) return i;
    return -1;
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
    if (fmt != PA_SAMPLE_S16LE && fmt != PA_SAMPLE_FLOAT32LE) {
        send_error(cl->fd, tag, PA_ERR_NOTSUPPORTED);
        return;
    }
    if (ch < 1 || ch > 2) ch = 2;

    /* Read the PA_STREAM_START_CORKED flag from the request tagstruct.
     * Layout after sample_spec: channel_map, sink_index(u32), sink_name(str),
     * maxlength(u32), corked(bool).  At proto<13 there is a name string first,
     * but we only negotiate proto>=13 so that case is not reached. */
    int start_corked = 0;
    if (cl->proto < 13u) tr_skip(ts);  /* name string — never reached at proto>=13 */
    if (tr_skip(ts) == 0 &&   /* channel_map */
        tr_skip(ts) == 0 &&   /* sink_index u32 */
        tr_skip(ts) == 0 &&   /* sink_name str */
        tr_skip(ts) == 0 &&   /* maxlength u32 */
        ts->pos < ts->len) {
        uint8_t b = ts->data[ts->pos++];
        start_corked = (b != PA_TAG_BOOL_FALSE);
    }

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
    streams[slot].write_index = 0;
    __sync_synchronize();           /* publish slot fields before active=1 */
    streams[slot].active      = 1;  /* last: makes slot visible to playback thread */

    uint32_t alsa_rate = g_rate;
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
        tw_sample_spec(&rep, fmt, reply_ch, alsa_rate);
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
 * Audio data handler (called when channel != PA_CONTROL_CHANNEL)
 * ====================================================================== */

static void handle_audio(uint32_t channel, const uint8_t *data, uint32_t len) {
    if (channel >= MAX_STREAMS || !streams[channel].active) return;
    PaStream *s = &streams[channel];

    uint32_t bps         = (s->format == PA_SAMPLE_S16LE) ? 2u : 4u;
    uint32_t frame_bytes = bps * s->channels;
    if (frame_bytes == 0) return;

    uint32_t nframes = len / frame_bytes;
    if (nframes > CONV_FRAMES) nframes = CONV_FRAMES;
    if (nframes == 0) return;

    for (uint32_t i = 0; i < nframes; i++) {
        const uint8_t *f = data + i * frame_bytes;
        float l, r;
        if (s->format == PA_SAMPLE_S16LE) {
            int16_t ls = 0, rs = 0;
            memcpy(&ls, f, 2);
            if (s->channels >= 2) memcpy(&rs, f + 2, 2); else rs = ls;
            l = ls / 32768.0f;
            r = rs / 32768.0f;
        } else {
            l = 0.0f; r = 0.0f;
            memcpy(&l, f, 4);
            if (s->channels >= 2) memcpy(&r, f + 4, 4); else r = l;
        }
        conv_L[i] = l;
        conv_R[i] = r;
    }

    ring_write(rb_L[channel], (char *)conv_L, nframes * sizeof(float));
    ring_write(rb_R[channel], (char *)conv_R, nframes * sizeof(float));

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

    /* Request more audio only when the ring is below the target fill level.
     * ring_target: 2 BUF_TLENGTH chunks in ring-float-bytes per channel (~42 ms). */
    uint32_t bps_req = (s->format == PA_SAMPLE_S16LE) ? 2u : 4u;
    uint32_t ring_chunk = BUF_TLENGTH / (bps_req * (uint32_t)s->channels) * (uint32_t)sizeof(float);
    if (s->client_fd != -1 && !s->corked &&
        ring_read_space(rb_L[channel]) < ring_chunk * 2u)
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
        /* Bytes still buffered in ring: convert from float frames back to PA bytes */
        uint32_t bps = (s->format == PA_SAMPLE_S16LE) ? 2u : 4u;
        size_t rb_floats = ring_read_space(rb_L[idx]) / sizeof(float);
        uint64_t unread  = (uint64_t)rb_floats * bps * s->channels;
        read_idx = (write_idx > unread) ? write_idx - unread : 0;
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
    tw_u32(&rep, 0xFFFFFFFFu);               /* monitor source index: none */
    tw_str(&rep, NULL);                      /* monitor source name: none */
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
 * Command dispatcher
 * ====================================================================== */

static void dispatch(Client *cl, uint32_t channel,
                     const uint8_t *data, uint32_t len) {
    if (channel != PA_CONTROL_CHANNEL) {
        handle_audio(channel, data, len);
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

    /* Record stream: stub with NOTIMPLEMENTED so games don't hang */
    case PA_CMD_CREATE_RECORD_STREAM:
    case PA_CMD_DELETE_RECORD_STREAM:
        send_error(cl->fd, tag, PA_ERR_NOTIMPLEMENTED);
        break;

    /* No sources/capture: return NOENTITY for single-item queries, empty for lists */
    case PA_CMD_GET_SOURCE_INFO:
    case PA_CMD_GET_SOURCE_OUTPUT_INFO:
        send_error(cl->fd, tag, PA_ERR_NOENTITY);
        break;
    case PA_CMD_GET_SOURCE_INFO_LIST:
    case PA_CMD_GET_SOURCE_OUTPUT_INFO_LIST:
        send_reply_empty(cl->fd, tag);
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
        uint64_t widx = (idx < MAX_STREAMS && streams[idx].active)
                        ? streams[idx].write_index : 0u;
        TsW rep = {0};
        tw_u32(&rep, PA_CMD_REPLY);
        tw_u32(&rep, tag);
        tw_u64(&rep, 'r', widx);   /* write_index (PA_TAG_S64) */
        tw_u64(&rep, 'r', widx);   /* read_index = write_index: buffer flushed */
        send_cmd(cl->fd, &rep);
        if (idx < MAX_STREAMS && streams[idx].active && !streams[idx].corked)
            send_request(cl->fd, idx, BUF_TLENGTH);
        break;
    }

    /* Acknowledge-only commands */
    case PA_CMD_DRAIN_PLAYBACK_STREAM:
    case PA_CMD_PREBUF_PLAYBACK_STREAM:
    case PA_CMD_SET_DEFAULT_SINK:
    case PA_CMD_SET_DEFAULT_SOURCE:
    case 39u:  /* PA_COMMAND_SET_SINK_MUTE */
    case 40u:  /* PA_COMMAND_SET_SOURCE_MUTE */
    case PA_CMD_SET_SINK_INPUT_VOLUME:
    case PA_CMD_SET_SINK_INPUT_MUTE:
    case PA_CMD_MOVE_SINK_INPUT:
    case PA_CMD_KILL_SINK_INPUT:
    case PA_CMD_GET_CLIENT_INFO:
    case PA_CMD_GET_SINK_INPUT_INFO:
    case PA_CMD_GET_SINK_INPUT_INFO_LIST:
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
            for (snd_pcm_uframes_t j = 0; j < fr; j++) {
                outL[j] += tmpL[j];
                outR[j] += tmpR[j];
            }
        }

        /* Soft clip + float→S16 interleave, tracking the block peak for the meter */
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
        }
        peak_publish(peakL, peakR);

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

static int alsa_open(const char *dev) {
    int err;
    if ((err = snd_pcm_open(&g_pcm, dev, SND_PCM_STREAM_PLAYBACK, 0)) < 0) {
        fprintf(stderr,
            "pulse-alsa-bridge: cannot open ALSA device '%s': %s\n"
            "  Check your .asoundrc / sound card (try AlsaTune's sound-card switcher).\n",
            dev, snd_strerror(err));
        return -1;
    }

    snd_pcm_hw_params_t *hw;
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(g_pcm, hw);

    if ((err = snd_pcm_hw_params_set_access(g_pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0 ||
        (err = snd_pcm_hw_params_set_format(g_pcm, hw, SND_PCM_FORMAT_S16_LE)) < 0 ||
        (err = snd_pcm_hw_params_set_channels(g_pcm, hw, 2)) < 0) {
        fprintf(stderr, "pulse-alsa-bridge: hw params (access/format/channels): %s\n",
                snd_strerror(err));
        return -1;
    }

    unsigned int rate = 44100;   /* match the dmix lock in .asoundrc, avoid resampling */
    if ((err = snd_pcm_hw_params_set_rate_near(g_pcm, hw, &rate, 0)) < 0) {
        fprintf(stderr, "pulse-alsa-bridge: set rate: %s\n", snd_strerror(err));
        return -1;
    }

    snd_pcm_uframes_t period = 1024;   /* mirror the dmix slave */
    snd_pcm_uframes_t bufsz  = 4096;
    snd_pcm_hw_params_set_period_size_near(g_pcm, hw, &period, 0);
    snd_pcm_hw_params_set_buffer_size_near(g_pcm, hw, &bufsz);

    if ((err = snd_pcm_hw_params(g_pcm, hw)) < 0) {
        fprintf(stderr, "pulse-alsa-bridge: hw params commit: %s\n", snd_strerror(err));
        return -1;
    }

    snd_pcm_hw_params_get_period_size(hw, &period, NULL);
    snd_pcm_hw_params_get_buffer_size(hw, &bufsz);
    g_rate   = rate;
    g_period = period;
    g_bufsz  = bufsz;
    if (g_period > PLAY_MAX_FRAMES) {
        fprintf(stderr, "pulse-alsa-bridge: period %lu > %d frames, capping write chunk\n",
                (unsigned long)g_period, PLAY_MAX_FRAMES);
    }

    /* sw params: start once a buffer is queued, wake per period */
    snd_pcm_sw_params_t *sw;
    snd_pcm_sw_params_alloca(&sw);
    snd_pcm_sw_params_current(g_pcm, sw);
    snd_pcm_sw_params_set_start_threshold(g_pcm, sw, g_bufsz);
    snd_pcm_sw_params_set_avail_min(g_pcm, sw, g_period);
    snd_pcm_sw_params(g_pcm, sw);

    snd_pcm_prepare(g_pcm);
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

    signal(SIGTERM, sighandler);
    signal(SIGINT,  sighandler);
    signal(SIGPIPE, SIG_IGN);

    /* Open ALSA (optional device override via env) */
    const char *dev = getenv("PULSE_BRIDGE_ALSA_DEV");
    if (!dev || !*dev) dev = "default";
    if (alsa_open(dev) < 0)
        return 1;

    /* Buffer sizing relative to ALSA period (kept in float units, like the
     * JACK variant, because the PA flow-control math is expressed that way) */
    uint32_t period_bytes = (uint32_t)g_period * 2u * (uint32_t)sizeof(float);
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
    return 1;
}
