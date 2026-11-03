# disktest

A disk speed and torture test for the Tandy Color Computer 3. It reads, writes and verifies a
file over and over until you stop it, and shows the progress, error count and throughput on an
80-column colour screen.

All disk access goes through DSKCON, Disk BASIC's sector call. So it runs the same on a stock
floppy controller and under HDB-DOS (DriveWire, FujiNet and so on). It needs a CoCo 3 for the
80-column screen.

## Using it

`make` builds `build/disktest.dsk`, a 35-track single-sided disk:

| File | Size | What it is |
|------|------|------------|
| AUTOEXEC.BAS | 1 granule   | `LOADM"DISKTEST":EXEC` |
| DISKTEST.BIN | 5 granules  | the program |
| DISKDATA.DAT | 25 granules | 57,600 bytes of test pattern, only ever read |
| DISKCOPY.DAT | 25 granules | the same size; each pass rewrites it |

HDB-DOS runs `AUTOEXEC.BAS` at boot, the way the FujiNet config disk starts. Stock Disk BASIC
doesn't autostart, so type `RUN"AUTOEXEC"` (or `LOADM"DISKTEST":EXEC`). It tests whichever
drive BASIC is using (its default drive), which is normally the one it booted from.

Each pass has three phases:

1. **Read:** reads `DISKDATA.DAT` and checks every byte.
2. **Write:** writes `DISKCOPY.DAT` with a pattern used only by this pass.
3. **Verify:** reads `DISKCOPY.DAT` back and checks every byte.

| Key | Action |
|-----|--------|
| BREAK | Stop. Then ENTER starts a new pass, and BREAK again returns to BASIC. |
| C | Switch between RGB and composite colours. It starts in RGB. |

A read error, write error or wrong data adds to the error count and becomes the "Last error",
e.g. `P3 READ T27 S2 DATA` (pass 3, track 27, sector 2, wrong data) or `... ST $10`
(DSKCON status bits). The test keeps going after errors.

`make run` boots it in XRoar (CoCo 3, stock Disk BASIC).

## How it works

**What gets written:** the program writes only to sectors `DISKCOPY.DAT` already owns. It never
writes the directory or the FAT, so a crash mid-pass can't damage the disk's structure.

**The pattern:** both files hold a pattern the program can compute, so it needs no RAM for the
data. In file sector *s*, byte *i* is *a* + *i*·*b* (mod 256), with
*a* = *s*·$9D + seed·$35 and *b* = 2·(*s* XOR seed) + 1.

- Because *b* is odd, every sector holds all 256 byte values once, in an order of its own. A
  stuck data bit or a sector read from the wrong place both show up.
- `tools/mkdata.py` writes both files with seed 0. Pass *n* writes the copy with seed *n*, so a
  write that silently doesn't happen fails the verify.

**Speeds are relative:** they come from BASIC's 60 Hz `TIMER`, the only clock on a stock CoCo.
DSKCON turns interrupts off while it transfers a sector, and `TIMER` stops with them. On a
floppy the controller also halts the CPU while it waits for the sector to come round. So the
K/s figures and the elapsed time read high, perhaps three times high on a floppy. They're
good for comparing passes, drives and set-ups, not as absolute speeds. The bars rescale when a
rate passes the end of the scale.

**CPU speed:** it is left as BASIC had it. Disk BASIC's floppy timeouts assume 0.89 MHz.

## Layout

- `src/disktest.c`: the program (CMOC).
- `src/autoexec.bas`: the autostart line.
- `tools/mkdata.py`: writes the data file.

Memory while it runs:

| Address | Contents |
|---------|----------|
| $2601 | `AUTOEXEC.BAS`, still running while LOADM brings the program in |
| $2700–$5BFF | the program |
| $5C00–$5FFF | its stack |
| $6000–$6EFF | the screen: GIME 80×24 text with attributes, physical $6C000 |
