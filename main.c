/*
 * GHOST HALLWAY - 2D side-view, level-based psychological horror
 * For PSP / PPSSPP (homebrew EBOOT.PBP, built with PSPSDK)
 *
 * Controls
 *   Left / Right or analog stick : walk
 *   CROSS                        : jump
 *   START                        : start / continue
 *
 * Collect the candles, reach the door, survive 5 hallways.
 */
#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspctrl.h>
#include <pspdebug.h>
#include <string.h>

PSP_MODULE_INFO("GhostHallway", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER);

/* ---------- HOME button exit ---------- */
static int exit_cb(int arg1, int arg2, void *common)
{
    (void)arg1; (void)arg2; (void)common;
    sceKernelExitGame();
    return 0;
}

static int cb_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    int id = sceKernelCreateCallback("exit_cb", exit_cb, NULL);
    sceKernelRegisterExitCallback(id);
    sceKernelSleepThreadCB();
    return 0;
}

static void setup_callbacks(void)
{
    int th = sceKernelCreateThread("cb_thread", cb_thread, 0x11, 0xFA0, 0, NULL);
    if (th >= 0)
        sceKernelStartThread(th, 0, NULL);
}

/* ---------- screen, colours ---------- */
#define SCR_W   480
#define SCR_H   272
#define STRIDE  512                 /* VRAM line width in pixels */
#define VRAM    ((u32 *)0x44000000) /* uncached VRAM */
static u32 back[SCR_W * SCR_H];     /* software back buffer */

#define COL(r, g, b) (0xFF000000u | ((u32)(b) << 16) | ((u32)(g) << 8) | (u32)(r))
#define C_BG         COL(8, 7, 12)
#define C_WALL       COL(24, 20, 30)
#define C_FLOOR      COL(40, 32, 28)
#define C_BOARD      COL(30, 25, 22)
#define C_MARK       COL(70, 18, 18)
#define C_CRATE      COL(96, 70, 40)
#define C_CRATE_IN   COL(60, 40, 20)
#define C_DOOR_LOCK  COL(110, 24, 24)
#define C_DOOR_OPEN  COL(40, 150, 70)
#define C_DOOR_IN_OK COL(20, 60, 30)
#define C_WAX        COL(230, 225, 205)
#define C_FLAME      COL(255, 200, 80)
#define C_PLAYER     COL(210, 185, 150)
#define C_SHIRT      COL(60, 90, 140)
#define C_GHOST      COL(160, 215, 235)
#define C_DECOY      COL(40, 60, 70)
#define C_BLACK      0xFF000000u
#define C_WHITE      0xFFFFFFFFu

/* ---------- world constants ---------- */
#define FLOOR_Y      220
#define PW           14
#define PH           30
#define CRATE_W      20
#define CRATE_H      16
#define DOOR_W       44
#define DOOR_H       90
#define DOOR_OFFSET  110            /* door sits this far from level end */
#define GHOST_CY     (FLOOR_Y - 32) /* ghost body centre */
#define GHOST_TOP    (GHOST_CY - 19)
#define HIST_N       128
#define MIRROR_DELAY 90
#define NUM_LEVELS   5
#define MAX_CANDLES  5
#define MAX_CRATES   6
#define MAX_MARKS    12
#define MIN_IDLE     60             /* frames standing still before ghost creeps */

typedef struct {
    const char *name;
    int length;        /* world width in px                      */
    int candles;       /* candles needed to open the door        */
    int ghost_period;  /* frames per 1px of ghost movement       */
    int light_r;       /* lantern radius in px                   */
    int blackout;      /* lantern cuts out periodically          */
    int fake_door;     /* a wrong door that scares you           */
    int mirror;        /* ghost copies your path, a moment late  */
    const char *intro; /* whisper shown when the level starts    */
} LevelDef;

