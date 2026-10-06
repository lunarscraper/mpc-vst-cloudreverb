/* =============================================================================
 * cloudreverb_vst.cpp - Cloud Reverb: a light reverb for long, moving ambient tails as a VST2
 * insert effect for the MPC OS plugin host (Force, MPC Live/One/X/Key), armhf. The DSP is
 * cloudreverb_core.h. This file is the plug-in around it: TYPE (CLOUD, SWELL, SHIMMER, TIDE,
 * DRONE, CHOIR, MODAL), nine controls plus ROOT and CHORD for MODAL, the 32 preset slots with LOAD/SAVE (pattern of mpc-vst-rattler /
 * mpc-vst-acid) and the project chunk. Hand-written like mpc-vst-vfilter's shell (no wrapper,
 * no SDK). MIT license (see ../LICENSE).
 * ========================================================================== */
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "params.h"
#include "popup.h"    /* mpc-vst-plugins wrapper/popup.h, copied into build/ by build.sh */
#include "cloudreverb_core.h"

/* ---- VST2 ABI (hand-written; no Steinberg SDK) ---------------------------- */
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
enum {
    effOpen = 0, effClose = 1, effSetProgram = 2, effGetProgram = 3, effSetProgramName = 4,
    effGetProgramName = 5, effGetParamLabel = 6, effGetParamDisplay = 7, effGetParamName = 8,
    effSetSampleRate = 10, effSetBlockSize = 11, effMainsChanged = 12, effGetChunk = 23,
    effSetChunk = 24, effCanBeAutomated = 26, effGetProgramNameIndexed = 29, effGetPlugCategory = 35,
    effGetEffectName = 45, effGetVendorString = 47, effGetProductString = 48,
    effGetVendorVersion = 49, effCanDo = 51, effGetTailSize = 52, effGetVstVersion = 58,
};
enum { audioMasterAutomate = 0, audioMasterUpdateDisplay = 42 };
enum { effFlagsCanReplacing = 1 << 4, effFlagsProgramChunks = 1 << 5 };

/* the parameters, by key (module.json); IDX[] is their position in the generated PARAMS[] */
enum { MIX, SIZE, DECAY, PREDELAY, DIFFUSION, MOD, TONE, LOWCUT, SLOT, LOAD, SAVE, TYPE, CHARACTER, ROOT, CHORD, NKEYS };
static const char *const KEYS[NKEYS] = {
    "mix", "size", "decay", "predelay", "diffusion", "mod", "tone", "lowcut", "slot", "load", "save", "type", "character", "root", "chord",
};
static int IDX[NKEYS];
static int KEY_OF[NPARAMS];   /* PARAMS[] position -> key enum, -1 = not ours */
/* slot, load and save belong to the preset bank: never part of a chunk or a preset */
static bool is_bank_key(int i) { const int k = KEY_OF[i]; return k == SLOT || k == LOAD || k == SAVE; }

struct Plugin {
    AEffect fx;
    audioMasterCallback master = nullptr;
    std::atomic<float> cache[NPARAMS];
    std::atomic<int> notify[NPARAMS];
    float open[NPARAMS] = {0};
    volatile int release[NPARAMS] = {0};
    bool down[NPARAMS] = {false};   /* LOAD/SAVE: the host currently reports them pressed */
    std::atomic<bool> dirty{true};
    std::atomic<bool> flush{false}; /* empty the reverb before the next block */
    std::atomic<int> cur{0};        /* selected preset slot, 0-based */
    cloud::Reverb dsp;
    float sr = 44100;
    std::vector<uint8_t> chunk;
};

