// surround-probe -- empirically test the 3DS "Surround" audio output mode.
//
// Plays a steady 440 Hz sine on one NDSP channel at the DSP-native 32728 Hz
// (no resampler in the path) and lets you flip the OUTPUT MODE (Stereo/Surround/
// Mono) and the voice ROUTING (front-only vs rear-only, or a continuous
// front<->rear blend) live, while capturing the headphone jack on a PC.
//
// Hypothesis: in STEREO the DSP folds rear into front at unity, so front-only
// and rear-only are identical; in SURROUND the firmware virtualizes them and
// they differ.  That difference is what makes the NW4C "span" command audible.
//
// TWO ways to drive it:
//   * MANUAL  -- press X/A/Y/L/R and record each condition to its own WAV
//                (a ~200 ms silence marker is dropped on every change so
//                 tools/analyze.py can align a pair of separate files).
//   * AUTO    -- press RIGHT on the d-pad to run the whole condition matrix
//                hands-off as ONE take: each segment is announced by a
//                COUNTABLE pip burst (segment 3 = 3 pips) then ~8 s of tone.
//                Record the entire run to ONE WAV, then tools/split_run.py
//                cuts it into the standard per-condition files. This is the
//                recommended path: every segment shares one clock, so the
//                null test aligns sample-accurately. See AUTO-RUN.md.

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <3ds.h>

#define SR        32728           // DSP native sample rate (avoids the resampler)
#define CYCLES    440             // whole sine cycles in the buffer ...
#define NSAMP     SR              // ... over exactly 1.0 s => exactly 440 Hz, seamless loop
#define AMP       0.30f           // 0.30 full-scale: headroom for any surround boost
#define MARKERFR  12              // manual marker silence length in frames (~200 ms @ 60 fps)

// --- AUTO-run timing, in 60 fps frames -------------------------------------
#define FR_INIT     90            // 1.5 s lead-in silence before the first segment
#define FR_PIP_ON    8            // ~133 ms pip tone
#define FR_PIP_OFF   8            // ~133 ms gap between pips
#define FR_LEAD     36            // ~600 ms silence between the pip header and the body
#define FR_BODY    480            // 8.0 s steady-tone body (analyze.py needs >= ~4 s)
#define FR_TAIL     36            // ~600 ms silence after the body

static float g_rearBlend = 0.0f;  // 0.0 = front-only, 1.0 = rear-only, in between = blend
static int   g_markerFr  = 0;     // >0 => manual output gated to silence (the marker gap)
static float g_fGain     = 1.0f;  // actual front mix gain pushed to the DSP
static float g_rGain     = 0.0f;  // actual rear  mix gain pushed to the DSP

static const ndspOutputMode MODES[3] = { NDSP_OUTPUT_STEREO, NDSP_OUTPUT_SURROUND, NDSP_OUTPUT_MONO };
static const char* MODE_NAME[3]      = { "STEREO  ", "SURROUND", "MONO    " };

// --- AUTO-run schedule -----------------------------------------------------
// One row per captured condition. The pip count == (row index + 1), so on the
// recording (and on the top screen) segment N is announced by N pips. Filenames
// here MUST match tools/split_run.py and the analyze.py commands in AUTO-RUN.md.
typedef struct { int mode; float blend; ndspSpeakerPos pos; const char* file; } Seg;
static const Seg SCHED[] = {
    { 0, 0.0f, NDSP_SPKPOS_SQUARE, "stereo_front.wav"       },  // 1
    { 0, 1.0f, NDSP_SPKPOS_SQUARE, "stereo_rear.wav"        },  // 2
    { 1, 0.0f, NDSP_SPKPOS_SQUARE, "surround_front.wav"     },  // 3
    { 1, 1.0f, NDSP_SPKPOS_SQUARE, "surround_rear.wav"      },  // 4
    { 1, 0.0f, NDSP_SPKPOS_WIDE,   "surround_front_wide.wav"},  // 5
    { 1, 1.0f, NDSP_SPKPOS_WIDE,   "surround_rear_wide.wav" },  // 6
    { 2, 0.0f, NDSP_SPKPOS_SQUARE, "mono_front.wav"         },  // 7
    { 2, 1.0f, NDSP_SPKPOS_SQUARE, "mono_rear.wav"          },  // 8
};
#define NSEG ((int)(sizeof(SCHED) / sizeof(SCHED[0])))

