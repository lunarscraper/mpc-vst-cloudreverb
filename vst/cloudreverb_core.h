/* =============================================================================
 * cloudreverb_core.h - the reverb of Cloud Reverb: a light algorithmic reverb for long, moving
 * ambient tails, written for the MPC OS plugin host (single precision, a small fixed structure).
 * The layout of the tail follows Valdemar Erlingsson's CloudSeed (diffusers into parallel
 * modulated feedback lines); the code is new, nothing is taken from CloudSeed or CloudReverb.
 *
 * Per channel (left and right differ only in their random delay times):
 *
 *   in -> [swell] -> low cut -> high cut -> pre-delay -> 4 allpasses (early diffusion) --+
 *                                               ^                                         |
 *                                               +---- x bloom <- delay <------------------+
 *        +--------------------------------------------------------------------------------+
 *        |    6 parallel lines, each a feedback loop of its own:
 *        +--> (+) -> modulated delay -> 2 allpasses (late diffusion) -> damping -+-> sum -> [tide] -> wet
 *              ^ ^                                                               |    |
 *              | +------------------------- x feedback (from DECAY) -------------+    |
 *              +---------------- x shimmer <- one octave up <-------------------------+
 *
 * Seven TYPEs share this; CHARACTER is the one control whose job depends on the TYPE:
 *   CLOUD    bloom: the early diffusers feed themselves, the reverb rises slowly instead of starting
 *   SWELL    every new note fades into the reverb; CHARACTER is the fade time
 *   SHIMMER  the tail is fed back one octave up; CHARACTER is how much
 *   TIDE     slow waves of level and brightness roll over the tail, left and right a quarter
 *            apart; CHARACTER is their period
 *   DRONE    hold: the lines keep what they have (at full: for good) and take in less the fuller
 *            they are
 *   CHOIR    the tail passes three vowel formants per side; CHARACTER walks A - E - I - O - U
 *   MODAL    in front of the reverb, the input excites a bank of tuned resonances (four notes of
 *            a chord on ROOT, six partials each) and rings back in that chord; CHARACTER lays
 *            the partials out from gamelan (squeezed) over string (harmonic) to bell (stretched).
 *            The bank has no feedback path: it rings, it cannot howl.
 * CHOIR and MODAL take after two patches of Phonicbloom's Siluria (Formant, Modal); the code is new.
 *
 * Every delay glides to a new length (tape style, at most half a sample per sample) instead of
 * jumping, so SIZE and PRE-DELAY can be turned while sound runs through.
 * MIT license (see ../LICENSE).
 * ========================================================================== */
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace cloud {

enum { NLINES = 6, NEARLY = 4, NLATE = 2, SUB = 16 };
enum { TYPE_CLOUD, TYPE_SWELL, TYPE_SHIMMER, TYPE_TIDE, TYPE_DRONE, TYPE_CHOIR, TYPE_MODAL, NTYPES };
enum { NFORMANTS = 3, NNOTES = 4, NPARTIALS = 6, NMODES = NNOTES * NPARTIALS, NCHORDS = 9 };
/* MODAL: the four notes, in semitones above ROOT (C3 .. B3) */
static const float CHORDS[NCHORDS][NNOTES] = {
    {-12, 0, 12, 24},      /* OCTAVES */
    {0, 7, 12, 19},        /* FIFTHS */
    {0, 4, 7, 12},         /* MAJOR */
    {0, 3, 7, 12},         /* MINOR */
    {0, 2, 7, 12},         /* SUS 2 */
    {0, 5, 7, 12},         /* SUS 4 */
    {0, 4, 7, 11},         /* MAJOR 7 */
    {0, 3, 7, 10},         /* MINOR 7 */
    {-5, 0, 0.12f, -12},   /* TANPURA: Pa Sa Sa and the low Sa, the two middle strings a hair apart */
};
/* CHOIR: formant frequencies of A E I O U (Peterson and Barney's averages for men) */
static const float VOWELS[5][NFORMANTS] = {
    {730, 1090, 2440}, {530, 1840, 2480}, {270, 2290, 3010}, {570, 840, 2410}, {300, 870, 2240},
};

struct Params {
    int type = TYPE_CLOUD;
    float character = 0.0f;    /* 0 .. 1, see above */
    int root = 2;              /* MODAL: 0 .. 11 = C .. B */
    int chord = 1;             /* MODAL: row of CHORDS */
    float mix = 0.3f;          /* 0 = dry only, 0.5 = dry and wet both full, 1 = wet only */
    float lineMs = 120.0f;     /* centre of the six line lengths (they spread 0.5x .. 1.5x) */
    float diffScale = 0.7f;    /* 0.35 .. 1: scales the allpass lengths along with the size */
    float decayS = 3.0f;       /* time for the tail to fall by 60 dB */
    float predelayMs = 0.0f;
    float diffusion = 0.7f;    /* 0 = separate echoes, 1 = smooth cloud */
    float mod = 0.3f;          /* 0 .. 1: depth and speed of the line modulation */
    float dampHz = 6000.0f;    /* low-pass inside the loops; >= 20 kHz switches it off */
    float lowCutHz = 120.0f;   /* high-pass in front of the reverb */
};
/* CHARACTER -> seconds, for the readout too */
static inline float swell_s(float c) { return 0.1f * std::pow(40.0f, c); }          /* 0.1 .. 4 s */
static inline float tide_s(float c) { return 30.0f * std::pow(1.0f / 15.0f, c); }   /* 30 .. 2 s */

/* a delay line whose length glides; T = double where the delay can be long (keeps the
 * fraction exact), float for the short ones */
template <class T> struct Delay {
    std::vector<float> buf;
    uint32_t mask = 0, w = 0;
    T d = 1, inc = 0, next = 1, base = 1, target = 1;

    void alloc(uint32_t minLen) {
        uint32_t n = 64;
        while (n < minLen) n <<= 1;
        buf.assign(n, 0.0f);
        mask = n - 1;
        w = 0;
    }
    void clear() { std::fill(buf.begin(), buf.end(), 0.0f); }
    T limit(T v) const { const T hi = (T)(mask - 2); return v < 1 ? 1 : v > hi ? hi : v; }
    void set(T samples) { target = limit(std::round(samples)); }
    void snap() { base = d = next = target; inc = 0; }
    /* once per sub-block of len samples: glide towards the target, add the modulation offset */
    void plan(T k, T offset, int len) {
        d = next;
        T step = (target - base) * k;
        const T cap = (T)0.5 * len;
        if (step > cap) step = cap; else if (step < -cap) step = -cap;
        base += step;
        if (std::fabs(target - base) < (T)1e-3) base = target;
        next = limit(base + offset);
        inc = (next - d) / len;
    }
    inline float at(T delay) const {
        const int32_t i = (int32_t)delay;
        const float f = (float)(delay - (T)i);
        const float a = buf[(w - (uint32_t)i) & mask], b = buf[(w - (uint32_t)i - 1) & mask];
        return a + f * (b - a);
    }
    inline float read() const { return at(d); }
    inline void write(float x) { buf[w & mask] = x; w++; d += inc; }
};

struct Allpass {
    Delay<float> dl;
    inline float tick(float x, float g) {
        const float out = dl.read();
        const float in = x + out * g;
        dl.write(in);
        return out - in * g;
    }
};

struct Line {
    Delay<double> dl;
    Allpass ap[NLATE];
    float apMs[NLATE] = {0};
    float ratio = 1, lfoRate = 1, lfoDepth = 1;   /* random per line, fixed */
    float phase = 0, phaseInc = 0, depth = 0;
    float g = 0, lp = 0, fb = 0, hp = 0;
};

/* one octave up: two taps run through a short delay at double speed, half a window apart,
 * each faded in and out as it wraps */
struct OctaveUp {
    Delay<float> dl;
    float win = 2048, p = 0, lp = 0, hp = 0;
    inline float tick(float x) {
        float p2 = p + 0.5f;
        if (p2 >= 1.0f) p2 -= 1.0f;
        const float wa = 1.0f - std::fabs(2.0f * p - 1.0f);
        const float y = wa * dl.at(1.0f + (1.0f - p) * win) + (1.0f - wa) * dl.at(1.0f + (1.0f - p2) * win);
        dl.write(x);
        p += 1.0f / win;
        if (p >= 1.0f) p -= 1.0f;
        return y;
    }
};

/* a band-pass that takes being moved while it rings (trapezoidal state-variable filter) */
struct Formant {
    float f = 500, ft = 500, g = 0.03f, a1 = 1, a2 = 0, a3 = 0, s1 = 0, s2 = 0, gain = 1;
    void tune(float sr, float k) {
        g = std::tan(3.14159265f * std::min(f, 0.45f * sr) / sr);
        a1 = 1.0f / (1.0f + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }
    inline float tick(float x) {
        const float v3 = x - s2, v1 = a1 * s1 + a2 * v3, v2 = s2 + a2 * s1 + a3 * v3;
        s1 = 2.0f * v1 - s1;
        s2 = 2.0f * v2 - s2;
        return v1;
    }
};

/* one resonance of the MODAL bank: a point turning on a shrinking circle, pushed by the input.
 * Double precision: at 65 Hz and several seconds of ring, single would be noisy. */
struct Mode {
    double c = 1, s = 0, u = 0, v = 0;
    float in = 0, panL = 0.5f, panR = 0.5f;
};

struct Channel {
    float hp = 0, lp = 0, bloomLp = 0, tideLp = 0, level = 0;
    Formant fm[NFORMANTS];
    Delay<double> pre;
    Delay<float> bloom;
    Allpass early[NEARLY];
    float earlyMs[NEARLY] = {0};
    Line line[NLINES];
    OctaveUp oct;
    float shim = 0;   /* last shimmer sample, into the lines one sample later */
};

class Reverb {
public:
    void setSampleRate(float rate) {
        sr = rate > 8000 ? rate : 44100;
        /* fixed random numbers: every instance and every run sounds the same */
        uint32_t seed = 0x2545F491u;
        auto rnd = [&seed]() { seed = seed * 1664525u + 1013904223u; return (float)(seed >> 8) * (1.0f / 16777216.0f); };
        for (int c = 0; c < 2; c++) {
            Channel &ch = chan[c];
            ch.pre.alloc((uint32_t)(0.27f * sr) + 8);
            for (int i = 0; i < NEARLY; i++) {
                ch.earlyMs[i] = 9.0f + 38.0f * ((float)i + rnd()) / NEARLY;       /* 9 .. 47 ms, one per quarter */
                ch.early[i].dl.alloc((uint32_t)(0.050f * sr) + 8);
            }
            for (int i = 0; i < NLINES; i++) {
                Line &l = ch.line[i];
                l.ratio = 0.5f + ((float)i + rnd()) / NLINES;                     /* 0.5 .. 1.5, one per sixth */
                l.lfoRate = 0.5f + 0.5f * rnd();
                l.lfoDepth = 0.6f + 0.4f * rnd();
                l.phase = 6.2831853f * rnd();
                l.dl.alloc((uint32_t)(0.93f * sr) + 8);
                for (int k = 0; k < NLATE; k++) {
                    l.apMs[k] = 14.0f + 52.0f * ((float)k + rnd()) / NLATE;       /* 14 .. 66 ms */
                    l.ap[k].dl.alloc((uint32_t)(0.070f * sr) + 8);
                }
            }
            ch.bloom.alloc((uint32_t)(0.36f * sr) + 8);
            ch.oct.win = std::floor((c ? 0.071f : 0.064f) * sr);
            ch.oct.dl.alloc((uint32_t)ch.oct.win + 16);
        }
        glide = 1.0f - std::exp(-(float)SUB / (0.10f * sr));
        gainK = 1.0f - std::exp(-(float)SUB / (0.02f * sr));
        aBloom = 1.0f - std::exp(-6.2831853f * 5000.0f / sr);
        aShimLp = 1.0f - std::exp(-6.2831853f * 7000.0f / sr);
        aShimHp = 1.0f - std::exp(-6.2831853f * 250.0f / sr);
        aLineHp = 1.0f - std::exp(-6.2831853f * 5.0f / sr);   /* only what would pile up below the audible */
        aLevel = 1.0f - std::exp(-1.0f / (0.15f * sr));
        aFast = 1.0f - std::exp(-1.0f / (0.004f * sr));
        aSlow = 1.0f - std::exp(-1.0f / (0.12f * sr));
        formantK = 1.0f - std::exp(-(float)SUB / (0.05f * sr));
        setParams(p);
        reset();
    }

    /* empty the reverb and jump to the current settings */
    void reset() {
        for (Channel &ch : chan) {
            ch.hp = ch.lp = ch.bloomLp = ch.tideLp = ch.level = ch.shim = 0;
            for (Formant &f : ch.fm) { f.s1 = f.s2 = 0; f.f = f.ft; f.tune(sr, FORMANT_K); }
            ch.pre.clear(); ch.pre.snap();
            ch.bloom.clear(); ch.bloom.snap();
            ch.oct.dl.clear(); ch.oct.lp = ch.oct.hp = 0;
            ch.oct.p = &ch == &chan[1] ? 0.37f : 0.0f;
            for (Allpass &a : ch.early) { a.dl.clear(); a.dl.snap(); }
            for (Line &l : ch.line) {
                l.dl.clear(); l.dl.snap();
                for (Allpass &a : l.ap) { a.dl.clear(); a.dl.snap(); }
                l.lp = l.fb = l.hp = 0;
            }
        }
        dry = dryT; wet = wetT; late = lateT;
        envFast = envSlow = swell = 0;
        tidePhase = 0;
        for (Mode &m : mode) m.u = m.v = 0;
        quiet = 1 << 30;
        refractory = 0;
        falling = false;
    }

    void setParams(const Params &np) {
        p = np;
        const float ms = sr * 0.001f;
        const float mix = clamp(p.mix, 0, 1), c = clamp(p.character, 0, 1);
        type = p.type < 0 || p.type >= NTYPES ? (int)TYPE_CLOUD : p.type;
        dryT = std::min(1.0f, 2.0f * (1.0f - mix));
        wetT = std::min(1.0f, 2.0f * mix);
        gEarly = 0.72f * clamp(p.diffusion, 0, 1);
        gLate = 0.66f * clamp(p.diffusion, 0, 1);
        earlyBlend = clamp(4.0f * p.diffusion, 0, 1);   /* with g = 0 an allpass is only a delay: fade the early block out instead */
        const float m = clamp(p.mod, 0, 1);
        const float depth = m * m * 2.0f * ms;                 /* up to 2 ms */
        const float rate = 0.2f + 0.8f * m;                    /* Hz, times 0.5 .. 1 per line */
        const float hold = type == TYPE_DRONE ? c : 0.0f;
        aDamp = p.dampHz >= 19999.0f ? 1.0f : 1.0f - std::exp(-6.2831853f * p.dampHz / sr);
        aDamp += (1.0f - aDamp) * hold * hold;                 /* what is held must not go dull */
        aIn = p.dampHz >= 19999.0f ? 1.0f : 1.0f - std::exp(-6.2831853f * std::min(20000.0f, 1500.0f + 2.0f * p.dampHz) / sr);
        aHp = 1.0f - std::exp(-6.2831853f * clamp(p.lowCutHz, 5, 2000) / sr);
        /* the allpasses in the loop hold some frequencies longer than their plain length, which
         * stretches the tail: measured about a quarter at full diffusion, taken out here */
        const float decay = std::max(0.05f, p.decayS) * sr / (1.0f + 0.25f * clamp(p.diffusion, 0, 1));
        const float keep = (1.0f - hold) * (1.0f - hold);      /* DRONE: the loss per round shrinks to nothing */
        double num = 0, den = 0;
        for (Channel &ch : chan) {
            ch.pre.set(std::max(1.0f, p.predelayMs * ms));
            ch.bloom.set((90.0f + 230.0f * (p.diffScale - 0.35f) / 0.65f) * ms);
            for (int i = 0; i < NEARLY; i++) ch.early[i].dl.set(ch.earlyMs[i] * p.diffScale * ms);
            for (Line &l : ch.line) {
                l.dl.set(p.lineMs * l.ratio * ms);
                double loop = l.dl.target;
                for (int k = 0; k < NLATE; k++) { l.ap[k].dl.set(l.apMs[k] * p.diffScale * ms); loop += l.ap[k].dl.target; }
                l.g = 1.0f - (1.0f - std::pow(10.0f, -3.0f * (float)loop / decay)) * keep;
                const float ref = std::pow(10.0f, -3.0f * (float)loop / (2.0f * sr));
                num += 1.0 - (double)l.g * l.g;
                den += 1.0 - (double)ref * ref;
                l.depth = depth * l.lfoDepth;
                l.phaseInc = 6.2831853f * rate * l.lfoRate * SUB / sr;
            }
        }
        /* a long tail piles up: take part of that back so DECAY doesn't double as a volume knob */
        const float comp = clamp(std::pow((float)(num / den), 0.25f), 0.35f, 1.5f);
        lateT = 0.8f / std::sqrt((float)NLINES) * comp;

        /* CLOUD: bloom. The early diffusers feed themselves through a delay; the lines get more
         * and more of that instead of the direct sound. */
        bloomFb = type == TYPE_CLOUD ? 0.86f * std::sqrt(c) : 0.0f;
        bloomMix = type == TYPE_CLOUD ? c : 0.0f;
        bloomNorm = std::sqrt(1.0f - bloomFb * bloomFb);
        /* SHIMMER: at most what the lines lose per round may come back an octave up */
        const float meanLoss = (float)(num / (2 * NLINES));
        shimGain = type == TYPE_SHIMMER ? 0.95f * c * std::sqrt(std::max(0.0f, meanLoss)) / std::sqrt((float)NLINES) : 0.0f;
        /* SWELL */
        swellUp = 1.0f - std::exp(-1.0f / (swell_s(c) * 0.4f * sr));
        /* TIDE */
        tideInc = 6.2831853f * SUB / (tide_s(c) * sr);
        tideDepth = type == TYPE_TIDE ? 0.7f : 0.0f;
        /* DRONE */
        duck = hold;
        /* CHOIR: where the three formants go. The right side sings with a slightly smaller mouth. */
        {
            const float pos = c * 4.0f;
            const int i0 = std::min(3, (int)pos);
            const float fr = pos - (float)i0;
            for (int ch = 0; ch < 2; ch++)
                for (int k = 0; k < NFORMANTS; k++) {
                    Formant &f = chan[ch].fm[k];
                    f.ft = (VOWELS[i0][k] + fr * (VOWELS[i0 + 1][k] - VOWELS[i0][k])) * (ch ? 1.07f : 1.0f);
                    f.gain = k == 0 ? 1.0f : k == 1 ? 0.63f : 0.32f;
                }
        }
        /* MODAL: tune the bank. Partial n of a note sits at note * n^e: e < 1 squeezes them
         * together (gamelan), 1 is a string, e > 1 pulls them apart (bell). They ring for half
         * the DECAY time (0.15 .. 8 s), the higher the shorter. */
        if (type == TYPE_MODAL) {
            const float e = 0.7f + 0.6f * c;
            const float ring = clamp(0.5f * p.decayS, 0.15f, 8.0f);
            const float *notes = CHORDS[p.chord < 0 || p.chord >= NCHORDS ? 1 : p.chord];
            const float rootHz = 130.8128f * std::pow(2.0f, (float)(p.root < 0 ? 0 : p.root > 11 ? 11 : p.root) / 12.0f);
            for (int n = 0; n < NNOTES; n++)
                for (int k = 0; k < NPARTIALS; k++) {
                    Mode &m = mode[n * NPARTIALS + k];
                    const float hz = rootHz * std::pow(2.0f, notes[n] / 12.0f) * std::pow((float)(k + 1), e);
                    const float t60 = ring / std::sqrt((float)(k + 1));
                    const double r = hz < 0.4f * sr && hz < 9000.0f ? std::pow(10.0, -3.0 / ((double)t60 * sr)) : 0.0;
                    const double w = 6.283185307179586 * hz / sr;
                    m.c = r * std::cos(w);
                    m.s = r * std::sin(w);
                    /* (1 - r) would give a held tone on the resonance the same level at every ring
                     * time, its square root would do that for noise: in between, so that neither a
                     * pad nor a drum loop falls out of range */
                    m.in = r > 0 ? (float)(std::pow(1.0 - r, 0.75) * MODAL_GAIN / (float)(k + 1)) : 0.0f;
                    const float side = ((n + k) & 1) ? 0.8f : 0.2f;
                    m.panL = std::sqrt(1.0f - side);
                    m.panR = std::sqrt(side);
                }
        }
    }

    /* in and out may be the same buffers */
    void process(const float *inL, const float *inR, float *outL, float *outR, uint32_t n) {
        float xin[2][SUB], wetb[2][SUB];
        while (n) {
            const int len = (int)std::min<uint32_t>(n, SUB);
            const float il = 1.0f / len;
            const bool isSwell = type == TYPE_SWELL, isDrone = type == TYPE_DRONE, isShim = shimGain > 0, isTide = tideDepth > 0;
            const bool isChoir = type == TYPE_CHOIR, isModal = type == TYPE_MODAL;
            float ring[2][SUB];
            for (int i = 0; i < len; i++) {   /* some of the other side: a hard-panned sound still fills the room */
                const float l = inL[i], r = inR[i];
                const float sw = isSwell ? swellGain(std::fabs(l) + std::fabs(r)) : 1.0f;
                xin[0][i] = (0.7f * l + 0.3f * r) * sw;
                xin[1][i] = (0.7f * r + 0.3f * l) * sw;
                if (isModal) {   /* the reverb hears the bank instead of the input */
                    const double x = 0.5 * (double)(l + r);
                    double ol = 0, orr = 0;
                    for (Mode &m : mode) {
                        const double u = m.c * m.u - m.s * m.v + m.in * x;
                        m.v = m.s * m.u + m.c * m.v;
                        m.u = u;
                        ol += m.v * m.panL;
                        orr += m.v * m.panR;
                    }
                    /* a tone parked on a resonance swells: rounded off before it goes on */
                    xin[0][i] = ring[0][i] = bankCeiling((float)ol);
                    xin[1][i] = ring[1][i] = bankCeiling((float)orr);
                }
            }
            const float late0 = late;
            late = approach(late, lateT);
            const float lateInc = (late - late0) * il;
            if (isTide) { tidePhase += tideInc; if (tidePhase > 6.2831853f) tidePhase -= 6.2831853f; }
            for (int c = 0; c < 2; c++) {
                Channel &ch = chan[c];
                ch.pre.plan(glide, 0, len);
                ch.bloom.plan(glide, 0, len);
                for (Allpass &a : ch.early) a.dl.plan(glide, 0, len);
                for (Line &l : ch.line) {
                    l.phase += l.phaseInc;
                    if (l.phase > 6.2831853f) l.phase -= 6.2831853f;
                    l.dl.plan(glide, l.depth * std::sin(l.phase), len);
                    for (Allpass &a : l.ap) a.dl.plan(glide, 0, len);
                }
                if (isChoir)   /* the formants glide to a new vowel */
                    for (Formant &f : ch.fm) {
                        if (f.f != f.ft) { f.f += (f.ft - f.f) * formantK; if (std::fabs(f.ft - f.f) < 0.5f) f.f = f.ft; }
                        f.tune(sr, FORMANT_K);
                    }
                /* TIDE: one wave per channel, a quarter apart; it moves level and brightness */
                float tg = 1.0f, ta = 1.0f;
                if (isTide) {
                    const float wave = 0.5f + 0.5f * std::sin(tidePhase + (c ? 1.5707963f : 0.0f));
                    tg = 1.0f - tideDepth * (1.0f - wave);
                    ta = 0.10f + 0.90f * wave * wave;
                }
                float hp = ch.hp, lp = ch.lp, lg = late0, bl = ch.bloomLp, tl = ch.tideLp, level = ch.level, shim = ch.shim;
                const float ge = gEarly, gl = gLate, ad = aDamp, eb = earlyBlend, bf = bloomFb, bm = bloomMix, bn = bloomNorm;
                for (int i = 0; i < len; i++) {
                    float x = xin[c][i];
                    hp += aHp * (x - hp);
                    x -= hp;
                    lp += aIn * (x - lp);
                    const float pd = ch.pre.read();
                    ch.pre.write(lp);
                    const float rec = ch.bloom.read();
                    float e = pd + bf * rec;
                    for (int k = 0; k < NEARLY; k++) e = ch.early[k].tick(e, ge);
                    bl += aBloom * (e - bl);
                    ch.bloom.write(bl);
                    e = pd + eb * (e - pd);
                    float feed = e + bm * (rec * bn - e) + shim;
                    if (isDrone) feed *= 1.0f / (1.0f + duck * 60.0f * level);   /* the fuller, the less goes in */
                    float sum = 0;
                    for (int k = 0; k < NLINES; k++) {
                        Line &l = ch.line[k];
                        float t = l.dl.read();
                        l.dl.write(feed + l.fb * l.g);
                        t = l.ap[0].tick(t, gl);
                        t = l.ap[1].tick(t, gl);
                        l.lp += ad * (t - l.lp);
                        float f = l.lp;
                        if (isDrone) {   /* a loop that loses nothing needs DC kept out and a soft ceiling */
                            l.hp += aLineHp * (f - l.hp);
                            f -= l.hp;
                            f = droneCeiling(f);
                        }
                        l.fb = f;
                        sum += f;
                    }
                    if (isShim) {
                        float s = ch.oct.tick(sum);
                        ch.oct.lp += aShimLp * (s - ch.oct.lp);
                        ch.oct.hp += aShimHp * (ch.oct.lp - ch.oct.hp);
                        s = (ch.oct.lp - ch.oct.hp) * shimGain;
                        shim = s > 1.0f ? 1.0f : s < -1.0f ? -1.0f : s;
                    } else shim = 0;
                    level += aLevel * (sum * sum * (1.0f / NLINES) - level);
                    float y = 0.3f * e * (1.0f - bm) + sum * lg;
                    if (isTide) { tl += ta * (y - tl); y = tl * tg; }
                    if (isChoir) y = CHOIR_GAIN * (ch.fm[0].gain * ch.fm[0].tick(y) + ch.fm[1].gain * ch.fm[1].tick(y) + ch.fm[2].gain * ch.fm[2].tick(y));
                    if (isModal) y += MODAL_DIRECT * ring[c][i];
                    wetb[c][i] = y;
                    lg += lateInc;
                }
                ch.hp = hp; ch.lp = lp; ch.bloomLp = bl; ch.tideLp = tl; ch.level = level; ch.shim = shim;
            }
            const float dry0 = dry, wet0 = wet;
            dry = approach(dry, dryT);
            wet = approach(wet, wetT);
            const float dInc = (dry - dry0) * il, wInc = (wet - wet0) * il;
            float dg = dry0, wg = wet0, peak = 0;
            for (int i = 0; i < len; i++) peak += std::fabs(wetb[0][i]) + std::fabs(wetb[1][i]);
            if (!(peak < 64.0f * len)) {   /* never seen; NaN or a runaway would otherwise stay in the loops for good */
                reset();
                std::memset(wetb, 0, sizeof wetb);
            }
            for (int i = 0; i < len; i++) {
                const float l = inL[i], r = inR[i];
                outL[i] = l * dg + ceiling(wetb[0][i] * wg);
                outR[i] = r * dg + ceiling(wetb[1][i] * wg);
                dg += dInc; wg += wInc;
            }
            inL += len; inR += len; outL += len; outR += len; n -= (uint32_t)len;
        }
    }

private:
    static float clamp(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
    float approach(float cur, float tgt) const {
        cur += (tgt - cur) * gainK;
        return std::fabs(tgt - cur) < 1e-5f ? tgt : cur;
    }
    /* the reverb alone never above 0.99: untouched below -3 dBFS, rounded off above */
    static inline float ceiling(float x) {
        const float a = std::fabs(x);
        if (a <= 0.7f) return x;
        const float t = std::min((a - 0.7f) / 0.3f, 3.0f), t2 = t * t;
        return std::copysign(0.7f + 0.29f * t * (27 + t2) / (27 + 9 * t2), x);
    }
    static inline float bankCeiling(float x) {
        const float a = std::fabs(x);
        if (a <= 0.5f) return x;
        const float t = std::min((a - 0.5f) / 0.5f, 3.0f), t2 = t * t;
        return std::copysign(0.5f + 0.49f * t * (27 + t2) / (27 + 9 * t2), x);
    }
    /* DRONE: inside the loop. Untouched up to 0.6 so that a quiet drone loses nothing per round,
     * rounded off towards 1 above. */
    static inline float droneCeiling(float x) {
        const float a = std::fabs(x);
        if (a <= 0.6f) return x;
        const float t = std::min((a - 0.6f) / 0.4f, 3.0f), t2 = t * t;
        return std::copysign(0.6f + 0.39f * t * (27 + t2) / (27 + 9 * t2), x);
    }
    /* SWELL: the gain on the way into the reverb. A new note (the level jumps) or sound after
     * silence takes it down to nothing within a few ms, then it rises over the swell time. */
    inline float swellGain(float a) {
        envFast += aFast * (a - envFast);
        envSlow += aSlow * (a - envSlow);
        const bool silent = envFast < 0.002f;
        if (silent) { if (quiet < (1 << 30)) quiet++; } else quiet = 0;
        if (refractory > 0) refractory--;
        else if (!silent && envFast > 2.0f * envSlow + 0.004f) { falling = true; refractory = (int)(0.08f * sr); }
        if (quiet > (int)(0.15f * sr)) falling = true;
        if (falling) {
            swell *= 1.0f - aFast;
            if (swell < 0.003f) { swell = 0; falling = false; }
        } else {
            swell += swellUp * (1.02f - swell);
            if (swell > 1.0f) swell = 1.0f;
        }
        return swell * swell;
    }

    static constexpr float FORMANT_K = 1.0f / 9.0f;   /* 1/Q of the vowel filters */
    static constexpr float CHOIR_GAIN = 0.5f;         /* three resonant peaks: brought down to the other types' level */
    static constexpr float MODAL_GAIN = 2.5f;
    static constexpr float MODAL_DIRECT = 0.5f;       /* the bank itself beside its reverb */
    Params p;
    Channel chan[2];
    Mode mode[NMODES];
    float formantK = 0.01f;
    int type = TYPE_CLOUD;
    float sr = 44100, glide = 0.01f, gainK = 0.02f;
    float dry = 1, wet = 0, late = 0, dryT = 1, wetT = 0, lateT = 0;
    float gEarly = 0, gLate = 0, earlyBlend = 1, aDamp = 1, aIn = 1, aHp = 0.01f;
    float aBloom = 0.5f, aShimLp = 0.5f, aShimHp = 0.03f, aLineHp = 0.005f, aLevel = 0.001f, aFast = 0.01f, aSlow = 0.001f;
    float bloomFb = 0, bloomMix = 0, bloomNorm = 1, shimGain = 0, swellUp = 0.001f, tideInc = 0, tideDepth = 0, tidePhase = 0, duck = 0;
    float envFast = 0, envSlow = 0, swell = 0;
    int quiet = 1 << 30, refractory = 0;
    bool falling = false;
};

} /* namespace cloud */