static int param_index(const char *key) {
    for (int i = 0; i < NPARAMS; i++) if (!std::strcmp(PARAMS[i].key, key)) return i;
    return -1;
}
static float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static void copy_str(void *dst, const char *s, size_t max) {
    std::strncpy((char *)dst, s, max - 1);
    ((char *)dst)[max - 1] = 0;
}
static int norm_to_ui(const param_t *p, float n) {
    if (p->nopts) return (int)std::lround(clamp01(n) * (p->nopts - 1));
    return (int)std::lround(p->min + (p->max - p->min) * clamp01(n));
}
static float ui_to_norm(const param_t *p, double v) {
    if (p->nopts) return p->nopts > 1 ? clamp01((float)(v / (p->nopts - 1))) : 0.0f;
    return p->max > p->min ? clamp01((float)((v - p->min) / (p->max - p->min))) : 0.0f;
}
/* a knob is used at full resolution (the 0..100 only names its ends) */
static float val_at(Plugin *w, int i) {
    const param_t *p = &PARAMS[i];
    const float n = w->cache[i].load();
    if (p->nopts) return (float)norm_to_ui(p, n);
    return p->min + (p->max - p->min) * clamp01(n);
}
static float pct(Plugin *w, int k) { return IDX[k] < 0 ? 0.0f : val_at(w, IDX[k]) * 0.01f; }
static void set_val(Plugin *w, int i, double v) {
    if (i < 0) return;
    w->cache[i].store(ui_to_norm(&PARAMS[i], v));
    w->notify[i].store(1);
}
static void start(Plugin *w, int k, double v) { set_val(w, IDX[k], v); }

struct NoDenormals {
#if defined(__arm__) && defined(__ARM_FP)
    uint32_t old = 0;
    NoDenormals() { asm volatile("vmrs %0, fpscr" : "=r"(old)); asm volatile("vmsr fpscr, %0" : : "r"(old | (1u << 24))); }
    ~NoDenormals() { asm volatile("vmsr fpscr, %0" : : "r"(old)); }
#elif defined(__x86_64__) || defined(__i386__)
    unsigned old = __builtin_ia32_stmxcsr();
    NoDenormals() { __builtin_ia32_ldmxcsr(old | 0x8040); }
    ~NoDenormals() { __builtin_ia32_ldmxcsr(old); }
#endif
};

/* knob position (0..1) -> real value */
static float law_line_ms(float x) { return 25.0f * std::pow(24.0f, x); }         /* 25 .. 600 ms */
static float law_decay_s(float x) { return 0.2f * std::pow(300.0f, x); }         /* 0.2 .. 60 s */
static float law_predelay_ms(float x) { return 250.0f * x * x; }                 /* 0 .. 250 ms */
static float law_tone_hz(float x) { return x > 0.995f ? 20000.0f : 400.0f * std::pow(50.0f, x); }   /* 400 Hz .. open */
static float law_lowcut_hz(float x) { return 20.0f * std::pow(50.0f, x); }       /* 20 Hz .. 1 kHz */
static int type_of(Plugin *w) { return IDX[TYPE] < 0 ? 0 : clampi((int)val_at(w, IDX[TYPE]), 0, cloud::NTYPES - 1); }

static void configure(Plugin *w) {
    cloud::Params p;
    const float size = pct(w, SIZE);
    p.type = type_of(w);
    p.character = pct(w, CHARACTER);
    p.root = IDX[ROOT] < 0 ? 2 : clampi((int)std::lround(val_at(w, IDX[ROOT])), 0, 11);
    p.chord = IDX[CHORD] < 0 ? 1 : clampi((int)val_at(w, IDX[CHORD]), 0, cloud::NCHORDS - 1);
    p.mix = pct(w, MIX);
    p.lineMs = law_line_ms(size);
    p.diffScale = 0.35f + 0.65f * size;
    p.decayS = law_decay_s(pct(w, DECAY));
    p.predelayMs = law_predelay_ms(pct(w, PREDELAY));
    p.diffusion = pct(w, DIFFUSION);
    p.mod = pct(w, MOD);
    p.dampHz = law_tone_hz(pct(w, TONE));
    p.lowCutHz = law_lowcut_hz(pct(w, LOWCUT));
    w->dsp.setParams(p);
}

static void start_values(Plugin *w) {   /* = factory slot 1, SOFT CLOUD */
    start(w, MIX, 40); start(w, SIZE, 70); start(w, DECAY, 68); start(w, PREDELAY, 0);
    start(w, DIFFUSION, 100); start(w, MOD, 45); start(w, TONE, 58); start(w, LOWCUT, 45);
    start(w, TYPE, cloud::TYPE_CLOUD); start(w, CHARACTER, 30);
    start(w, ROOT, 2); start(w, CHORD, 1);   /* D, FIFTHS */
}

/* ---- state as text: "key=value;" for every sound parameter. Used for the project chunk
 * and for the preset slots alike; restored by key (missing keys stay at their start values). */
