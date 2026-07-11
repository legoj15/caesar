// surround-probe (v2) -- empirically test the 3DS "Surround" output mode.
//
// v1 (a steady, L/R-symmetric 440 Hz mono tone, mono-summed analysis) came back
// inconclusive: every condition nulled to the analog floor and Surround showed
// zero L/R decorrelation. A steady centered tone is blind to front/back
// virtualization (which lives in HRTF spectral coloration + inter-channel
// crosstalk, needing BROADBAND content and an OFF-CENTER source).
//
// v2 fixes the probe design:
//   * SIGNAL: a periodic, band-limited (100 Hz-14 kHz) Schroeder-phase pink
//     multitone, precomputed on the host and embedded as g_probe[] (probe_buf.h).
//     Periodic => the wave buffer loops seamlessly and every recorded segment
//     holds the same realization (analysis is power-spectrum based, so the
//     per-segment circular phase offset is harmless).
//   * ROUTING: the mono source is hard-panned to ONE quad corner -- front-left
//     (FL) vs back-left (BL). Same side, so the naive stereo fold (L=FL+BL)
//     makes them identical in Stereo but the front-left/back-left HRTF makes
//     them differ in Surround, and the R output channel is a clean crosstalk
//     meter (silent unless Surround bleeds).
//   * MATRIX: a 10-segment AUTO run adds a depth positive-control (0x7FFF vs
//     0xFFFF) so a null can be told apart from residual blindness.
//
// AUTO run (d-pad RIGHT): plays the matrix hands-off as ONE continuous
// recording; each segment is announced by a countable pip burst (segment N =
// N FL pips). tools/split_run.py cuts the take (and emits noise_floor.wav from
// the lead-in); tools/analyze_surround.py computes the per-channel verdict.

#include <stdio.h>
#include <string.h>
#include <3ds.h>
#include "probe_buf.h"           // PROBE_NSAMP, g_probe[]

#define SR        32728          // DSP native rate (matches the embedded buffer)

// --- AUTO-run timing, in 60 fps frames -------------------------------------
#define FR_INIT     90           // 1.5 s lead-in silence (becomes noise_floor.wav)
#define FR_PIP_ON    8           // ~133 ms pip
#define FR_PIP_OFF   8           // ~133 ms gap between pips
#define FR_LEAD     36           // ~600 ms silence between pip header and body
#define FR_BODY    480           // 8.0 s body (~40 Welch blocks at 48 kHz)
#define FR_TAIL     36           // ~600 ms silence after the body

#define MARKERFR    12           // manual marker silence (~200 ms)

// The four MAIN-bus mix lanes we drive (aux buses left at 0): FL FR BL BR.
static float g_mix4[4] = { 1.0f, 0.0f, 0.0f, 0.0f };   // default: front-left
static int   g_markerFr = 0;

static const ndspOutputMode MODES[3] = { NDSP_OUTPUT_STEREO, NDSP_OUTPUT_SURROUND, NDSP_OUTPUT_MONO };
static const char* MODE_NAME[3]      = { "STEREO  ", "SURROUND", "MONO    " };

