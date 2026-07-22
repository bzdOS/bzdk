# Makefile — bzdOS microkernel (AArch64, Allwinner A64 / BPI-M64).
#
# Two products (see PROJECT.md):
#   microkernel-stage0.bin = start.o + main_stage0.o        (load/exec/return self-test, no USB)
#   microkernel.bin        = start.o + main.o + musb.o      (real Stage-1 firmware)
#
# musb.o is built from musb.c, which is owned by a different agent/lane and
# is NOT provided by this Makefile's lane. Until musb.c exists, `make` /
# `make all` / `make microkernel.bin` will fail at the final link with
# "musb.o: No such file" (or musb.c missing) — that is EXPECTED. `make stage0`
# does not depend on musb.c at all and must always build cleanly on its own.

CROSS   ?= aarch64-linux-gnu-
CC      := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
SIZE    := $(CROSS)size

# Flags per PROJECT.md "Сборка": freestanding, no libc, raw position-fixed
# binary linked at 0x42000000 (link.ld). -fno-pic/-fno-pie/-no-pie keep the
# link free of GOT/PLT/dynamic relocations so objcopy -O binary is valid.
CFLAGS  := -ffreestanding -nostdlib -mgeneral-regs-only -march=armv8-a \
           -fno-stack-protector -Wall -O2 -fno-pic -fno-pie -g
ASFLAGS := -march=armv8-a -g
LDFLAGS := -nostdlib -static -no-pie -Wl,--build-id=none -T link.ld

STAGE0_ELF := microkernel-stage0.elf
STAGE0_BIN := microkernel-stage0.bin
MAIN_ELF   := microkernel.elf
MAIN_BIN   := microkernel.bin
NET_ELF    := microkernel-net.elf
NET_BIN    := microkernel-net.bin
REPL_ELF   := microkernel-repl.elf
REPL_BIN   := microkernel-repl.bin
FBSD_ELF   := microkernel-fbsd.elf
FBSD_BIN   := microkernel-fbsd.bin
DBG_ELF    := microkernel-dbg.elf
DBG_BIN    := microkernel-dbg.bin
HDMI_ELF   := microkernel-hdmi.elf
HDMI_BIN   := microkernel-hdmi.bin

.PHONY: all stage0 net repl fbsd dbg hdmi clean test
all: $(STAGE0_BIN) $(MAIN_BIN)

# --- Hosted unit tests (T2 "Хостовые тесты", ROADMAP.md) ----------------
# Plain x86_64 gcc, NOT $(CC)/$(CROSS) — these build and run entirely on the
# dev host, no cross-compiler and no board. They mirror the pure ring-parsing
# logic of vblk_emmc.c and the pure table-building logic of stage2.c (see the
# block comments at the top of each test_*.c for exactly why they mirror
# rather than #include the real .c files: both are full of raw ARMv8 inline
# asm — cache maintenance, exclusive-monitor spinlocks, system-register
# access — that plain gcc cannot assemble for x86_64). Fast enough to run on
# every commit; does not touch vblk_emmc.c/stage2.c themselves.
test: test_vblk_ring test_stage2_tables
	./test_vblk_ring
	./test_stage2_tables

test_vblk_ring: test_vblk_ring.c
	gcc -Wall -Wextra -O2 -o $@ $<

test_stage2_tables: test_stage2_tables.c
	gcc -Wall -Wextra -O2 -o $@ $<

stage0: $(STAGE0_BIN)

net: $(NET_BIN)

repl: $(REPL_BIN)

# --- Stage-0 (no USB): proves load/exec/return chain ---
$(STAGE0_ELF): start.o main_stage0.o smp.o timer.o libmin.o link.ld
	$(CC) $(LDFLAGS) -o $@ start.o main_stage0.o smp.o timer.o libmin.o
	$(SIZE) $@

$(STAGE0_BIN): $(STAGE0_ELF)
	$(OBJCOPY) -O binary $< $@

# --- Real Stage-1 firmware: needs musb.o (musb.c owned by another lane) ---
$(MAIN_ELF): start.o main.o musb.o wdt.o smp.o timer.o libmin.o link.ld
	$(CC) $(LDFLAGS) -o $@ start.o main.o musb.o wdt.o smp.o timer.o libmin.o
	$(SIZE) $@

$(MAIN_BIN): $(MAIN_ELF)
	$(OBJCOPY) -O binary $< $@

