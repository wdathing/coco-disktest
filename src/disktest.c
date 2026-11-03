/* disktest - disk speed and torture test for the Tandy Color Computer 3
 *
 * Over and over until BREAK: reads DISKDATA.DAT and checks every byte,
 * writes DISKCOPY.DAT with a pattern new to this pass, then reads it back
 * and checks it. All disk access goes through DSKCON, so it runs the same
 * on a stock floppy controller and on HDB-DOS. Writes only go to sectors
 * DISKCOPY.DAT already owns; the directory and FAT are never written.
 *
 * Both files hold a pattern the program can compute, so nothing has to be
 * kept in RAM. In file sector s, byte i is a + i*b (mod 256) with
 *     a = s*$9D + seed*$35,  b = 2*(s XOR seed) + 1   (all mod 256)
 * b is odd, so every sector holds all 256 byte values once, in an order of
 * its own. tools/mkdata.py writes both files with seed 0; pass n writes the
 * copy with seed n, so a write that silently doesn't happen shows up.
 *
 * Speeds come from BASIC's TIMER. DSKCON turns interrupts off while it
 * transfers (on a floppy, while it waits for the sector too), and TIMER
 * stops with them, so the figures read high: good for comparing passes and
 * set-ups, not as absolute speeds.
 *
 * The CPU speed is left as BASIC had it: Disk BASIC's timeouts assume
 * 0.89 MHz, and some DriveWire builds of HDB-DOS assume 1.79 MHz.
 *
 * Memory map while running:
 *   $0000-$0DFF  BASIC / Disk BASIC system RAM (DSKCON etc.)
 *   $2601-       AUTOEXEC.BAS, still running when LOADM brings us in
 *   $2700-$5BFF  this program
 *   $5C00-$5FFF  our stack (runOnOwnStack)
 *   $6000-$6EFF  the 80x24 screen, physical page $36 in MMU slot 3
 */
#include <coco.h>

typedef unsigned long u32;

#define VERSION     "1.0"
#define MMU_SLOT3   (* (byte *) 0xFFA3)
#define BASIC_SLOT3 0x3B
#define SCR_PAGE    0x36            /* physical $6C000, where BASIC keeps WIDTH 80 */
#define SCREEN      ((byte *) 0x6000)
#define TIMER       (* (word *) 0x0112)    /* BASIC's 60 Hz counter */
#define DEFDRV      (* (byte *) 0x095A)    /* Disk BASIC's default drive */

#define OP_READ     2
#define OP_WRITE    3

#define MAXGRAN     156             /* an 80-track RS-DOS disk; 35 tracks: 68 */
#define SECS        9               /* sectors per granule */

#define BAR_X       12              /* throughput bars */
#define BAR_W       36
#define PROG_X      12              /* progress bars */
#define PROG_W      50              /* drawProgress() relies on this */

/* ------------------------------------------------------------------ colour */

/* Attribute byte: foreground palette 8-15 in bits 5-3, background 0-7 in
 * bits 2-0. */
#define AT(fg, bg)  ((byte) ((((fg) & 7) << 3) | (bg)))

enum { BG_DESK, BG_SHADOW, BG_PANEL, BG_TITLE, BG_EMPTY, BG_READ, BG_WRITE, BG_VERIFY };
enum { FG_BLACK = 8, FG_WHITE, FG_YELLOW, FG_BLUE, FG_DARK, FG_RED, FG_GREEN, FG_CYAN };

/* Palette as colour cube indices r*16 + g*4 + b, levels 0-3. */
const byte palCube[16] = {
    0x02, 0x00, 0x2A, 0x3F, 0x15, 0x0B, 0x34, 0x08,   /* backgrounds */
    0x00, 0x3F, 0x3D, 0x02, 0x15, 0x35, 0x1D, 0x1F    /* text */
};

/* cube index -> nearest composite code (from the FLC player) */
const byte cmpTab[64] = {
     0, 12, 12, 27,  2, 14, 13, 28,  2, 15, 30, 29, 17, 17, 31, 46,
     6,  9, 10, 27,  4, 32, 27, 27,  3, 17, 30, 44, 18, 17, 47, 46,
     7,  8, 25, 26,  5, 22, 25, 42, 20, 20, 32, 43, 19, 34, 33, 61,
     7, 23, 24, 41, 22, 22, 39, 41, 21, 37, 38, 56, 36, 36, 51, 48
};