// AUTO state machine
enum { A_IDLE, A_INIT, A_PIP, A_LEAD, A_BODY, A_TAIL, A_DONE };
static int g_aState  = A_IDLE;
static int g_aSeg    = 0;         // current segment index (0..NSEG-1)
static int g_aTimer  = 0;         // frames left in the current sub-phase
static int g_aPips   = 0;         // pips left to emit in this header
static int g_aPipOn  = 0;         // 1 while a pip tone is sounding

// module-scope so the UI and the auto machine share them
static int   modeIdx   = 0;                 // start in STEREO
static u16   depth     = 0x7FFF;            // ndsp default
static u16   rearRatio = 0x8000;            // ndsp default
static ndspSpeakerPos pos = NDSP_SPKPOS_SQUARE;

static void push_mix(void) {                // push the current g_fGain/g_rGain
    float mix[12];
    memset(mix, 0, sizeof(mix));
    mix[0] = mix[1] = g_fGain;              // front L / front R
    mix[2] = mix[3] = g_rGain;              // rear  L / rear  R
    ndspChnSetMix(0, mix);
}

static void trigger_marker(void) { g_markerFr = MARKERFR; }

// Apply a segment's output mode + surround params to the DSP.
static void apply_seg_params(const Seg* s) {
    modeIdx = s->mode;
    pos     = s->pos;
    ndspSetOutputMode(MODES[modeIdx]);
    ndspSurroundSetDepth(depth);            // re-assert on every mode switch
    ndspSurroundSetPos(pos);
    ndspSurroundSetRearRatio(rearRatio);
}

static void auto_begin_segment(int i) {
    g_aSeg = i;
    if (i >= NSEG) { g_aState = A_DONE; return; }
    apply_seg_params(&SCHED[i]);
    g_rearBlend = SCHED[i].blend;           // body routing (pips force front below)
    g_aPips  = i + 1;                        // segment N announced by N pips
    g_aPipOn = 1;
    g_aTimer = FR_PIP_ON;
    g_aState = A_PIP;
}

static void auto_start(void) {
    g_aState = A_INIT;
    g_aTimer = FR_INIT;
    g_aSeg   = 0;
}

// Advance the auto machine one frame and set g_fGain/g_rGain for this frame.
static void auto_step(void) {
    switch (g_aState) {
    case A_INIT:
        g_fGain = g_rGain = 0.0f;                    // lead-in silence
        if (--g_aTimer <= 0) auto_begin_segment(0);
        break;
    case A_PIP:
        if (g_aPipOn) { g_fGain = 1.0f; g_rGain = 0.0f; }   // pips are front-only
        else          { g_fGain = g_rGain = 0.0f; }
        if (--g_aTimer <= 0) {
            if (g_aPipOn) { g_aPipOn = 0; g_aTimer = FR_PIP_OFF; }
            else {
                if (--g_aPips > 0) { g_aPipOn = 1; g_aTimer = FR_PIP_ON; }
                else { g_aState = A_LEAD; g_aTimer = FR_LEAD; }
            }
        }
        break;
    case A_LEAD:
        g_fGain = g_rGain = 0.0f;                    // settle after mode switch
        if (--g_aTimer <= 0) { g_aState = A_BODY; g_aTimer = FR_BODY; }
        break;
    case A_BODY:
        g_fGain = 1.0f - g_rearBlend; g_rGain = g_rearBlend;   // segment routing
        if (--g_aTimer <= 0) { g_aState = A_TAIL; g_aTimer = FR_TAIL; }
        break;
    case A_TAIL:
        g_fGain = g_rGain = 0.0f;
        if (--g_aTimer <= 0) auto_begin_segment(g_aSeg + 1);
        break;
    case A_DONE:
        g_fGain = g_rGain = 0.0f;
        g_aState = A_IDLE;                           // hand control back to manual
        break;
    default:
        break;
    }
}

static void padline(int row, const char* s) {  // print s at row, padded to clear the line
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "%-49s", s);
    (void)n;
    printf("\x1b[%d;1H%s", row, buf);
}

