# Makefile — bzdOS microkernel (AArch64, Allwinner A64 / BPI-M64).
#
# Products (see README.md — `dbg` is the current milestone build):
#   microkernel-dbg.bin  = the live-debug EL2 hypervisor (main_dbg.c) — what
#                          reliable_load.py flashes to the board; `make dbg`.
#   microkernel-qemu.elf = portable QEMU-virt CI target; `make qemu`.
# The stage0/net/repl/fbsd/hdmi/zephyr targets build earlier, narrower
# milestones from the same tree and are kept for reference/bisection.

CROSS   ?= aarch64-linux-gnu-
CC      := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
SIZE    := $(CROSS)size

# Flags per PROJECT.md "Сборка": freestanding, no libc, raw position-fixed
# binary linked at 0x42000000 (link.ld). -fno-pic/-fno-pie/-no-pie keep the
# link free of GOT/PLT/dynamic relocations so objcopy -O binary is valid.
CFLAGS  := -ffreestanding -nostdlib -mgeneral-regs-only -march=armv8-a \
           -fno-stack-protector -Wall -O2 -fno-pic -fno-pie -g $(EXTRA_CFLAGS)
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
GDB_ELF    := microkernel-gdb.elf
GDB_BIN    := microkernel-gdb.bin
HDMI_ELF   := microkernel-hdmi.elf
HDMI_BIN   := microkernel-hdmi.bin
ZEPHYR_ELF := microkernel-zephyr.elf
ZEPHYR_BIN := microkernel-zephyr.bin

.PHONY: all stage0 net repl fbsd dbg gdb hdmi zephyr qemu clean clean-qemu test
all: $(STAGE0_BIN) $(MAIN_BIN)

# --- Hosted unit tests (T2 "Хостовые тесты", ROADMAP.md) ----------------
# Plain x86_64 gcc, NOT $(CC)/$(CROSS) — these build and run entirely on the
# dev host, no cross-compiler and no board. They mirror the pure ring-parsing
# logic of vblk_emmc.c/vnet_emac.c, the pure table-building logic of
# stage2.c, the pure ELF/modinfo-tag logic of kload.c, and the pure
# register-decode/IIR-LSR-USR emulation logic of vconsole.c (see the block
# comments at the top of each test_*.c for exactly why they mirror rather
# than #include the real .c files: all are full of raw ARMv8 inline asm —
# cache maintenance, exclusive-monitor spinlocks, system-register access —
# that plain gcc cannot assemble for x86_64). Fast enough to run on every
# commit; does not touch vblk_emmc.c/stage2.c/vnet_emac.c/kload.c/vconsole.c
# themselves.
test: test_vblk_ring test_stage2_tables test_vnet_ring test_kload_modinfo test_vconsole_uart
	./test_vblk_ring
	./test_stage2_tables
	./test_vnet_ring
	./test_kload_modinfo
	./test_vconsole_uart

test_vblk_ring: test_vblk_ring.c
	gcc -Wall -Wextra -O2 -o $@ $<

test_stage2_tables: test_stage2_tables.c
	gcc -Wall -Wextra -O2 -o $@ $<

test_vnet_ring: test_vnet_ring.c
	gcc -Wall -Wextra -O2 -o $@ $<

test_kload_modinfo: test_kload_modinfo.c
	gcc -Wall -Wextra -O2 -o $@ $<

test_vconsole_uart: test_vconsole_uart.c
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
            musb.o usbacm.o emmc_bio.o sd_bio.o vblk_emmc.o vblk_async.o vnet_emac.o el2_ncmap.o snapshot.o flightrec.o coredump.o \
            netcon.o snapshot_net.o rsb.o axp803.o hdmi.o fb.o hud.o \
            gdbstub.o gdbstub_hw.o
$(DBG_ELF): $(DBG_OBJS) link.ld
	$(CC) $(LDFLAGS) -o $@ $(DBG_OBJS)
	$(SIZE) $@
$(DBG_BIN): $(DBG_ELF)
	$(OBJCOPY) -O binary $< $@

# --- GDB-stub build: same skeleton as `dbg`, but the tick-path debugger is
# gdbstub.o + gdbstub_hw.o instead of dbgmon.o (see main_gdb.c / gdbstub.c's
# header comment). NEW target — does not touch/replace `dbg`. ---
gdb: $(GDB_BIN)

GDB_OBJS := start.o main_gdb.o exceptions.o el2_exc.o kload.o stage2.o guest.o \
            gic_timer.o sched.o timer.o wdt.o libmin.o vconsole.o gtrace.o \
            emac.o gdbstub.o gdbstub_hw.o reboot.o hwbp.o backtrace.o smp.o firstfault.o onebp.o vgic.o \
            musb.o usbacm.o emmc_bio.o vblk_emmc.o el2_ncmap.o flightrec.o coredump.o
$(GDB_ELF): $(GDB_OBJS) link.ld
	$(CC) $(LDFLAGS) -o $@ $(GDB_OBJS)
	$(SIZE) $@
$(GDB_BIN): $(GDB_ELF)
	$(OBJCOPY) -O binary $< $@

fbsd: $(FBSD_BIN)

