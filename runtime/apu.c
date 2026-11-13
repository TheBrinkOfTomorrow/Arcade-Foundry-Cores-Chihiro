// The MCPX audio processor (APU, 0xFE800000), as DirectSound drives it.
//
// The voice processor (VP, +0x20000) takes methods through a PIO FIFO that
// set up voices: 128-byte records in RAM (at NV_PAPU_VPVADDR) linked into
// three lists (2D, 3D, multipass), each pointing at its samples through a
// scatter-gather table (buffers) or a list of stream segments (streams). The
// hardware runs a frame of 32 samples at 48 kHz (1500 a second): every
// active voice is stepped through its envelopes, resampled to its pitch,
// and mixed into up to 8 of 32 mix bins with a volume each; the global
// processor, a DSP running code DirectSound loads, then mixes the bins down
// (with effects) and the encode processor sends the result out.
//
// Here the frames run on the interrupt thread, in step with the core's clock
// (real time, or in render mode 1/60 s per video frame; clock.c), and
// the DSPs are not run: each voice goes straight to a stereo output at the
// loudest of its bin volumes. Voices that end raise their notifier and an
// interrupt; the frame counter (XGSCNT) counts the samples played, which
// DirectSound and the movie player time themselves by.
#include "af_debug.h"
#include "xbox.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define APU (0xFE800000u)
#define APU_LEVEL 5

// registers
#define ISTS 0x1000      // interrupt status, written 1 to clear
#define ISTS_GINT (1u << 0)
#define ISTS_FETINT (1u << 4)
#define ISTS_FENINT (1u << 5)
#define ISTS_FEVINT (1u << 6)
#define IEN 0x1004
#define FECTL 0x1100
#define FECTL_MODE 0xe0
#define FECTL_HALTED 0x80
#define FECTL_TRAPPED 0xe0
#define FECTL_TRAPREASON 0xf00
#define FECV 0x1110      // current voice
#define FEAV 0x1118      // antecedent voice
#define FENADDR 0x115c   // notifier memory
#define FEDECMETH 0x1300
#define FEDECPARAM 0x1304
#define FEMEMADDR 0x1324
#define FEMEMDATA 0x1334
#define FETFORCE1 0x1504
#define FETFORCE1_IDLE_VOICE (1u << 15)
#define SECTL 0x2000
#define XGSCNT 0x200c
#define VPVADDR 0x202c
#define VPSGEADDR 0x2030
#define VPSSLADDR 0x2034
static const uint32_t list_top[3] = { 0x2054, 0x2060, 0x206c };   // 2D, 3D, multipass
static const uint32_t list_cur[3] = { 0x2058, 0x2064, 0x2070 };
static const uint32_t list_next[3] = { 0x205c, 0x2068, 0x2074 };

// VP methods (offsets in the PIO window at +0x20000)
#define VP 0x20000
#define M_FREE 0x010
#define M_ANTECEDENT 0x120
#define M_VOICE_ON 0x124
#define M_VOICE_OFF 0x128
#define M_VOICE_RELEASE 0x12c
#define M_VOICE_PAUSE 0x140
#define M_SET_CURRENT_SSL 0x190
#define M_SUBMIX_HEADROOM 0x200   // + 4 per bin
#define M_HRTF_HEADROOM 0x280
#define M_HRTF_SUBMIXES 0x2c0
#define M_CURRENT_VOICE 0x2f8
#define M_VOICE_LOCK 0x2fc
#define M_SSL_A 0x320
#define M_SSL_B 0x35c
#define M_TAR_PITCH 0x37c
#define M_INBUF_SGE 0x804
#define M_INBUF_SGE_OFFSET 0x808
#define M_SSL_SEGMENT 0x600       // offset/length pairs, 64 of them
#define M_OUTBUF_SGE 0x1800
#define M_OUTBUF_SGE_OFFSET 0x1808
#define SE2FE_IDLE_VOICE 0x8000

// a voice record (NV_PAVS)
#define V_VBIN 0x00
#define V_FMT 0x04
#define V_ENV0 0x08
#define V_ENVA 0x0c
#define V_ENV1 0x10
#define V_ENVF 0x14
#define V_MISC 0x18
#define V_HRTF 0x1c
#define V_PSL_START 0x20   // buffer base (24 bits)
#define V_PSH_SAMPLE 0x24  // loop start (24 bits)
#define V_ECNT 0x34        // envelope counters: amplitude low 16, filter high 16
#define V_STATE 0x54
#define V_OFFSET 0x58      // current sample (24 bits), amplitude level (high 8)
#define V_NEXT 0x5c        // end sample (24 bits), filter level (high 8)
#define V_VOLA 0x60
#define V_VOLB 0x64
#define V_VOLC 0x68
#define V_LFO_ENV 0x6c     // amplitude release rate (12 bits)
#define V_PITCH_LINK 0x7c  // next voice (low 16), pitch (high 16)

#define ST_PAUSED (1u << 18)
#define ST_NEW (1u << 20)
#define ST_ACTIVE (1u << 21)
enum { ENV_OFF, ENV_DELAY, ENV_ATTACK, ENV_HOLD, ENV_DECAY, ENV_SUSTAIN, ENV_RELEASE, ENV_FORCE_RELEASE };

#define FMT_MULTIPASS (1u << 21)
#define FMT_PERSIST (1u << 23)
#define FMT_STREAM (1u << 24)
#define FMT_LOOP (1u << 25)
#define FMT_STEREO (1u << 27)

