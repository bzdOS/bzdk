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

# Build identifier (dbgtools.c / hv_addrmap.h HVMAP_DBGTOOLS_BUILDID, added
# 2026-07-26): a git commit hash if this checkout is a git repo, plus a
# trailing '+' if the tree is dirty (uncommitted changes on top of that
# commit) -- falls back to a UTC date-time stamp if `git` isn't available or
# this isn't a git checkout, so the build never fails for lack of it. Written
# once at HV startup to a fixed DRAM address; host tooling (hvdbg.py) runs
# the SAME `git rev-parse` from the SAME directory to compute the expected
# value and compares before trusting any nm-resolved symbol address against
# the live board -- see dbgtools.h's header comment for the bug this fixes
# (an `nm`-resolved reboot_clean() address silently misfired against an
# OLDER build still running on the board).
BUILD_ID := $(strip $(shell git rev-parse --short=12 HEAD 2>/dev/null)$(shell git diff --quiet 2>/dev/null || echo +))
ifeq ($(BUILD_ID),)
BUILD_ID := $(shell date -u +%Y%m%d%H%M%S)
endif

# Flags per PROJECT.md "Сборка": freestanding, no libc, raw position-fixed
# binary linked at 0x42000000 (link.ld). -fno-pic/-fno-pie/-no-pie keep the
# link free of GOT/PLT/dynamic relocations so objcopy -O binary is valid.
CFLAGS  := -ffreestanding -nostdlib -mgeneral-regs-only -march=armv8-a \
           -fno-stack-protector -Wall -O2 -fno-pic -fno-pie -g \
           -DBZDOS_BUILD_ID='"$(BUILD_ID)"' $(EXTRA_CFLAGS)
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

.PHONY: all stage0 net repl fbsd dbg gdb hdmi zephyr zephyr-qemu linux-qemu vgic-qemu qemu snapshot-qemu holdtest clean clean-qemu clean-linux-qemu clean-holdtest test
all: $(STAGE0_BIN) $(MAIN_BIN)

# --- Hosted unit tests (T2 "Хостовые тесты", ROADMAP.md) ----------------
# Plain x86_64 gcc, NOT $(CC)/$(CROSS) — these build and run entirely on the
# dev host, no cross-compiler and no board. They mirror the pure ring-parsing
# logic of vblk_emmc.c/vnet_emac.c, the pure table-building logic of
# stage2.c, the pure ELF/modinfo-tag logic of kload.c, the pure
# register-decode/IIR-LSR-USR emulation logic of vconsole.c, the pure
# VA-resolution logic of gdbstub.c's resolve()/kload_va_to_pa(), and the pure
# cross-core request/poll state machine of gdbstub_hw.c's hwop_run() (see the
# block comments at the top of each test_*.c for exactly why they mirror
# rather than #include the real .c files: all are full of raw ARMv8 inline
# asm — cache maintenance, exclusive-monitor spinlocks, system-register
# access, debug-register banking — that plain gcc cannot assemble for
# x86_64). Fast enough to run on every commit; does not touch
# vblk_emmc.c/stage2.c/vnet_emac.c/kload.c/vconsole.c/gdbstub.c/gdbstub_hw.c
# themselves.
#
# test_automount.py is the exception to that mirror-don't-include rule and the
# stronger kind of test: it drives the REAL reliable_load.auto_mount_root()
# over a pty, so it CAN catch a transcription error. No board required.
# ── toolchain check (ROADMAP T5) ───────────────────────────────────────────
# TOOLCHAIN.md documents the verified-working versions but nothing checked them,
# and it said so itself: "The Makefile does not pin or version-check the
# toolchain". This target closes that.
#
# It deliberately FAILS only when a tool is MISSING, and merely warns on a
# version that differs from the verified one. Hard-failing on a version
# mismatch would lock the project to one Fedora release for no measured reason
# -- TOOLCHAIN.md's own stance is "check here first before assuming the source
# regressed", which is advice, not a constraint. A warning delivers that advice
# at the moment it is useful; an error would just make other distros patch the
# Makefile out.
TC_GCC_WANT := 15.2.1
TC_LD_WANT  := 2.45

