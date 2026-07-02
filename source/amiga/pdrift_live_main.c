/*
 * pdrift_live_main.c -- first LIVE Power Drift Amiga RTG runtime.
 *
 * Runs the shared three-68000 machine model (pdrift_machine) and the
 * MAME-validated compositor (pdrift_render) in-process, presenting each frame
 * through an 8-bit RTG chunky blit. The arcade palette is mapped live to a fixed
 * RGB332 256-pen screen CLUT (Side Arms method). Sound is the Z80 + YM2151 +
 * SegaPCM board on Paula (pdrift_audio). Input is keyboard + CD32 pad/joystick.
 *
 * ROM blobs are embedded in the executable (pdrift_romdata.S) -- fully self-
 * contained (no external files), booted from its own standalone RTG HDF/config.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <graphics/gfxbase.h>
#include <graphics/modeid.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <devices/inputevent.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <proto/dos.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "pdrift_machine.h"
#include "pdrift_render.h"
#include "pdrift_audio.h"

extern void pdr_audio_amiga_open(void);
extern void pdr_audio_amiga_frame(void);
extern void pdr_audio_amiga_close(void);

struct IntuitionBase *IntuitionBase;
extern struct GfxBase *GfxBase;

/* 864x486 like the rest of the collection: no native AGA mode has this size, so
 * BestModeID is forced to pick a clean uaegfx RTG mode -- avoids the hires-AGA
 * DblNTSC overscan/flicker you get at 640x480 when the native monitors are loaded.
 * The 320x224 game is presented 2x (640x448) centred, with black margins. */
#define RTG_W 864
#define RTG_H 486
#define RTG_MODE_ID 0x50ff1000UL
#define SCALE 2
#define GAME_W (PDR_W * SCALE)   /* 640 */
#define GAME_H (PDR_H * SCALE)   /* 448 */
#define GAME_OX ((RTG_W - GAME_W) / 2)   /* 112 */
#define GAME_OY ((RTG_H - GAME_H) / 2)   /* 19 */

/* Amiga raw key codes */
#define RK_ESC   0x45
#define RK_1     0x01
#define RK_5     0x05
#define RK_C     0x33
#define RK_SPACE 0x40
#define RK_UP    0x4c
#define RK_DOWN  0x4d
#define RK_RIGHT 0x4e
#define RK_LEFT  0x4f
#define RK_F     0x23   /* toggle the FPS overlay */
#define RK_S     0x21   /* cycle the frameskip cap */

/* CD32 pad / joystick on port 1 via raw hardware registers -- the same proven
 * read the other ports use (Side Arms / ArcadeIntro); no lowlevel.library. */
#define CIAA_PRA  (*(volatile unsigned char  *)0xbfe001UL)
#define CIAA_DDRA (*(volatile unsigned char  *)0xbfe201UL)
#define POTGO_REG (*(volatile unsigned short *)0xdff034UL)
#define POTINP    (*(volatile unsigned short *)0xdff016UL)
#define JOY1DAT   (*(volatile unsigned short *)0xdff00cUL)
#define P1_FIRE      0x80      /* CIAA PRA bit7, active low */
#define P1_DATRY     0x4000    /* POTINP bit14 */
#define CD32_BLUE      0x80
#define CD32_RED       0x40
#define CD32_YELLOW    0x20
#define CD32_GREEN     0x10
#define CD32_RSHOULDER 0x08    /* R1 */
#define CD32_LSHOULDER 0x04    /* L1 */
#define CD32_PLAY      0x02

/* Clock the 7 CD32 buttons out of the pad's shift register (returns the button
 * bits in CD32_* positions). Reject an all-ones "floating pad" read. */
static unsigned read_cd32(void)
{
    unsigned out = 0; volatile unsigned char t;
    CIAA_DDRA |= P1_FIRE;
    CIAA_PRA  &= (unsigned char)~P1_FIRE;
    POTGO_REG = 0x6f00;
    for (int i = 7; i >= 0; i--) {
        t = CIAA_PRA; t = CIAA_PRA; t = CIAA_PRA; t = CIAA_PRA; (void)t;
        if (!(POTINP & P1_DATRY)) out |= (1u << i);
        CIAA_PRA |= P1_FIRE;
        CIAA_PRA &= (unsigned char)~P1_FIRE;
    }
    CIAA_DDRA &= (unsigned char)~P1_FIRE;
    POTGO_REG = 0xff00;
    CIAA_PRA |= 0xC0;
    { int nb = 0; for (int b = 0; b < 8; b++) if (out & (1u << b)) nb++;
      if (nb >= 5) out = 0; }
    return out;
}