#define VOICES 256
#define VOICES_3D 64
#define FRAME 32
#define RATE 48000

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int ready;
static int frames_on;          // CH_NOSOUND turns the frames off
static uint64_t frame_count;   // frames run: XGSCNT = 32 per frame
static uint64_t aci_frames;    // the codec's frames, in real time
static uint64_t apu_last;      // the real-time frame the APU last ran
static uint64_t start_ns;
static uint32_t ssl_page, inbuf_sge, outbuf_sge;
static uint8_t submix_headroom[32], hrtf_headroom, hrtf_submix[4];

// per voice: which stream segment list and segment it is in, and the
// resampler's state (the next source position and the two samples around it)
static struct {
    uint32_t ssl_base[2];
    uint8_t ssl_count[2];
    int ssl_index, ssl_seg;
    double frac;
    float s0[2], s1[2];
    int primed;
    int adpcm_block;
    int16_t adpcm[65 * 2];
} vs[VOICES];

// ---- registers and memory -------------------------------------------------------------------------
static uint32_t rd(uint32_t o) { return reg_rd32(APU + o); }
static void wr(uint32_t o, uint32_t v) { reg_wr32(APU + o, v); }
static uint32_t prd32(uint32_t pa) { uint32_t v; memcpy(&v, M + dma_addr(pa), 4); return v; }
static void pwr32(uint32_t pa, uint32_t v) { memcpy(M + dma_addr(pa), &v, 4); }
static uint8_t *pptr(uint32_t pa) { return M + dma_addr(pa); }

static uint32_t vaddr(int v, uint32_t off) { return rd(VPVADDR) + v * 0x80 + off; }
static uint32_t vget(int v, uint32_t off) { return prd32(vaddr(v, off)); }
static void vset(int v, uint32_t off, uint32_t val) { pwr32(vaddr(v, off), val); }
static uint32_t vfield(int v, uint32_t off, int sh, uint32_t mask) { return vget(v, off) >> sh & mask; }
static void vsetf(int v, uint32_t off, int sh, uint32_t mask, uint32_t val)
{
    vset(v, off, (vget(v, off) & ~(mask << sh)) | (val & mask) << sh);
}

static void update_irq(void)
{
    uint32_t ists = rd(ISTS);
    if ((rd(FECTL) & FECTL_MODE) == FECTL_TRAPPED) ists |= ISTS_FETINT;
    if ((rd(IEN) & ISTS_GINT) && (ists & ~ISTS_GINT & rd(IEN))) ists |= ISTS_GINT;
    else ists &= ~ISTS_GINT;
    wr(ISTS, ists);
    if (ists & ISTS_GINT) ke_raise_interrupt(APU_LEVEL);
}

// a voice's notifier: 16 bytes each, four per voice after two global ones;
// the last byte is the status, the one before it the envelope state
static void notify(int v, int which)
{
    uint32_t a = rd(FENADDR) + 16 * (2 + v * 4 + which) + 15;
    *pptr(a) = 1;        // done, success
    *pptr(a - 1) = 1;
    wr(ISTS, rd(ISTS) | ISTS_FEVINT | ISTS_FENINT);
}

static void voice_off(int v)
{
    vsetf(v, V_STATE, 0, ST_ACTIVE, 0);
    int which = vget(v, V_FMT) & FMT_STREAM ? vs[v].ssl_index : 0;
    notify(v, which);
}

// ---- methods ------------------------------------------------------------------------------------
static void env_start(int v, int shift, uint32_t cfg0, uint32_t cfga, int state)
{
    vsetf(v, V_STATE, shift, 0xf, state);
    uint32_t count = 0;
    if (state == ENV_DELAY) count = (vget(v, cfg0) >> 12 & 0xfff) * 16;
    else if (state == ENV_HOLD) count = (vget(v, cfga) >> 12 & 0xfff) * 16;
    vsetf(v, V_ECNT, shift == 28 ? 0 : 16, 0xffff, count);
}