static std::string build_state(Plugin *w) {
    std::string t;
    char buf[96];
    for (int i = 0; i < NPARAMS; i++) {
        if (is_bank_key(i)) continue;
        std::snprintf(buf, sizeof buf, "%s=%ld;", PARAMS[i].key, std::lround((double)val_at(w, i) * 1000.0));
        t += buf;
    }
    return t;
}
static void apply_state(Plugin *w, const std::string &t, bool with_slot) {
    for (size_t pos = 0; pos < t.size();) {
        size_t semi = t.find(';', pos);
        if (semi == std::string::npos) break;
        std::string kv = t.substr(pos, semi - pos);
        pos = semi + 1;
        size_t eq = kv.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = kv.substr(0, eq);
        if (key == "prog") {   /* which slot a project had selected; never loads the slot */
            const int v = std::atoi(kv.c_str() + eq + 1);
            if (with_slot && v >= 0 && v < 32) w->cur.store(v);
            continue;
        }
        const int i = param_index(key.c_str());
        if (i >= 0 && !is_bank_key(i)) set_val(w, i, std::atol(kv.c_str() + eq + 1) / 1000.0);
    }
    w->dirty.store(true);
}

/* ---------------------------------------------------------------------------
 * Preset bank (pattern of mpc-vst-rattler): NSLOTS slots shared by every Cloud Reverb instance
 * and every project, kept in one text file on the SD card (one line per saved slot:
 * "index<TAB>name<TAB>state"). The slots are also the plugin's VST programs, so the host's
 * PRESET list shows and selects them; the PRESET knob + LOAD/SAVE buttons on the PRESET tab
 * do the same from the skin. The first slots come with factory settings until SAVE overwrites
 * them (a factory setting is never written to the file). Values are thousandths of the 0..100
 * a control shows on the host's side (integers: no decimal point, so no locale trouble).
 * ------------------------------------------------------------------------- */
#define NSLOTS 32
static std::mutex g_bank_lock;
static bool g_bank_loaded;
static std::string g_slot_chunk[NSLOTS], g_slot_name[NSLOTS], g_bank_path;

#define ST(type, character, mix, size, decay, pre, diff, mod, tone, lowcut) \
    "type=" #type "000;character=" #character "000;mix=" #mix "000;size=" #size "000;decay=" #decay "000;predelay=" #pre \
    "000;diffusion=" #diff "000;mod=" #mod "000;tone=" #tone "000;lowcut=" #lowcut "000;"
static const struct { const char *name, *state; } FACTORY[] = {
    /*                      type char mix size decay pre diff mod tone lowcut
     * type: 0 CLOUD 1 SWELL 2 SHIMMER 3 TIDE 4 DRONE 5 CHOIR 6 MODAL; root 0..11 = C..B; chord: row of cloud::CHORDS */
    {"SOFT CLOUD",       ST(0, 30, 40, 70, 68, 0, 100, 45, 58, 45)},
    {"SLOW BLOOM",       ST(0, 75, 45, 75, 72, 0, 100, 40, 55, 45)},
    {"DEEP BLOOM",       ST(0, 100, 50, 90, 82, 0, 100, 50, 45, 50)},
    {"GENTLE SWELL",     ST(1, 55, 50, 70, 70, 0, 100, 40, 60, 45)},
    {"LONG SWELL",       ST(1, 85, 55, 80, 78, 0, 100, 50, 55, 45)},
    {"SHIMMER HAZE",     ST(2, 45, 40, 75, 72, 0, 100, 45, 65, 50)},
    {"SHIMMER CHOIR",    ST(2, 80, 45, 85, 80, 0, 100, 55, 60, 55)},
    {"SLOW TIDE",        ST(3, 25, 50, 80, 78, 0, 100, 45, 60, 45)},
    {"BREATHING",        ST(3, 60, 50, 70, 72, 0, 100, 35, 62, 45)},
    {"RIPPLES",          ST(3, 95, 45, 60, 66, 0, 90, 30, 68, 50)},
    {"SOFT DRONE",       ST(4, 80, 50, 80, 75, 0, 100, 45, 55, 50)},
    {"ETERNAL",          ST(4, 100, 55, 90, 80, 0, 100, 40, 50, 55)},
    {"DARK DRONE",       ST(4, 92, 55, 85, 78, 0, 100, 35, 35, 45)},
    {"WARM HALL",        ST(0, 0, 30, 55, 52, 15, 85, 30, 60, 45)},
    {"SEND CLOUD (WET)", ST(0, 40, 100, 75, 72, 0, 100, 45, 58, 50)},
    {"SEND TIDE (WET)",  ST(3, 35, 100, 80, 78, 0, 100, 45, 60, 50)},
    {"CHOIR AH",         ST(5, 0, 50, 75, 74, 0, 100, 45, 75, 45)},
    {"CHOIR OOH",        ST(5, 100, 50, 80, 78, 0, 100, 50, 70, 45)},
    {"SYMPATHETIC",      ST(6, 50, 50, 70, 70, 0, 100, 35, 70, 40) "root=2000;chord=8000;"},
    {"SINGING BOWLS",    ST(6, 90, 55, 80, 76, 0, 100, 40, 75, 40) "root=2000;chord=1000;"},
    {"GAMELAN MIST",     ST(6, 8, 50, 75, 72, 0, 100, 40, 70, 40) "root=9000;chord=4000;"},
};
enum { NFACTORY = (int)(sizeof FACTORY / sizeof FACTORY[0]) };