static struct Screen *scr;
static struct Window *win;
static unsigned char *scaled;      /* RTG_W*RTG_H 8-bit framebuffer */
static uint8_t *pen8;              /* 0x4000 arcade-pen -> RGB332 pen LUT */
static ULONG loadrgb[1 + 256 * 3 + 1];
static FILE *logf;
static uint32_t pal_csum = 0xffffffffu;

static void log_msg(const char *msg)
{
    if (logf) { fputs(msg, logf); fputc('\n', logf); fflush(logf); }
}

/* System time in 1/50-second ticks, for the FPS readout. */
static ULONG now_ticks(void)
{
    struct DateStamp ds;
    DateStamp(&ds);
    return ((ULONG)ds.ds_Days * 1440UL + (ULONG)ds.ds_Minute) * 3000UL + (ULONG)ds.ds_Tick;
}

/* Canonical RGB332 screen palette: pen index P displays the colour P encodes,
 * so pen8[] (which produces RGB332 indices from arcade RGB) lands correctly. */
static void make_palette(void)
{
    loadrgb[0] = 256UL << 16;
    for (int i = 0; i < 256; i++) {
        unsigned r = (unsigned)(i & 0xe0);
        unsigned g = (unsigned)((i & 0x1c) << 3);
        unsigned b = (unsigned)((i & 0x03) << 6);
        r |= r >> 3 | r >> 6;
        g |= g >> 3 | g >> 6;
        b |= b >> 2 | b >> 4 | b >> 6;
        loadrgb[1 + i * 3 + 0] = r * 0x01010101UL;
        loadrgb[1 + i * 3 + 1] = g * 0x01010101UL;
        loadrgb[1 + i * 3 + 2] = b * 0x01010101UL;
    }
    loadrgb[1 + 256 * 3] = 0;
}

/* Map the live 320x224 arcade-pen frame into the centred 2x RTG framebuffer.
 * The horizontal 2x is done with WORD writes (both dest bytes = same pen in one
 * store) and the vertical 2x with a row memcpy, instead of four byte stores per
 * source pixel -- roughly halves the present cost, which is what lifts DISP. */
static void present_frame(void)
{
    for (int y = 0; y < PDR_H; y++) {
        const uint16_t *src = pdr_outpen + y * PDR_W;
        unsigned char *row0 = scaled + (GAME_OY + y * SCALE) * RTG_W + GAME_OX;
        uint32_t *d32 = (uint32_t *)row0;
        for (int x = 0; x < PDR_W; x += 2) {       /* 2 src px -> 4 dest bytes in one store */
            unsigned p0 = pen8[src[x]     & 0x3fff];
            unsigned p1 = pen8[src[x + 1] & 0x3fff];
            d32[x >> 1] = (p0 << 24) | (p0 << 16) | (p1 << 8) | p1;   /* m68k big-endian */
        }
        memcpy(row0 + RTG_W, row0, GAME_W);        /* duplicate scanline (vertical 2x) */
    }
    /* blit only the game rectangle; the black margins were cleared once at start */
    WriteChunkyPixels(&scr->RastPort, GAME_OX, GAME_OY,
                      GAME_OX + GAME_W - 1, GAME_OY + GAME_H - 1,
                      scaled + GAME_OY * RTG_W + GAME_OX, RTG_W);
}

/* ROM blobs are linked into the executable (pdrift_romdata.S) so the runtime is
 * a single self-contained binary, exactly like the other ports in AGS SHARED. */
extern const unsigned char pdr_rom_maincpu[];
extern const unsigned char pdr_rom_subx[];
extern const unsigned char pdr_rom_suby[];
extern const unsigned char pdr_rom_ysprites_raw[];   /* 4MB raw; expanded at load */
extern const unsigned char pdr_rom_bsprites[];
extern const unsigned char pdr_rom_soundcpu[];       /* Z80 sound program (64KB) */
extern const unsigned char pdr_rom_pcm[];            /* SegaPCM samples (2MB) */

static uint8_t *ypix_expanded;   /* 8MB nibble-expanded Y-board sprite pixels */

