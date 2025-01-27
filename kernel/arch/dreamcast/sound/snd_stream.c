/* KallistiOS ##version##

   snd_stream.c
   Copyright (C) 2000, 2001, 2002, 2003, 2004 Megan Potter
   Copyright (C) 2002 Florian Schulze
   Copyright (C) 2020 Lawrence Sebald
   Copyright (C) 2023 Ruslan Rostovtsev

   SH-4 support routines for SPU streaming sound driver
*/

#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <malloc.h>
#include <sys/queue.h>

#include <kos/mutex.h>
#include <arch/cache.h>
#include <arch/timer.h>
#include <dc/g2bus.h>
#include <dc/sq.h>
#include <dc/spu.h>
#include <dc/sound/sound.h>
#include <dc/sound/stream.h>
#include <dc/sound/sfxmgr.h>

#include "arm/aica_cmd_iface.h"

/*

This module uses a nice circularly queued data stream in SPU RAM, which is
looped by a program running in the SPU itself.

Basically the poll routine checks to see if a certain minimum amount of
data is available to the SPU to be played, and if not, we ask the user
routine for more sound data and load it up. That's about it.

This version is capable of playing back N streams at once, with the limit
being available CPU time and channels.

*/

typedef struct filter {
    TAILQ_ENTRY(filter) lent;
    snd_stream_filter_t func;
    void *data;
} filter_t;

/* Each of these represents an active streaming channel */
typedef struct strchan {
    /* Which AICA channels are we using? */
    int ch[2];

    /* The last write position in the playing buffer */
    uint32_t last_write_pos;

    /* The buffer size allocated for this stream. */
    size_t buffer_size;

    /* Stream data location in AICA RAM */
    uint32_t spu_ram_sch[2];

    /* "Get data" callback; we'll call this any time we want to get
       another buffer of output data. */
    snd_stream_callback_t get_data;

    /* Our list of filter callback functions for this stream */
    TAILQ_HEAD(filterlist, filter) filters;

    /* Sample type */
    int type;

    /* Sample size */
    int bitsize;

    /* Stereo/mono flag */
    int channels;

    /* Playback frequency */
    int frequency;

    /* Stream queueing is where we get everything ready to go but don't
       actually start it playing until the signal (for music sync, etc) */
    int queueing;

    /* Have we been initialized yet? (and reserved a buffer, etc) */
    volatile int initted;

    /* User data. */
    void *user_data;
} strchan_t;

/* Our stream structs */
static strchan_t streams[SND_STREAM_MAX];

/* Separation buffers (for stereo) */
static uint32_t *sep_buffer[2] = {NULL, NULL};

static mutex_t stream_mutex = MUTEX_INITIALIZER;

#define LOCK_TIMEOUT_MS 1000

/* Check an incoming handle */
#define CHECK_HND(x) do { \
        assert( (x) >= 0 && (x) < SND_STREAM_MAX ); \
        assert( streams[(x)].initted ); \
    } while(0)

void snd_pcm16_split_sq_start(uint32_t *data, uintptr_t left, uintptr_t right, size_t size);

static inline size_t samples_to_bytes(snd_stream_hnd_t hnd, size_t samples) {
    switch(streams[hnd].bitsize) {
        case 4:
            return samples >> 1;
        case 8:
            return samples;
        case 16:
        default:
            return samples << 1;
    }
}

static inline size_t bytes_to_samples(snd_stream_hnd_t hnd, size_t bytes) {
    switch(streams[hnd].bitsize) {
        case 4:
            return bytes << 1;
        case 8:
            return bytes;
        case 16:
        default:
            return bytes >> 1;
    }
}

/* Set "get data" callback */
void snd_stream_set_callback(snd_stream_hnd_t hnd, snd_stream_callback_t cb) {
    CHECK_HND(hnd);
    streams[hnd].get_data = cb;
}

void snd_stream_set_userdata(snd_stream_hnd_t hnd, void *d) {
    CHECK_HND(hnd);
    streams[hnd].user_data = d;
}

void *snd_stream_get_userdata(snd_stream_hnd_t hnd) {
    CHECK_HND(hnd);
    return streams[hnd].user_data;
}