# --- Ethernet (sun8i-emac) console test firmware: independent of musb.o ---
# start.o + main_net.o + emac.o + wdt.o, linked at 0x42000000 via link.ld.
$(NET_ELF): start.o main_net.o emac.o wdt.o smp.o timer.o libmin.o link.ld
	$(CC) $(LDFLAGS) -o $@ start.o main_net.o emac.o wdt.o smp.o timer.o libmin.o
	$(SIZE) $@

$(NET_BIN): $(NET_ELF)
	$(OBJCOPY) -O binary $< $@

# --- Resident interactive REPL firmware ---------------------------------
# EMAC is the REPL's own console channel; MUSB is the device-under-test the
# REPL pokes at (mi/mp/mpN). start.o + main_repl.o + repl.o + emac.o + musb.o
# + wdt.o, linked at 0x42000000 via link.ld.
REPL_OBJS := start.o main_repl.o repl.o emac.o musb.o wdt.o exceptions.o el2_exc.o \
             timer.o ring.o alloc.o gic_timer.o netcon.o sched.o guest.o libmin.o stage2.o kload.o vconsole.o gtrace.o reboot.o hdmi.o fb.o hud.o smp.o hwbp.o backtrace.o ktimer.o ksync.o wcet.o firstfault.o onebp.o flightrec.o
$(REPL_ELF): $(REPL_OBJS) link.ld
	$(CC) $(LDFLAGS) -o $@ $(REPL_OBJS)
	$(SIZE) $@

$(REPL_BIN): $(REPL_ELF)
	$(OBJCOPY) -O binary $< $@

hdmi: $(HDMI_BIN)

HDMI_OBJS := start.o main_hdmi.o hdmi.o fb.o timer.o wdt.o libmin.o reboot.o smp.o
$(HDMI_ELF): $(HDMI_OBJS) link.ld
	$(CC) $(LDFLAGS) -o $@ $(HDMI_OBJS)
	$(SIZE) $@
$(HDMI_BIN): $(HDMI_ELF)
	$(OBJCOPY) -O binary $< $@

dbg: $(DBG_BIN)

DBG_OBJS := start.o main_dbg.o exceptions.o el2_exc.o kload.o stage2.o guest.o \
            gic_timer.o sched.o timer.o wdt.o libmin.o vconsole.o gtrace.o \
            emac.o dbgmon.o bmc.o reboot.o hwbp.o backtrace.o smp.o firstfault.o onebp.o vgic.o \
            musb.o usbacm.o emmc_bio.o vblk_emmc.o el2_ncmap.o snapshot.o flightrec.o coredump.o
$(DBG_ELF): $(DBG_OBJS) link.ld
	$(CC) $(LDFLAGS) -o $@ $(DBG_OBJS)
	$(SIZE) $@
$(DBG_BIN): $(DBG_ELF)
	$(OBJCOPY) -O binary $< $@

fbsd: $(FBSD_BIN)

FBSD_OBJS := start.o main_fbsd.o exceptions.o el2_exc.o kload.o stage2.o guest.o \
             gic_timer.o sched.o timer.o wdt.o libmin.o vconsole.o gtrace.o reboot.o smp.o hwbp.o backtrace.o firstfault.o onebp.o
$(FBSD_ELF): $(FBSD_OBJS) link.ld
	$(CC) $(LDFLAGS) -o $@ $(FBSD_OBJS)
	$(SIZE) $@
$(FBSD_BIN): $(FBSD_ELF)
	$(OBJCOPY) -O binary $< $@

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

%.o: %.S
	$(CC) $(ASFLAGS) -c -o $@ $<

clean:
	rm -f start.o main.o main_stage0.o main_net.o main_repl.o repl.o \
	      musb.o emac.o wdt.o exceptions.o el2_exc.o timer.o ring.o alloc.o gic_timer.o netcon.o sched.o guest.o libmin.o stage2.o kload.o vconsole.o gtrace.o reboot.o hdmi.o fb.o hud.o \
	      snapshot.o \
	      $(STAGE0_ELF) $(STAGE0_BIN) $(MAIN_ELF) $(MAIN_BIN) \
	      $(NET_ELF) $(NET_BIN) $(REPL_ELF) $(REPL_BIN) \
	      test_vblk_ring test_stage2_tables