static int load_roms(void)
{
    log_msg("wiring embedded ROMs");
    memcpy(pdm_maincpu, pdr_rom_maincpu, PDM_MAINCPU_SIZE);
    memcpy(pdm_subx, pdr_rom_subx, PDM_SUBX_SIZE);
    memcpy(pdm_suby, pdr_rom_suby, PDM_SUBY_SIZE);

    /* Expand the 4MB raw sprite ROM to 8MB nibbles (byte -> hi,lo), matching the
     * host ysprites_pix layout the renderer expects. */
    ypix_expanded = AllocMem(PDR_YPIX_BYTES, MEMF_ANY);
    if (!ypix_expanded) { log_msg("FAIL alloc ypix"); return 0; }
    for (unsigned i = 0; i < PDR_YPIX_BYTES / 2; i++) {
        unsigned char b = pdr_rom_ysprites_raw[i];
        ypix_expanded[2 * i + 0] = b >> 4;
        ypix_expanded[2 * i + 1] = b & 0x0f;
    }
    pdr_ypixrom = ypix_expanded;
    pdr_brom = pdr_rom_bsprites;

    pdr_audio_init(pdr_rom_soundcpu, pdr_rom_pcm, 0x200000);
    log_msg("ROMs wired + audio init");
    return 1;
}

static void draw_text(struct RastPort *rp, LONG x, LONG y, const char *s)
{
    Move(rp, x, y);
    Text(rp, s, strlen(s));
}

static void close_all(void)
{
    if (win) { CloseWindow(win); win = 0; }
    if (scr) { CloseScreen(scr); scr = 0; }
    if (scaled) { FreeMem(scaled, RTG_W * RTG_H); scaled = 0; }
    if (pen8) { FreeMem(pen8, 0x4000); pen8 = 0; }
    if (ypix_expanded) { FreeMem(ypix_expanded, PDR_YPIX_BYTES); ypix_expanded = 0; }
    if (IntuitionBase) { CloseLibrary((struct Library *)IntuitionBase); IntuitionBase = 0; }
    if (GfxBase) { CloseLibrary((struct Library *)GfxBase); GfxBase = 0; }
}

static int open_display(void)
{
    IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 39);
    GfxBase = (struct GfxBase *)OpenLibrary("graphics.library", 39);
    if (!IntuitionBase || !GfxBase) { log_msg("FAIL open libs"); return 0; }

    scaled = AllocMem(RTG_W * RTG_H, MEMF_PUBLIC | MEMF_CLEAR);
    pen8 = AllocMem(0x4000, MEMF_PUBLIC | MEMF_CLEAR);
    if (!scaled || !pen8) { log_msg("FAIL alloc buffers"); return 0; }
    make_palette();

    ULONG mode = BestModeID(BIDTAG_NominalWidth, RTG_W,
                            BIDTAG_NominalHeight, RTG_H,
                            BIDTAG_Depth, 8,
                            TAG_DONE);
    if (mode == INVALID_ID) mode = RTG_MODE_ID;
    scr = OpenScreenTags(0,
                         SA_DisplayID, mode,
                         SA_Width, RTG_W, SA_Height, RTG_H, SA_Depth, 8,
                         SA_Quiet, TRUE, SA_ShowTitle, FALSE, SA_Exclusive, TRUE,
                         TAG_DONE);
    if (!scr)
        scr = OpenScreenTags(0, SA_Width, RTG_W, SA_Height, RTG_H, SA_Depth, 8,
                             SA_Quiet, TRUE, SA_ShowTitle, FALSE, TAG_DONE);
    if (!scr) { log_msg("FAIL OpenScreen"); return 0; }
    LoadRGB32(&scr->ViewPort, loadrgb);

    win = OpenWindowTags(0,
                         WA_CustomScreen, (ULONG)scr,
                         WA_Left, 0, WA_Top, 0, WA_Width, RTG_W, WA_Height, RTG_H,
                         WA_Borderless, TRUE, WA_Backdrop, TRUE, WA_Activate, TRUE,
                         WA_RMBTrap, TRUE, WA_IDCMP, IDCMP_RAWKEY,
                         TAG_DONE);
    if (!win) { log_msg("FAIL OpenWindow"); return 0; }
    log_msg("display open");
    return 1;
}

/* Full key state + edge-triggered pulses so a quick tap still registers a coin
 * even at low fps (the press+release can otherwise be consumed in one poll). */
static unsigned char keydown[128];
static int coin_pulse, start_pulse, gear_state;
static int show_fps = 0;           /* F toggles the FPS overlay (off by default) */
/* Frameskip cap: max guest frames advanced per rendered frame. Lower = smoother
 * display but slower game (1 = draw every frame, smoothest; 3 = fastest game but
 * choppy display). Cycle live with S. */
static int max_skip = 2;
/* pad state, refreshed once per rendered frame in poll_input */
static int pad_left, pad_right, pad_gas, pad_brake;
static unsigned pad_prev;