/* Composite monitor. Starts off: BASIC's palette can't tell us (an
 * autostart never ran RGB), and 80 columns are hard to read on composite
 * anyway. C switches. */
byte cmpMode;
byte savedPalette[16];

/* cube index -> GIME palette register value for the monitor in use */
byte palColor(byte q)
{
    byte r = q >> 4, g = (q >> 2) & 3, b = q & 3;
    if (cmpMode)
        return cmpTab[q];
    return ((r & 2) << 4) | ((g & 2) << 3) | ((b & 2) << 2)
         | ((r & 1) << 2) | ((g & 1) << 1) | (b & 1);
}

void loadPalette(void)
{
    byte i;
    for (i = 0; i < 16; ++i)
        * (byte *) (0xFFB0 + i) = palColor(palCube[i]);
}

/* ------------------------------------------------------------------ screen */

/* GIME 80x24 text with attributes at physical $6C000, mapped at $6000. */
void screenOn(void)
{
    byte i;
    for (i = 0; i < 16; ++i)
        savedPalette[i] = * (byte *) (0xFFB0 + i);
    MMU_SLOT3 = SCR_PAGE;
    memset(SCREEN, 0, 80 * 24 * 2);
    loadPalette();
    * (byte *) 0xFF90 = 0x4C;           /* CoCo 3 mode, MMU on, $FExx constant */
    * (byte *) 0xFF98 = 0x03;           /* text, 8 lines per row */
    * (byte *) 0xFF99 = 0x15;           /* 192 lines, 80 columns, attributes */
    * (byte *) 0xFF9A = 0;              /* border: palette 0 */
    * (byte *) 0xFF9C = 0;
    * (byte *) 0xFF9D = SCR_PAGE << 2;  /* physical $6C000 / 8 */
    * (byte *) 0xFF9E = 0;
    * (byte *) 0xFF9F = 0;
}

/* Back to BASIC's 32 column screen. */
void screenOff(void)
{
    byte i;
    /* Same values as the CoCo 3 ROM's reset table at $C22E. */
    * (byte *) 0xFF90 = 0xCC;
    * (byte *) 0xFF98 = 0x00;
    * (byte *) 0xFF99 = 0x00;
    * (byte *) 0xFF9A = 0x00;
    * (byte *) 0xFF9C = 0x0F;
    * (byte *) 0xFF9D = 0xE0;
    * (byte *) 0xFF9E = 0;
    * (byte *) 0xFF9F = 0;
    for (i = 0; i < 16; ++i)
        * (byte *) (0xFFB0 + i) = savedPalette[i];
    MMU_SLOT3 = BASIC_SLOT3;
}

byte *cell(byte x, byte y)
{
    return SCREEN + (word) y * 160 + (word) x * 2;
}

void fill(byte x, byte y, byte w, byte h, byte at)
{
    byte *p;
    byte i;
    if (x + w > 80)
        w = 80 - x;
    for (; h; --h, ++y)
        for (p = cell(x, y), i = w; i; --i)
        {
            *p++ = ' ';
            *p++ = at;
        }
}

/* Changes the colours of a run of cells, keeping the characters. */
void paint(byte x, byte y, byte w, byte at)
{
    byte *p = cell(x, y) + 1;
    for (; w; --w, p += 2)
        *p = at;
}

void text(byte x, byte y, byte at, const char *s)
{
    byte *p = cell(x, y);
    while (*s && x++ < 80)
    {
        *p++ = (byte) *s++;
        *p++ = at;
    }
}

/* text, then spaces to width w */
void field(byte x, byte y, byte w, byte at, const char *s)
{
    fill(x, y, w, 1, at);
    text(x, y, at, s);
}

/* A window: a title row, a body of h-1 rows, a shadow to the right and
 * below. */
void window(byte x, byte y, byte w, byte h, const char *title)
{
    fill(x, y, w, 1, AT(FG_BLUE, BG_TITLE));
    text(x + (w - (byte) strlen(title)) / 2, y, AT(FG_BLUE, BG_TITLE), title);
    fill(x, y + 1, w, h - 1, AT(FG_BLACK, BG_PANEL));
    for (++y; h; --h, ++y)
        paint(x + w, y, 2, AT(FG_DARK, BG_SHADOW));
    paint(x + 2, y - 1, w, AT(FG_DARK, BG_SHADOW));
}