static const LevelDef levels[NUM_LEVELS] = {
    { "HALLWAY",    1800, 3, 4, 80, 0, 0, 0, "you came back. good." },
    { "STILLNESS",  2000, 4, 3, 74, 0, 0, 0, "stop moving... if you dare" },
    { "WRONG DOOR", 2100, 4, 3, 72, 0, 1, 0, "not every door leads out" },
    { "BLACKOUT",   2200, 5, 3, 72, 1, 0, 0, "the light is not yours to keep" },
    { "MIRROR",     2400, 5, 1, 68, 0, 0, 1, "it copies you. a little late." },
};

static const char *whispers[] = {
    "don't look back",
    "it's closer than you think",
    "you can't see me. good.",
    "keep walking",
    "behind you",
    "why did you stop?",
    "there is no exit",
    "you already passed the door",
    "I can see your light",
    "turn around... slowly",
    "it's not behind you",
    "one more candle",
    "you're almost out",
};
#define NUM_WHISPERS (sizeof(whispers) / sizeof(whispers[0]))

typedef enum { ST_TITLE, ST_PLAY, ST_DEAD, ST_CLEAR, ST_END } State;

typedef struct {
    int lvl;
    int px, py, vy;               /* player: world x, screen y (top), vertical speed */
    int on_ground;
    int idle;                     /* frames without walking */
    int gx, gtick;                /* ghost: world x, tick counter */
    int frame;
    int cam;                      /* camera world x */
    int candles_got;
    int cand_x[MAX_CANDLES];
    int cand_on[MAX_CANDLES];
    int crate_x[MAX_CRATES];
    int ncrates;
    int mark_x[MAX_MARKS];
    int mark_y[MAX_MARKS];
    int fake_x;
    int fake_used;
    int hist[HIST_N];             /* past player x, for MIRROR level */
    int hist_i;
    int decoy_t;                  /* frames the decoy flash stays up */
    int decoy_x;
    int whisper_t;
    const char *whisper;
    int timer;                    /* countdown for DEAD / CLEAR */
} Game;

static Game g;
static State state = ST_TITLE;
static int deaths = 0;
static int tick = 0;
static u32 rng_state = 1;
static u32 prev_buttons = 0;

/* ---------- small helpers ---------- */
static u32 rnd(void)
{
    rng_state = rng_state * 1103515245u + 12345u;
    return (rng_state >> 16) & 0x7FFF;
}

static int iabs(int v) { return v < 0 ? -v : v; }

static int aabb(int x1, int y1, int w1, int h1, int x2, int y2, int w2, int h2)
{
    return x1 < x2 + w2 && x1 + w1 > x2 && y1 < y2 + h2 && y1 + h1 > y2;
}

static void set_whisper(const char *s, int t)
{
    g.whisper = s;
    g.whisper_t = t;
}

/* ---------- drawing into the back buffer ---------- */
static void fill_rect(int x, int y, int w, int h, u32 c)
{
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = (x + w) > SCR_W ? SCR_W : (x + w);
    int y1 = (y + h) > SCR_H ? SCR_H : (y + h);
    for (int j = y0; j < y1; j++)
        for (int i = x0; i < x1; i++)
            back[j * SCR_W + i] = c;
}

static int isqrt(int v)
{
    int s = 0;
    while ((s + 1) * (s + 1) <= v)
        s++;
    return s;
}

static void fill_circle(int cx, int cy, int r, u32 c)
{
    for (int dy = -r; dy <= r; dy++) {
        int half = isqrt(r * r - dy * dy);
        fill_rect(cx - half, cy + dy, half * 2 + 1, 1, c);
    }
}

/* sx, sy = body centre, s = scale (1 = in-game size) */
static void draw_ghost(int sx, int sy, int s, u32 c)
{
    fill_circle(sx, sy - 8 * s, 11 * s, c);
    fill_rect(sx - 11 * s, sy - 8 * s, 23 * s, 18 * s, c);
    for (int i = 0; i < 4; i++)                       /* ragged hem */
        fill_rect(sx - 11 * s + i * 6 * s, sy + 10 * s, 4 * s, 4 * s, c);
    fill_rect(sx - 5 * s, sy - 11 * s, 3 * s, 4 * s, C_BLACK); /* eyes */
    fill_rect(sx + 3 * s, sy - 11 * s, 3 * s, 4 * s, C_BLACK);
}