void snd_stream_filter_add(snd_stream_hnd_t hnd, snd_stream_filter_t filtfunc, void * obj) {
    filter_t *f;

    CHECK_HND(hnd);

    f = malloc(sizeof(filter_t));
    f->func = filtfunc;
    f->data = obj;
    TAILQ_INSERT_TAIL(&streams[hnd].filters, f, lent);
}

void snd_stream_filter_remove(snd_stream_hnd_t hnd, snd_stream_filter_t filtfunc, void * obj) {
    filter_t *f;

    CHECK_HND(hnd);

    TAILQ_FOREACH(f, &streams[hnd].filters, lent) {
        if(f->func == filtfunc && f->data == obj) {
            TAILQ_REMOVE(&streams[hnd].filters, f, lent);
            free(f);
            return;
        }
    }
}

static void process_filters(snd_stream_hnd_t hnd, void **buffer, int *samplecnt) {
    filter_t *f;

    TAILQ_FOREACH(f, &streams[hnd].filters, lent) {
        f->func(hnd, f->data, streams[hnd].frequency, streams[hnd].channels, buffer, samplecnt);
    }
}

static void snd_pcm16_split_unaligned(void *buffer, void *left, void *right, size_t len) {
    uint32_t *buf = (uint32_t *)buffer;
    uint32_t *left_ptr = (uint32_t *)left;
    uint32_t *right_ptr = (uint32_t *)right;
    uint32_t data;
    uint32_t left_val;
    uint32_t right_val;

    for(; len > 8; len -= 8) {
        dcache_pref_block(buf + 8);

        data = *buf++;
        left_val = (data >> 16);
        right_val = (data & 0xffff);

        data = *buf++;
        left_val |= (data & 0xffff0000);
        right_val |= (data & 0xffff) << 16;

        if(((uintptr_t)left_ptr & 31) == 0) {
            dcache_alloc_block(left_ptr++, left_val);
            dcache_alloc_block(right_ptr++, right_val);
        }
        else {
            *left_ptr++ = left_val;
            *right_ptr++ = right_val;
        }
    }
    if(len) {
        data = *buf++;
        *(uint16_t *)left_ptr = (data >> 16);
        *(uint16_t *)right_ptr = (data & 0xffff);
    }
}

void snd_pcm16_split_sq(uint32_t *data, uintptr_t left, uintptr_t right, size_t size) {
    g2_ctx_t ctx;

    /* SPU memory in cached area */
    left |= SPU_RAM_BASE;
    right |= SPU_RAM_BASE;

    uintptr_t masked_left = SQ_MASK_DEST_ADDR(left);
    uintptr_t masked_right = SQ_MASK_DEST_ADDR(right);

    sq_lock();

    /* Set store queue memory area as desired */
    SET_QACR_REGS(left, right);

    /* Make sure the FIFOs are empty */
    ctx = g2_lock();

    /* Separating channels and do fill/write queues as many times necessary. */
    snd_pcm16_split_sq_start(data, masked_left, masked_right, size);

    /* Wait for both store queues to complete if they are already used */
    uint32_t *d = (uint32_t *)MEM_AREA_SQ_BASE;
    d[0] = d[8] = 0;

    g2_unlock(ctx);
    sq_unlock();
}

static void snd_stream_prefill_part(snd_stream_hnd_t hnd, uint32_t offset) {
    const size_t buffer_size = streams[hnd].buffer_size;
    const int chans = streams[hnd].channels;
    const uintptr_t left = streams[hnd].spu_ram_sch[0] + offset;
    const uintptr_t right = streams[hnd].spu_ram_sch[1] + offset;
    const int max_got = (buffer_size / 2) * chans;
    int got = max_got;
    void *buf = streams[hnd].get_data(hnd, max_got, &got);

    if(buf == NULL) {
        dbglog(DBG_ERROR, "snd_stream_prefill_part(): get_data() failed\n");
        return;
    }

    if(got > max_got) {
        got = max_got;
    }

    process_filters(hnd, &buf, &got);

    if(chans == 1) {
        spu_memload_sq(left, buf, got);
        spu_memload_sq(right, buf, got);
        return;
    }

    if(streams[hnd].bitsize == 16) {
        if((uintptr_t)buf & 31) {
            snd_pcm16_split_unaligned(buf, sep_buffer[0], sep_buffer[1], got);
        }
        else {
            snd_pcm16_split_sq((uint32_t *)buf, left, right, got);
            return;
        }
    }
    else if(streams[hnd].bitsize == 8) {
        snd_pcm8_split(buf, sep_buffer[0], sep_buffer[1], got);
    }
    else if(streams[hnd].bitsize == 4) {
        snd_adpcm_split(buf, sep_buffer[0], sep_buffer[1], got);
    }

    spu_memload_sq(left, sep_buffer[0], got / 2);
    spu_memload_sq(right, sep_buffer[1], got / 2);
}