static void method(uint32_t m, uint32_t arg)
{
    wr(FEDECMETH, m);
    wr(FEDECPARAM, arg);
    int cur = rd(FECV) & 0xffff;
    switch (m) {
    case M_ANTECEDENT: wr(FEAV, arg); break;
    case M_CURRENT_VOICE: wr(FECV, arg); break;
    case M_VOICE_LOCK: break;   // the frames run under the same lock as the methods
    case M_VOICE_ON: {
        int v = arg & 0xffff, list = rd(FEAV) >> 16 & 3;
        if (v >= VOICES) break;
        if (list) {   // to the top of a list
            vsetf(v, V_PITCH_LINK, 0, 0xffff, rd(list_top[list - 1]));
            wr(list_top[list - 1], v);
        } else {      // after the antecedent voice
            int a = rd(FEAV) & 0xffff;
            if (a < VOICES) {
                vsetf(v, V_PITCH_LINK, 0, 0xffff, vfield(a, V_PITCH_LINK, 0, 0xffff));
                vsetf(a, V_PITCH_LINK, 0, 0xffff, v);
            }
        }
        vsetf(v, V_OFFSET, 0, 0xffffff, 0);
        vs[v].ssl_index = vs[v].ssl_seg = 0;
        vs[v].frac = 0;
        vs[v].primed = 0;
        vs[v].adpcm_block = -1;
        env_start(v, 28, V_ENV0, V_ENVA, arg >> 28);
        env_start(v, 24, V_ENV1, V_ENVF, arg >> 24 & 0xf);
        vsetf(v, V_STATE, 0, ST_ACTIVE, ST_ACTIVE);
        break;
    }
    case M_VOICE_RELEASE: {
        int v = arg & 0xffff;
        if (v >= VOICES) break;
        vsetf(v, V_ECNT, 0, 0xffff, (vget(v, V_LFO_ENV) & 0xfff) * 16);
        vsetf(v, V_STATE, 28, 0xf, ENV_RELEASE);
        vsetf(v, V_ECNT, 16, 0xffff, (vget(v, V_MISC) & 0xfff) * 16);
        vsetf(v, V_STATE, 24, 0xf, ENV_RELEASE);
        break;
    }
    case M_VOICE_OFF:
        if ((arg & 0xffff) < VOICES) voice_off(arg & 0xffff);
        break;
    case M_VOICE_PAUSE:
        if ((arg & 0xffff) < VOICES) vsetf(arg & 0xffff, V_STATE, 0, ST_PAUSED, arg & (1u << 18) ? ST_PAUSED : 0);
        break;
    case M_SSL_A: case M_SSL_B:
        if (cur < VOICES) {
            int i = m == M_SSL_B;
            vs[cur].ssl_base[i] = arg >> 8;
            vs[cur].ssl_count[i] = arg & 0xff;
        }
        break;
    case M_SET_CURRENT_SSL: ssl_page = arg; break;
    case M_INBUF_SGE: inbuf_sge = arg; break;
    case M_INBUF_SGE_OFFSET: pwr32(rd(VPSGEADDR) + inbuf_sge * 8, arg & 0xfffff000); break;
    case M_OUTBUF_SGE: outbuf_sge = arg; break;
    case M_OUTBUF_SGE_OFFSET: pwr32(rd(VPSGEADDR) + outbuf_sge * 8, arg & 0xfffff000); break;
    case M_HRTF_HEADROOM: hrtf_headroom = arg & 7; break;
    case M_HRTF_SUBMIXES:
        for (int i = 0; i < 4; i++) hrtf_submix[i] = arg >> (8 * i) & 0x1f;
        break;
    case M_TAR_PITCH:
        if (cur < VOICES) vsetf(cur, V_PITCH_LINK, 16, 0xffff, arg >> 16);
        break;
    case 0x3a0: if (cur < VOICES) vsetf(cur, V_PSL_START, 0, 0xffffff, arg); break;    // buffer base
    case 0x3a4: if (cur < VOICES) vsetf(cur, V_PSH_SAMPLE, 0, 0xffffff, arg); break;   // loop start
    case 0x3d8: if (cur < VOICES) vsetf(cur, V_OFFSET, 0, 0xffffff, arg); break;       // current
    case 0x3dc: if (cur < VOICES) vsetf(cur, V_NEXT, 0, 0xffffff, arg); break;         // end
    case SE2FE_IDLE_VOICE:
        if (rd(FETFORCE1) & FETFORCE1_IDLE_VOICE) {   // stop and tell the driver
            wr(FECTL, (rd(FECTL) & ~(FECTL_MODE | FECTL_TRAPREASON)) | FECTL_TRAPPED | FECTL_TRAPREASON);
        }
        break;
    default: {
        // whole words of the current voice's record
        static const struct { uint16_t m, off; } direct[] = {
            { 0x300, V_VBIN }, { 0x304, V_FMT }, { 0x308, V_ENV0 }, { 0x30c, V_ENVA }, { 0x310, V_ENV1 },
            { 0x314, V_ENVF }, { 0x318, V_MISC }, { 0x360, V_VOLA }, { 0x364, V_VOLB }, { 0x368, V_VOLC },
            { 0x36c, V_LFO_ENV }, { 0x374, 0x74 }, { 0x378, 0x78 },
        };
        for (unsigned i = 0; i < sizeof direct / sizeof *direct; i++)
            if (m == direct[i].m) {
                if (cur < VOICES) vset(cur, direct[i].off, arg);
                return;
            }
        if (m == 0x31c) {   // HRTF target
            if (cur < VOICES) vsetf(cur, V_HRTF, 0, 0xffff, arg);
        } else if (m >= M_SSL_SEGMENT && m < M_SSL_SEGMENT + 8 * 64) {
            pwr32(rd(VPSSLADDR) + ssl_page * 8 + (m - M_SSL_SEGMENT), arg);
        } else if (m >= M_SUBMIX_HEADROOM && m < M_SUBMIX_HEADROOM + 4 * 32) {
            submix_headroom[(m - M_SUBMIX_HEADROOM) / 4] = arg & 7;
        } else if (hle_verbose > 1) {
            static uint8_t seen[0x2000 / 4];
            if (m < 0x2000 && !seen[m / 4]++) hle_log("apu: VP method 0x%x = 0x%x not handled\n", m, arg);
        }
        break;
    }
    }
}