/* A bar of n cells out of w, in colour bg, the rest empty. */
void bar(byte x, byte y, byte w, byte n, byte bg)
{
    if (n > w)
        n = w;
    fill(x, y, n, 1, AT(FG_BLACK, bg));
    fill(x + n, y, w - n, 1, AT(FG_DARK, BG_EMPTY));
}

/* ------------------------------------------------------------------ disk */

struct File
{
    byte gran[MAXGRAN];
    word sectors;
};

struct File dataFile, copyFile;

#define DATA_NAME   "DISKDATA.DAT"
#define COPY_NAME   "DISKCOPY.DAT"

byte drive;
byte buf[256];
byte fat[256];

/* One sector through DSKCON. Returns DCSTA: 0 if it worked, else the
 * controller's status bits. */
byte dskcon(byte op, byte track, byte sector)
{
    * (byte *) 0x00EA = op;             /* DCOPC */
    * (byte *) 0x00EB = drive;          /* DCDRV */
    * (byte *) 0x00EC = track;          /* DCTRK */
    * (byte *) 0x00ED = sector;         /* DCSEC */
    * (byte **) 0x00EE = buf;           /* DCBPT */
    asm
    {
        pshs    u,y
        jsr     [$C004]
        puls    y,u
    }
    return * (byte *) 0x00F0;           /* DCSTA */
}

void granToTS(byte g, byte *track, byte *sector)
{
    byte t = g >> 1;
    if (t >= 17)                        /* track 17 holds the directory */
        ++t;
    *track = t;
    *sector = (g & 1) ? 10 : 1;
}

/* Finds the file called name (8 + 3, space padded, as in the directory)
 * and lists its granules in f. Returns an error string or 0. */
const char *findFile(struct File *f, const char *name)
{
    byte sector, i, g, n;
    byte *e;

    for (sector = 3; sector <= 11; ++sector)
    {
        if (dskcon(OP_READ, 17, sector))
            return "CAN'T READ THE DIRECTORY";
        for (i = 0; i < 8; ++i)
        {
            e = buf + i * 32;
            if (e[0] == 0xFF)
                return "NOT ON THIS DISK";
            if (memcmp(e, name, 11))
                continue;
            g = e[13];
            for (n = 0; n < MAXGRAN && g < MAXGRAN && fat[g] < 0xC0; ++n)
            {
                f->gran[n] = g;
                g = fat[g];
            }
            if (n == MAXGRAN || g >= MAXGRAN)
                return "HAS A BROKEN GRANULE CHAIN";
            f->gran[n] = g;
            f->sectors = (word) n * SECS + (fat[g] & 0x3F);
            return 0;
        }
    }
    return "NOT ON THIS DISK";
}

/* ------------------------------------------------------------------ pattern */

/* buf = the pattern starting at a, stepping by b */
void fillPattern(byte a, byte b)
{
    asm
    {
        ldx     #_buf
        lda     :a
        ldb     :b
        pshs    b
        clrb                            ; 256 bytes
fp_loop:
        sta     ,x+
        adda    ,s
        decb
        bne     fp_loop
        leas    1,s
    }
}

/* Does buf hold the pattern? */
byte checkPattern(byte a, byte b)
{
    byte bad = 1;
    asm
    {
        ldx     #_buf
        lda     :a
        ldb     :b
        pshs    b
        clrb
cp_loop:
        cmpa    ,x+
        bne     cp_done
        adda    ,s
        decb
        bne     cp_loop
        clr     :bad
cp_done:
        leas    1,s
    }
    return !bad;
}

/* ------------------------------------------------------------------ test */

enum { PH_READ, PH_WRITE, PH_VERIFY };

const char *const phaseName[3] = { "READ", "WRITE", "VERIFY" };
const char *const phaseWhat[3] = { "Reading the data file",
                                   "Writing the copy",
                                   "Reading the copy back" };
const byte phaseBg[3] = { BG_READ, BG_WRITE, BG_VERIFY };

word nsec;                  /* sectors per phase (the data file's) */
word pass;
word errors;
char lastError[36] = "none";
/* Kept as counters so the sector loop never does 32-bit sums or
 * divisions: CMOC's helpers for those cost more than DSKCON did. */
word movedM, movedK;        /* data read and written: M + K (< 1024) */
byte movedQ;                /* + quarters of a K (sectors) */
word hours, secs;           /* elapsed: hours + seconds (< 3600) */
word ticks;                 /* + TIMER ticks (< 60) */
word lastTimer;