/* Prefill buffers -- implicitly called by snd_stream_start() */
void snd_stream_prefill(snd_stream_hnd_t hnd) {
    CHECK_HND(hnd);

    if(!streams[hnd].get_data) {
        return;
    }

    mutex_lock_timed(&stream_mutex, LOCK_TIMEOUT_MS);
    snd_stream_prefill_part(hnd, 0);
    snd_stream_prefill_part(hnd, streams[hnd].buffer_size / 2);

    /* Start playing from the beginning */
    streams[hnd].last_write_pos = 0;
    mutex_unlock(&stream_mutex);
}

/* Initialize stream system */
int snd_stream_init(void) {
    /* Create stereo separation buffers */
    if(!sep_buffer[0]) {
        sep_buffer[0] = memalign(32, SND_STREAM_BUFFER_MAX);
        sep_buffer[1] = sep_buffer[0] + (SND_STREAM_BUFFER_MAX / 8);
    }

    /* Finish loading the stream driver */
    if(snd_init() < 0) {
        dbglog(DBG_ERROR, "snd_stream_init(): snd_init() failed, giving up\n");
        return -1;
    }

    return 0;
}

snd_stream_hnd_t snd_stream_alloc(snd_stream_callback_t cb, int bufsize) {
    int i;
    snd_stream_hnd_t hnd;

    mutex_lock_timed(&stream_mutex, LOCK_TIMEOUT_MS);

    /* Get an unused handle */
    hnd = -1;

    for(i = 0; i < SND_STREAM_MAX; i++) {
        if(!streams[i].initted) {
            hnd = i;
            break;
        }
    }

    if(hnd != -1)
        streams[hnd].initted = 1;

    if(hnd == -1) {
        mutex_unlock(&stream_mutex);
        return SND_STREAM_INVALID;
    }

    /* Default this for now */
    streams[hnd].buffer_size = bufsize;

    /* Start off with queueing disabled */
    streams[hnd].queueing = 0;

    /* Setup the callback */
    snd_stream_set_callback(hnd, cb);

    /* Initialize our filter chain list */
    TAILQ_INIT(&streams[hnd].filters);

    /* Allocate stream buffers */
    streams[hnd].spu_ram_sch[0] = snd_mem_malloc(streams[hnd].buffer_size * 2);
    streams[hnd].spu_ram_sch[1] = streams[hnd].spu_ram_sch[0] + streams[hnd].buffer_size;

    /* And channels */
    streams[hnd].ch[0] = snd_sfx_chn_alloc();
    streams[hnd].ch[1] = snd_sfx_chn_alloc();
    dbglog(DBG_INFO, "snd_stream: alloc'd channels %d/%d\n", streams[hnd].ch[0], streams[hnd].ch[1]);

    mutex_unlock(&stream_mutex);

    return hnd;
}

snd_stream_hnd_t snd_stream_reinit(snd_stream_hnd_t hnd, snd_stream_callback_t cb) {
    CHECK_HND(hnd);

    /* Start off with queueing disabled */
    streams[hnd].queueing = 0;

    /* Setup the callback */
    snd_stream_set_callback(hnd, cb);

    return hnd;
}

