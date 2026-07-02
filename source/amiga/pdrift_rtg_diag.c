/*
 * pdrift_rtg_diag.c -- first Power Drift Amiga RTG diagnostic.
 *
 * This is not the playable Y-board runtime yet. It proves the Power Drift
 * package boots in UAE/Picasso96 and presents the host-validated composite
 * frame through the same RTG class used by the working arcade ports.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <graphics/gfxbase.h>
#include <graphics/modeid.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "pdrift_diag_frame.h"

struct IntuitionBase *IntuitionBase;
extern struct GfxBase *GfxBase;

#define RTG_W 640
#define RTG_H 480
#define RTG_MODE_ID 0x50ff1000UL
#define SCALE 2
/* hold each attract snapshot ~2s (WaitTOF ticks at 50Hz PAL) */
#define ANIM_TICKS 100

static struct Screen *scr;
static struct Window *win;
static unsigned char *scaled;
static ULONG loadrgb[1 + 256 * 3 + 1];
static FILE *logf;

static void log_msg(const char *msg)
{
    if (logf) {
        fputs(msg, logf);
        fputc('\n', logf);
        fflush(logf);
    }
}

static void make_palette(void)
{
    loadrgb[0] = 256UL << 16;
    for (int i = 0; i < 256; i++) {
        loadrgb[1 + i * 3 + 0] = pdrift_diag_palette[i][0] * 0x01010101UL;
        loadrgb[1 + i * 3 + 1] = pdrift_diag_palette[i][1] * 0x01010101UL;
        loadrgb[1 + i * 3 + 2] = pdrift_diag_palette[i][2] * 0x01010101UL;
    }
    loadrgb[1 + 256 * 3] = 0;
}

static void scale_frame(int frame_index)
{
    memset(scaled, 0, RTG_W * RTG_H);
    const int ox = (RTG_W - PDRIFT_DIAG_W * SCALE) / 2;
    const int oy = (RTG_H - PDRIFT_DIAG_H * SCALE) / 2;
    const unsigned char *frame = pdrift_diag_frames[frame_index % PDRIFT_DIAG_FRAMES];
    for (int y = 0; y < PDRIFT_DIAG_H; y++) {
        const unsigned char *src = frame + y * PDRIFT_DIAG_W;
        unsigned char *dst0 = scaled + (oy + y * SCALE) * RTG_W + ox;
        unsigned char *dst1 = dst0 + RTG_W;
        for (int x = 0; x < PDRIFT_DIAG_W; x++) {
            unsigned char p = src[x];
            dst0[x * 2 + 0] = p;
            dst0[x * 2 + 1] = p;
            dst1[x * 2 + 0] = p;
            dst1[x * 2 + 1] = p;
        }
    }
}

static void draw_text(struct RastPort *rp, LONG x, LONG y, const char *s)
{
    Move(rp, x, y);
    Text(rp, s, strlen(s));
}

static void close_all(void)
{
    if (win) {
        CloseWindow(win);
        win = 0;
    }
    if (scr) {
        CloseScreen(scr);
        scr = 0;
    }
    if (scaled) {
        FreeMem(scaled, RTG_W * RTG_H);
        scaled = 0;
    }
    if (IntuitionBase) {
        CloseLibrary((struct Library *)IntuitionBase);
        IntuitionBase = 0;
    }
    if (GfxBase) {
        CloseLibrary((struct Library *)GfxBase);
        GfxBase = 0;
    }
}

static int open_all(void)
{
    IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 39);
    GfxBase = (struct GfxBase *)OpenLibrary("graphics.library", 39);
    if (!IntuitionBase || !GfxBase) {
        log_msg("FAIL open intuition/graphics");
        return 0;
    }
    log_msg("opened intuition/graphics");

    scaled = AllocMem(RTG_W * RTG_H, MEMF_PUBLIC | MEMF_CLEAR);
    if (!scaled) {
        log_msg("FAIL alloc scaled");
        return 0;
    }
    log_msg("allocated frame");
    make_palette();
    scale_frame(0);

    ULONG mode = BestModeID(BIDTAG_NominalWidth, RTG_W,
                            BIDTAG_NominalHeight, RTG_H,
                            BIDTAG_Depth, 8,
                            TAG_DONE);
    if (mode == INVALID_ID)
        mode = RTG_MODE_ID;
    scr = OpenScreenTags(0,
                         SA_DisplayID, mode,
                         SA_Width, RTG_W,
                         SA_Height, RTG_H,
                         SA_Depth, 8,
                         SA_Quiet, TRUE,
                         SA_ShowTitle, FALSE,
                         SA_Exclusive, TRUE,
                         TAG_DONE);
    if (!scr) {
        log_msg("BestMode/OpenScreen failed; trying plain 8-bit screen");
        scr = OpenScreenTags(0,
                             SA_Width, RTG_W,
                             SA_Height, RTG_H,
                             SA_Depth, 8,
                             SA_Quiet, TRUE,
                             SA_ShowTitle, FALSE,
                             TAG_DONE);
    }
    if (!scr) {
        log_msg("FAIL OpenScreen");
        return 0;
    }
    log_msg("opened screen");
    LoadRGB32(&scr->ViewPort, loadrgb);
    log_msg("loaded palette");

    win = OpenWindowTags(0,
                         WA_CustomScreen, (ULONG)scr,
                         WA_Left, 0,
                         WA_Top, 0,
                         WA_Width, RTG_W,
                         WA_Height, RTG_H,
                         WA_Borderless, TRUE,
                         WA_Backdrop, TRUE,
                         WA_Activate, TRUE,
                         WA_RMBTrap, TRUE,
                         WA_IDCMP, 0,
                         TAG_DONE);
    if (!win) {
        log_msg("FAIL OpenWindow");
        return 0;
    }
    log_msg("opened window");
    return 1;
}

static void draw_screen(int frame_index)
{
    char label[48];
    struct RastPort *rp = &scr->RastPort;
    WriteChunkyPixels(rp, 0, 0, RTG_W - 1, RTG_H - 1, scaled, RTG_W);
    SetAPen(rp, PDRIFT_DIAG_PEN_WHITE);
    draw_text(rp, 12, 14, "POWER DRIFT RTG DIAGNOSTIC - ATTRACT SNAPSHOT PLAYBACK");
    sprintf(label, "SNAPSHOT %s (%d/%d)",
            pdrift_diag_frame_names[frame_index],
            frame_index + 1, PDRIFT_DIAG_FRAMES);
    draw_text(rp, RTG_W - 12 - 8 * (LONG)strlen(label), 14, label);
    draw_text(rp, 12, 470, "ATTRACT TEST - CLOSE EMULATOR TO EXIT - LIVE RUNTIME NEXT");
}

int main(void)
{
    logf = fopen("SYS:pdrift_diag.log", "w");
    log_msg("PowerDriftDiag start");
    if (!open_all()) {
        close_all();
        log_msg("PowerDriftDiag failed");
        if (logf) fclose(logf);
        return 20;
    }
    draw_screen(0);
    log_msg("enter animation loop");

    int frame_index = 0;
    int ticks = 0;
    for (;;) {
        WaitTOF();
        if (++ticks >= ANIM_TICKS) {
            ticks = 0;
            frame_index = (frame_index + 1) % PDRIFT_DIAG_FRAMES;
            scale_frame(frame_index);
            draw_screen(frame_index);
        }
    }

    close_all();
    log_msg("PowerDriftDiag exit");
    if (logf) fclose(logf);
    return 0;
}