int main(int argc, char** argv) {
    gfxInitDefault();
    PrintConsole top;
    consoleInit(GFX_TOP, &top);
    consoleSelect(&top);

    // --- read the System Settings sound mode (Mono/Stereo/Surround), block 0x00070001
    u8 sysSound = 0xFF;
    if (R_SUCCEEDED(cfguInit())) {
        CFGU_GetConfigInfoBlk2(1, 0x00070001, &sysSound);
        cfguExit();
    }

    // --- audio up.  dspInit() so DSP_GetHeadphoneStatus works; ndspInit() for playback.
    bool haveDsp = R_SUCCEEDED(dspInit());
    if (R_FAILED(ndspInit())) {
        consoleClear();
        printf("ndspInit() failed.\n\nThe DSP firmware could not be loaded.\n"
               "On CFW this usually means dspfirm was not dumped.\n\nPress START to exit.\n");
        while (aptMainLoop()) { hidScanInput(); if (hidKeysDown() & KEY_START) break; gspWaitForVBlank(); }
        if (haveDsp) dspExit();
        gfxExit();
        return 0;
    }
    bool haveMcu = R_SUCCEEDED(mcuHwcInit());

    // --- generate one seamless-looping 440 Hz mono PCM16 buffer
    s16* buf = (s16*)linearAlloc(NSAMP * sizeof(s16));
    for (int i = 0; i < NSAMP; i++)
        buf[i] = (s16)(AMP * 32767.0f * sinf(2.0f * M_PI * (float)CYCLES * (float)i / (float)NSAMP));
    DSP_FlushDataCache(buf, NSAMP * sizeof(s16));

    // --- channel setup
    ndspSetMasterVol(1.0f);
    ndspSetOutputMode(MODES[modeIdx]);
    ndspSurroundSetDepth(depth);
    ndspSurroundSetPos(pos);
    ndspSurroundSetRearRatio(rearRatio);

    ndspChnReset(0);
    ndspChnIirBiquadSetEnable(0, false);    // ensure no stray filter colors the tone
    ndspChnSetInterp(0, NDSP_INTERP_NONE);  // native rate => no interpolation
    ndspChnSetRate(0, (float)SR);
    ndspChnSetFormat(0, NDSP_FORMAT_MONO_PCM16);
    push_mix();

    static ndspWaveBuf wb;
    memset(&wb, 0, sizeof(wb));
    wb.data_pcm16 = buf;
    wb.nsamples   = NSAMP;
    wb.looping    = true;
    ndspChnWaveBufAdd(0, &wb);

    // --- static UI
    consoleClear();
    printf("\x1b[1;1H===== 3DS surround-probe =====");
    printf("\x1b[13;1H------------------------------------------------");
    printf("\x1b[14;1H>>> RIGHT: run FULL AUTO capture (record one WAV) <<<");
    printf("\x1b[15;1HA: front/rear    Up/Down: blend sweep");
    printf("\x1b[16;1HX: output mode   Y: surround pos");
    printf("\x1b[17;1HL/R: rear ratio  B: re-drop marker");
    printf("\x1b[18;1HLEFT: abort auto  SELECT: reset surround params");
    printf("\x1b[19;1HSTART: exit");
    printf("\x1b[21;1H440 Hz @ 32728 Hz.  Keep the volume slider FIXED");
    printf("\x1b[22;1Hfor the whole session.  In AUTO, start the PC");
    printf("\x1b[23;1Hrecording BEFORE pressing RIGHT; stop after it ends.");

    trigger_marker();

    while (aptMainLoop()) {
        hidScanInput();
        u32 kDown = hidKeysDown();
        u32 kHeld = hidKeysHeld();

        if (kDown & KEY_START) break;

        bool running = (g_aState != A_IDLE);

        if (running) {
            // during an auto run, ignore manual audio keys; LEFT aborts
            if (kDown & KEY_DLEFT) { g_aState = A_IDLE; trigger_marker(); }
            else                   { auto_step(); }
        } else {
            if (kDown & KEY_DRIGHT) { auto_start(); }
            else {
                if (kDown & KEY_A) { g_rearBlend = (g_rearBlend < 0.5f) ? 1.0f : 0.0f; trigger_marker(); }
                if (kDown & KEY_X) {
                    modeIdx = (modeIdx + 1) % 3;
                    ndspSetOutputMode(MODES[modeIdx]);
                    ndspSurroundSetDepth(depth);       // re-assert surround params on mode switch
                    ndspSurroundSetPos(pos);
                    ndspSurroundSetRearRatio(rearRatio);
                    trigger_marker();
                }
                if (kDown & KEY_Y) {
                    pos = (pos == NDSP_SPKPOS_SQUARE) ? NDSP_SPKPOS_WIDE : NDSP_SPKPOS_SQUARE;
                    ndspSurroundSetPos(pos);
                    trigger_marker();
                }
                if (kDown & KEY_L) {
                    rearRatio = (rearRatio >= 0x0800) ? (u16)(rearRatio - 0x0800) : 0;
                    ndspSurroundSetRearRatio(rearRatio); trigger_marker();
                }
                if (kDown & KEY_R) {
                    rearRatio = (rearRatio <= 0xF7FF) ? (u16)(rearRatio + 0x0800) : 0xFFFF;
                    ndspSurroundSetRearRatio(rearRatio); trigger_marker();
                }
                if (kDown & KEY_B) trigger_marker();
                if (kDown & KEY_SELECT) {
                    depth = 0x7FFF; rearRatio = 0x8000; pos = NDSP_SPKPOS_SQUARE;
                    ndspSurroundSetDepth(depth); ndspSurroundSetPos(pos); ndspSurroundSetRearRatio(rearRatio);
                    trigger_marker();
                }
                // continuous front<->rear blend sweep (does NOT drop a marker)
                if (kHeld & KEY_DUP)   { g_rearBlend += 0.01f; if (g_rearBlend > 1.0f) g_rearBlend = 1.0f; }
                if (kHeld & KEY_DDOWN) { g_rearBlend -= 0.01f; if (g_rearBlend < 0.0f) g_rearBlend = 0.0f; }
            }
        }

        // --- compute this frame's mix gains
        if (g_aState != A_IDLE) {
            // auto_step() already set g_fGain/g_rGain for this frame
        } else {
            if (g_markerFr > 0) { g_markerFr--; g_fGain = g_rGain = 0.0f; }
            else { g_fGain = 1.0f - g_rearBlend; g_rGain = g_rearBlend; }
        }
        push_mix();

        // --- live status
        char line[64];
        const char* sysName = (sysSound == 0) ? "MONO    " :
                              (sysSound == 1) ? "STEREO  " :
                              (sysSound == 2) ? "SURROUND" : "unknown ";
        snprintf(line, sizeof(line), "System Settings sound mode : %s", sysName);
        padline(3, line);

        bool hp = false; const char* hpStr = "n/a";
        if (haveDsp && R_SUCCEEDED(DSP_GetHeadphoneStatus(&hp))) hpStr = hp ? "INSERTED" : "no";
        bool headset = osIsHeadsetConnected();
        u8 slider = 0; char slStr[8] = "n/a";
        if (haveMcu && R_SUCCEEDED(MCUHWC_GetSoundSliderLevel(&slider))) snprintf(slStr, sizeof(slStr), "%u", slider);
        snprintf(line, sizeof(line), "Headphone(dsp):%s  headset:%s  vol:%s", hpStr, headset ? "yes" : "no", slStr);
        padline(4, line);

        snprintf(line, sizeof(line), "FORCED output mode (ndsp)  : %s", MODE_NAME[modeIdx]);
        padline(6, line);

        const char* route = (g_rearBlend <= 0.001f) ? "FRONT-only" :
                            (g_rearBlend >= 0.999f) ? "REAR-only " : "BLEND     ";
        snprintf(line, sizeof(line), "Routing : %s  (rear=%3d%%)", route, (int)(g_rearBlend * 100.0f + 0.5f));
        padline(7, line);
        snprintf(line, sizeof(line), "Surround: pos=%-6s depth=0x%04X rear=0x%04X",
                 (pos == NDSP_SPKPOS_SQUARE) ? "SQUARE" : "WIDE", depth, rearRatio);
        padline(8, line);

        // --- auto-run status
        if (g_aState != A_IDLE) {
            const char* ph = (g_aState == A_INIT) ? "lead-in" :
                             (g_aState == A_PIP)  ? "pips   " :
                             (g_aState == A_LEAD) ? "settle " :
                             (g_aState == A_BODY) ? "TONE   " : "gap    ";
            snprintf(line, sizeof(line), "AUTO %d/%d [%s] -> %s", g_aSeg + 1, NSEG, ph,
                     (g_aSeg < NSEG) ? SCHED[g_aSeg].file : "");
            padline(10, line);
            padline(11, "recording? capture the WHOLE run to one WAV.");
        } else {
            padline(10, "idle (manual). RIGHT = full auto capture run.");
            padline(11, (g_markerFr > 0) ? ">>> MARKER (silence) <<<" : "tone playing");
        }

        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }

    ndspChnWaveBufClear(0);
    ndspExit();
    if (haveMcu) mcuHwcExit();
    if (haveDsp) dspExit();
    linearFree(buf);
    gfxExit();
    return 0;
}