void snd_stream_destroy(snd_stream_hnd_t hnd) {
    filter_t *c, *n;

    assert(hnd >= 0 && hnd < SND_STREAM_MAX);
    mutex_lock_timed(&stream_mutex, LOCK_TIMEOUT_MS);

    if(!streams[hnd].initted) {
        mutex_unlock(&stream_mutex);
        return;
    }

    snd_sfx_chn_free(streams[hnd].ch[0]);
    snd_sfx_chn_free(streams[hnd].ch[1]);

    c = TAILQ_FIRST(&streams[hnd].filters);

    while(c) {
        n = TAILQ_NEXT(c, lent);
        free(c);
        c = n;
    }

    TAILQ_INIT(&streams[hnd].filters);

    snd_stream_stop(hnd);
    snd_mem_free(streams[hnd].spu_ram_sch[0]);
    dbglog(DBG_INFO, "snd_stream: dealloc'd channels %d/%d\n", streams[hnd].ch[0], streams[hnd].ch[1]);
    memset(streams + hnd, 0, sizeof(streams[0]));

    mutex_unlock(&stream_mutex);
}

/* Shut everything down and free mem */
void snd_stream_shutdown(void) {
    /* Stop and destroy all active stream */
    int i;

    for(i = 0; i < SND_STREAM_MAX; i++) {
        if(streams[i].initted)
            snd_stream_destroy(i);
    }

    /* Free global buffers */
    if(sep_buffer[0]) {
        free(sep_buffer[0]);
        sep_buffer[0] = NULL;
        sep_buffer[1] = NULL;
    }
}

/* Enable / disable stream queueing */
void snd_stream_queue_enable(snd_stream_hnd_t hnd) {
    CHECK_HND(hnd);
    streams[hnd].queueing = 1;
}

void snd_stream_queue_disable(snd_stream_hnd_t hnd) {
    CHECK_HND(hnd);
    streams[hnd].queueing = 0;
}

/* Start streaming (or if queueing is enabled, just get ready) */
static void snd_stream_start_type(snd_stream_hnd_t hnd, uint32_t type, uint32_t freq, int st) {
    AICA_CMDSTR_CHANNEL(tmp, cmd, chan);

    CHECK_HND(hnd);

    if(!streams[hnd].get_data) return;

    streams[hnd].type = type;
    streams[hnd].channels = st ? 2 : 1;
    streams[hnd].frequency = freq;

    if(streams[hnd].type == AICA_SM_16BIT) {
        streams[hnd].bitsize = 16;
    }
    else if(streams[hnd].type == AICA_SM_8BIT) {
        streams[hnd].bitsize = 8;
    }
    else if(streams[hnd].type == AICA_SM_ADPCM_LS) {
        streams[hnd].bitsize = 4;

        if(streams[hnd].buffer_size > (32 << 10)) {
            /* The channel position data size is 16-bits.
                Need to make sure that we don't go beyond these limits */
            streams[hnd].buffer_size = (32 << 10);
        }
    }

    /* Make sure these are sync'd (and/or delayed) */
    snd_sh4_to_aica_stop();

    /* Prefill buffers */
    snd_stream_prefill(hnd);

    /* Channel 0 */
    cmd->cmd = AICA_CMD_CHAN;
    cmd->timestamp = 0;
    cmd->size = AICA_CMDSTR_CHANNEL_SIZE;
    cmd->cmd_id = streams[hnd].ch[0];
    chan->cmd = AICA_CH_CMD_START | AICA_CH_START_DELAY;
    chan->base = streams[hnd].spu_ram_sch[0];
    chan->type = type;
    chan->length = bytes_to_samples(hnd, streams[hnd].buffer_size);
    chan->loop = 1;
    chan->loopstart = 0;
    chan->loopend = chan->length - 1;
    chan->freq = freq;
    chan->vol = 255;
    chan->pan = streams[hnd].channels == 2 ? 0 : 128;
    snd_sh4_to_aica(tmp, cmd->size);

    if(streams[hnd].channels == 2) {
        /* Channel 1 */
        cmd->cmd_id = streams[hnd].ch[1];
        chan->base = streams[hnd].spu_ram_sch[1];
        chan->pan = 255;
        snd_sh4_to_aica(tmp, cmd->size);

        /* Start both channels simultaneously */
        cmd->cmd_id = (1 << streams[hnd].ch[0]) |
                      (1 << streams[hnd].ch[1]);
    }
    else {
        /* Start one channel */
        cmd->cmd_id = (1 << streams[hnd].ch[0]);
    }

    chan->cmd = AICA_CH_CMD_START | AICA_CH_START_SYNC;
    snd_sh4_to_aica(tmp, cmd->size);

    /* Process the changes */
    if(!streams[hnd].queueing)
        snd_sh4_to_aica_start();
}