.PHONY: toolchain-check
toolchain-check:
	@missing=0; \
	for t in gcc ld objcopy size nm; do \
	    command -v $(CROSS)$$t >/dev/null 2>&1 || { \
	        echo "toolchain: MISSING $(CROSS)$$t"; missing=1; }; \
	done; \
	command -v gcc >/dev/null 2>&1 || { \
	    echo "toolchain: MISSING host gcc (needed for the host-only tests)"; \
	    missing=1; }; \
	[ $$missing -eq 0 ] || { \
	    echo "toolchain: FAIL -- see TOOLCHAIN.md for the install command"; \
	    exit 1; }; \
	g=$$($(CROSS)gcc -dumpfullversion 2>/dev/null \
	     || $(CROSS)gcc -dumpversion 2>/dev/null); \
	l=$$($(CROSS)ld --version 2>/dev/null | sed -n '1s/.* //p'); \
	echo "toolchain: $(CROSS)gcc $$g (verified $(TC_GCC_WANT)), $(CROSS)ld $$l (verified $(TC_LD_WANT))"; \
	case "$$g" in $(TC_GCC_WANT)*) ;; *) \
	    echo "toolchain: WARNING gcc $$g is not the verified $(TC_GCC_WANT) -- if the build breaks, suspect this first (TOOLCHAIN.md)";; \
	esac; \
	case "$$l" in $(TC_LD_WANT)*) ;; *) \
	    echo "toolchain: WARNING ld $$l is not the verified $(TC_LD_WANT) -- likewise";; \
	esac; \
	echo "toolchain: OK"

test: toolchain-check test_vblk_ring test_vblk_stitch test_stage2_tables test_vnet_ring test_kload_modinfo test_vconsole_uart test_gdbstub_resolve test_gdbstub_hwop test_vgic_pendq test_sd_bio_addr test_snapshot_fmt test_bmc_arm_gate test_coredump_elf
	./test_vblk_ring
	./test_vblk_stitch
	./test_stage2_tables
	./test_vnet_ring
	./test_kload_modinfo
	./test_vconsole_uart
	./test_gdbstub_resolve
	./test_gdbstub_hwop
	./test_vgic_pendq
	./test_sd_bio_addr
	./test_snapshot_fmt
	./test_bmc_arm_gate
	./test_coredump_elf
	python3 test_automount.py
	python3 coredump-recv.py selftest
	python3 snapshot_net.py selftest
	python3 bmc_client.py selftest
	python3 bzdctl.py selftest
	python3 crash_report.py selftest

test_vblk_stitch: test_vblk_stitch.c
	gcc -Wall -Wextra -O2 -o $@ $<

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

test_gdbstub_resolve: test_gdbstub_resolve.c
	gcc -Wall -Wextra -O2 -o $@ $<

test_gdbstub_hwop: test_gdbstub_hwop.c
	gcc -Wall -Wextra -O2 -o $@ $<

test_sd_bio_addr: test_sd_bio_addr.c
	gcc -Wall -Wextra -O2 -o $@ $<

test_snapshot_fmt: test_snapshot_fmt.c
	gcc -Wall -Wextra -O2 -o $@ $<

test_bmc_arm_gate: test_bmc_arm_gate.c
	gcc -Wall -Wextra -O2 -o $@ $<

test_vgic_pendq: test_vgic_pendq.c
	gcc -Wall -Wextra -O2 -o $@ $<

test_coredump_elf: test_coredump_elf.c
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
             timer.o ring.o alloc.o gic_timer.o netcon.o sched.o guest.o libmin.o stage2.o kload.o vconsole.o gtrace.o reboot.o hdmi.o fb.o hud.o smp.o hwbp.o backtrace.o ksym.o ktimer.o ksync.o wcet.o firstfault.o onebp.o flightrec.o vgic.o usbacm.o rsb.o
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
            emac.o dbgmon.o bmc.o reboot.o hwbp.o backtrace.o ksym.o smp.o firstfault.o onebp.o vgic.o \
            musb.o usbacm.o emmc_bio.o sd_bio.o vblk_emmc.o vblk_async.o vnet_emac.o el2_ncmap.o snapshot.o flightrec.o coredump.o \
            netcon.o snapshot_net.o rsb.o axp803.o hdmi.o fb.o hud.o \
            gdbstub.o gdbstub_hw.o hmac_sha256.o dbgtools.o
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
            emac.o gdbstub.o gdbstub_hw.o reboot.o hwbp.o backtrace.o ksym.o smp.o firstfault.o onebp.o vgic.o \
            musb.o usbacm.o emmc_bio.o vblk_emmc.o el2_ncmap.o flightrec.o coredump.o dbgtools.o