// ---- envelopes ------------------------------------------------------------------------------------
// one step a frame; counts run in 16ths of the configured times
static float envelope(int v, int sh, int cnt_sh, uint32_t cfg0, uint32_t cfga, uint32_t rr_off, uint32_t lvl_off)
{
    int cur = vget(v, V_STATE) >> sh & 0xf;
    uint32_t count = vget(v, V_ECNT) >> cnt_sh & 0xffff;
    uint32_t c0 = vget(v, cfg0), ca = vget(v, cfga);
    float value;
    switch (cur) {
    case ENV_OFF:
        vsetf(v, lvl_off, 24, 0xff, 0xff);
        return 1.0f;
    case ENV_DELAY:
        vsetf(v, lvl_off, 24, 0xff, 0);
        if (count == 0) vsetf(v, V_STATE, sh, 0xf, ENV_ATTACK);
        else vsetf(v, V_ECNT, cnt_sh, 0xffff, count - 1);
        return 0.0f;
    case ENV_ATTACK: {
        uint32_t len = (c0 & 0xfff) * 16;
        value = len == 0 || count >= len ? 255.0f : 255.0f * count / len;
        vsetf(v, lvl_off, 24, 0xff, (uint32_t)value);
        if (count >= len) {
            vsetf(v, V_STATE, sh, 0xf, ENV_HOLD);
            vsetf(v, V_ECNT, cnt_sh, 0xffff, (ca >> 12 & 0xfff) * 16);
        } else {
            vsetf(v, V_ECNT, cnt_sh, 0xffff, count + 1);
        }
        return value / 255.0f;
    }
    case ENV_HOLD:
        vsetf(v, lvl_off, 24, 0xff, 0xff);
        if (count == 0) {
            vsetf(v, V_STATE, sh, 0xf, ENV_DECAY);
            vsetf(v, V_ECNT, cnt_sh, 0xffff, (ca & 0xfff) * 16);
        } else {
            vsetf(v, V_ECNT, cnt_sh, 0xffff, count - 1);
        }
        return 1.0f;
    case ENV_DECAY: {
        uint32_t rate = ca & 0xfff, sustain = ca >> 24;
        // an exponential fall from full towards the sustain level
        value = rate ? 255.0f * powf(0.99988799f, (float)(rate * 16 - count) * 4096 / rate) : 0.0f;
        if (value <= sustain + 0.2f || value > 255.0f) {
            vsetf(v, V_STATE, sh, 0xf, ENV_SUSTAIN);
            value = sustain;
        } else {
            vsetf(v, V_ECNT, cnt_sh, 0xffff, count ? count - 1 : 0);
            vsetf(v, lvl_off, 24, 0xff, (uint32_t)value);
        }
        return value / 255.0f;
    }
    case ENV_SUSTAIN:
        vsetf(v, V_ECNT, cnt_sh, 0xffff, 0);
        vsetf(v, lvl_off, 24, 0xff, ca >> 24);
        return (ca >> 24) / 255.0f;
    case ENV_RELEASE: {
        uint32_t rate = vget(v, rr_off) & 0xfff;
        if (rate == 0 || count == 0) {
            vsetf(v, V_STATE, sh, 0xf, ENV_FORCE_RELEASE);
            return 0.0f;
        }
        float pos = 1.0f - count / (rate * 16.0f);
        if (pos < 0) pos = 0;
        value = expf(-6.91f * pos) * (vget(v, lvl_off) >> 24);
        vsetf(v, V_ECNT, cnt_sh, 0xffff, count - 1);
        return value / 255.0f;
    }
    case ENV_FORCE_RELEASE:
        if (sh == 28) voice_off(v);   // the amplitude envelope ends the voice
        return 0.0f;
    }
    return 0.0f;
}

// ---- samples --------------------------------------------------------------------------------------
static void adpcm_decode(const uint8_t *in, int channels, int16_t *out)
{
    static const int16_t steps[89] = {
        7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66,
        73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449,
        494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499,
        2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487,
        12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767 };
    static const int8_t index_step[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };
    int pred[2], idx[2];
    for (int ch = 0; ch < channels; ch++) {   // headers: the first sample and the step index
        pred[ch] = (int16_t)(in[0] | in[1] << 8);
        idx[ch] = in[2] > 88 ? 88 : in[2];
        out[ch] = pred[ch];
        in += 4;
    }
    // then 4 bytes (8 samples) of each channel in turn
    for (int chunk = 0; chunk < 8; chunk++)
        for (int ch = 0; ch < channels; ch++, in += 4)
            for (int n = 0; n < 8; n++) {
                int nib = in[n / 2] >> (4 * (n & 1)) & 0xf;
                int step = steps[idx[ch]], diff = step >> 3;
                if (nib & 1) diff += step >> 2;
                if (nib & 2) diff += step >> 1;
                if (nib & 4) diff += step;
                pred[ch] += nib & 8 ? -diff : diff;
                if (pred[ch] > 32767) pred[ch] = 32767;
                if (pred[ch] < -32768) pred[ch] = -32768;
                idx[ch] += index_step[nib & 7];
                if (idx[ch] < 0) idx[ch] = 0;
                if (idx[ch] > 88) idx[ch] = 88;
                out[(1 + chunk * 8 + n) * channels + ch] = pred[ch];
            }
}

// a buffer's byte through the scatter-gather table (one entry per 4 KB page)
static uint32_t sge_addr(uint32_t linear)
{
    return prd32(rd(VPSGEADDR) + (linear >> 12) * 8) + (linear & 0xfff);
}

static float sample_of(uint32_t a, int size)
{
    switch (size) {
    case 0: return (*pptr(a) - 128) / 128.0f;
    case 1: { int16_t s; memcpy(&s, pptr(a), 2); return s / 32768.0f; }
    case 2: return (int32_t)(prd32(a) << 8) / 2147483648.0f;
    default: return (int32_t)prd32(a) / 2147483648.0f;
    }
}