static void poll_input(int *quit)
{
    struct IntuiMessage *m;
    while (win->UserPort && (m = (struct IntuiMessage *)GetMsg(win->UserPort))) {
        UWORD code = m->Code;
        ReplyMsg((struct Message *)m);
        int up = code & 0x80;
        int k = code & 0x7f;
        int was = keydown[k];
        keydown[k] = up ? 0 : 1;
        if (!up && !was) {                 /* key press edge */
            if (k == RK_ESC) *quit = 1;
            else if (k == RK_5 || k == RK_C) coin_pulse = 4;   /* hold coin ~4 frames */
            else if (k == RK_1) start_pulse = 8;               /* hold start ~8 frames */
            else if (k == RK_SPACE) gear_state ^= 1;           /* gear toggle */
            else if (k == RK_F) show_fps ^= 1;                 /* toggle FPS overlay */
            else if (k == RK_S) { max_skip++; if (max_skip > 3) max_skip = 1; }  /* cycle skip */
        }
    }

    /* CD32 pad / joystick on port 1: dpad steers, gas/brake on stick or face
     * buttons, gears on the shoulders (R1 up / L1 down), Play=start, Green/Yellow
     * =coin. Combined with the keyboard so either works. */
    static int pot_init = 0;
    if (!pot_init) { POTGO_REG = 0xff00; pot_init = 1; }
    unsigned cd32 = read_cd32();
    unsigned v = JOY1DAT;
    pad_right = (v >> 1) & 1;
    pad_left  = (v >> 9) & 1;
    int pad_up   = ((v >> 9) ^ (v >> 8)) & 1;
    int pad_down = ((v >> 1) ^ v) & 1;
    int fire1 = !(CIAA_PRA & P1_FIRE);
    pad_gas   = pad_up   || fire1 || (cd32 & CD32_RED);
    pad_brake = pad_down || (cd32 & CD32_BLUE);
    unsigned pressed = cd32 & ~pad_prev;                       /* button rising edges */
    if (pressed & CD32_RSHOULDER) gear_state = 1;              /* R1 -> high gear */
    if (pressed & CD32_LSHOULDER) gear_state = 0;              /* L1 -> low gear  */
    if (pressed & CD32_PLAY) start_pulse = 8;
    if (pressed & (CD32_GREEN | CD32_YELLOW)) coin_pulse = 4;
    pad_prev = cd32;
}

/* Translate held keys + pad + pulses into the arcade input ports for this frame. */
static void apply_input(void)
{
    uint8_t g = 0xdf;
    if (coin_pulse > 0) { g &= (uint8_t)~0x40; coin_pulse--; }   /* COIN1 active-low */
    if (start_pulse > 0) { g &= (uint8_t)~0x08; start_pulse--; } /* START1 active-low */
    if (gear_state) g |= 0x20; else g &= (uint8_t)~0x20;         /* gear-shift active-high */
    pdm_in.general = g;

    pdm_in.adc[4] = (keydown[RK_UP]   || pad_gas)   ? 0xff : 0x00;   /* gas pedal */
    pdm_in.adc[3] = (keydown[RK_DOWN] || pad_brake) ? 0xff : 0x00;   /* brake pedal */

    /* Steering: RAMP gradually toward the extreme (like the analog wheel) instead
     * of snapping to full lock, with auto-centre on release. STEER_DELTA per guest
     * frame sets the sensitivity (smaller = gentler); STEER_RANGE limits how far
     * from centre a keypress reaches so it never hits the twitchy full lock. */
    #define STEER_DELTA  3     /* units/frame while a key is held (MAME KEYDELTA=4) */
    #define STEER_CENTER 5     /* units/frame returning to centre on release */
    #define STEER_RANGE  0x60  /* full MAME range 0x20..0xe0; the ramp, not a cap,
                                * provides the gentler feel (hold longer = sharper) */
    static int steer = 0x80;
    int left  = keydown[RK_LEFT]  || pad_left;
    int right = keydown[RK_RIGHT] || pad_right;
    if (left)       steer -= STEER_DELTA;
    else if (right) steer += STEER_DELTA;
    else if (steer > 0x80) { steer -= STEER_CENTER; if (steer < 0x80) steer = 0x80; }
    else if (steer < 0x80) { steer += STEER_CENTER; if (steer > 0x80) steer = 0x80; }
    if (steer < 0x80 - STEER_RANGE) steer = 0x80 - STEER_RANGE;
    if (steer > 0x80 + STEER_RANGE) steer = 0x80 + STEER_RANGE;
    pdm_in.adc[5] = (uint8_t)steer;                            /* steering */
}