word rate[3];               /* K/s in tenths: this pass, or the last */
word best[3];
u32 rateSum[3];
word rateCount[3];
word avgRate[3];            /* rateSum / rateCount, worked out once a phase */
word scale = 40;            /* tenths of K/s at the end of the bars */

char line[81];

/* K/s in tenths for n bytes in t ticks */
word kps(u32 n, word t)
{
    u32 r;
    if (t == 0)
        t = 1;
    r = n * 75 / 128 / t;           /* n / 1024 * 60 / t * 10 */
    return r > 65000 ? 65000 : (word) r;
}

void drawScale(void)
{
    byte i;
    word v;
    fill(BAR_X - 1, 15, BAR_W + 7, 1, AT(FG_BLUE, BG_PANEL));
    for (i = 0; i <= 4; ++i)
    {
        v = scale / 40 * i;         /* K/s; see fitScale() */
        sprintf(line, "%u", v);
        text(BAR_X + i * (BAR_W / 4) - (i ? 1 : 0), 15, AT(FG_BLUE, BG_PANEL), line);
    }
    text(BAR_X + BAR_W + 2, 15, AT(FG_BLUE, BG_PANEL), "K/s");
}

void drawRate(byte ph)
{
    word avg = avgRate[ph];
    bar(BAR_X, 12 + ph, BAR_W, (byte) ((u32) rate[ph] * BAR_W / scale), phaseBg[ph]);
    sprintf(line, "%3u.%u", rate[ph] / 10, rate[ph] % 10);
    field(BAR_X + BAR_W + 1, 12 + ph, 6, AT(FG_BLACK, BG_PANEL), line);
    if (rateCount[ph])
    {
        sprintf(line, "avg %u.%u", avg / 10, avg % 10);
        field(BAR_X + BAR_W + 8, 12 + ph, 10, AT(FG_DARK, BG_PANEL), line);
        sprintf(line, "best %u.%u", best[ph] / 10, best[ph] % 10);
        field(BAR_X + BAR_W + 19, 12 + ph, 10, AT(FG_DARK, BG_PANEL), line);
    }
}

/* Grows the scale to fit r. */
void fitScale(word r)
{
    /* in tenths; a quarter of each is a whole number of K/s */
    static const word steps[] = { 40, 80, 200, 400, 800, 2000, 4000, 8000, 20000, 40000, 0 };
    byte i, ph;
    if (r <= scale)
        return;
    for (i = 0; steps[i] && steps[i] < r; ++i)
        ;
    scale = steps[i] ? steps[i] : (word) 64000;
    drawScale();
    for (ph = 0; ph < 3; ++ph)
        drawRate(ph);
}

void drawStatus(byte ph)
{
    byte at = AT(FG_BLACK, BG_PANEL);
    sprintf(line, "%u", pass);
    field(55, 3, 22, at, line);
    field(55, 4, 22, at, phaseName[ph]);
    sprintf(line, "%u:%02u:%02u", hours, secs / 60, secs % 60);
    field(55, 5, 22, at, line);
    if (movedM)
        sprintf(line, "%u.%uM", movedM, movedK * 10 / 1024);
    else
        sprintf(line, "%uK", movedK);
    field(55, 6, 22, at, line);
    sprintf(line, "%u", errors);
    field(55, 7, 22, errors ? AT(FG_RED, BG_PANEL) : at, line);
    field(55, 8, 22, errors ? AT(FG_RED, BG_PANEL) : at, lastError);
}

void drawProgress(byte ph, word done)
{
    /* percentages first: the bars are 50 cells, half of them */
    byte at = AT(FG_BLACK, BG_PANEL);
    byte p1 = (byte) ((u32) done * 100 / nsec);
    byte p2 = (byte) ((u32) (ph * nsec + done) * 100 / (nsec * 3));
    bar(PROG_X, 20, PROG_W, p1 / 2, phaseBg[ph]);
    sprintf(line, "%3u%%  %s", p1, phaseName[ph]);
    field(PROG_X + PROG_W + 2, 20, 12, at, line);
    bar(PROG_X, 21, PROG_W, p2 / 2, BG_EMPTY);
    paint(PROG_X, 21, p2 / 2, AT(FG_BLACK, BG_TITLE));
    sprintf(line, "%3u%%  PASS %u", p2, pass);
    field(PROG_X + PROG_W + 2, 21, 12, at, line);
    field(4, 19, 40, AT(FG_BLUE, BG_PANEL), phaseWhat[ph]);
}