/* Darkness everywhere except a circle around the lantern. */
static void draw_darkness(int cx, int cy, int r)
{
    for (int y = 0; y < SCR_H; y++) {
        int dy = y - cy;
        if (r <= 0 || dy <= -r || dy >= r) {
            fill_rect(0, y, SCR_W, 1, C_BLACK);
            continue;
        }
        int half = isqrt(r * r - dy * dy);
        fill_rect(0, y, cx - half, 1, C_BLACK);
        fill_rect(cx + half + 1, y, SCR_W - (cx + half + 1), 1, C_BLACK);
    }
}

/* ---------- level setup ---------- */
static void reset_level(int lvl)
{
    const LevelDef *L = &levels[lvl];

    memset(&g, 0, sizeof(g));
    g.lvl = lvl;
    g.px = 40;
    g.py = FLOOR_Y - PH;
    g.on_ground = 1;
    g.gx = g.px - 220;                /* ghost starts behind you */

    for (int i = 0; i < L->candles; i++) {
        g.cand_x[i] = 300 + rnd() % (L->length - 600);
        g.cand_on[i] = 1;
    }

    g.ncrates = MAX_CRATES;
    for (int i = 0; i < MAX_CRATES; i++)
        g.crate_x[i] = 260 + i * ((L->length - 500) / MAX_CRATES) + rnd() % 60;

    for (int i = 0; i < MAX_MARKS; i++) {
        g.mark_x[i] = rnd() % L->length;
        g.mark_y[i] = 40 + rnd() % 150;
    }

    g.fake_x = L->length / 2 + 80;
    for (int i = 0; i < HIST_N; i++)
        g.hist[i] = g.px - 150;

    set_whisper(L->intro, 240);
    state = ST_PLAY;
}

static void die(void)
{
    deaths++;
    state = ST_DEAD;
    g.timer = 120;
}

/* ---------- game update ---------- */
static int surface_under(int x, int old_bot, int new_bot, int ncr)
{
    int best = FLOOR_Y;
    for (int i = 0; i < ncr; i++) {
        int ct = FLOOR_Y - CRATE_H;   /* crate top */
        if (x + PW > g.crate_x[i] && x < g.crate_x[i] + CRATE_W &&
            old_bot <= ct && new_bot >= ct && ct < best)
            best = ct;
    }
    return best;
}