/* Next to the plugin's own folder rather than inside it, so reinstalling the plugin folder
 * doesn't take the presets with it; inside it if the parent can't be written. No dladdr:
 * /proc/self/maps has the path. CLOUDREVERB_PRESETS overrides the place (offline test). */
static std::string so_dir() {
    std::string dir;
    if (FILE *f = std::fopen("/proc/self/maps", "r")) {
        char line[1024];
        while (std::fgets(line, sizeof line, f)) {
            char *p = std::strstr(line, "/cloudreverb.so");
            char *first = std::strchr(line, '/');
            if (!p || !first || first > p) continue;
            dir.assign(first, (size_t)(p - first));
            break;
        }
        std::fclose(f);
    }
    return dir;
}
static void bank_load_locked() {
    if (g_bank_loaded) return;
    g_bank_loaded = true;
    g_bank_path.clear();
    if (const char *env = std::getenv("CLOUDREVERB_PRESETS")) g_bank_path = env;
    else {
        std::string dir = so_dir(), cand[2];
        if (dir.empty()) dir = "/tmp";
        size_t cut = dir.rfind('/');
        cand[0] = (cut != std::string::npos && cut > 0 ? dir.substr(0, cut) : dir) + "/cloudreverb_presets.txt";
        cand[1] = dir + "/cloudreverb_presets.txt";
        for (const std::string &c : cand)             /* an existing file wins ... */
            if (FILE *f = std::fopen(c.c_str(), "r")) { std::fclose(f); g_bank_path = c; break; }
        for (int i = 0; i < 2 && g_bank_path.empty(); i++)   /* ... else the first place we may write */
            if (FILE *f = std::fopen(cand[i].c_str(), "a")) { std::fclose(f); g_bank_path = cand[i]; }
    }
    if (g_bank_path.empty()) return;
    if (FILE *f = std::fopen(g_bank_path.c_str(), "r")) {
        static char line[4096];
        while (std::fgets(line, sizeof line, f)) {
            line[std::strcspn(line, "\r\n")] = 0;
            char *t1 = std::strchr(line, '\t');
            char *t2 = t1 ? std::strchr(t1 + 1, '\t') : nullptr;
            int idx = std::atoi(line);
            if (!t2 || idx < 1 || idx > NSLOTS) continue;
            *t1 = *t2 = 0;
            g_slot_name[idx - 1] = t1 + 1;
            g_slot_chunk[idx - 1] = t2 + 1;
        }
        std::fclose(f);
    }
}
static bool bank_write_locked() {   /* whole file, via a temp file so a power cut can't leave half a bank */
    if (g_bank_path.empty()) return false;
    std::string tmp = g_bank_path + ".tmp";
    FILE *f = std::fopen(tmp.c_str(), "w");
    if (!f) return false;
    for (int i = 0; i < NSLOTS; i++)
        if (!g_slot_chunk[i].empty())
            std::fprintf(f, "%d\t%s\t%s\n", i + 1, g_slot_name[i].c_str(), g_slot_chunk[i].c_str());
    return std::fclose(f) == 0 && std::rename(tmp.c_str(), g_bank_path.c_str()) == 0;
}
static std::string slot_title(int i) {
    std::lock_guard<std::mutex> lk(g_bank_lock);
    char buf[32];
    if (g_slot_chunk[i].empty()) {
        if (i < NFACTORY) return FACTORY[i].name;
        std::snprintf(buf, sizeof buf, "%02d (empty)", i + 1);
        return buf;
    }
    if (!g_slot_name[i].empty()) return g_slot_name[i];
    std::snprintf(buf, sizeof buf, "Cloud %02d", i + 1);
    return buf;
}
static void slot_load(Plugin *w) {
    const int n = clampi(w->cur.load(), 0, NSLOTS - 1);
    std::string c;
    { std::lock_guard<std::mutex> lk(g_bank_lock); c = g_slot_chunk[n]; }
    if (c.empty() && n < NFACTORY) c = FACTORY[n].state;
    if (c.empty()) return;              /* an empty slot leaves the current setting alone */
    start_values(w);
    apply_state(w, c, false);
}
static bool slot_save(Plugin *w) {
    const int n = clampi(w->cur.load(), 0, NSLOTS - 1);
    const std::string c = build_state(w);
    std::lock_guard<std::mutex> lk(g_bank_lock);
    g_slot_chunk[n] = c;
    return bank_write_locked();
}