$(GDB_ELF): $(GDB_OBJS) link.ld
	$(CC) $(LDFLAGS) -o $@ $(GDB_OBJS)
	$(SIZE) $@
$(GDB_BIN): $(GDB_ELF)
	$(OBJCOPY) -O binary $< $@

fbsd: $(FBSD_BIN)

FBSD_OBJS := start.o main_fbsd.o exceptions.o el2_exc.o kload.o stage2.o guest.o \
             gic_timer.o sched.o timer.o wdt.o libmin.o vconsole.o gtrace.o reboot.o smp.o hwbp.o backtrace.o ksym.o firstfault.o onebp.o flightrec.o vgic.o musb.o usbacm.o emac.o
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
               gic_timer.o sched.o timer.o wdt.o libmin.o vconsole.o gtrace.o reboot.o smp.o hwbp.o backtrace.o ksym.o firstfault.o onebp.o flightrec.o vgic.o musb.o usbacm.o emac.o
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

# --- Snapshot/restore exercise on QEMU virt (ROADMAP D1: "freeze the guest,
# dump RAM, restore it") — see main_snapshot_qemu.c / el2_exc_snapshot_qemu.c
# for the full tick schedule and what the printed verdict means.
#
# Object list = the `qemu` target's skeleton (start_qemu.o, exceptions.o,
# guest.o, stage2.o, timer.o, gic_timer_qemu.o, pl011_qemu.o, libmin.o) with
# el2_exc_snapshot_qemu.o in place of el2_exc_qemu.o and
# guest_snapshot_payload.o in place of guest_qemu_payload.o, PLUS:
#   snapshot.o        -- the REAL, unmodified snapshot_save()/
#                         snapshot_restore()/snapshot_present() this exercise
#                         is actually testing.
#   wdt_qemu_stub.o    -- inert wdt_pet() (see that file's header): the real
#                         wdt.o pokes A64-only MMIO that doesn't exist under
#                         QEMU virt, and snapshot.c calls wdt_pet()
#                         unconditionally during its DRAM copy.
# snapshot_net.o is deliberately NOT linked here: this exercise drives the
# LOCAL DRAM-store path (snapshot_save/snapshot_restore) directly, not the
# EMAC bulk transport (which needs emac.o and a real NIC this target has
# none of).
SNAPSHOT_QEMU_ELF  := microkernel-snapshot-qemu.elf
SNAPSHOT_QEMU_OBJS := start_qemu.o main_snapshot_qemu.o exceptions.o guest.o stage2.o timer.o \
             gic_timer_qemu.o pl011_qemu.o el2_exc_snapshot_qemu.o guest_snapshot_payload.o \
             snapshot.o wdt_qemu_stub.o libmin.o

snapshot-qemu: $(SNAPSHOT_QEMU_ELF)

$(SNAPSHOT_QEMU_ELF): $(SNAPSHOT_QEMU_OBJS) link_qemu.ld
	$(CC) $(LDFLAGS_QEMU) -o $@ $(SNAPSHOT_QEMU_OBJS)
	$(SIZE) $@

clean-qemu:
	rm -f start_qemu.o main_qemu.o gic_timer_qemu.o pl011_qemu.o el2_exc_qemu.o guest_qemu_payload.o \
	      $(QEMU_ELF) main_zephyr_qemu.o el2_exc_zephyr_qemu.o $(ZEPHYR_QEMU_ELF) \
	      main_vgic_qemu.o el2_exc_vgic_qemu.o vgic_qemu.o vgic_qemu.d $(VGIC_QEMU_ELF) \
	      main_snapshot_qemu.o el2_exc_snapshot_qemu.o guest_snapshot_payload.o \
	      wdt_qemu_stub.o $(SNAPSHOT_QEMU_ELF)