static void update_play(u32 buttons, int lx)
{
    const LevelDef *L = &levels[g.lvl];
    g.frame++;

    /* input */
    int move = 0;
    if ((buttons & PSP_CTRL_LEFT)  || lx < 90)  move = -1;
    if ((buttons & PSP_CTRL_RIGHT) || lx > 166) move = 1;
    if (move) { g.idle = 0; } else { g.idle++; }

    /* horizontal, blocked by crate sides */
    int nx = g.px + move * 2;
    if (nx < 0) nx = 0;
    if (nx > L->length - PW) nx = L->length - PW;
    int blocked = 0;
    for (int i = 0; i < g.ncrates; i++)
        if (aabb(nx, g.py, PW, PH, g.crate_x[i], FLOOR_Y - CRATE_H, CRATE_W, CRATE_H))
            blocked = 1;
    if (!blocked)
        g.px = nx;

    /* jump + gravity, can land on crate tops */
    if (g.on_ground && (buttons & PSP_CTRL_CROSS))
        g.vy = -12;
    int old_bot = g.py + PH;
    g.vy += 1;
    if (g.vy > 12) g.vy = 12;
    int new_top = g.py + g.vy;
    int new_bot = new_top + PH;
    int surface = surface_under(g.px, old_bot, new_bot, g.ncrates);
    if (new_bot >= surface) {
        g.py = surface - PH;
        g.vy = 0;
        g.on_ground = 1;
    } else {
        g.py = new_top;
        g.on_ground = 0;
    }

    /* candles */
    for (int i = 0; i < L->candles; i++) {
        if (g.cand_on[i] && iabs(g.px + PW / 2 - g.cand_x[i]) < 14) {
            g.cand_on[i] = 0;
            g.candles_got++;
        }
    }

    /* fake door: looks exactly like the real one, and it lies */
    if (L->fake_door && !g.fake_used &&
        g.px + PW > g.fake_x && g.px < g.fake_x + DOOR_W) {
        g.fake_used = 1;
        g.gx = g.px - 230;            /* the ghost is suddenly behind you */
        g.gtick = 0;
        set_whisper("that was not the door", 150);
    }

    /* real door */
    int door_x = L->length - DOOR_OFFSET;
    if (g.px + PW > door_x && g.px < door_x + DOOR_W) {
        if (g.candles_got >= L->candles) {
            state = ST_CLEAR;
            g.timer = 150;
            return;
        }
        if (g.whisper_t < 60)
            set_whisper("it's locked... something moved behind it", 120);
    }

    /* random decoy flash: a ghost that is not really there */
    if (g.decoy_t > 0) {
        g.decoy_t--;
    } else if (g.frame > 120 && rnd() % 600 == 0) {
        g.decoy_t = 4;
        g.decoy_x = g.px + ((rnd() & 1) ? 120 : -120) + (int)(rnd() % 40) - 20;
    }

    /* ghost movement */
    if (L->mirror) {
        g.gx = g.hist[(g.hist_i + HIST_N - MIRROR_DELAY) % HIST_N];
    } else {
        int period = L->ghost_period;
        if (g.lvl >= 1 && g.idle > MIN_IDLE)   /* it creeps when you stop */
            period = 1;
        if (++g.gtick >= period) {
            g.gtick = 0;
            if (g.gx < g.px)      g.gx++;
            else if (g.gx > g.px) g.gx--;
        }
    }

    /* caught? */
    if (g.frame > 60 &&
        iabs(g.gx - (g.px + PW / 2)) < 12 &&
        g.py + PH > GHOST_TOP) {
        die();
        return;
    }

    /* camera, shakes when the ghost is close */
    g.cam = g.px + PW / 2 - SCR_W / 2;
    if (g.cam < 0) g.cam = 0;
    if (g.cam > L->length - SCR_W) g.cam = L->length - SCR_W;
    if (iabs(g.gx - g.px) < 90)
        g.cam += (int)(rnd() % 5) - 2;

    /* history for the mirror ghost */
    g.hist[g.hist_i] = g.px;
    g.hist_i = (g.hist_i + 1) % HIST_N;

    /* whispers */
    if (g.whisper_t > 0)
        g.whisper_t--;
    else if (g.frame > 120 && rnd() % 500 == 0)
        set_whisper(whispers[rnd() % NUM_WHISPERS], 150);
}

