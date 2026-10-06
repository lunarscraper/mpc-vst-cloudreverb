/* Offline x86 test of cloudreverb_vst.cpp (test.sh builds it with ASan/UBSan): MIX 0 passes the
 * input untouched, every TYPE grows a tail, DECAY sets its length (measured against the
 * readout), PRE-DELAY, TONE, LOW CUT, DIFFUSION and MODULATION do their job, the host's block
 * size does not change the sound; then what each TYPE is for: CLOUD's bloom delays the rise,
 * SWELL fades notes in, SHIMMER puts octaves into the tail, TIDE rolls waves over it (left and
 * right apart), DRONE holds and stays bounded, CHOIR colours the tail like a vowel, MODAL rings in
 * the chord on ROOT. Everything swept under loud noise, the 32 preset
 * slots, a project chunk, other sample rates, no NaN/denormals. With "bench" as the second
 * argument it only measures CPU load, per TYPE. Prints PASSED/FAILED. */
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <dlfcn.h>
#include <unistd.h>

struct AEffect;
typedef intptr_t (*audioMasterCallback)(AEffect *, int32_t, int32_t, intptr_t, void *, float);
struct AEffect {
    int32_t magic;
    intptr_t (*dispatcher)(AEffect *, int32_t, int32_t, intptr_t, void *, float);
    void (*process)(AEffect *, float **, float **, int32_t);
    void (*setParameter)(AEffect *, int32_t, float);
    float (*getParameter)(AEffect *, int32_t);
    int32_t numPrograms, numParams, numInputs, numOutputs, flags;
    intptr_t resvd1, resvd2;
    int32_t initialDelay, realQualities, offQualities;
    float ioRatio;
    void *object, *user;
    int32_t uniqueID, version;
    void (*processReplacing)(AEffect *, float **, float **, int32_t);
    void (*processDoubleReplacing)(AEffect *, double **, double **, int32_t);
    char future[56];
};
static intptr_t master(AEffect *, int32_t, int32_t, intptr_t, void *, float) { return 0; }
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); } } while (0)
static float SR = 44100;
static int BS = 128;
static bool bad = false;
typedef std::vector<float> Buf;

static int param(AEffect *e, const char *name) {
    char buf[64];
    for (int i = 0; i < e->numParams; i++) { buf[0] = 0; e->dispatcher(e, 8, i, 0, buf, 0); if (!std::strcmp(buf, name)) return i; }
    std::printf("FAIL: no parameter %s\n", name); fails++; return 0;
}
static std::string display(AEffect *e, const char *name) { char buf[64] = {0}; e->dispatcher(e, 7, param(e, name), 0, buf, 0); return buf; }
static std::string progname(AEffect *e, int i) { char b[64] = {0}; e->dispatcher(e, 29, i, 0, b, 0); return b; }
static void set(AEffect *e, const char *name, double v) {
    double lo = 0, hi = 100;
    if (!std::strcmp(name, "Preset")) { lo = 1; hi = 32; }
    if (!std::strcmp(name, "Type")) { lo = 0; hi = 6; }
    if (!std::strcmp(name, "Root")) { lo = 0; hi = 11; }
    if (!std::strcmp(name, "Chord")) { lo = 0; hi = 8; }
    e->setParameter(e, param(e, name), (float)((v - lo) / (hi - lo)));
}
static void press(AEffect *e, const char *name) { e->setParameter(e, param(e, name), 1.0f); e->setParameter(e, param(e, name), 0.0f); }
static const char *const KNOBS[8] = {"Mix", "Size", "Decay", "Predelay", "Diffusion", "Modulation", "Tone", "Low Cut"};
/* all ten at once */
static void setup(AEffect *e, int type, double character, double mix, double size, double decay, double pre, double diff, double mod, double tone, double lowcut) {
    set(e, "Type", type); set(e, "Character", character);
    set(e, "Mix", mix); set(e, "Size", size); set(e, "Decay", decay); set(e, "Predelay", pre);
    set(e, "Diffusion", diff); set(e, "Modulation", mod); set(e, "Tone", tone); set(e, "Low Cut", lowcut);
}