// the voice's next source sample (both channels); 0 when it has stopped
static int next_sample(int v, float out[2])
{
    uint32_t fmt = vget(v, V_FMT);
    int stereo = fmt >> 27 & 1, channels = stereo + 1;
    int size = fmt >> 28 & 3, container = fmt >> 30 & 3;
    int spb = (fmt >> 16 & 0x1f) + 1;
    uint32_t cbo = vget(v, V_OFFSET) & 0xffffff, ebo = vget(v, V_NEXT) & 0xffffff;
    uint32_t seg_off = 0;
    int stream = fmt & FMT_STREAM ? 1 : 0;
    if (!(vget(v, V_STATE) & ST_ACTIVE)) return 0;
    if (stream) {
        int i = vs[v].ssl_index;
        if (vs[v].ssl_count[i] == 0) {   // nothing queued
            vsetf(v, V_OFFSET, 0, 0xffffff, 0);
            vs[v].ssl_seg = 0;
            if (!(fmt & FMT_PERSIST)) {
                vs[v].ssl_index = 0;
                voice_off(v);
            } else {
                notify(v, i);
            }
            return 0;
        }
        uint32_t page = vs[v].ssl_base[i] + vs[v].ssl_seg;
        seg_off = prd32(rd(VPSSLADDR) + page * 8);
        uint32_t seg_len = prd32(rd(VPSSLADDR) + page * 8 + 4);
        container = seg_len >> 16 & 3;
        if (container == 2) size = 2;
        ebo = (seg_len & 0xffff) - 1;
    }
    out[0] = out[1] = 0;
    if (container == 2) {   // ADPCM: 36 bytes per channel for 64 samples
        int block = cbo / 64, bsize = 36 * spb;
        if (vs[v].adpcm_block != block) {
            uint8_t buf[72];
            for (int i = 0; i < bsize && i < 72; i++)
                buf[i] = stream ? *pptr(seg_off + block * bsize + i)
                                : *pptr(sge_addr((vget(v, V_PSL_START) & 0xffffff) + block * bsize + i));
            adpcm_decode(buf, channels, vs[v].adpcm);
            vs[v].adpcm_block = block;
        }
        for (int ch = 0; ch < channels; ch++) out[ch] = vs[v].adpcm[(cbo % 64) * channels + ch] / 32768.0f;
    } else {
        static const int csize[4] = { 1, 2, 0, 4 };
        uint32_t frame = csize[container] * spb, a;
        if (stream) a = seg_off + cbo * frame;
        else a = sge_addr((vget(v, V_PSL_START) & 0xffffff) + cbo * frame);
        for (int ch = 0; ch < channels; ch++) out[ch] = sample_of(a + ch * csize[container], size);
    }
    if (!stereo) out[1] = out[0];
    // advance; at the end: the next segment, the loop start, or stop
    if (cbo >= ebo) {
        if (stream) {
            cbo = 0;
            vs[v].adpcm_block = -1;
            if (++vs[v].ssl_seg >= vs[v].ssl_count[vs[v].ssl_index]) {
                int done = vs[v].ssl_index;
                vs[v].ssl_index ^= 1;
                vs[v].ssl_seg = 0;
                notify(v, done);
            }
        } else if (fmt & FMT_LOOP) {
            cbo = vget(v, V_PSH_SAMPLE) & 0xffffff;
        } else {
            vsetf(v, V_OFFSET, 0, 0xffffff, ebo);
            voice_off(v);
            return 1;
        }
    } else {
        cbo++;
    }
    vsetf(v, V_OFFSET, 0, 0xffffff, cbo);
    return 1;
}

static float attenuation(uint32_t vol)
{
    vol &= 0xfff;
    return vol == 0xfff ? 0.0f : powf(10.0f, vol / (64.0f * -20.0f));
}

static float mix[FRAME][2];

static void voice_frame(int v)
{
    uint32_t state = vget(v, V_STATE);
    vsetf(v, V_STATE, 0, ST_NEW, 0);
    if (state & ST_PAUSED) return;
    uint32_t fmt = vget(v, V_FMT);
    float ef = envelope(v, 24, 16, V_ENV1, V_ENVF, V_MISC, V_NEXT);
    int16_t pitch = vget(v, V_PITCH_LINK) >> 16;
    int8_t pscale = vget(v, V_ENV0) >> 24;
    double step = pow(2.0, (pitch + pscale * 32 * ef) / 4096.0);
    float ea = envelope(v, 28, 0, V_ENV0, V_ENVA, V_LFO_ENV, V_OFFSET);
    if (fmt & FMT_MULTIPASS) return;   // a submix voice: needs the DSP's mix bins
    // volume: the loudest of its eight bins
    uint32_t a = vget(v, V_VOLA), b = vget(v, V_VOLB), c = vget(v, V_VOLC), bins = vget(v, V_VBIN);
    uint32_t vol[8] = { a >> 4 & 0xfff, a >> 20, b >> 4 & 0xfff, b >> 20, c >> 4 & 0xfff, c >> 20,
                        (c & 0xf) << 8 | (b & 0xf) << 4 | (a & 0xf), (c >> 16 & 0xf) << 8 | (b >> 16 & 0xf) << 4 | (a >> 16 & 0xf) };
    int bin[8] = { bins & 0x1f, bins >> 5 & 0x1f, bins >> 10 & 0x1f, bins >> 16 & 0x1f, bins >> 21 & 0x1f,
                   bins >> 26 & 0x1f, fmt & 0x1f, fmt >> 5 & 0x1f };
    float g = 0;
    for (int i = 0; i < 8; i++) {
        int hr = v < VOICES_3D && i < 4 ? hrtf_headroom : submix_headroom[bin[i]];
        float x = attenuation(vol[i]) / (1 << hr);
        if (x > g) g = x;
    }
    g *= ea;
    // linear resampling: frac is the position between s0 and s1
    for (int i = 0; i < FRAME; i++) {
        if (!vs[v].primed) {
            if (!next_sample(v, vs[v].s0) || !next_sample(v, vs[v].s1)) return;
            vs[v].primed = 1;
        }
        float f = (float)vs[v].frac;
        mix[i][0] += g * (vs[v].s0[0] + (vs[v].s1[0] - vs[v].s0[0]) * f);
        mix[i][1] += g * (vs[v].s0[1] + (vs[v].s1[1] - vs[v].s0[1]) * f);
        vs[v].frac += step;
        while (vs[v].frac >= 1.0) {
            vs[v].frac -= 1.0;
            memcpy(vs[v].s0, vs[v].s1, sizeof vs[v].s0);
            if (!next_sample(v, vs[v].s1)) {
                vs[v].primed = 0;
                return;
            }
        }
    }
}