// --- AUTO-run schedule (single-corner; pip count == row index + 1) ----------
// mix = {FL,FR,BL,BR}. FL-only={1,0,0,0} (front), BL-only={0,0,1,0} (back).
// Filenames MUST match tools/split_run.py SCHEDULE and analyze_surround.py.
typedef struct { int mode; float mix[4]; ndspSpeakerPos pos; u16 depth; const char* file; } Seg;
static const Seg SCHED[] = {
    { 0, {1,0,0,0}, NDSP_SPKPOS_SQUARE, 0x7FFF, "stereo_front.wav"        }, // 1
    { 0, {0,0,1,0}, NDSP_SPKPOS_SQUARE, 0x7FFF, "stereo_rear.wav"         }, // 2
    { 1, {1,0,0,0}, NDSP_SPKPOS_SQUARE, 0x7FFF, "surround_front.wav"      }, // 3
    { 1, {0,0,1,0}, NDSP_SPKPOS_SQUARE, 0x7FFF, "surround_rear.wav"       }, // 4
    { 1, {1,0,0,0}, NDSP_SPKPOS_SQUARE, 0xFFFF, "surround_front_deep.wav" }, // 5  depth+
    { 1, {0,0,1,0}, NDSP_SPKPOS_SQUARE, 0xFFFF, "surround_rear_deep.wav"  }, // 6  depth+
    { 1, {1,0,0,0}, NDSP_SPKPOS_WIDE,   0x7FFF, "surround_front_wide.wav" }, // 7
    { 1, {0,0,1,0}, NDSP_SPKPOS_WIDE,   0x7FFF, "surround_rear_wide.wav"  }, // 8
    { 2, {1,0,0,0}, NDSP_SPKPOS_SQUARE, 0x7FFF, "mono_front.wav"          }, // 9
    { 2, {0,0,1,0}, NDSP_SPKPOS_SQUARE, 0x7FFF, "mono_rear.wav"           }, // 10
};
#define NSEG ((int)(sizeof(SCHED) / sizeof(SCHED[0])))

enum { A_IDLE, A_INIT, A_PIP, A_LEAD, A_BODY, A_TAIL, A_DONE };
static int g_aState = A_IDLE, g_aSeg = 0, g_aTimer = 0, g_aPips = 0, g_aPipOn = 0;

// module-scope current DSP state (shared with the UI)
static int   modeIdx   = 0;
static u16   depth     = 0x7FFF;
static u16   rearRatio = 0x8000;
static ndspSpeakerPos pos = NDSP_SPKPOS_SQUARE;
static int   corner    = 0;                 // manual: 0 = front-left, 1 = back-left

static void set4(float a, float b, float c, float d) { g_mix4[0]=a; g_mix4[1]=b; g_mix4[2]=c; g_mix4[3]=d; }

static void push_mix(void) {                // push g_mix4 into the 4 main lanes
    float mix[12];
    memset(mix, 0, sizeof(mix));
    mix[0]=g_mix4[0]; mix[1]=g_mix4[1]; mix[2]=g_mix4[2]; mix[3]=g_mix4[3];
    ndspChnSetMix(0, mix);
}

static void trigger_marker(void) { g_markerFr = MARKERFR; }

static void apply_seg_params(const Seg* s) {
    modeIdx = s->mode; pos = s->pos; depth = s->depth;
    ndspSetOutputMode(MODES[modeIdx]);
    ndspSurroundSetDepth(depth);
    ndspSurroundSetPos(pos);
    ndspSurroundSetRearRatio(rearRatio);
}

static void auto_begin_segment(int i) {
    g_aSeg = i;
    if (i >= NSEG) { g_aState = A_DONE; return; }
    apply_seg_params(&SCHED[i]);
    g_aPips = i + 1; g_aPipOn = 1; g_aTimer = FR_PIP_ON; g_aState = A_PIP;
}

static void auto_start(void) { g_aState = A_INIT; g_aTimer = FR_INIT; g_aSeg = 0; }

static void auto_step(void) {
    switch (g_aState) {
    case A_INIT:
        set4(0,0,0,0);
        if (--g_aTimer <= 0) auto_begin_segment(0);
        break;
    case A_PIP:
        if (g_aPipOn) set4(1,0,0,0); else set4(0,0,0,0);   // pips are FL-only
        if (--g_aTimer <= 0) {
            if (g_aPipOn) { g_aPipOn = 0; g_aTimer = FR_PIP_OFF; }
            else if (--g_aPips > 0) { g_aPipOn = 1; g_aTimer = FR_PIP_ON; }
            else { g_aState = A_LEAD; g_aTimer = FR_LEAD; }
        }
        break;
    case A_LEAD:
        set4(0,0,0,0);
        if (--g_aTimer <= 0) { g_aState = A_BODY; g_aTimer = FR_BODY; }
        break;
    case A_BODY: {
        const float* m = SCHED[g_aSeg].mix;
        set4(m[0], m[1], m[2], m[3]);                       // segment routing
        if (--g_aTimer <= 0) { g_aState = A_TAIL; g_aTimer = FR_TAIL; }
        break;
    }
    case A_TAIL:
        set4(0,0,0,0);
        if (--g_aTimer <= 0) auto_begin_segment(g_aSeg + 1);
        break;
    case A_DONE:
        set4(0,0,0,0); g_aState = A_IDLE;
        break;
    default: break;
    }
}