# --- Zephyr guest on QEMU virt: the board-free half of the `zephyr` target
# (see main_zephyr_qemu.c's banner for the full argument). Runs the SAME
# guest-loading sequence `make zephyr` runs on real hardware -- kload_parse_elf
# -> kload_place_segments -> guest_config -> stage2 -> kload_enter -- on a
# target `zephyr-qemu-ci.sh` can execute with no board attached, with the
# guest's console served by the real vconsole.c 16550 trap-emulator.
#
# Object list = the `qemu` target's board-free skeleton (start_qemu.o entry,
# exceptions.o vectors, pl011_qemu.o HV console, stage2.o, guest.o, libmin.o),
# MINUS the timer/demo-payload pieces this target does not use
# (gic_timer_qemu.o, timer.o, guest_qemu_payload.o -- no tick is armed, see
# main_zephyr_qemu.c), PLUS the three objects that make it a real guest boot
# rather than a hand-written payload:
#   kload.o      -- ELF parse/place, UNCHANGED from the board build
#   vconsole.o   -- the 16550 trap-emulator, UNCHANGED from the board build
#   wdt.o        -- only for wdt_note_progress(), which vconsole.o calls on
#                   every guest THR write; it just timestamps CNTPCT into a
#                   variable. wdt_pet()/wdt_init(), the parts that poke the
#                   A64 WDOG at 0x01c20cb0 (nonexistent under QEMU), are
#                   never called from this target.
#   flightrec.o  -- likewise pulled in by vconsole.o (one flightrec_log() per
#                   console byte); writes only DRAM at 0x50012000 and needs
#                   no init, so it is portable as-is.
# and el2_exc_zephyr_qemu.o in place of el2_exc_qemu.o.
ZEPHYR_QEMU_ELF  := microkernel-zephyr-qemu.elf
ZEPHYR_QEMU_OBJS := start_qemu.o main_zephyr_qemu.o exceptions.o guest.o stage2.o \
                    kload.o vconsole.o wdt.o flightrec.o \
                    el2_exc_zephyr_qemu.o pl011_qemu.o libmin.o

zephyr-qemu: $(ZEPHYR_QEMU_ELF)

$(ZEPHYR_QEMU_ELF): $(ZEPHYR_QEMU_OBJS) link_qemu.ld
	$(CC) $(LDFLAGS_QEMU) -o $@ $(ZEPHYR_QEMU_OBJS)
	$(SIZE) $@

# --- Linux/arm64 guest on QEMU virt: ROADMAP D2, first pass (see
# main_linux_qemu.c's banner for the full argument, and linux-qemu-ci.sh).
#
# Runs the SAME board-free skeleton zephyr-qemu uses -- start_qemu.o entry,
# exceptions.o vectors, pl011_qemu.o HV console, stage2.o, guest.o, kload.o
# (for kload_enter() ONLY -- this guest's Image format is not an ELF, so
# kload_parse_elf()/kload_place_segments() are linked in but never called;
# see main_linux_qemu.c), vconsole.o + wdt.o + flightrec.o (the real 16550
# trap-emulator, unmodified, same as the board/Zephyr targets) -- with
# el2_exc_linux_qemu.o in place of el2_exc_zephyr_qemu.o (PSCI SMC
# passthrough is the one thing this target's trap dispatch needs that
# Zephyr's never did; see that file's header).
#
# Object list is therefore IDENTICAL to ZEPHYR_QEMU_OBJS except for the two
# guest-specific files (main_*.o, el2_exc_*.o) -- deliberately, since the
# whole point of this pass is that the guest-loading INFRASTRUCTURE (stage-2,
# console trap-emulation, EL2->EL1 handoff) is guest-OS-agnostic and Linux is
# the second, independent proof of that, not a reason to invent a new one.
LINUX_QEMU_ELF  := microkernel-linux-qemu.elf
LINUX_QEMU_OBJS := start_qemu.o main_linux_qemu.o exceptions.o guest.o stage2.o \
                   kload.o vconsole.o wdt.o flightrec.o \
                   el2_exc_linux_qemu.o pl011_qemu.o libmin.o

linux-qemu: $(LINUX_QEMU_ELF)

$(LINUX_QEMU_ELF): $(LINUX_QEMU_OBJS) link_qemu.ld
	$(CC) $(LDFLAGS_QEMU) -o $@ $(LINUX_QEMU_OBJS)
	$(SIZE) $@