/* Keys between sectors: C swaps RGB/composite colours. TRUE on BREAK. */
byte stopKey(void)
{
    byte k = inkey();
    if (k == 'C' || k == 'c')
    {
        cmpMode = !cmpMode;
        loadPalette();
        field(4, 8, 30, AT(FG_BLACK, BG_PANEL), cmpMode ? "Colours: composite" : "Colours: RGB");
    }
    return k == 3;
}

/* Adds the TIMER ticks since the last call to the elapsed time. */
void tick(void)
{
    word t = TIMER;
    ticks += t - lastTimer;
    lastTimer = t;
    if (ticks >= 60)
    {
        secs += ticks / 60;
        ticks %= 60;
        while (secs >= 3600)
        {
            secs -= 3600;
            ++hours;
        }
    }
}

/* Any key down? A cheap look at the keyboard matrix (all columns at
 * once) before asking BASIC which. */
byte keyDown(void)
{
    byte v;
    * (byte *) 0xFF02 = 0;
    v = * (byte *) 0xFF00;
    * (byte *) 0xFF02 = 0xFF;
    return (v & 0x7F) != 0x7F;
}

/* Redraws the live figures for phase ph, done sectors in. */
void update(byte ph, word done, word t0)
{
    tick();
    rate[ph] = kps((u32) done << 8, TIMER - t0);
    fitScale(rate[ph]);
    drawRate(ph);
    drawProgress(ph, done);
    drawStatus(ph);
}

void logError(byte ph, byte track, byte sector, byte status)
{
    ++errors;
    if (status)
        sprintf(lastError, "P%u %s T%u S%u ST $%02X", pass, phaseName[ph], track, sector, status);
    else
        sprintf(lastError, "P%u %s T%u S%u DATA", pass, phaseName[ph], track, sector);
}

/* One phase over f: READ and VERIFY check the pattern for seed, WRITE
 * writes it. FALSE if BREAK stopped it. The loop does no more than it
 * must per sector; the screen is redrawn after a granule, at most four
 * times a second. */
byte runPhase(byte ph, struct File *f, byte seed)
{
    word s = 0, t0 = TIMER, drawn = t0;
    byte g = 0, i, track, sector, st, b;
    byte a = seed * 0x35;               /* a for sector s: + $9D each */
    byte op = ph == PH_WRITE ? OP_WRITE : OP_READ;

    update(ph, 0, t0);
    while (s < nsec)
    {
        granToTS(f->gran[g++], &track, &sector);
        for (i = 0; i < SECS && s < nsec; ++i, ++s, ++sector, a += 0x9D)
        {
            b = (((byte) s ^ seed) << 1) | 1;
            if (op == OP_WRITE)
                fillPattern(a, b);
            st = dskcon(op, track, sector);
            if (st)
                logError(ph, track, sector, st);
            else if (op == OP_READ && !checkPattern(a, b))
                logError(ph, track, sector, 0);
            if (++movedQ == 4)
            {
                movedQ = 0;
                if (++movedK == 1024)
                {
                    movedK = 0;
                    ++movedM;
                }
            }
            /* every sector: the keyboard has no buffer, a key only
             * shows while it is down */
            if (keyDown() && stopKey())
            {
                update(ph, s + 1, t0);
                return FALSE;
            }
        }
        if (TIMER - drawn >= 15 || s == nsec)
        {
            update(ph, s, t0);
            drawn = TIMER;
        }
    }
    if (rate[ph] > best[ph])
        best[ph] = rate[ph];
    rateSum[ph] += rate[ph];
    ++rateCount[ph];
    avgRate[ph] = (word) (rateSum[ph] / rateCount[ph]);
    drawRate(ph);
    return TRUE;
}

void footer(const char *s)
{
    fill(0, 23, 80, 1, AT(FG_BLACK, BG_TITLE));
    text(1, 23, AT(FG_BLACK, BG_TITLE), s);
    text(66, 23, AT(FG_BLUE, BG_TITLE), "DSKCON  TIMER");
}