/* ---------- drawing the level ---------- */
static void draw_play(void)
{
    const LevelDef *L = &levels[g.lvl];
    int c = g.cam;

    fill_rect(0, 0, SCR_W, SCR_H, C_BG);

    /* wall panels, anchored to the world */
    for (int k = c / 64; k * 64 - c < SCR_W; k++)
        fill_rect(k * 64 - c, 0, 2, FLOOR_Y, C_WALL);

    /* scratches / handprints on the wall */
    for (int i = 0; i < MAX_MARKS; i++) {
        int sx = g.mark_x[i] - c;
        fill_rect(sx, g.mark_y[i], 10, 3, C_MARK);
        fill_rect(sx + 3, g.mark_y[i] + 3, 3, 12, C_MARK);
    }

    /* floor and boards */
    fill_rect(0, FLOOR_Y, SCR_W, SCR_H - FLOOR_Y, C_FLOOR);
    for (int k = c / 40; k * 40 - c < SCR_W; k++)
        fill_rect(k * 40 - c, FLOOR_Y + 8, 2, SCR_H - FLOOR_Y - 8, C_BOARD);

    /* doors (the fake one is drawn identical to the real one) */
    int door_x = L->length - DOOR_OFFSET - c;
    int unlocked = g.candles_got >= L->candles;
    fill_rect(door_x, FLOOR_Y - DOOR_H, DOOR_W, DOOR_H,
              unlocked ? C_DOOR_OPEN : C_DOOR_LOCK);
    fill_rect(door_x + 4, FLOOR_Y - DOOR_H + 4, DOOR_W - 8, DOOR_H - 4,
              unlocked ? C_DOOR_IN_OK : C_BLACK);
    if (L->fake_door && !g.fake_used) {
        int fx = g.fake_x - c;
        fill_rect(fx, FLOOR_Y - DOOR_H, DOOR_W, DOOR_H, C_DOOR_LOCK);
        fill_rect(fx + 4, FLOOR_Y - DOOR_H + 4, DOOR_W - 8, DOOR_H - 4, C_BLACK);
    }

    /* crates */
    for (int i = 0; i < g.ncrates; i++) {
        int sx = g.crate_x[i] - c;
        fill_rect(sx, FLOOR_Y - CRATE_H, CRATE_W, CRATE_H, C_CRATE);
        fill_rect(sx + 2, FLOOR_Y - CRATE_H + 2, CRATE_W - 4, 2, C_CRATE_IN);
    }

    /* candles */
    for (int i = 0; i < L->candles; i++) {
        if (!g.cand_on[i])
            continue;
        int sx = g.cand_x[i] - c;
        fill_rect(sx, FLOOR_Y - 12, 5, 12, C_WAX);
        fill_rect(sx + 2, FLOOR_Y - 17, 2, 5, C_FLAME);
    }

    /* player */
    int psx = g.px - c;
    fill_rect(psx + 3, g.py, 8, 8, C_PLAYER);
    fill_rect(psx, g.py + 8, PW, PH - 8, C_SHIRT);

    /* the ghost, drawn before the darkness: you only see it in the light */
    draw_ghost(g.gx - c, GHOST_CY, 1, C_GHOST);

    /* lantern: radius flickers, and in BLACKOUT it cuts out */
    int r = L->light_r;
    if (L->blackout && (g.frame % 240) < 100)
        r = 0;
    if (r > 0)
        r += (int)(rnd() % 5) - 2;
    draw_darkness(psx + PW / 2, g.py + PH / 2, r);

    /* decoy flash, drawn after the darkness so it appears in the black */
    if (g.decoy_t > 0)
        draw_ghost(g.decoy_x - c, GHOST_CY, 1, C_DECOY);
}

/* ---------- screen per state (shapes) ---------- */
static void draw_shapes(void)
{
    switch (state) {
    case ST_TITLE:
        fill_rect(0, 0, SCR_W, SCR_H, C_BLACK);
        draw_ghost(SCR_W / 2, SCR_H / 2 + 10, 3, C_GHOST);
        break;
    case ST_PLAY:
        draw_play();
        break;
    case ST_DEAD:
        fill_rect(0, 0, SCR_W, SCR_H, g.timer > 110 ? C_WHITE : C_BLACK);
        if (g.timer <= 110)
            draw_ghost(SCR_W / 2, SCR_H / 2, 4, C_GHOST);
        break;
    case ST_CLEAR:
        fill_rect(0, 0, SCR_W, SCR_H, C_BLACK);
        fill_rect(SCR_W / 2 - 22, FLOOR_Y - DOOR_H, DOOR_W, DOOR_H, C_DOOR_OPEN);
        fill_rect(SCR_W / 2 - 18, FLOOR_Y - DOOR_H + 4, DOOR_W - 8, DOOR_H - 4, C_DOOR_IN_OK);
        break;
    case ST_END:
        fill_rect(0, 0, SCR_W, SCR_H, C_BLACK);
        /* a faint ghost that keeps moving where you can't quite see it */
        draw_ghost((int)(rnd() % SCR_W), (int)(rnd() % SCR_H), 1, C_DECOY);
        break;
    }
}