clean-linux-qemu:
	rm -f main_linux_qemu.o main_linux_qemu.d el2_exc_linux_qemu.o el2_exc_linux_qemu.d $(LINUX_QEMU_ELF)

# --- vGIC on QEMU virt: the board-free gate for INTERRUPT VIRTUALIZATION
# (see main_vgic_qemu.c's banner for the full argument, and vgic-qemu-ci.sh).
#
# Runs vgic.c's own bare-metal self-test guest — which nothing had ever run,
# on the board or off it — against QEMU virt's real GICv2 virtualization
# extensions (GICH 0x08030000 / GICV 0x08040000, cited from the machine's own
# devicetree in vgic.h). This is the ONLY board-free check that touches GIC
# virtualization at all: qemu-ci.sh proves only EL2's own physical GIC use,
# and zephyr-qemu-ci.sh's guest GIC accesses go to a black hole by
# construction (its banner says so).
#
# vgic_qemu.o is the REAL vgic.c — same source file the board targets link —
# recompiled with ONLY the four GIC base addresses overridden. Nothing about
# vgic.c or the board builds changes; vgic.h's defaults are unchanged and are
# what every other target still gets (the #ifndef guards there are the entire
# diff). Note GICC is deliberately pointed at GICV: this target has no
# stage-2 GICC->GICV redirect, so the self-test payload reaches the virtual
# interface directly and the redirect itself stays hardware-only.
#
# Object list = the `qemu` target's skeleton (start_qemu.o, exceptions.o,
# guest.o, stage2.o, timer.o, gic_timer_qemu.o, pl011_qemu.o, libmin.o) with
# el2_exc_vgic_qemu.o in place of el2_exc_qemu.o, no guest_qemu_payload.o (the
# EL1 payload lives inside vgic.c), PLUS vgic_qemu.o and flightrec.o (pulled
# in by vgic.c's flightrec_log() on each injected LR; writes only DRAM at
# 0x50012000 and needs no init, so it is portable as-is — same reason the
# zephyr-qemu target links it).
VGIC_QEMU_ELF  := microkernel-vgic-qemu.elf
# VGST_BC_BASE override: vgic.c's default self-test breadcrumb window
# (0x50001d00) is written BY THE GUEST but sits inside the hv-scratch window
# stage2.c deliberately leaves INVALID in the guest's stage-2 map, so the
# payload faults on its first store (see the long comment at vgic.c's
# VGST_BC_BASE for the measured ESR/FAR). 0x50200000 is the first 2 MiB-aligned
# DRAM address that satisfies all three constraints simultaneously: inside
# stage2.h's identity-mapped DRAM range (0x40000000..0x80000000), OUTSIDE
# hv-image (0x42000000 + 2 MiB), and OUTSIDE hv-scratch (0x50000000 + 2 MiB).
# It is not taken on trust: main_vgic_qemu.c/el2_exc_vgic_qemu.c assert the
# guest actually wrote the "VGST" magic and CurrentEL==1 there, so a bad or
# colliding window fails the run loudly instead of passing quietly.
VGIC_QEMU_DEFS := -DVGIC_GICD_BASE=0x08000000UL -DVGIC_GICC_BASE=0x08040000UL \
                  -DVGIC_GICH_BASE=0x08030000UL -DVGIC_GICV_BASE=0x08040000UL \
                  -DVGST_BC_BASE=0x50200000UL
VGIC_QEMU_OBJS := start_qemu.o main_vgic_qemu.o exceptions.o guest.o stage2.o \
                  timer.o gic_timer_qemu.o el2_exc_vgic_qemu.o vgic_qemu.o \
                  flightrec.o pl011_qemu.o libmin.o

vgic-qemu: $(VGIC_QEMU_ELF)

# Explicit rule (not the generic %.o: %.c) so the QEMU GIC bases apply to THIS
# object only: the board's own vgic.o must keep compiling with vgic.h's
# unchanged A64 defaults. -MMD -MP writes vgic_qemu.d, so editing vgic.h/vgic.c
# rebuilds it, same header-staleness protection every other object gets.
vgic_qemu.o: vgic.c
	$(CC) $(CFLAGS) $(VGIC_QEMU_DEFS) -MMD -MP -c -o $@ $<