static void processReplacing(AEffect *e, float **in, float **out, int32_t n) {
    Plugin *w = (Plugin *)e->object;
    NoDenormals nd;
    if (w->dirty.exchange(false)) configure(w);
    if (w->flush.exchange(false)) w->dsp.reset();
    if (n > 0) w->dsp.process(in[0], in[1], out[0], out[1], (uint32_t)n);
    bool any = false;
    for (int i = 0; i < NPARAMS; i++) {
        if (w->release[i]) { w->release[i] = 0; any = true; w->master(&w->fx, audioMasterAutomate, i, 0, 0, 0.0f); }
        if (!w->notify[i].exchange(0)) continue;
        any = true;
        w->master(&w->fx, audioMasterAutomate, i, 0, 0, w->cache[i].load());
    }
    if (any) w->master(&w->fx, audioMasterUpdateDisplay, 0, 0, 0, 0.0f);
}

static void setParameter(AEffect *e, int32_t i, float n) {
    Plugin *w = (Plugin *)e->object;
    if (i < 0 || i >= NPARAMS) return;
    const param_t *p = &PARAMS[i];
    if (popup_set(w->open, i, n)) return;
    const int key = KEY_OF[i];
    if (key == SLOT) {   /* browsing only: LOAD loads, so turning the knob can't change what is playing */
        const int v = (int)std::lround(clamp01(n) * (NSLOTS - 1));
        if (v != w->cur.load()) { w->cur.store(v); w->master(&w->fx, audioMasterUpdateDisplay, 0, 0, 0, 0.0f); }
        return;
    }
    if (key == LOAD || key == SAVE) {
        const bool down = n > 0.5f;
        const bool rising = down && !w->down[i];   /* an echo of our own automate must not fire again */
        w->down[i] = down;
        if (rising) {
            if (key == LOAD) slot_load(w); else slot_save(w);
            w->release[i] = 1;
            w->master(&w->fx, audioMasterUpdateDisplay, 0, 0, 0, 0.0f);
        }
        return;
    }
    bool nudge = false;
    if (p->nopts > 1) {
        float pos = clamp01(n) * (p->nopts - 1);
        if (std::fabs(pos - std::round(pos)) > 0.001f) {
            float cur = w->cache[i].load() * (p->nopts - 1);
            n = (float)clampi((int)std::lround(cur) + (pos > cur ? 1 : -1), 0, p->nopts - 1) / (p->nopts - 1);
            nudge = true;
        }
    }
    w->cache[i].store(clamp01(n));
    w->dirty.store(true);
    if (key == TYPE) w->master(&w->fx, audioMasterUpdateDisplay, 0, 0, 0, 0.0f);   /* CHARACTER's readout changes with it */
    if (!nudge) popup_picked(w->open, w->release, i);
}
static float getParameter(AEffect *e, int32_t i) {
    Plugin *w = (Plugin *)e->object;
    if (i < 0 || i >= NPARAMS) return 0.0f;
    const int key = KEY_OF[i];
    if (key == SLOT) return (float)w->cur.load() / (NSLOTS - 1);
    if (key == LOAD || key == SAVE) return 0.0f;   /* triggers always read released */
    if (popup_is(i)) return w->open[i];
    return w->cache[i].load();
}