/* ---------- text (ASCII only, PSP debug font) ---------- */
static void txt(int col, int row, const char *s)
{
    pspDebugScreenSetXY(col, row);
    pspDebugScreenPrintf("%s", s);
}

static void draw_text(void)
{
    switch (state) {
    case ST_TITLE:
        txt(18, 3, "G H O S T   H A L L W A Y");
        txt(12, 6, "a walk down a hallway that is not empty");
        txt(12, 9, "LEFT/RIGHT or stick: walk     X: jump");
        txt(14, 11, "collect the candles, reach the door");
        txt(20, 24, "PRESS START");
        break;
    case ST_PLAY:
        pspDebugScreenSetXY(1, 0);
        pspDebugScreenPrintf("LV%d %s  CANDLES %d/%d  DEATHS %d",
                             g.lvl + 1, levels[g.lvl].name,
                             g.candles_got, levels[g.lvl].candles, deaths);
        if (g.whisper_t > 0 && g.whisper) {
            int len = (int)strlen(g.whisper) + 2;
            pspDebugScreenSetXY((60 - len) / 2, 25);
            pspDebugScreenPrintf("\"%s\"", g.whisper);
        }
        break;
    case ST_DEAD:
        if (g.timer <= 110) {
            txt(20, 3, "IT GOT YOU");
            txt(17, 5, "it was waiting for you");
        }
        break;
    case ST_CLEAR:
        txt(18, 14, "DOOR OPENED");
        txt(12, 16, "you walked away from it. did it follow?");
        break;
    case ST_END:
        txt(16, 8, "YOU MADE IT OUT.");
        txt(12, 10, "did you bring me with you?");
        pspDebugScreenSetXY(20, 14);
        pspDebugScreenPrintf("deaths: %d", deaths);
        txt(20, 24, "PRESS START");
        break;
    }
}

/* ---------- framebuffer copy ---------- */
static void present(void)
{
    for (int y = 0; y < SCR_H; y++)
        memcpy(&VRAM[y * STRIDE], &back[y * SCR_W], SCR_W * sizeof(u32));
}

/* ---------- main ---------- */
int main(void)
{
    setup_callbacks();

    pspDebugScreenInit();
    pspDebugScreenSetBackColor(0x00000000);
    pspDebugScreenSetTextColor(0xFFDDDDDD);

    sceDisplaySetMode(0, SCR_W, SCR_H);
    sceDisplaySetFrameBuf((void *)0x44000000, STRIDE,
                          PSP_DISPLAY_PIXEL_FORMAT_8888,
                          PSP_DISPLAY_SETBUF_IMMEDIATE);

    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);

    for (;;) {
        SceCtrlData pad;
        sceCtrlPeekBufferPositive(&pad, 1);
        u32 buttons = pad.Buttons;
        u32 pressed = buttons & ~prev_buttons;
        prev_buttons = buttons;
        tick++;

        switch (state) {
        case ST_TITLE:
            if (pressed & PSP_CTRL_START) {
                rng_state = (u32)tick * 2654435761u;
                deaths = 0;
                reset_level(0);
            }
            break;

        case ST_PLAY:
            update_play(buttons, pad.Lx);
            break;

        case ST_DEAD:
            if (--g.timer <= 0)
                reset_level(g.lvl);          /* same hallway, you try again */
            break;

        case ST_CLEAR:
            if (--g.timer <= 0) {
                if (g.lvl + 1 < NUM_LEVELS)
                    reset_level(g.lvl + 1);
                else
                    state = ST_END;
            }
            break;

        case ST_END:
            if (pressed & PSP_CTRL_START)
                state = ST_TITLE;
            break;
        }

        draw_shapes();
        present();
        draw_text();
        sceDisplayWaitVblankStart();
    }

    return 0;
}