// ---- output ---------------------------------------------------------------------------------------
// a ring of interleaved stereo 16-bit samples for the front end, and an
// optional WAV file (CH_WAV) to check the sound without one
#define RING (RATE / 2)
static int16_t ring[RING][2];
static uint32_t ring_w, ring_r;
static uint64_t outs, outs_epoch;   // render mode: frames output since the clock's start (clk_epoch)
static FILE *wav;
static uint32_t wav_bytes;

int chihiro_audio_read(int16_t *out, int frames)
{
    int n = 0;
    pthread_mutex_lock(&lock);
    while (n < frames && ring_r != ring_w) {
        out[2 * n] = ring[ring_r % RING][0];
        out[2 * n + 1] = ring[ring_r % RING][1];
        ring_r++, n++;
    }
    pthread_mutex_unlock(&lock);
    return n;
}

static void wav_header(void)
{
    uint8_t h[44] = "RIFF\0\0\0\0WAVEfmt \x10\0\0\0\x01\0\x02\0\x80\xbb\0\0\0\xee\x02\0\x04\0\x10\0data";
    uint32_t riff = 36 + wav_bytes;
    memcpy(h + 4, &riff, 4);
    memcpy(h + 40, &wav_bytes, 4);
    fseek(wav, 0, SEEK_SET);
    fwrite(h, 1, 44, wav);
    fseek(wav, 0, SEEK_END);
}

static void output(void)
{
    int16_t s[FRAME][2];
    for (int i = 0; i < FRAME; i++)
        for (int ch = 0; ch < 2; ch++) {
            float x = mix[i][ch];
            if (x > 1) x = 1;
            if (x < -1) x = -1;
            s[i][ch] = (int16_t)(x * 32767);
        }
    for (int i = 0; i < FRAME; i++) {
        if (ring_w - ring_r >= RING) ring_r++;   // nobody reading: drop the oldest
        ring[ring_w % RING][0] = s[i][0];
        ring[ring_w % RING][1] = s[i][1];
        ring_w++;
    }
    outs++;
    if (wav) {
        fwrite(s, 1, sizeof s, wav);
        wav_bytes += sizeof s;
        if ((frame_count & 1023) == 0) wav_header();
    }
}


// ---- ACI: the AC97 interface (0xFEC00000) ----------------------------------------------------------
// The codec's mixer registers at +0, and the bus master at +0x100: PCM in,
// PCM out, mic and S/PDIF boxes (a buffer descriptor list each), global
// control and status. DirectSound resets the codec, waits for it to say it
// is ready, sets it up and points PCM out and S/PDIF at buffers the encode
// processor fills. Those buffers are not played (the voices go to the
// output directly); the boxes consume them at 48 kHz so the driver's
// interrupts and positions run as on the hardware.
#define ACI_LEVEL 6
#define GLOB_CNT 0x12c
#define GLOB_STA 0x130
#define CAS 0x134
static uint8_t aci[0x200];
static const uint16_t boxes[4] = { 0x100, 0x110, 0x120, 0x170 };   // PCM in, PCM out, mic, S/PDIF

static uint32_t aci_rd(uint32_t o, int size) { uint32_t v = 0; memcpy(&v, aci + o, size); return v; }
static void aci_wr(uint32_t o, uint32_t v, int size) { memcpy(aci + o, &v, size); }

static void aci_update_irq(void)
{
    uint32_t sta = aci_rd(GLOB_STA, 4) & ~(0xe0u | (1u << 28));
    for (int b = 0; b < 4; b++) {
        uint16_t sr = aci_rd(boxes[b] + 6, 2);
        uint8_t cr = aci[boxes[b] + 0xb];
        int pending = ((sr & 8) && (cr & 0x10)) || ((sr & 4) && (cr & 4)) || ((sr & 0x10) && (cr & 8));
        if (pending) sta |= b == 0 ? 0x20 : b == 1 ? 0x40 : b == 2 ? 0x80 : 1u << 28;
    }
    aci_wr(GLOB_STA, sta | 0x100, 4);   // the primary codec is always ready
    if (sta & (0xe0u | 1u << 28)) ke_raise_interrupt(ACI_LEVEL);
}