/* stereo input through the effect, in place (as hosts do) */
static void run(AEffect *e, const Buf &inL, const Buf &inR, Buf &outL, Buf &outR) {
    outL = inL; outR = inR;
    for (size_t pos = 0; pos < outL.size(); pos += BS) {
        const int n = (int)std::min<size_t>(BS, outL.size() - pos);
        float *io[2] = {outL.data() + pos, outR.data() + pos};
        e->processReplacing(e, io, io, n);
    }
    for (size_t i = 0; i < outL.size(); i++)
        for (float s : {outL[i], outR[i]}) if (!std::isfinite(s) || std::fpclassify(s) == FP_SUBNORMAL) bad = true;
}
static Buf silence(double sec) { return Buf((size_t)(sec * SR), 0.0f); }
static Buf impulse(double sec) { Buf b = silence(sec); b[0] = 1.0f; return b; }
static uint32_t rs = 12345;
static Buf noise(double sec, double amp) {
    Buf b((size_t)(sec * SR));
    for (float &s : b) { rs = rs * 1664525u + 1013904223u; s = (float)(amp * ((rs >> 8) * (2.0 / 16777216.0) - 1.0)); }
    return b;
}
static Buf sine(double sec, double f, double amp) {
    Buf b((size_t)(sec * SR));
    for (size_t i = 0; i < b.size(); i++) b[i] = (float)(amp * std::sin(2 * M_PI * f * i / SR));
    return b;
}
static double rms(const Buf &a, double t0, double t1) {
    size_t i0 = (size_t)(t0 * SR), i1 = std::min(a.size(), (size_t)(t1 * SR));
    double s = 0;
    for (size_t i = i0; i < i1; i++) s += (double)a[i] * a[i];
    return std::sqrt(s / std::max<size_t>(1, i1 - i0));
}
static double peak(const Buf &a) { double p = 0; for (float s : a) p = std::max(p, (double)std::fabs(s)); return p; }
static double db(double x) { return 20 * std::log10(x + 1e-12); }
/* let the controls arrive (MIX and the delays glide), then empty the reverb: mains off */
static void flush(AEffect *e) {
    Buf a = silence(1.0), l, r;
    run(e, a, a, l, r);
    e->dispatcher(e, 12, 0, 0, nullptr, 0);
    e->dispatcher(e, 12, 0, 1, nullptr, 0);
    a = silence(0.05);
    run(e, a, a, l, r);
}
/* impulse response of the wet signal (left) */
static Buf ir(AEffect *e, double sec, Buf *right = nullptr) {
    flush(e);
    Buf l, r, x = impulse(sec);
    run(e, x, x, l, r);
    if (right) *right = r;
    return l;
}
/* time for the tail to fall 60 dB, from the slope of the energy between two windows */
static double t60(const Buf &a, double t0, double t1, double win) {
    const double d = db(rms(a, t0, t0 + win)) - db(rms(a, t1, t1 + win));
    return d > 0.01 ? 60.0 * (t1 - t0) / d : 1e9;
}

static double goertzel(const Buf &a, double f, double t0, double t1) {
    size_t i0 = (size_t)(t0 * SR), i1 = std::min(a.size(), (size_t)(t1 * SR));
    double w = 2 * M_PI * f / SR, c = std::cos(w), s1 = 0, s2 = 0;
    for (size_t i = i0; i < i1; i++) { double s0 = a[i] + 2 * c * s1 - s2; s2 = s1; s1 = s0; }
    return 2 * std::sqrt(std::max(0.0, s1 * s1 + s2 * s2 - 2 * c * s1 * s2)) / (double)(i1 - i0);
}
static Buf cat(Buf a, const Buf &b) { a.insert(a.end(), b.begin(), b.end()); return a; }
enum { CLOUD, SWELL, SHIMMER, TIDE, DRONE, CHOIR, MODAL, NT };
/* level around a frequency in noise: several bins, so one lucky bin doesn't decide */
static double band(const Buf &a, double f, double t0, double t1) {
    double s = 0;
    for (double k : {0.96, 0.98, 1.0, 1.02, 1.04}) { const double g = goertzel(a, f * k, t0, t1); s += g * g; }
    return std::sqrt(s / 5);
}

