# t61input.vxd - see t61input.c. Same Open Watcom setup and fixlink as nvmini.vxd.
WATCOM ?= /home/guilherme/watcom
DRV = ../../../driver/vmdisp9x-nv
export WATCOM
export PATH := $(WATCOM)/binl64:$(WATCOM)/binl:$(PATH)
export INCLUDE := $(WATCOM)/h/win:$(WATCOM)/h

t61input.vxd: t61input.c $(DRV)/vmm.h
	wcc386 -q -wx -s -zls -mf -DVXD32 -ei -oeatxhn -6s -i=$(DRV) -fo=t61input.obj t61input.c
	wlink op quiet system win_vxd dynamic option nodefaultlibs option map=t61input.map \
	  name t61input.vxd file t61input.obj \
	  segment _TEXT PRELOAD NONDISCARDABLE segment _DATA PRELOAD NONDISCARDABLE \
	  segment _BSS PRELOAD NONDISCARDABLE export T61IN_DDB.1
	$(DRV)/fixlink.bin -vxd32 $@

clean:
	rm -f t61input.obj t61input.map t61input.vxd