// a frame's worth (32 stereo samples, 64 words) for each running box
static void aci_frame(void)
{
    for (int b = 0; b < 4; b++) {
        uint32_t o = boxes[b];
        uint16_t sr = aci_rd(o + 6, 2);
        if (!(aci[o + 0xb] & 1) || (sr & 1)) continue;   // not running, or halted
        uint32_t left = 64;
        while (left) {
            uint32_t picb = aci_rd(o + 8, 2);
            uint8_t civ = aci[o + 4] & 31;
            uint32_t bd = aci_rd(o, 4) + civ * 8;
            if (picb == 0) {   // load the current descriptor
                picb = prd32(bd + 4) & 0xffff;
                aci_wr(o + 8, picb, 2);
                if (!picb) picb = 1;
            }
            uint32_t n = picb < left ? picb : left;
            picb -= n, left -= n;
            aci_wr(o + 8, picb, 2);
            if (picb) continue;
            // the buffer is done
            uint32_t flags = prd32(bd + 4);
            if (flags & 0x80000000u) sr |= 8;   // interrupt on completion
            if (civ == (aci[o + 5] & 31)) {     // the last valid one: stop
                sr |= 2 | 1;
                aci_wr(o + 6, sr, 2);
                break;
            }
            aci[o + 4] = (civ + 1) & 31;
            aci[o + 0xa] = (civ + 2) & 31;
        }
        aci_wr(o + 6, sr, 2);
    }
    aci_update_irq();
}

static uint32_t aci_mmio_read(uint32_t o, int size)
{
    if (o >= sizeof aci) return 0;
    uint32_t v = aci_rd(o, size);
    if (o == GLOB_STA) v |= 0x100;
    if (o == CAS) aci[CAS] = 1;   // the codec access semaphore: free (0), then taken
    if (o < 0x80) {
        aci[CAS] = 0;
        if (o == 0x26) v |= 0xf;   // powerdown status: everything ready
    }
    return v;
}

static void aci_mmio_write(uint32_t o, uint32_t v, int size)
{
    if (o >= sizeof aci) return;
    for (int b = 0; b < 4; b++) {
        uint32_t box = boxes[b];
        if (o == box + 6 || (o == box + 4 && size == 4)) {   // status: write 1 to clear
            uint32_t sr = aci_rd(box + 6, 2) & ~((size == 4 ? v >> 16 : v) & 0x1c);
            aci_wr(box + 6, sr, 2);
            if (o == box + 4) aci[box + 5] = v >> 8 & 31;
            aci_update_irq();
            return;
        }
        if (o == box + 0xb) {   // control: reset, run
            if (v & 2) {
                aci_wr(box + 4, 0, 4);
                aci_wr(box + 8, 0, 4);
                aci_wr(box + 6, 1, 2);
                aci[box + 0xb] = 0;
                aci_update_irq();
                return;
            }
            aci[o] = v;
            uint16_t sr = aci_rd(box + 6, 2);
            if (v & 1) sr &= ~1;   // running
            else sr |= 1;
            aci_wr(box + 6, sr, 2);
            aci_update_irq();
            return;
        }
        if (o == box + 5) {   // last valid index: a halted box at the end runs again
            aci[o] = v & 31;
            uint16_t sr = aci_rd(box + 6, 2);
            if ((aci[box + 0xb] & 1) && (sr & 1)) aci_wr(box + 6, sr & ~3, 2), aci[box + 4] = (aci[box + 4] + 1) & 31;
            return;
        }
    }
    if (o == GLOB_STA) {   // write 1 to clear (the resume bits)
        aci_wr(o, aci_rd(o, 4) & ~(v & 0x300c00), 4);
        return;
    }
    aci_wr(o, v, size);
    if (o == CAS || o < 0x80) aci[CAS] = 0;   // a codec access done frees the semaphore
}

// ---- frames ---------------------------------------------------------------------------------------
// The global processor's code is not run. DirectSound talks to it through
// a word in the GP's scratch memory (system RAM, through the scratch
// scatter-gather table): it writes a command there and waits for the DSP to
// clear it. Clear it as the DSP would once it has taken the command.
#define GPSADDR 0x2040
#define GPRST 0x3fffc
static void gp_frame(void)
{
    if ((rd(GPRST) & 3) != 3 || !rd(GPSADDR)) return;
    uint32_t page = prd32(rd(GPSADDR));
    if (!page) return;
    uint32_t cmd = prd32(page + 0x810);
    if (cmd) {
        if (hle_verbose > 1) hle_log("apu: GP command %u taken\n", cmd);
        pwr32(page + 0x810, 0);
    }
}

static void run_frame(void)
{
    gp_frame();
    memset(mix, 0, sizeof mix);
    for (int l = 0; l < 3; l++) {
        uint32_t v = rd(list_top[l]) & 0xffff;
        for (int n = 0; v != 0xffff && v < VOICES && n < VOICES; n++) {
            wr(list_cur[l], v);
            uint32_t next = vfield(v, V_PITCH_LINK, 0, 0xffff);
            wr(list_next[l], next);
            if (!(vget(v, V_STATE) & ST_ACTIVE)) method(SE2FE_IDLE_VOICE, v);
            else voice_frame(v);
            v = next;
        }
        wr(list_cur[l], 0xffff);
    }
    output();
    frame_count++;
}

static uint64_t now_ns(void) { return clk_mono_ns(); }   // the core's clock

// render mode (clock.c): an output frame for every APU frame of time since the
// clock's start, so the sound keeps step with the picture (25 frames, 800
// samples, per video frame): the APU's own while it runs, silence while it is
// stopped, trapped or not yet set up (booting, restarting).
static void render_tick(void)
{
    pthread_mutex_lock(&lock);
    if (outs_epoch != clk_epoch()) outs_epoch = clk_epoch(), outs = 0;
    uint64_t due = (now_ns() - outs_epoch) * (RATE / FRAME) / 1000000000ull;
    if (ready && !start_ns) start_ns = outs_epoch;
    while (outs < due) {
        uint32_t fectl = ready ? rd(FECTL) : 0;
        int running = ready && frames_on && (rd(SECTL) >> 3 & 3) != 0 && (fectl & FECTL_MODE) != FECTL_TRAPPED &&
                      (fectl & FECTL_MODE) != FECTL_HALTED;
        if (running) {
            run_frame();
        } else {
            memset(mix, 0, sizeof mix);
            output();
        }
    }
    if (ready) {
        if (due > aci_frames + 64) aci_frames = due - 64;
        for (uint64_t f = aci_frames; f < due; f++) aci_frame();
        aci_frames = due;
        update_irq();
        aci_update_irq();
    }
    pthread_mutex_unlock(&lock);
}