int main(int argc, char **argv) {
    char bank[] = "/tmp/cloudreverb_presets_XXXXXX";
    int fd = mkstemp(bank);
    if (fd >= 0) close(fd);
    unlink(bank);
    setenv("CLOUDREVERB_PRESETS", bank, 1);
    void *h = dlopen(argc > 1 ? argv[1] : "./cloudreverb.so", RTLD_NOW | RTLD_LOCAL);
    if (!h) { std::printf("FAILED: dlopen %s\n", dlerror()); return 1; }
    auto mainf = (AEffect * (*)(audioMasterCallback)) dlsym(h, "VSTPluginMain");
    AEffect *e = mainf(master);
    CHECK(e->numInputs == 2 && e->numOutputs == 2, "not a stereo effect");
    CHECK(e->numPrograms == 32, "not 32 programs");
    e->dispatcher(e, 0, 0, 0, nullptr, 0);
    e->dispatcher(e, 10, 0, 0, nullptr, SR);
    e->dispatcher(e, 12, 0, 1, nullptr, 0);
    static const char *const TN[NT] = {"CLOUD", "SWELL", "SHIMMER", "TIDE", "DRONE", "CHOIR", "MODAL"};
    if (argc > 2 && !std::strcmp(argv[2], "bench")) {
        std::printf("bench (this CPU, one core), stereo, everything on, per TYPE:\n");
        for (int t = 0; t < NT; t++) {
            setup(e, t, 100, 50, 80, 80, 30, 100, 100, 70, 40);
            Buf x = noise(20.0, 0.2), l, r;
            auto t0 = std::chrono::steady_clock::now();
            run(e, x, x, l, r);
            double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            std::printf("  %-8s %5.2f %%\n", TN[t], 100.0 * s / 20.0);
        }
        return 0;
    }
    std::printf("defaults: type %s, character %s, mix %s, size %s, decay %s, predelay %s, diffusion %s, mod %s, tone %s, low cut %s, preset %s\n",
                display(e, "Type").c_str(), display(e, "Character").c_str(), display(e, "Mix").c_str(), display(e, "Size").c_str(),
                display(e, "Decay").c_str(), display(e, "Predelay").c_str(), display(e, "Diffusion").c_str(), display(e, "Modulation").c_str(),
                display(e, "Tone").c_str(), display(e, "Low Cut").c_str(), display(e, "Preset").c_str());
    CHECK(display(e, "Type") == "CLOUD" && display(e, "Character") == "BLOOM 30 %" && display(e, "Preset") == "SOFT CLOUD", "start values");

    /* 1. MIX 0: the input comes out untouched, to the bit, in every TYPE, with the reverb full of sound */
    for (int t = 0; t < NT; t++) {
        setup(e, t, 80, 0, 50, 60, 20, 80, 50, 60, 30);
        Buf x = noise(1.5, 0.5), y = sine(1.5, 220, 0.9), l, r;
        run(e, x, y, l, r);
        size_t diff = 0, i0 = (size_t)(0.5 * SR);
        for (size_t i = i0; i < x.size(); i++) if (l[i] != x[i] || r[i] != y[i]) diff++;
        CHECK(diff == 0, "%s: MIX 0 changes the signal (%zu samples)", TN[t], diff);
    }
    std::printf("  mix 0: input untouched in all seven types\n");

    /* 2. every TYPE at MIX 100: no dry signal, a tail, a sensible level */
    for (int t = 0; t < NT; t++) {
        setup(e, t, 50, 100, 60, 60, 0, 100, 30, 70, 30);
        Buf a = ir(e, 3.0);
        flush(e);
        Buf x = noise(4.0, 0.25), l, r;
        run(e, x, x, l, r);
        std::printf("  %-8s %-13s impulse: first sample %.4f, tail %6.1f dB (0.3-1.5 s) | noise at -18 dB rms: wet %5.1f dB rms\n", TN[t],
                    display(e, "Character").c_str(), a[0], db(rms(a, 0.3, 1.5)), db(rms(l, 2.0, 4.0)));
        CHECK(std::fabs(a[0]) < 1e-6, "%s: MIX 100 still lets the dry impulse through", TN[t]);
        if (t != SWELL) CHECK(rms(a, 0.3, 1.5) > 3e-5, "%s: no tail", TN[t]);   /* SWELL fades a click out before it gets in */
        CHECK(db(rms(l, 2.0, 4.0)) > -34 && db(rms(l, 2.0, 4.0)) < -6, "%s: wet level %.1f dB", TN[t], db(rms(l, 2.0, 4.0)));
    }

    /* 3. DECAY: the tail's measured length follows the readout (CLOUD, no bloom) */
    for (double dk : {30.0, 50.0, 70.0}) {
        setup(e, CLOUD, 0, 100, 45, dk, 0, 85, 0, 100, 0);
        const double want = std::atof(display(e, "Decay").c_str());
        const double span = std::min(want * 0.6, 6.0);
        Buf a = ir(e, 0.3 + span + 0.5);
        const double got = t60(a, 0.3, 0.3 + span, 0.3);
        std::printf("  decay %2.0f: readout %s, measured %.2f s\n", dk, display(e, "Decay").c_str(), got);
        CHECK(got > want * 0.75 && got < want * 1.35, "DECAY %g: tail of %.2f s for a readout of %.2f s", dk, got, want);
    }

    /* 4. PRE-DELAY: nothing before it, sound right after */
    setup(e, CLOUD, 0, 100, 40, 40, 100.0 * std::sqrt(100.0 / 250.0), 80, 0, 100, 0);
    {
        Buf a = ir(e, 0.6);
        std::printf("  predelay %s: %.1f dB before 95 ms, %.1f dB in 100-200 ms\n", display(e, "Predelay").c_str(),
                    db(rms(a, 0.001, 0.095)), db(rms(a, 0.1, 0.2)));
        CHECK(display(e, "Predelay") == "100 ms", "predelay readout %s", display(e, "Predelay").c_str());
        CHECK(rms(a, 0.001, 0.095) < 1e-6, "sound before the pre-delay");
        CHECK(rms(a, 0.1, 0.2) > 1e-3, "no sound after the pre-delay");
    }

    /* 5. TONE darkens the tail (8 kHz), LOW CUT thins it (60 Hz); DIFFUSION smooths it */
    {
        double hf[2], lf[2], crest[2];
        Buf l, r;
        for (int k = 0; k < 2; k++) {
            setup(e, CLOUD, 0, 100, 45, 55, 0, 85, 0, k ? 30 : 100, 0);
            flush(e);
            Buf x = noise(1.5, 0.25);
            run(e, x, x, l, r);
            hf[k] = goertzel(l, 8000, 1.0, 1.5) + goertzel(l, 9100, 1.0, 1.5) + goertzel(l, 7300, 1.0, 1.5);
        }
        for (int k = 0; k < 2; k++) {
            setup(e, CLOUD, 0, 100, 45, 55, 0, 85, 0, 100, k ? 80 : 0);
            flush(e);
            Buf x = sine(1.5, 60, 0.25);
            run(e, x, x, l, r);
            lf[k] = rms(l, 1.0, 1.5);
        }
        for (int k = 0; k < 2; k++) {
            setup(e, CLOUD, 0, 100, 50, 50, 0, k ? 100 : 0, 0, 100, 0);
            Buf a = ir(e, 1.0);
            Buf w(a.begin() + (size_t)(0.15 * SR), a.begin() + (size_t)(0.6 * SR));
            crest[k] = peak(w) / (rms(w, 0, 1) + 1e-12);
        }
        std::printf("  tone: 8 kHz %.1f dB (open) vs %.1f dB (1.3 kHz); low cut: 60 Hz %.1f dB (20 Hz) vs %.1f dB (457 Hz); diffusion: crest %.1f (0) vs %.1f (100)\n",
                    db(hf[0]), db(hf[1]), db(lf[0]), db(lf[1]), crest[0], crest[1]);
        CHECK(hf[1] < hf[0] * 0.25, "TONE does not darken");
        CHECK(lf[1] < lf[0] * 0.25, "LOW CUT does not thin");
        CHECK(crest[0] > crest[1] * 2.0, "DIFFUSION does not smooth the tail");
        set(e, "Tone", 100);
        CHECK(display(e, "Tone") == "OPEN", "tone readout at 100: %s", display(e, "Tone").c_str());
    }

    /* 6. MODULATION moves the tail; without it the host's block size does not change the sound */
    {
        Buf a[4];
        for (int k = 0; k < 4; k++) {
            setup(e, CLOUD, 0, 100, 50, 60, 0, 85, k == 3 ? 80 : 0, 100, 0);
            BS = k == 1 ? 77 : k == 2 ? 1 : 128;
            a[k] = ir(e, 1.5);
        }
        BS = 128;
        double d01 = 0, d02 = 0, d03 = 0;
        for (size_t i = 0; i < a[0].size(); i++) {
            d01 = std::max(d01, (double)std::fabs(a[0][i] - a[1][i])); d02 = std::max(d02, (double)std::fabs(a[0][i] - a[2][i]));
            d03 = std::max(d03, (double)std::fabs(a[0][i] - a[3][i]));
        }
        std::printf("  modulation 0: blocks of 128 vs 77: %.2g, vs 1: %.2g; modulation 0 vs 80: %.3f\n", d01, d02, d03);
        CHECK(d01 < 1e-6 && d02 < 1e-6, "the host's block size changes the sound");
        CHECK(d03 > 1e-3, "MODULATION changes nothing");
    }

    /* 7. CLOUD, bloom: the reverb rises instead of starting. Where half of the tail's energy
     *    has arrived moves back. */
    {
        double half[2];
        for (int k = 0; k < 2; k++) {
            setup(e, CLOUD, k ? 100 : 0, 100, 70, 65, 0, 100, 0, 80, 0);
            Buf a = ir(e, 6.0);
            double tot = 0, acc = 0;
            for (float s : a) tot += (double)s * s;
            size_t i = 0;
            for (; i < a.size() && acc < 0.5 * tot; i++) acc += (double)a[i] * a[i];
            half[k] = i / SR;
            if (k) CHECK(display(e, "Character") == "BLOOM 100 %", "bloom readout %s", display(e, "Character").c_str());
        }
        std::printf("  CLOUD   bloom: half of the tail's energy is in after %.2f s (0) vs %.2f s (100)\n", half[0], half[1]);
        CHECK(half[1] > half[0] * 1.8 && half[1] > half[0] + 0.3, "bloom does not delay the rise");
    }

    /* 8. SWELL: a note after silence fades into the reverb, and so does a new, louder note */
    {
        setup(e, SWELL, 70, 100, 50, 55, 0, 100, 0, 100, 0);
        const double T = std::atof(display(e, "Character").c_str() + 6);
        flush(e);
        Buf x = cat(cat(silence(0.5), sine(3.0, 330, 0.1)), sine(3.0, 495, 0.4)), l, r;
        run(e, x, x, l, r);
        const double a0 = rms(l, 0.55, 0.75), a1 = rms(l, 2.5, 3.4), b0 = goertzel(l, 495, 3.55, 3.75), b1 = goertzel(l, 495, 5.5, 6.4);
        setup(e, CLOUD, 0, 100, 50, 55, 0, 100, 0, 100, 0);
        flush(e);
        Buf l2, r2;
        run(e, x, x, l2, r2);
        const double c0 = rms(l2, 0.55, 0.75), c1 = rms(l2, 2.5, 3.4);
        std::printf("  SWELL   %s (%.1f s): first note %.1f dB at the start vs %.1f dB later (CLOUD: %.1f vs %.1f); second note %.1f dB vs %.1f dB\n",
                    "fade", T, db(a0), db(a1), db(c0), db(c1), db(b0), db(b1));
        CHECK(T > 0.5 && T < 3.0, "swell readout %s", display(e, "Character").c_str());
        CHECK(a0 < a1 * 0.25, "SWELL does not fade the first note in");
        CHECK(c0 > c1 * 0.4, "CLOUD fades in as well: the comparison says nothing");
        CHECK(b0 < b1 * 0.35, "SWELL does not fade a new note in");
    }

    /* 9. SHIMMER: the octaves above a note appear in the tail */
    {
        double oct[2], two[2];
        for (int k = 0; k < 2; k++) {
            setup(e, SHIMMER, k ? 100 : 0, 100, 60, 72, 0, 100, 0, 100, 0);
            flush(e);
            Buf x = cat(sine(1.0, 220, 0.3), silence(4.0)), l, r;
            run(e, x, x, l, r);
            if (k) std::printf("  SHIMMER the note itself in that tail: 220 Hz %.1f dB\n", db(goertzel(l, 220, 2.0, 4.0)));
            oct[k] = goertzel(l, 440, 2.0, 4.0);
            two[k] = goertzel(l, 880, 2.0, 4.0);
        }
        std::printf("  SHIMMER a 220 Hz note, tail 1-3 s after it: 440 Hz %.1f dB (0) vs %.1f dB (100), 880 Hz %.1f dB vs %.1f dB\n",
                    db(oct[0]), db(oct[1]), db(two[0]), db(two[1]));
        CHECK(oct[1] > oct[0] * 10 && oct[1] > 1e-4, "no octave in the tail");
        CHECK(two[1] > two[0] * 5, "no second octave in the tail");
    }

    /* 10. TIDE: waves of level roll over a steady input, left and right not together */
    {
        setup(e, TIDE, 80, 100, 55, 60, 0, 100, 0, 100, 0);
        const double T = std::atof(display(e, "Character").c_str() + 5);
        flush(e);
        Buf x = noise(4 * T + 1.0, 0.2), l, r;
        run(e, x, x, l, r);
        std::vector<double> el, er;
        for (double t = 1.0; t + 0.1 <= 4 * T + 1.0; t += 0.1) { el.push_back(rms(l, t, t + 0.1)); er.push_back(rms(r, t, t + 0.1)); }
        double lo = 1e9, hi = 0, ml = 0, mr = 0, cc = 0, vl = 0, vr = 0;
        for (size_t i = 0; i < el.size(); i++) { lo = std::min(lo, el[i]); hi = std::max(hi, el[i]); ml += el[i] / el.size(); mr += er[i] / er.size(); }
        for (size_t i = 0; i < el.size(); i++) { cc += (el[i] - ml) * (er[i] - mr); vl += (el[i] - ml) * (el[i] - ml); vr += (er[i] - mr) * (er[i] - mr); }
        /* period: count the upward crossings of the mean */
        int ups = 0;
        for (size_t i = 1; i < el.size(); i++) if (el[i - 1] < ml && el[i] >= ml) ups++;
        std::printf("  TIDE    %s: level swings %.1f dB, %d waves in %.1f s, left/right in step %.2f (1 = together)\n", display(e, "Character").c_str(),
                    db(hi) - db(lo), ups, 4 * T, cc / std::sqrt(vl * vr + 1e-30));
        CHECK(T > 2.0 && T < 4.0, "tide readout %s", display(e, "Character").c_str());
        CHECK(db(hi) - db(lo) > 8, "no waves");
        CHECK(ups >= 3 && ups <= 5, "%d waves in four periods", ups);
        CHECK(cc / std::sqrt(vl * vr + 1e-30) < 0.6, "left and right move together");
    }

    /* 11. DRONE: at full hold the tail stays, without hold it goes; a loud input for a long time stays bounded */
    {
        double early[3], lateR[3];
        for (int k = 0; k < 3; k++) {   /* hold 0; hold 100 without modulation; hold 100 with */
            setup(e, DRONE, k ? 100 : 0, 100, 60, 45, 0, 100, k == 2 ? 40 : 0, 60, 30);
            flush(e);
            Buf x = cat(noise(2.0, 0.2), silence(28.0)), l, r;
            run(e, x, x, l, r);
            early[k] = rms(l, 3.0, 4.0);
            lateR[k] = rms(l, 28.0, 30.0);
        }
        std::printf("  DRONE   2 s of noise, then silence, level after 1 s -> after 26 s: hold 100 %.1f -> %.1f dB, with modulation 40 %.1f -> %.1f dB; hold 0 %.1f -> %.1f dB\n",
                    db(early[1]), db(lateR[1]), db(early[2]), db(lateR[2]), db(early[0]), db(lateR[0]));
        CHECK(lateR[1] > early[1] * 0.6 && lateR[1] > 1e-3, "full hold does not hold");
        CHECK(lateR[2] > early[2] * 0.15, "full hold with modulation fades too fast");
        CHECK(lateR[0] < early[0] * 0.01, "without hold the tail stays");
        setup(e, DRONE, 100, 100, 80, 100, 0, 100, 40, 100, 0);
        flush(e);
        Buf x = noise(40.0, 0.5), l, r;
        run(e, x, x, l, r);
        std::printf("  DRONE   40 s of noise at -9 dB rms into full hold: wet %.1f dB rms in the last 5 s, peak %.2f\n", db(rms(l, 35, 40)), std::max(peak(l), peak(r)));
        CHECK(std::max(peak(l), peak(r)) < 1.0 && rms(l, 35, 40) < 0.5, "the drone runs away");
    }

    /* 11b. CHOIR: the tail takes the colour of the vowel. A has its first two formants at 730
     *      and 1090 Hz, I at 270 and 2290 Hz. */
    {
        double lv[2][4];
        const double F[4] = {270, 730, 1090, 2290};
        for (int k = 0; k < 2; k++) {
            setup(e, CHOIR, k ? 50 : 0, 100, 60, 60, 0, 100, 0, 100, 0);
            CHECK(display(e, "Character") == (k ? "VOWEL I" : "VOWEL A"), "vowel readout %s", display(e, "Character").c_str());
            flush(e);
            Buf x = noise(5.0, 0.25), l, r;
            run(e, x, x, l, r);
            for (int f = 0; f < 4; f++) lv[k][f] = band(l, F[f], 1.0, 5.0);
        }
        set(e, "Character", 37);
        CHECK(display(e, "Character") == "VOWEL E-I", "vowel readout between: %s", display(e, "Character").c_str());
        std::printf("  CHOIR   noise in, level at 270 / 730 / 1090 / 2290 Hz: vowel A %.1f / %.1f / %.1f / %.1f dB, vowel I %.1f / %.1f / %.1f / %.1f dB\n",
                    db(lv[0][0]), db(lv[0][1]), db(lv[0][2]), db(lv[0][3]), db(lv[1][0]), db(lv[1][1]), db(lv[1][2]), db(lv[1][3]));
        CHECK(lv[0][1] > lv[0][0] * 2 && lv[0][2] > lv[0][3], "vowel A does not sound like A");
        CHECK(lv[1][0] > lv[1][1] * 2 && lv[1][3] > lv[1][2], "vowel I does not sound like I");
    }

    /* 11c. MODAL: noise rings back in the chord. ROOT A, FIFTHS, string: 220 Hz is in, 277 Hz
     *      (the C sharp nobody asked for) is not; ROOT C moves it; BELL moves the second partial
     *      from 440 to 542 Hz; a tone held on a resonance stays bounded; silence stays silent. */
    {
        setup(e, MODAL, 50, 100, 50, 70, 0, 100, 0, 100, 0);
        set(e, "Root", 9); set(e, "Chord", 1);
        CHECK(display(e, "Root") == "A" && display(e, "Chord") == "FIFTHS" && display(e, "Character") == "STRING 50 %", "modal readouts: %s, %s, %s",
              display(e, "Root").c_str(), display(e, "Chord").c_str(), display(e, "Character").c_str());
        flush(e);
        Buf x = cat(noise(0.5, 0.25), silence(3.0)), l, r;
        run(e, x, x, l, r);
        const double a220 = band(l, 220, 1.0, 3.0), a277 = band(l, 277.2, 1.0, 3.0), a542 = band(l, 541.7, 1.0, 3.0), lvl = rms(l, 1.0, 2.0);
        set(e, "Root", 0);
        flush(e);
        run(e, x, x, l, r);
        const double c220 = band(l, 220, 1.0, 3.0), c131 = band(l, 130.8, 1.0, 3.0);
        set(e, "Root", 9); set(e, "Character", 100);
        CHECK(display(e, "Character") == "BELL 100 %", "bell readout %s", display(e, "Character").c_str());
        flush(e);
        run(e, x, x, l, r);
        const double b542 = band(l, 541.7, 1.0, 3.0);
        std::printf("  MODAL   a noise burst, the tail 0.5-2.5 s after it (%.1f dB rms): root A: 220 Hz %.1f dB, 277 Hz %.1f dB; root C: 131 Hz %.1f dB, 220 Hz %.1f dB; 542 Hz: string %.1f dB, bell %.1f dB\n",
                    db(lvl), db(a220), db(a277), db(c131), db(c220), db(a542), db(b542));
        CHECK(a220 > a277 * 5, "the chord's note does not stand out");
        CHECK(c131 > c220 * 5, "ROOT does not move the chord");
        CHECK(b542 > a542 * 4, "the structure does not move the partials");
        set(e, "Character", 50); set(e, "Decay", 100);
        flush(e);
        Buf t = sine(6.0, 220, 0.5);
        run(e, t, t, l, r);
        const double pk = std::max(peak(l), peak(r));
        Buf z = silence(1.0);
        flush(e);
        run(e, z, z, l, r);
        std::printf("  MODAL   6 s of a 220 Hz tone at -6 dB on the resonance, longest ring: peak %.2f; silence in: %.1f dB out\n", pk, db(rms(l, 0, 1)));
        CHECK(pk < 1.0, "a tone on the resonance runs away (%.2f)", pk);
        CHECK(rms(l, 0, 1) < 1e-6, "MODAL sounds by itself");
    }

    /* 12. everything turned under loud noise, longest tail, every TYPE: finite and below 0 dBFS */
    {
        double worst = 0;
        for (int t = 0; t < NT; t++) {
            setup(e, t, 100, 100, 0, 100, 0, 100, 100, 100, 0);
            flush(e);
            for (int step = 0; step < 40; step++) {
                set(e, "Size", (step * 37) % 101); set(e, "Predelay", (step * 53) % 101); set(e, "Diffusion", (step * 29) % 101);
                set(e, "Modulation", (step * 71) % 101); set(e, "Tone", 40 + (step * 13) % 61); set(e, "Character", (step * 43) % 101);
                set(e, "Root", (step * 7) % 12); set(e, "Chord", (step * 5) % 9);
                Buf x = noise(0.25, 0.5), l, r;
                run(e, x, x, l, r);
                worst = std::max(worst, std::max(peak(l), peak(r)));
            }
        }
        std::printf("  sweep under noise at -9 dB rms, decay %s, all types: highest peak %.2f\n", display(e, "Decay").c_str(), worst);
        CHECK(worst < 1.0, "output above 0 dBFS (%.2f)", worst);
    }

    /* 13. preset slots */
    {
        CHECK(progname(e, 0) == "SOFT CLOUD" && progname(e, 11) == "ETERNAL" && progname(e, 31) == "32 (empty)", "program names: %s / %s / %s",
              progname(e, 0).c_str(), progname(e, 11).c_str(), progname(e, 31).c_str());
        e->dispatcher(e, 2, 0, 7, nullptr, 0);   /* effSetProgram: SLOW TIDE */
        CHECK(e->dispatcher(e, 3, 0, 0, nullptr, 0) == 7, "effGetProgram");
        CHECK(display(e, "Preset") == "SLOW TIDE" && display(e, "Type") == "TIDE" && display(e, "Character").compare(0, 5, "WAVE ") == 0 && display(e, "Mix") == "50 %",
              "program 8 not loaded: %s, %s, %s", display(e, "Type").c_str(), display(e, "Character").c_str(), display(e, "Mix").c_str());
        const std::string decay = display(e, "Decay");
        set(e, "Preset", 28);
        CHECK(display(e, "Preset") == "28 (empty)" && display(e, "Decay") == decay, "browsing changed the sound");
        press(e, "Load");
        CHECK(display(e, "Decay") == decay && display(e, "Type") == "TIDE", "loading an empty slot changed the sound");
        set(e, "Type", SHIMMER);
        CHECK(display(e, "Decay") == decay && display(e, "Character").compare(0, 8, "SHIMMER ") == 0, "a new TYPE: %s, %s", display(e, "Decay").c_str(), display(e, "Character").c_str());
        e->setParameter(e, param(e, "Decay"), 0.4321f);
        set(e, "Mix", 77);
        const std::string mine = display(e, "Decay");
        press(e, "Save");
        CHECK(display(e, "Preset") == "Cloud 28" && progname(e, 27) == "Cloud 28", "saved slot name: %s", display(e, "Preset").c_str());
        CHECK(e->getParameter(e, param(e, "Load")) == 0 && e->getParameter(e, param(e, "Save")) == 0, "buttons do not read released");
        set(e, "Preset", 12); press(e, "Load");
        CHECK(display(e, "Type") == "DRONE" && display(e, "Character") == "HOLD 100 %" && display(e, "Mix") == "55 %", "factory slot 12 not loaded");
        set(e, "Preset", 28); press(e, "Load");
        CHECK(display(e, "Type") == "SHIMMER" && display(e, "Mix") == "77 %" && display(e, "Decay") == mine, "slot 28 does not come back: %s %s %s",
              display(e, "Type").c_str(), display(e, "Mix").c_str(), display(e, "Decay").c_str());
        CHECK(std::fabs(e->getParameter(e, param(e, "Decay")) - 0.4321f) < 1e-4f, "slot: knob position not exact");
        e->dispatcher(e, 4, 0, 0, (void *)"MY CLOUD", 0);
        CHECK(progname(e, 27) == "MY CLOUD", "renaming: %s", progname(e, 27).c_str());
        AEffect *e2 = mainf(master);
        e2->dispatcher(e2, 10, 0, 0, nullptr, SR);
        e2->dispatcher(e2, 2, 0, 27, nullptr, 0);
        CHECK(display(e2, "Mix") == "77 %" && display(e2, "Type") == "SHIMMER" && display(e2, "Preset") == "MY CLOUD", "second instance cannot load slot 28");
        e2->dispatcher(e2, 1, 0, 0, nullptr, 0);
        FILE *f = std::fopen(bank, "r");
        char line[512] = {0};
        if (f) { if (!std::fgets(line, sizeof line, f)) line[0] = 0; std::fclose(f); }
        std::printf("  presets: 16 factory, slot 28 saved and reloaded; bank file: %s", line[0] ? line : "(missing)\n");
        CHECK(!std::strncmp(line, "28\tMY CLOUD\tmix=77000;", 22) && std::strstr(line, "type=2000;"), "bank file line");
    }

    /* 14. chunk: a project comes back as it was, sound included */
    setup(e, SHIMMER, 66, 100, 63, 37.5, 10, 55, 0, 71, 33);
    set(e, "Preset", 5);
    {
        void *chunk = nullptr;
        intptr_t len = e->dispatcher(e, 23, 0, 0, &chunk, 0);
        std::vector<uint8_t> copy((uint8_t *)chunk, (uint8_t *)chunk + len);
        AEffect *e2 = mainf(master);
        e2->dispatcher(e2, 10, 0, 0, nullptr, SR);
        e2->dispatcher(e2, 24, 0, (intptr_t)copy.size(), copy.data(), 0);
        for (const char *k : {"Type", "Character", "Mix", "Size", "Decay", "Predelay", "Diffusion", "Modulation", "Tone", "Low Cut", "Preset"})
            CHECK(display(e2, k) == display(e, k), "chunk: %s %s vs %s", k, display(e2, k).c_str(), display(e, k).c_str());
        Buf a = ir(e, 0.5), b = ir(e2, 0.5);
        double d = 0;
        for (size_t i = 0; i < a.size(); i++) d = std::max(d, (double)std::fabs(a[i] - b[i]));
        std::printf("  chunk %ld bytes: %.*s\n  chunk: largest difference between the two instances' tails %.2g\n", (long)len, (int)len, (const char *)copy.data(), d);
        CHECK(d < 1e-6, "the restored instance sounds different");
        e2->dispatcher(e2, 1, 0, 0, nullptr, 0);
    }

    /* 15. other sample rates: longest delays in every TYPE, and still a tail of the right length */
    for (float sr : {48000.0f, 96000.0f, 22050.0f}) {
        SR = sr;
        e->dispatcher(e, 10, 0, 0, nullptr, sr);
        double pk = 0;
        for (int t = 0; t < NT; t++) {
            setup(e, t, 100, 100, 100, 50, 100, 85, 100, 100, 0);
            Buf x = noise(0.5, 0.25), l, r;
            run(e, x, x, l, r);
            pk = std::max(pk, std::max(peak(l), peak(r)));
        }
        setup(e, CLOUD, 0, 100, 45, 50, 0, 85, 0, 100, 0);
        const double want = std::atof(display(e, "Decay").c_str());
        Buf a = ir(e, 0.3 + want * 0.6 + 0.5);
        const double got = t60(a, 0.3, 0.3 + want * 0.6, 0.3);
        std::printf("  %.0f Hz: decay readout %s, measured %.2f s; all types with everything long: peak %.2f\n", sr, display(e, "Decay").c_str(), got, pk);
        CHECK(got > want * 0.75 && got < want * 1.35 && pk < 1.0, "%.0f Hz: tail of %.2f s, peak %.2f", sr, got, pk);
    }

    CHECK(!bad, "NaN, Inf or denormals in the output");
    e->dispatcher(e, 1, 0, 0, nullptr, 0);
    unlink(bank);
    std::printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails ? 1 : 0;
}