void snd_stream_start(snd_stream_hnd_t hnd, uint32_t freq, int st) {
    snd_stream_start_type(hnd, AICA_SM_16BIT, freq, st);
}

void snd_stream_start_pcm8(snd_stream_hnd_t hnd, uint32_t freq, int st) {
    snd_stream_start_type(hnd, AICA_SM_8BIT, freq, st);
}

void snd_stream_start_adpcm(snd_stream_hnd_t hnd, uint32_t freq, int st) {
    snd_stream_start_type(hnd, AICA_SM_ADPCM_LS, freq, st);
}

/* Actually make it go (in queued mode) */
void snd_stream_queue_go(snd_stream_hnd_t hnd) {
    CHECK_HND(hnd);
    snd_sh4_to_aica_start();
}

/* Stop streaming */
void snd_stream_stop(snd_stream_hnd_t hnd) {
    AICA_CMDSTR_CHANNEL(tmp, cmd, chan);

    CHECK_HND(hnd);

    if(!streams[hnd].get_data) return;

    /* Stop stream */
    /* Channel 0 */
    cmd->cmd = AICA_CMD_CHAN;
    cmd->timestamp = 0;
    cmd->size = AICA_CMDSTR_CHANNEL_SIZE;
    cmd->cmd_id = streams[hnd].ch[0];
    chan->cmd = AICA_CH_CMD_STOP;
    snd_sh4_to_aica(tmp, cmd->size);

    if(streams[hnd].channels == 2) {
        /* Channel 1 */
        cmd->cmd_id = streams[hnd].ch[1];
        snd_sh4_to_aica(tmp, cmd->size);
    }
}

/* The DMA will chain to this to start the second DMA. */
static uint32_t dmacnt;
static uintptr_t dmadest;
static inline void dma_done(void *data) {
    (void)data;
    mutex_unlock(&stream_mutex);
}

static inline void dma_chain(void *data) {
    (void)data;
    spu_dma_transfer(sep_buffer[1], dmadest, dmacnt, 0, dma_done, 0);
}