// from the interrupt thread, every millisecond: the frames due since the last call
void apu_tick(void)
{
    if (clk_render) {
        render_tick();
        return;
    }
    if (!ready) return;
    pthread_mutex_lock(&lock);
    uint64_t now = now_ns();
    if (!start_ns) start_ns = now;
    uint64_t due = (now - start_ns) * (RATE / FRAME) / 1000000000ull;
    uint32_t fectl = rd(FECTL);
    int running = frames_on && (rd(SECTL) >> 3 & 3) != 0 && (fectl & FECTL_MODE) != FECTL_TRAPPED &&
                  (fectl & FECTL_MODE) != FECTL_HALTED;
    if (due > aci_frames + 64) aci_frames = due - 64;
    if (!running) {
        apu_last = 0;   // stopped: the APU clock does not move
    } else {
        // the frames due since it last ran (none for the time it was stopped)
        if (!apu_last || due > apu_last + 64) apu_last = due - 1;
        while (apu_last < due && (rd(FECTL) & FECTL_MODE) != FECTL_TRAPPED) run_frame(), apu_last++;
    }
    for (uint64_t f = aci_frames; f < due; f++) aci_frame();   // the codec plays whether or not the APU runs
    aci_frames = due;
    update_irq();
    aci_update_irq();
    pthread_mutex_unlock(&lock);
}

// back to power-on state (the registers are in guest memory, mapped afresh)
void apu_reset(void)
{
    pthread_mutex_lock(&lock);
    ready = 0;
    frame_count = aci_frames = apu_last = start_ns = 0;
    ssl_page = inbuf_sge = outbuf_sge = 0;
    memset(submix_headroom, 0, sizeof submix_headroom);
    hrtf_headroom = 0;
    memset(hrtf_submix, 0, sizeof hrtf_submix);
    memset(mix, 0, sizeof mix);
    ring_w = ring_r = 0;
    if (wav) {
        wav_header();
        fclose(wav);
    }
    wav = NULL;
    wav_bytes = 0;
    memset(aci, 0, sizeof aci);
    pthread_mutex_unlock(&lock);
}

void apu_init(void)
{
    frames_on = !af_debug_env("CH_NOSOUND");
    const char *w = af_debug_env("CH_WAV");
    if (w && (wav = fopen(w, "wb"))) wav_header();
    for (int l = 0; l < 3; l++) wr(list_top[l], 0xffff);
    for (int b = 0; b < 4; b++) aci_wr(boxes[b] + 6, 1, 2);   // halted
    aci_wr(0x7c, 0x83847600, 4);   // codec vendor ID
    ready = 1;
}

// ---- MMIO -----------------------------------------------------------------------------------------
static uint8_t touched[0x80];   // 4 KB pages, for the log

uint32_t aci_read(uint32_t o, int size)
{
    pthread_mutex_lock(&lock);
    uint32_t v = aci_mmio_read(o, size);
    pthread_mutex_unlock(&lock);
    return v;
}

void aci_write(uint32_t o, uint32_t v, int size)
{
    pthread_mutex_lock(&lock);
    aci_mmio_write(o, v, size);
    pthread_mutex_unlock(&lock);
}

uint32_t apu_mmio_read(uint32_t o, int size)
{
    if (!touched[o >> 12] && hle_verbose > 1) {
        touched[o >> 12] = 1;
        hle_log("apu: first access to 0x%05x (read)\n", o);
    }
    uint32_t v;
    switch (o & ~3u) {
    case VP + M_FREE:
        v = 0x80;   // room for every method
        break;
    case XGSCNT:
        pthread_mutex_lock(&lock);
        v = (uint32_t)(frame_count * FRAME);
        pthread_mutex_unlock(&lock);
        break;
    default:
        v = rd(o & ~3u);
        break;
    }
    v >>= 8 * (o & 3);
    return size == 4 ? v : size == 2 ? v & 0xffff : v & 0xff;
}

void apu_mmio_write(uint32_t o, uint32_t v, int size)
{
    if (!touched[o >> 12] && hle_verbose > 1) {
        touched[o >> 12] = 1;
        hle_log("apu: first access to 0x%05x (write 0x%x)\n", o, v);
    }
    if (size != 4) {
        uint32_t w = rd(o & ~3u), sh = 8 * (o & 3), m = (size == 1 ? 0xffu : 0xffffu) << sh;
        v = (w & ~m) | ((v << sh) & m);
    }
    o &= ~3u;
    pthread_mutex_lock(&lock);
    if (o >= VP && o < VP + 0x10000) {
        method(o - VP, v);
    } else if (o == ISTS) {
        wr(ISTS, rd(ISTS) & ~v);
        update_irq();
    } else if (o == FEMEMDATA) {
        // the driver writes a value it then expects at FEMEMADDR
        pwr32(rd(FEMEMADDR), v);
        wr(o, v);
    } else {
        wr(o, v);
        if (o == FECTL) update_irq();
    }
    pthread_mutex_unlock(&lock);
}
