# disktest - disk speed and torture test for the Color Computer 3
#
#   make            build build/disktest.dsk (35 tracks, single sided)
#   make run        boot XRoar (CoCo 3) with it in drive 0
#   make clean

CMOC    ?= cmoc
DECB    ?= decb
XROAR   ?= xroar
PYTHON  ?= python3

BUILD   := build
BIN     := $(BUILD)/DISKTEST.BIN
DATA    := $(BUILD)/DISKDATA.DAT
DISK    := $(BUILD)/disktest.dsk

all: $(DISK)

$(BIN): src/disktest.c | $(BUILD)
	$(CMOC) -O2 --coco --org=2700 --limit=5C00 -i --intdir=$(BUILD) -o $@ $<

$(DATA): tools/mkdata.py | $(BUILD)
	$(PYTHON) tools/mkdata.py $@

# Both data files start out the same; AUTOEXEC.BAS starts the test under
# HDB-DOS (on stock Disk BASIC: RUN"AUTOEXEC").
$(DISK): $(BIN) $(DATA) src/autoexec.bas
	rm -f $@
	$(DECB) dskini $@
	$(DECB) copy -0 -t -l src/autoexec.bas $@,AUTOEXEC.BAS
	$(DECB) copy -2 -b $(BIN) $@,DISKTEST.BIN
	$(DECB) copy -1 -b $(DATA) $@,DISKDATA.DAT
	$(DECB) copy -1 -b $(DATA) $@,DISKCOPY.DAT
	$(DECB) free $@

$(BUILD):
	mkdir -p $@

run: $(DISK)
	$(XROAR) -default-machine coco3 -ram 512 -tv-input rgb -load-fd0 $(DISK) \
	    -type 'RGB:RUN"AUTOEXEC"\r'

clean:
	rm -rf $(BUILD)

.PHONY: all run clean