void drawScreen(void)
{
    byte at = AT(FG_BLACK, BG_PANEL);

    fill(0, 0, 80, 1, AT(FG_BLUE, BG_TITLE));
    text(1, 0, AT(FG_BLUE, BG_TITLE), "DISKTEST " VERSION);
    text(16, 0, AT(FG_BLACK, BG_TITLE), "Disk speed and torture test");
    text(63, 0, AT(FG_BLACK, BG_TITLE), "Color Computer 3");
    fill(0, 1, 80, 22, AT(FG_WHITE, BG_DESK));

    window(2, 2, 36, 7, "Test set-up");
    sprintf(line, "Drive:      %u", drive);
    text(4, 3, at, line);
    text(4, 4, at, "Data file:  " DATA_NAME);
    text(4, 5, at, "Copy file:  " COPY_NAME);
    sprintf(line, "Each phase: %u sectors, %uK", nsec, nsec >> 2);
    text(4, 6, at, line);
    text(4, 7, at, "Speeds:     TIMER, reads high");
    text(4, 8, at, cmpMode ? "Colours: composite" : "Colours: RGB");

    window(41, 2, 37, 7, "Status");
    text(43, 3, at, "Pass:");
    text(43, 4, at, "Phase:");
    text(43, 5, at, "Elapsed:");
    text(43, 6, at, "Moved:");
    text(43, 7, at, "Errors:");
    text(43, 8, at, "Last error:");

    window(2, 11, 76, 5, "Throughput");
    text(4, 12, at, "READ");
    text(4, 13, at, "WRITE");
    text(4, 14, at, "VERIFY");
    drawScale();
    drawRate(PH_READ);
    drawRate(PH_WRITE);
    drawRate(PH_VERIFY);

    window(2, 18, 76, 4, "Progress");
    text(4, 20, at, "Phase");
    text(4, 21, at, "Pass");
}

/* A message box; waits for a key. */
void message(const char *title, const char *s1, const char *s2)
{
    window(16, 9, 48, 5, title);
    text(18, 11, AT(FG_BLACK, BG_PANEL), s1);
    text(18, 12, AT(FG_BLACK, BG_PANEL), s2);
    footer("Press any key to return to BASIC");
    while (!inkey())
        ;
}

/* Reads the FAT and finds both files. Returns an error string or 0. */
const char *setUp(void)
{
    const char *err;
    if (dskcon(OP_READ, 17, 2))
        return "Can't read the disk's FAT.";
    memcpy(fat, buf, sizeof(fat));
    if ((err = findFile(&dataFile, "DISKDATADAT")) != 0)
    {
        sprintf(line, DATA_NAME " %s", err);
        return line;
    }
    if ((err = findFile(&copyFile, "DISKCOPYDAT")) != 0)
    {
        sprintf(line, COPY_NAME " %s", err);
        return line;
    }
    nsec = dataFile.sectors;
    if (copyFile.sectors < nsec)
        return "DISKCOPY.DAT is smaller than DISKDATA.DAT.";
    if (nsec == 0)
        return "DISKDATA.DAT is empty.";
    return 0;
}

void appMain(void)
{
    const char *err;
    byte k;

    drive = DEFDRV;
    screenOn();
    fill(0, 0, 80, 24, AT(FG_WHITE, BG_DESK));
    err = setUp();
    if (err)
    {
        message("Can't test this disk", err, "Use the disk disktest came on.");
        screenOff();
        return;
    }
    drawScreen();
    lastTimer = TIMER;
    for (;;)
    {
        footer("BREAK Stop   C Colours");
        for (;;)
        {
            ++pass;
            if (!runPhase(PH_READ, &dataFile, 0)
                || !runPhase(PH_WRITE, &copyFile, (byte) pass)
                || !runPhase(PH_VERIFY, &copyFile, (byte) pass))
                break;
        }
        footer("Stopped.   ENTER Run again   BREAK Back to BASIC");
        while ((k = inkey()) != 13 && k != 3)
            ;
        if (k == 3)
            break;
    }
    screenOff();
}

word basicStack;

/* Runs appMain on our own stack: BASIC's is at the top of its RAM, in the
 * slot the screen is mapped into. */
void runOnOwnStack(void)
{
    asm
    {
        sts     _basicStack
        lds     #$6000
        jsr     _appMain
        lds     _basicStack
    }
}

int main()
{
    initCoCoSupport();
    if (!isCoCo3)
    {
        printf("SORRY, DISKTEST NEEDS A COCO 3.\n");
        return 0;
    }
    runOnOwnStack();
    cls(255);
    return 0;
}