FBSD_OBJS := start.o main_fbsd.o exceptions.o el2_exc.o kload.o stage2.o guest.o \
             gic_timer.o sched.o timer.o wdt.o libmin.o vconsole.o gtrace.o reboot.o smp.o hwbp.o backtrace.o firstfault.o onebp.o
$(FBSD_ELF): $(FBSD_OBJS) link.ld
	$(CC) $(LDFLAGS) -o $@ $(FBSD_OBJS)
	$(SIZE) $@
$(FBSD_BIN): $(FBSD_ELF)
	$(OBJCOPY) -O binary $< $@

# --- Zephyr guest-boot build: same skeleton as `fbsd` (see main_zephyr.c's
# header comment for exactly what differs and why). NEW target — does not
# touch/replace `fbsd`. The Zephyr RTOS image itself (zephyr.elf, built out
# of tree from zephyr-guest/) is NOT part of this object list and is not
# linked into this binary — it is a separate artifact this firmware loads
# and jumps into at runtime, exactly like main_fbsd.c does for the FreeBSD
# kernel ELF. ---
zephyr: $(ZEPHYR_BIN)

ZEPHYR_OBJS := start.o main_zephyr.o exceptions.o el2_exc.o kload.o stage2.o guest.o \
               gic_timer.o sched.o timer.o wdt.o libmin.o vconsole.o gtrace.o reboot.o smp.o hwbp.o backtrace.o firstfault.o onebp.o
$(ZEPHYR_ELF): $(ZEPHYR_OBJS) link.ld
	$(CC) $(LDFLAGS) -o $@ $(ZEPHYR_OBJS)
	$(SIZE) $@
$(ZEPHYR_BIN): $(ZEPHYR_ELF)
	$(OBJCOPY) -O binary $< $@

# --- QEMU `virt`-machine CI target (ROADMAP.md T3) ----------------------
# A second, portable target that needs no physical board: proves the CORE
# hypervisor logic (stage-2 identity mapping, guest EL1 entry, GICv2
# timer/IRQ handling, a serial console) under `qemu-system-aarch64 -M virt`.
# See docs/qemu-ci.md for the full invocation + what a passing/failing run
# looks like.
#
# NEW, SEPARATE object list/target/linker script — does NOT reuse or alter
# link.ld, DBG_OBJS/GDB_OBJS/REPL_OBJS/FBSD_OBJS, or any of their targets.
# Reuses stage2.c/.h, guest.c/.h, exceptions.S/.h and timer.c/.h from the
# real-board build completely UNCHANGED (see main_qemu.c's banner for why
# that's possible); only the board-specific bits (entry/stack setup, GIC
# base addresses, the UART, and a minimal EL2 trap dispatch + EL1 payload
# in place of the full board debugger) are QEMU-specific NEW files.
#
# NOTE: this target intentionally does NOT use $(CFLAGS)/$(LDFLAGS)/link.ld
# above (different link address/layout — see link_qemu.ld) but DOES reuse
# the same freestanding compiler flags, just with its own linker script.
QEMU_ELF  := microkernel-qemu.elf
QEMU_OBJS := start_qemu.o main_qemu.o exceptions.o guest.o stage2.o timer.o \
             gic_timer_qemu.o pl011_qemu.o el2_exc_qemu.o guest_qemu_payload.o libmin.o
LDFLAGS_QEMU := -nostdlib -static -no-pie -Wl,--build-id=none -T link_qemu.ld

qemu: $(QEMU_ELF)

$(QEMU_ELF): $(QEMU_OBJS) link_qemu.ld
	$(CC) $(LDFLAGS_QEMU) -o $@ $(QEMU_OBJS)
	$(SIZE) $@

clean-qemu:
	rm -f start_qemu.o main_qemu.o gic_timer_qemu.o pl011_qemu.o el2_exc_qemu.o guest_qemu_payload.o \
	      $(QEMU_ELF)

# Header dependency tracking: -MMD -MP emits a .d per object listing the
# headers it includes, so editing e.g. vblk_emmc.h rebuilds vblk_emmc.o. This
# binary is flashed to live hardware — a stale object silently wrong against
# its header is the most dangerous build failure mode, so track deps.
%.o: %.c
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

%.o: %.S
	$(CC) $(ASFLAGS) -MMD -MP -c -o $@ $<

-include $(wildcard *.d)

# Remove every build artifact this Makefile can produce (all targets), the
# per-object .d dependency files, and the hosted test binaries. A blanket
# *.o/*.d avoids the old hand-maintained list silently going stale as files
# are added (dbgmon.o, emmc_bio.o, vblk_*.o, … were all missing before).
clean: clean-qemu
	rm -f *.o *.d \
	      $(STAGE0_ELF) $(STAGE0_BIN) $(MAIN_ELF) $(MAIN_BIN) \
	      $(NET_ELF) $(NET_BIN) $(REPL_ELF) $(REPL_BIN) \
	      $(FBSD_ELF) $(FBSD_BIN) $(DBG_ELF) $(DBG_BIN) \
	      $(GDB_ELF) $(GDB_BIN) $(HDMI_ELF) $(HDMI_BIN) \
	      $(ZEPHYR_ELF) $(ZEPHYR_BIN) \
	      test_vblk_ring test_stage2_tables test_vnet_ring test_kload_modinfo test_vconsole_uart