$(VGIC_QEMU_ELF): $(VGIC_QEMU_OBJS) link_qemu.ld
	$(CC) $(LDFLAGS_QEMU) -o $@ $(VGIC_QEMU_OBJS)
	$(SIZE) $@

# --- holdtest: throwaway QEMU diagnostic for the "pause guest before its
# first instruction + software breakpoint" board-hang investigation
# (2026-07-26/27 session; see main_holdtest_qemu.c's file banner for the
# full rationale and memory dbgtools-infra-added / hold-gate-plus-
# breakpoint-crashes-board). Tests ONE narrow hypothesis in isolation: does
# arming MDCR_EL2.TDE + patching a guest software BRK BEFORE the guest's
# first-ever instruction behave differently from doing the exact same thing
# AFTER the guest has already been running for a while.
#
# NOT a reuse of the real board's main_gdb.c/gdbstub.c/el2_exc.c/smp.c (see
# main_holdtest_qemu.c's banner for exactly why) -- reuses ONLY the generic,
# board-independent QEMU-target pieces (start_qemu.o entry, exceptions.o
# vector table, pl011_qemu.o console), same as the `qemu` target above.
# Deliberately skips stage2.o/gic_timer_qemu.o: this hypothesis does not
# depend on stage-2 translation or a timer tick existing at all.
HOLDTEST_ELF  := microkernel-holdtest.elf
HOLDTEST_OBJS := start_qemu.o main_holdtest_qemu.o exceptions.o \
                 el2_exc_holdtest_qemu.o guest_holdtest_asm.o \
                 guest_holdtest_payload.o pl011_qemu.o

holdtest: $(HOLDTEST_ELF)

$(HOLDTEST_ELF): $(HOLDTEST_OBJS) link_qemu.ld
	$(CC) $(LDFLAGS_QEMU) -o $@ $(HOLDTEST_OBJS)
	$(SIZE) $@

clean-holdtest:
	rm -f main_holdtest_qemu.o el2_exc_holdtest_qemu.o guest_holdtest_asm.o \
	      guest_holdtest_payload.o $(HOLDTEST_ELF)

# Header dependency tracking: -MMD -MP emits a .d per object listing the
# headers it includes, so editing e.g. vblk_emmc.h rebuilds vblk_emmc.o. This
# binary is flashed to live hardware — a stale object silently wrong against
# its header is the most dangerous build failure mode, so track deps.
%.o: %.c
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

%.o: %.S
	$(CC) $(ASFLAGS) -MMD -MP -c -o $@ $<

# dbgtools.o embeds BUILD_ID (above), recomputed from `git` on EVERY `make`
# invocation -- but plain file-timestamp dependency tracking has no way to
# know that a $(shell ...) value changed if dbgtools.c itself didn't, so
# without this it could go stale (keep reporting a build-id from whenever it
# was last ACTUALLY recompiled) after other files change and get rebuilt.
# FORCE it to always recompile; it's tiny, so the cost is negligible.
.PHONY: FORCE
FORCE:
dbgtools.o: FORCE

-include $(wildcard *.d)

# Remove every build artifact this Makefile can produce (all targets), the
# per-object .d dependency files, and the hosted test binaries. A blanket
# *.o/*.d avoids the old hand-maintained list silently going stale as files
# are added (dbgmon.o, emmc_bio.o, vblk_*.o, … were all missing before).
clean: clean-qemu clean-holdtest
	rm -f *.o *.d \
	      $(STAGE0_ELF) $(STAGE0_BIN) $(MAIN_ELF) $(MAIN_BIN) \
	      $(NET_ELF) $(NET_BIN) $(REPL_ELF) $(REPL_BIN) \
	      $(FBSD_ELF) $(FBSD_BIN) $(DBG_ELF) $(DBG_BIN) \
	      $(GDB_ELF) $(GDB_BIN) $(HDMI_ELF) $(HDMI_BIN) \
	      $(ZEPHYR_ELF) $(ZEPHYR_BIN) \
	      test_vblk_ring test_vblk_stitch test_stage2_tables test_vnet_ring test_kload_modinfo test_vconsole_uart \
	      test_gdbstub_resolve test_gdbstub_hwop test_vgic_pendq