static void padline(int row, const char* s) {
    char buf[64]; snprintf(buf, sizeof(buf), "%-49s", s);
    printf("\x1b[%d;1H%s", row, buf);
}

int main(int argc, char** argv) {
    gfxInitDefault();
    PrintConsole top; consoleInit(GFX_TOP, &top); consoleSelect(&top);

    u8 sysSound = 0xFF;
    if (R_SUCCEEDED(cfguInit())) { CFGU_GetConfigInfoBlk2(1, 0x00070001, &sysSound); cfguExit(); }

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

    // embedded broadband probe -> linear memory for the DSP
    s16* buf = (s16*)linearAlloc(PROBE_NSAMP * sizeof(s16));
    memcpy(buf, g_probe, PROBE_NSAMP * sizeof(s16));
    DSP_FlushDataCache(buf, PROBE_NSAMP * sizeof(s16));

    ndspSetMasterVol(1.0f);
    ndspSetOutputMode(MODES[modeIdx]);
    ndspSurroundSetDepth(depth);
    ndspSurroundSetPos(pos);
    ndspSurroundSetRearRatio(rearRatio);

    ndspChnReset(0);
    ndspChnIirBiquadSetEnable(0, false);
    ndspChnSetInterp(0, NDSP_INTERP_NONE);   // native rate => no interpolation
    ndspChnSetRate(0, (float)SR);
    ndspChnSetFormat(0, NDSP_FORMAT_MONO_PCM16);
    push_mix();

    static ndspWaveBuf wb;
    memset(&wb, 0, sizeof(wb));
    wb.data_pcm16 = buf;
    wb.nsamples   = PROBE_NSAMP;
    wb.looping    = true;
    ndspChnWaveBufAdd(0, &wb);                // added once; never reset across the run

    consoleClear();
    printf("\x1b[1;1H===== 3DS surround-probe v2 =====");
    printf("\x1b[13;1H------------------------------------------------");
    printf("\x1b[14;1H>>> RIGHT: run FULL AUTO capture (record one WAV) <<<");
    printf("\x1b[15;1HA: front(FL)/back(BL)   X: output mode");
    printf("\x1b[16;1HY: surround pos         L/R: depth -/+");
    printf("\x1b[17;1HLEFT: abort auto  B: marker  SELECT: reset");
    printf("\x1b[18;1HSTART: exit");
    printf("\x1b[20;1HSet System Settings sound = SURROUND before capture.");
    printf("\x1b[21;1HBroadband probe; keep the volume slider FIXED. Start");
    printf("\x1b[22;1Hthe PC recording BEFORE pressing RIGHT, stop after.");

    trigger_marker();

    while (aptMainLoop()) {
        hidScanInput();
        u32 kDown = hidKeysDown();
        if (kDown & KEY_START) break;

        bool running = (g_aState != A_IDLE);
        if (running) {
            if (kDown & KEY_DLEFT) { g_aState = A_IDLE; corner = 0; trigger_marker(); }
            else auto_step();
        } else if (kDown & KEY_DRIGHT) {
            auto_start();
        } else {
            if (kDown & KEY_A) { corner ^= 1; trigger_marker(); }
            if (kDown & KEY_X) {
                modeIdx = (modeIdx + 1) % 3;
                ndspSetOutputMode(MODES[modeIdx]);
                ndspSurroundSetDepth(depth); ndspSurroundSetPos(pos); ndspSurroundSetRearRatio(rearRatio);
                trigger_marker();
            }
            if (kDown & KEY_Y) { pos = (pos==NDSP_SPKPOS_SQUARE)?NDSP_SPKPOS_WIDE:NDSP_SPKPOS_SQUARE; ndspSurroundSetPos(pos); trigger_marker(); }
            if (kDown & KEY_L) { depth = (depth>=0x0800)?(u16)(depth-0x0800):0;      ndspSurroundSetDepth(depth); trigger_marker(); }
            if (kDown & KEY_R) { depth = (depth<=0xF7FF)?(u16)(depth+0x0800):0xFFFF; ndspSurroundSetDepth(depth); trigger_marker(); }
            if (kDown & KEY_B) trigger_marker();
            if (kDown & KEY_SELECT) {
                depth=0x7FFF; rearRatio=0x8000; pos=NDSP_SPKPOS_SQUARE;
                ndspSurroundSetDepth(depth); ndspSurroundSetPos(pos); ndspSurroundSetRearRatio(rearRatio);
                trigger_marker();
            }
        }

        // compute this frame's mix
        if (g_aState != A_IDLE) {
            // auto_step() already set g_mix4
        } else if (g_markerFr > 0) {
            g_markerFr--; set4(0,0,0,0);
        } else {
            if (corner == 0) set4(1,0,0,0); else set4(0,0,1,0);
        }
        push_mix();

        // status
        char line[64];
        const char* sysName = (sysSound==0)?"MONO    ":(sysSound==1)?"STEREO  ":(sysSound==2)?"SURROUND":"unknown ";
        snprintf(line,sizeof(line),"System Settings sound mode : %s",sysName); padline(3,line);

        bool hp=false; const char* hpStr="n/a";
        if (haveDsp && R_SUCCEEDED(DSP_GetHeadphoneStatus(&hp))) hpStr = hp?"INSERTED":"no";
        u8 slider=0; char slStr[8]="n/a";
        if (haveMcu && R_SUCCEEDED(MCUHWC_GetSoundSliderLevel(&slider))) snprintf(slStr,sizeof(slStr),"%u",slider);
        snprintf(line,sizeof(line),"Headphone:%s  vol:%s",hpStr,slStr); padline(4,line);

        snprintf(line,sizeof(line),"FORCED mode: %s  pos:%s  depth:0x%04X",
                 MODE_NAME[modeIdx], (pos==NDSP_SPKPOS_SQUARE)?"SQ":"WD", depth); padline(6,line);
        snprintf(line,sizeof(line),"mix FL/FR/BL/BR = %.0f %.0f %.0f %.0f",
                 g_mix4[0],g_mix4[1],g_mix4[2],g_mix4[3]); padline(7,line);

        if (g_aState != A_IDLE) {
            const char* ph=(g_aState==A_INIT)?"lead-in":(g_aState==A_PIP)?"pips   ":
                           (g_aState==A_LEAD)?"settle ":(g_aState==A_BODY)?"BODY   ":"gap    ";
            snprintf(line,sizeof(line),"AUTO %d/%d [%s] -> %s",g_aSeg+1,NSEG,ph,
                     (g_aSeg<NSEG)?SCHED[g_aSeg].file:""); padline(9,line);
            padline(10,"record the WHOLE run to one WAV.");
        } else {
            snprintf(line,sizeof(line),"idle: corner=%s. RIGHT=auto run.", corner?"back(BL)":"front(FL)"); padline(9,line);
            padline(10,(g_markerFr>0)?">>> MARKER (silence) <<<":"probe playing");
        }

        gfxFlushBuffers(); gfxSwapBuffers(); gspWaitForVBlank();
    }

    ndspChnWaveBufClear(0);
    ndspExit();
    if (haveMcu) mcuHwcExit();
    if (haveDsp) dspExit();
    linearFree(buf);
    gfxExit();
    return 0;
}