/* Poll streamer to load more data if neccessary */
int snd_stream_poll(snd_stream_hnd_t hnd) {
    uint32_t ch0pos, ch1pos, write_pos;
    uint16_t current_play_pos;
    int needed_samples = 0;
    int needed_bytes = 0;
    int got_bytes = 0;
    void *data;
    void *first_dma_buf = sep_buffer[0];
    strchan_t *stream;

    assert(hnd >= 0 && hnd < SND_STREAM_MAX);
    mutex_lock_timed(&stream_mutex, LOCK_TIMEOUT_MS);

    stream = &streams[hnd];

    if(!stream->initted || !stream->get_data) {
        mutex_unlock(&stream_mutex);
        return -1;
    }

    /* Get channels position */
    ch0pos = g2_read_32(SPU_RAM_UNCACHED_BASE +
                        AICA_CHANNEL(stream->ch[0]) +
                        offsetof(aica_channel_t, pos));

    if(stream->channels == 2) {
        ch1pos = g2_read_32(SPU_RAM_UNCACHED_BASE +
                    AICA_CHANNEL(stream->ch[1]) +
                    offsetof(aica_channel_t, pos));
        /* The channel position register is 16-bit on AICA side, keep it in mind */
        current_play_pos = (ch0pos < ch1pos ? ch0pos : ch1pos) & 0xffff;
    }
    else {
        current_play_pos = (ch0pos & 0xffff);
    }

    if(samples_to_bytes(hnd, current_play_pos) >= stream->buffer_size) {
        dbglog(DBG_ERROR, "snd_stream_poll: chan0(%d).pos = %ld\n", stream->ch[0], ch0pos);
        mutex_unlock(&stream_mutex);
        return -1;
    }

    /* Count just till the end of the buffer, so we don't have to
       handle buffer wraps */
    if(stream->last_write_pos <= current_play_pos) {
        needed_samples = current_play_pos - stream->last_write_pos - 1;
        /* Round it to max sector size of supported storage devices */
        needed_samples &= ~(bytes_to_samples(hnd, 2048 / stream->channels) - 1);
    }
    else {
        needed_samples = bytes_to_samples(hnd, stream->buffer_size);
        needed_samples -= stream->last_write_pos;
    }

    if(needed_samples <= 0) {
        mutex_unlock(&stream_mutex);
        return 0;
    }

    needed_bytes = samples_to_bytes(hnd, needed_samples);

    if((uint32_t)needed_bytes > stream->buffer_size / stream->channels) {
        needed_bytes = (int)stream->buffer_size / stream->channels;
    }

    data = stream->get_data(hnd, needed_bytes * stream->channels, &got_bytes);
    process_filters(hnd, &data, &got_bytes);

    if(got_bytes < needed_bytes * stream->channels) {
        needed_bytes = got_bytes / stream->channels;
    }

    /* Round it a bit */
    if(needed_bytes & 3) {
        needed_bytes = (needed_bytes + 4) & ~3;
    }

    needed_samples = bytes_to_samples(hnd, needed_bytes);
    write_pos = samples_to_bytes(hnd, stream->last_write_pos);

    if(data == NULL) {
        /* Fill with zeros */
        spu_memset(stream->spu_ram_sch[0] + write_pos, 0, needed_bytes);
        spu_memset(stream->spu_ram_sch[1] + write_pos, 0, needed_bytes);
        mutex_unlock(&stream_mutex);
        return -3;
    }

    if(stream->channels == 2) {
        sep_buffer[1] = sep_buffer[0] + (SND_STREAM_BUFFER_MAX / 8);

        if(streams[hnd].bitsize == 16) {
            if((uintptr_t)data & 31) {
                snd_pcm16_split_unaligned(data,
                    sep_buffer[0], sep_buffer[1], needed_bytes * 2);
            }
            else {
                snd_pcm16_split((uint32_t *)data,
                    sep_buffer[0], sep_buffer[1], needed_bytes * 2);
            }
        }
        else if(streams[hnd].bitsize == 8) {
            snd_pcm8_split(data, sep_buffer[0], sep_buffer[1], needed_bytes * 2);
        }
        else if(streams[hnd].bitsize == 4) {
            snd_adpcm_split(data, sep_buffer[0], sep_buffer[1], needed_bytes * 2);
        }

        dcache_purge_range((uintptr_t)sep_buffer[0], needed_bytes);
        dcache_purge_range((uintptr_t)sep_buffer[1], needed_bytes);

        /* Second DMA will get started by the chain handler */
        dmadest = stream->spu_ram_sch[1] + write_pos;
        dmacnt = needed_bytes;
        spu_dma_transfer(first_dma_buf,
            stream->spu_ram_sch[0] + write_pos, needed_bytes, 0, dma_chain, 0);
    }
    else {
        if((uintptr_t)data & 31) {
            memcpy(sep_buffer[0], data, needed_bytes);
        }
        else {
            first_dma_buf = data;
        }
        dcache_purge_range((uintptr_t)first_dma_buf, needed_bytes);
        spu_dma_transfer(first_dma_buf,
            stream->spu_ram_sch[0] + write_pos, needed_bytes, 0, dma_done, 0);
    }

    stream->last_write_pos += needed_samples;
    write_pos = (uint32_t)bytes_to_samples(hnd, stream->buffer_size);

    if(stream->last_write_pos >= write_pos) {
        stream->last_write_pos -= write_pos;
    }

    return 0;
}

/* Set the volume on the streaming channels */
void snd_stream_volume(snd_stream_hnd_t hnd, int vol) {
    AICA_CMDSTR_CHANNEL(tmp, cmd, chan);

    CHECK_HND(hnd);

    cmd->cmd = AICA_CMD_CHAN;
    cmd->timestamp = 0;
    cmd->size = AICA_CMDSTR_CHANNEL_SIZE;
    cmd->cmd_id = streams[hnd].ch[0];
    chan->cmd = AICA_CH_CMD_UPDATE | AICA_CH_UPDATE_SET_VOL;
    chan->vol = vol;
    snd_sh4_to_aica(tmp, cmd->size);

    cmd->cmd_id = streams[hnd].ch[1];
    snd_sh4_to_aica(tmp, cmd->size);
}