int main(void)
{
    /* Log to the game's own dir (PROGDIR:), never SYS: -- when launched from the
     * WhittyArcade launcher a SYS: reference pops a "please insert volume SYS:"
     * requester if SYS: isn't the expected volume. PROGDIR: is always valid. */
    logf = fopen("PROGDIR:pdrift_live.log", "w");
    log_msg("PowerDriftLive start");

    if (!open_display()) {
        close_all();
        log_msg("startup failed");
        if (logf) fclose(logf);
        return 20;
    }

    SetAPen(&scr->RastPort, 255);
    draw_text(&scr->RastPort, 12, RTG_H / 2, "POWER DRIFT - live Y-board runtime starting...");

    if (!load_roms()) {
        close_all();
        log_msg("startup failed");
        if (logf) fclose(logf);
        return 20;
    }
    SetAPen(&scr->RastPort, 0);
    RectFill(&scr->RastPort, 0, 0, RTG_W - 1, RTG_H - 1);

    pdrift_machine_default_inputs();
    pdrift_machine_init();
    pdr_audio_amiga_open();
    log_msg("machine booted; entering run loop");

    struct RastPort *rp = &scr->RastPort;
    /* Wall-clock game pacing + frameskip: the machine advances by however many
     * 60Hz guest frames of real time have elapsed (capped), and we render ONCE
     * per loop. So the GAME always runs at the right speed even when the racing
     * scene makes rendering slow -- the display just gets choppy, not slow-mo. */
    ULONG dispf = 0, gframe = 0, disp_fps = 0, game_fps = 0;
    ULONG win_disp = 0, win_game = 0, win_tick = now_ticks();
    unsigned long mrate = 0, mlast = pdr_audio_eclock(&mrate), mfrac = 0;
    char hud[96];
    int quit = 0;
    while (!quit) {
        poll_input(&quit);

        int due = 1;
        if (mrate) {
            unsigned long now = pdr_audio_eclock(&mrate);
            unsigned long dt = now - mlast; mlast = now;
            unsigned long cap = mrate / 8;         /* clamp a stall at 0.125s */
            if (dt > cap) dt = cap;
            mfrac += dt * 60u;
            due = (int)(mfrac / mrate); mfrac %= mrate;
            if (due < 1) due = 1;
            if (due > max_skip) due = max_skip;    /* frameskip cap (F to cycle 1/2/3) */
        }
        for (int i = 0; i < due; i++) {
            apply_input();
            pdrift_machine_run_frame();
            uint8_t c;                             /* forward sound commands per guest frame */
            while (pdrift_machine_sound_latch_take(&c)) pdr_audio_command(c);
        }
        gframe += due;

        pdrift_render_frame();
        pdrift_render_build_pen8(pen8);
        present_frame();
        pdr_audio_amiga_frame();                   /* refill Paula ring (wall-clock paced) */

        dispf++;
        if (dispf - win_disp >= 16) {
            ULONG t = now_ticks(), dt = t - win_tick;
            if (!dt) dt = 1;
            disp_fps = (dispf - win_disp) * 50UL / dt;
            game_fps = (gframe - win_game) * 50UL / dt;
            win_disp = dispf; win_game = gframe; win_tick = t;
        }
        /* Top strip: gear indicator always; FPS only when toggled on (F). Redraw
         * only when something actually changes -- the FPS numbers update every 16
         * frames, so this skips the RectFill+Text on most frames. */
        static int last_gear = -1, last_show = -1, last_skip = -1;
        static ULONG last_g = ~0UL, last_d = ~0UL;
        if (gear_state != last_gear || show_fps != last_show ||
            (show_fps && (game_fps != last_g || disp_fps != last_d || max_skip != last_skip))) {
            SetAPen(rp, 0);
            RectFill(rp, 0, 0, RTG_W - 1, GAME_OY - 1);
            SetAPen(rp, 255);
            draw_text(rp, GAME_OX, 12, gear_state ? "HIGH GEAR" : "LOW GEAR");
            if (show_fps) {
                snprintf(hud, sizeof hud, "GAME %lu  DISP %lu  S%d",
                         (unsigned long)game_fps, (unsigned long)disp_fps, max_skip);
                draw_text(rp, RTG_W - 260, 12, hud);
            }
            last_gear = gear_state; last_show = show_fps; last_skip = max_skip;
            last_g = game_fps; last_d = disp_fps;
        }

        WaitTOF();
    }

    log_msg("PowerDriftLive exit");
    pdr_audio_amiga_close();
    close_all();
    if (logf) fclose(logf);
    return 0;
}