/* project chunk: "CLRV1;" + the state + "prog=<slot>;" */
static intptr_t get_chunk(Plugin *w, void **ptr) {
    std::string t = "CLRV1;" + build_state(w);
    char buf[24];
    std::snprintf(buf, sizeof buf, "prog=%d;", w->cur.load());
    t += buf;
    w->chunk.assign(t.begin(), t.end());
    *ptr = w->chunk.data();
    return (intptr_t)w->chunk.size();
}
static intptr_t set_chunk(Plugin *w, const void *data, intptr_t len) {
    if (!data || len < 6) return 0;
    std::string t((const char *)data, (size_t)len);
    t.resize(std::strlen(t.c_str()));   /* stop at a NUL the host may have included */
    if (t.compare(0, 6, "CLRV1;")) return 0;
    apply_state(w, t.substr(6), true);
    return 1;
}

static void fmt_hz(char *b, size_t n, double hz) {
    if (hz >= 1000) std::snprintf(b, n, "%.1f kHz", hz / 1000);
    else std::snprintf(b, n, "%.0f Hz", hz);
}
static void display(Plugin *w, int idx, char *buf, size_t n) {
    const param_t *pp = &PARAMS[idx];
    const int key = KEY_OF[idx];
    if (key == SLOT) { std::snprintf(buf, n, "%s", slot_title(clampi(w->cur.load(), 0, NSLOTS - 1)).c_str()); return; }
    if (key == LOAD || key == SAVE) { buf[0] = 0; return; }
    if (popup_is(idx)) {
        const int u = norm_to_ui(pp, w->open[idx]);
        if (pp->nopts) std::snprintf(buf, n, "%s", pp->opts[u]); else std::snprintf(buf, n, "%d", u);
        return;
    }
    const int u = norm_to_ui(pp, w->cache[idx].load());
    if (pp->nopts) { std::snprintf(buf, n, "%s", pp->opts[u]); return; }
    const float x = (val_at(w, idx) - pp->min) / (pp->max > pp->min ? pp->max - pp->min : 1);
    switch (key) {
    case DECAY: {
        const float s = law_decay_s(x);
        if (s < 10) std::snprintf(buf, n, "%.2f s", s); else std::snprintf(buf, n, "%.1f s", s);
        break;
    }
    case PREDELAY: std::snprintf(buf, n, "%.0f ms", law_predelay_ms(x)); break;
    case TONE: if (x > 0.995f) std::snprintf(buf, n, "OPEN"); else fmt_hz(buf, n, law_tone_hz(x)); break;
    case LOWCUT: fmt_hz(buf, n, law_lowcut_hz(x)); break;
    case ROOT: {
        static const char *const NOTE[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
        std::snprintf(buf, n, "%s", NOTE[clampi((int)std::lround(val_at(w, idx)), 0, 11)]);
        break;
    }
    case CHARACTER:   /* what it does depends on the TYPE, so the readout says */
        switch (type_of(w)) {
        case cloud::TYPE_SWELL: std::snprintf(buf, n, "SWELL %.1f s", cloud::swell_s(x)); break;
        case cloud::TYPE_SHIMMER: std::snprintf(buf, n, "SHIMMER %d %%", u); break;
        case cloud::TYPE_TIDE: std::snprintf(buf, n, "WAVE %.1f s", cloud::tide_s(x)); break;
        case cloud::TYPE_DRONE: std::snprintf(buf, n, "HOLD %d %%", u); break;
        case cloud::TYPE_CHOIR: {   /* on a vowel: "VOWEL A"; between two: "VOWEL A-E" */
            static const char V[] = "AEIOU";
            const float pos = x * 4.0f;
            const int i0 = clampi((int)pos, 0, 3);
            const float fr = pos - (float)i0;
            if (fr < 0.25f) std::snprintf(buf, n, "VOWEL %c", V[i0]);
            else if (fr > 0.75f) std::snprintf(buf, n, "VOWEL %c", V[i0 + 1]);
            else std::snprintf(buf, n, "VOWEL %c-%c", V[i0], V[i0 + 1]);
            break;
        }
        case cloud::TYPE_MODAL: std::snprintf(buf, n, "%s %d %%", u < 40 ? "GAMELAN" : u > 60 ? "BELL" : "STRING", u); break;
        default: std::snprintf(buf, n, "BLOOM %d %%", u); break;
        }
        break;
    default: std::snprintf(buf, n, "%d %%", u); break;
    }
}

static intptr_t dispatcher(AEffect *e, int32_t op, int32_t idx, intptr_t v, void *p, float o) {
    Plugin *w = (Plugin *)e->object;
    switch (op) {
    case effOpen: return 1;
    case effClose: delete w; return 1;
    case effSetProgram:
        if (v >= 0 && v < NSLOTS) {
            w->cur.store((int)v);
            slot_load(w);
            w->master(&w->fx, audioMasterUpdateDisplay, 0, 0, 0, 0.0f);
        }
        return 0;
    case effGetProgram: return w->cur.load();
    case effGetProgramName: if (p) copy_str(p, slot_title(clampi(w->cur.load(), 0, NSLOTS - 1)).c_str(), 24); return 0;
    case effGetProgramNameIndexed:
        if (idx < 0 || idx >= NSLOTS || !p) return 0;
        copy_str(p, slot_title(idx).c_str(), 24);
        return 1;
    case effSetProgramName: {   /* only a saved slot has a line in the file to carry the name */
        const int n = clampi(w->cur.load(), 0, NSLOTS - 1);
        std::lock_guard<std::mutex> lk(g_bank_lock);
        if (!p || g_slot_chunk[n].empty()) return 0;
        std::string nm((const char *)p);
        for (char &c : nm) if (c == '\t' || c == '\n' || c == '\r') c = ' ';
        g_slot_name[n] = nm.substr(0, 23);
        bank_write_locked();
        return 0;
    }
    case effGetPlugCategory: return 1;   /* kPlugCategEffect */
    case effGetEffectName:
    case effGetProductString: copy_str(p, PLUG_NAME, 32); return 1;
    case effGetVendorString: copy_str(p, PLUG_VENDOR, 32); return 1;
    case effGetVendorVersion: return PLUG_VERSION;
    case effGetVstVersion: return 2400;
    case effCanBeAutomated: return idx >= 0 && idx < NPARAMS && !is_bank_key(idx);
    case effGetParamName: if (idx >= 0 && idx < NPARAMS) copy_str(p, PARAMS[idx].name, 32); return 1;
    case effGetParamLabel: if (idx >= 0 && idx < NPARAMS) copy_str(p, PARAMS[idx].unit, 8); return 1;
    case effGetParamDisplay: {
        if (idx < 0 || idx >= NPARAMS) return 0;
        char buf[32];
        display(w, idx, buf, sizeof buf);
        copy_str(p, buf, 24);
        return 1;
    }
    case effSetSampleRate:
        if (o > 0 && o != w->sr) { w->sr = o; w->dsp.setSampleRate(o); w->dirty.store(true); }
        return 1;
    case effSetBlockSize: return 1;
    case effMainsChanged: if (!v) w->flush.store(true); return 1;   /* switched off: no old tail when it comes back */
    case effGetTailSize: return (intptr_t)(law_decay_s(pct(w, DECAY)) * w->sr);
    case effCanDo: return -1;
    case effGetChunk: return get_chunk(w, (void **)p);
    case effSetChunk: return set_chunk(w, p, v);
    default: return 0;
    }
}

extern "C" __attribute__((visibility("default"))) AEffect *VSTPluginMain(audioMasterCallback master) {
    static std::once_flag once;
    std::call_once(once, [] {
        for (int i = 0; i < NPARAMS; i++) KEY_OF[i] = -1;
        for (int k = 0; k < NKEYS; k++) { IDX[k] = param_index(KEYS[k]); if (IDX[k] >= 0) KEY_OF[IDX[k]] = k; }
    });
    { std::lock_guard<std::mutex> lk(g_bank_lock); bank_load_locked(); }
    Plugin *w = new Plugin();
    w->master = master;
    for (int i = 0; i < NPARAMS; i++) { w->cache[i].store(PARAMS[i].def); w->notify[i].store(0); }
    start_values(w);
    configure(w);
    w->dsp.setSampleRate(w->sr);
    w->dirty.store(false);
    AEffect *e = &w->fx;
    std::memset(e, 0, sizeof *e);
    e->magic = 0x56737450;
    e->dispatcher = dispatcher;
    e->setParameter = setParameter;
    e->getParameter = getParameter;
    e->processReplacing = processReplacing;
    e->numParams = NPARAMS;
    e->numPrograms = NSLOTS;
    e->numInputs = 2;
    e->numOutputs = 2;
    e->flags = effFlagsCanReplacing | effFlagsProgramChunks;
    e->uniqueID = PLUG_UID;
    e->version = PLUG_VERSION;
    e->object = w;
    return e;
}
