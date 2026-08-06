/* SPDX-License-Identifier: BSD-2-Clause */

/* main_linux_qemu.c — boot a mainline Linux/arm64 kernel as an EL1 guest
 * under this hypervisor ON QEMU `virt`, with no physical board involved.
 *
 * ROADMAP D2 ("Linux as a second guest"), first pass. Sibling of
 * main_zephyr_qemu.c — same shape, same reuse discipline (kload.c, stage2.c,
 * guest.c, vconsole.c, exceptions.S used COMPLETELY UNMODIFIED) — but a
 * different guest-image format, which is the entire reason this is a new
 * file rather than a third call site inside main_zephyr_qemu.c:
 *
 *   Zephyr's zephyr.elf is a plain ELF EXEC with one PT_LOAD, so
 *   kload_parse_elf() + kload_place_segments() (ELF-header-driven copy +
 *   relocation) is the right tool and main_zephyr_qemu.c uses it verbatim.
 *
 *   Linux/arm64's boot artifact is `Image` (arch/arm64/boot/Image): a RAW
 *   binary, not an ELF, with a 64-byte header of its own
 *   (Documentation/arch/arm64/booting.rst §4) whose fields (magic,
 *   text_offset, image_size) this file reads directly. There is nothing for
 *   kload_parse_elf() to parse — an ELF loader cannot open a file with no
 *   ELF header — so THIS file has to speak the Image-header contract
 *   itself. What it still reuses from kload.h, unmodified and read-only, is
 *   the one piece that is guest-format-agnostic: kload_enter(entry, x0, sp),
 *   the SPSR_EL2/ELR_EL2/SP_EL1 EL2->EL1 `eret` primitive. Everything above
 *   that line in this file is new, load-bearing Image-header logic; nothing
 *   in kload.c/kload.h is touched.
 *
 * WHY NO "PLACE SEGMENTS" STEP: booting.rst says the Image "must be placed
 * text_offset bytes from a 2 MiB aligned base address anywhother in usable
 * system RAM" — i.e. the loader gets to CHOOSE the base, there is no fixed
 * KVA the way an ELF's p_vaddr pins one. So instead of staging the raw file
 * at one address and copying it to a second, chosen "placement" address (the
 * two-step dance kload_place_segments() does for a relocatable ELF), this
 * target picks the final resting address up front and has QEMU's generic
 * loader (`-device loader,file=Image,addr=LX_LOAD_PA,force-raw=on`) drop the
 * bytes there directly — zero-copy, and there is then genuinely nothing left
 * for a "place" step to do. The same is true of the DTB: no relocation, it
 * is staged once at its own final address and handed to the guest as-is.
 *
 * ADDRESS CHOICES (see docs/linux-guest.md for the full memory map):
 *   LX_LOAD_PA = 0x51000000 — reused from zephyr-guest's board port's own
 *     "sram0" address purely because it is the address ALREADY vetted in
 *     this tree as clear of every HV-owned window under QEMU
 *     (link_qemu.ld's [0x40080000,0x42080000) HV image, stage2.c's
 *     [0x50000000,0x50200000) breadcrumb scratch, 0x60000000 self-test IPA).
 *     Zephyr and this target never run in the same QEMU process, so reusing
 *     the number is a convenience, not a conflict.
 *   LX_DTB_PA  = 0x44000000 — the same staging address zephyr-qemu-ci.sh
 *     uses for ITS guest ELF (a different artifact, never present at the
 *     same time), chosen here purely because it already sits, proven, clear
 *     of every window above.
 *   2 MiB alignment: required by booting.rst for the Image base. Both
 *     addresses above are exact multiples of 0x200000.
 *
 * TEXT_OFFSET: booting.rst says the historical convention (kernel < v4.6)
 * fixes text_offset at 0x80000 and modern kernels can use anything, but in
 * practice every post-v4.6 build's header still carries text_offset == 0 —
 * head.S no longer needs a nonzero one now that it can run from any 2 MiB
 * boundary. This file ASSERTS that (dies loudly, like
 * zephyr-qemu-ci.sh's "wrong board?" check, rather than silently entering at
 * the wrong PC) instead of silently adding a text_offset we have never
 * actually seen nonzero — see linux_image_check() below.
 *
 * WHAT THIS DOES NOT AND CANNOT PROVE (see docs/linux-guest.md's status
 * table for the honest, row-by-row version):
 *   - GIC programming. Exactly zephyr-qemu-ci.sh's limitation: the GIC
 *     (0x01c81000) is identity-mapped by stage2.c and, under QEMU, a black
 *     hole (nothing answers, reads return 0, no abort). Linux's irq-gic.c
 *     WILL probe it; whether that probe "succeeds against a black hole" the
 *     way Zephyr's did, or fails a sanity check and panics, is exactly the
 *     kind of thing this file cannot answer in a comment — it is what the
 *     boot log says, which is why this file's job stops at handing off, not
 *     at predicting the outcome.
 *   - Anything past whatever line the guest console shows last. This is a
 *     board-free FIRST PASS at the entry-contract gap (ROADMAP D2's deliverable
 *     #1), not a working Linux port.
 *
 * Paired with el2_exc_linux_qemu.c, which is where the guest's emulated
 * console bytes are echoed, PSCI SMC-from-EL1 calls are forwarded to QEMU's
 * own firmware emulation, and the PASS marker is decided.
 */
#include <stdint.h>
#include "exceptions.h"
#include "kload.h"
#include "stage2.h"
#include "guest.h"
#include "vconsole.h"
#include "pl011_qemu.h"

/* Staging/final addresses — see file header "ADDRESS CHOICES". Both are
 * where QEMU's `-device loader` puts the bytes; this file never copies
 * either. */
#define LX_LOAD_PA   0x51000000UL
#define LX_DTB_PA    0x44000000UL

/* arch/arm64/boot/Image header, Documentation/arch/arm64/booting.rst §4.
 * All fields little-endian (true unconditionally since v3.17; every kernel
 * this file will ever be pointed at postdates that by a decade). */
#define LX_HDR_OFF_TEXT_OFFSET  8u
#define LX_HDR_OFF_IMAGE_SIZE  16u
#define LX_HDR_OFF_MAGIC       56u
#define LX_MAGIC  0x644d5241u   /* "ARM\x64" */

/* SP_EL1: booting.rst documents no requirement on it at all — unlike x0-x3,
 * which get their own explicit "Primary CPU general-purpose register
 * settings" list, SP is absent from that list. That silence is the
 * evidence, not an oversight on our part: arm64 head.S sets up its own
 * boot-time SP (from a static array in the kernel image, not the incoming
 * register) before the first stack-relative access, the same reason
 * main_zephyr_qemu.c gives for treating Zephyr's incoming SP_EL1 as a
 * don't-care. Kept as a named placeholder rather than a bare 0 so a crash
 * dump distinguishes "never got overwritten" (this exact address) from a
 * real fault. */
#define SP_EL1_PLACEHOLDER 0x51000000UL

/* Same breadcrumb window/style as main_zephyr_qemu.c's ZEP_BC, distinct
 * magic+base to stay non-colliding with every other window in the tree
 * (see vconsole.h's own window-address list; this one is unused by anyone
 * else). "LNX1". */
#define LX_BC(i, v) do { \
	volatile uint32_t *p = (volatile uint32_t *)(0x50008400UL + (uint32_t)(i) * 4u); \
	*p = (uint32_t)(v); \
	__asm__ volatile("dc civac, %0\n\tdsb sy" :: "r"(p) : "memory"); \
} while (0)

#define S2_AT_PAR_LO (*(volatile uint32_t *)0x50000c1cUL)

static void qemu_poweroff(void) __attribute__((noreturn));

static void
qemu_poweroff(void)
{
	register uint64_t x0 __asm__("x0") = 0x84000008ull;

	__asm__ volatile("smc #0" :: "r"(x0) : "memory");
	for (;;)
		__asm__ volatile("wfi");
}

static void die(const char *why) __attribute__((noreturn));

static void
die(const char *why)
{
	pl011_puts("LINUX-QEMU-CI: FAIL — ");
	pl011_puts(why);
	pl011_puts("\n");
	qemu_poweroff();
}

/* Cache maintenance over a staged image/DTB before eret — booting.rst
 * §4 "Caches, MMUs": the loaded region "must be cleaned to the PoC" and the
 * icache "must not hold stale entries corresponding to the loaded kernel
 * image". Same 64-byte-line dc-cvac + ic-ivau idiom kload.c's own cache
 * maintenance uses (Cortex-A53's D/I cache line size — also what
 * zephyr-guest's cpu@0 node declares) — this file does not call kload.c's
 * (static, unexported) helper, it is a three-line idiom, not worth a new
 * cross-file dependency for. */
#define LX_CACHE_LINE 64ul

static void
lx_cache_clean_to_pou(uint64_t addr, uint64_t len)
{
	uint64_t start = addr & ~(LX_CACHE_LINE - 1);
	uint64_t end = (addr + len + LX_CACHE_LINE - 1) & ~(LX_CACHE_LINE - 1);
	uint64_t o;

	for (o = start; o < end; o += LX_CACHE_LINE)
		__asm__ volatile("dc cvac, %0" :: "r"(o) : "memory");
	__asm__ volatile("dsb ish");
	for (o = start; o < end; o += LX_CACHE_LINE)
		__asm__ volatile("ic ivau, %0" :: "r"(o) : "memory");
	__asm__ volatile("dsb ish\n\tisb");
}

static inline uint32_t
lx_rd32(uint64_t pa)
{
	return *(volatile uint32_t *)pa;
}

static inline uint64_t
lx_rd64(uint64_t pa)
{
	return *(volatile uint64_t *)pa;
}

/* Validate the Image header staged at LX_LOAD_PA and return its entry PA
 * (== LX_LOAD_PA + text_offset). Dies with a specific reason on any
 * mismatch rather than silently guessing — see file header "TEXT_OFFSET". */
static uint64_t
linux_image_check(uint64_t base_pa)
{
	uint32_t magic = lx_rd32(base_pa + LX_HDR_OFF_MAGIC);
	uint64_t text_offset = lx_rd64(base_pa + LX_HDR_OFF_TEXT_OFFSET);
	uint64_t image_size = lx_rd64(base_pa + LX_HDR_OFF_IMAGE_SIZE);

	LX_BC(2, magic);
	LX_BC(3, (uint32_t)text_offset);
	LX_BC(4, (uint32_t)image_size);

	if (magic != LX_MAGIC) {
		pl011_puts("HV: Image header magic=0x");
		pl011_put_hex32(magic);
		pl011_puts(" (want 0x644d5241 \"ARM\\x64\") at 0x");
		pl011_put_hex64(base_pa);
		pl011_puts("\n");
		die("no valid Linux Image staged at LX_LOAD_PA (did QEMU get "
		    "-device loader,addr=0x51000000?)");
	}

	if (text_offset != 0) {
		/* See file header: every real build we have ever seen has
		 * text_offset==0; a nonzero value here means either a pre-4.6
		 * kernel or something this file's fixed-base placement does
		 * not account for. Fail loudly rather than entering at a
		 * silently-wrong PC. */
		die("Image header text_offset != 0 — this loader assumes 0 "
		    "(see main_linux_qemu.c's TEXT_OFFSET note); rebuild or "
		    "extend this file before trusting this run");
	}

	if (image_size == 0) {
		/* Legal per the spec (pre-3.17 semantics) but every build
		 * this file targets is post-3.17; a zero here means the
		 * loader dropped something unexpected. */
		die("Image header image_size == 0 — unexpected for any kernel "
		    "built by linux-guest/build.sh");
	}

	pl011_puts("HV: Image header OK — magic=ARM\\x64 text_offset=0 image_size=0x");
	pl011_put_hex64(image_size);
	pl011_puts("\n");

	return base_pa + text_offset;
}

long
main(void)
{
	uint64_t entry;

	pl011_init();
	pl011_puts("\n=== bzdOS microkernel -- Linux/arm64 EL1 guest on QEMU virt ===\n");

	LX_BC(0, 0x4c4e5831);   /* "LNX1" */
	LX_BC(1, 1);

	el2_install();
	pl011_puts("HV: EL2 vector table installed (exceptions.S, unchanged)\n");

	/* Must precede stage2_enable() — see main_zephyr_qemu.c's identical
	 * ordering note; vconsole's capture ring header must exist before the
	 * guest can possibly fault on UART0. */
	vconsole_init();
	pl011_puts("HV: vconsole 16550 trap-emulator armed (guest console = 0x01c28000, "
	           "same emulation Zephyr and FreeBSD use)\n");
	LX_BC(1, 2);

	/* --- Validate the guest Image, staged directly at its final resting
	 * place by QEMU's loader (see file header "WHY NO PLACE SEGMENTS
	 * STEP") --------------------------------------------------------- */
	entry = linux_image_check(LX_LOAD_PA);
	LX_BC(1, 3);
	LX_BC(5, (uint32_t)entry);
	pl011_puts("HV: Image entry_pa=0x");
	pl011_put_hex64(entry);
	pl011_puts("\n");

	/* Cache maintenance over the staged Image (booting.rst mandatory —
	 * see lx_cache_clean_to_pou()'s comment). image_size, not the file's
	 * on-disk length, is the field the spec defines this requirement
	 * over. */
	lx_cache_clean_to_pou(LX_LOAD_PA, lx_rd64(LX_LOAD_PA + LX_HDR_OFF_IMAGE_SIZE));
	/* Same for the DTB — booting.rst's dtb placement rule doesn't repeat
	 * the cache-clean requirement verbatim, but it is bound by the same
	 * "MMU off, no stale icache" preamble; 2 MiB is the spec's own upper
	 * bound on DTB size, cheap enough to clean unconditionally rather
	 * than parse the FDT header just to learn its real (smaller) size. */
	lx_cache_clean_to_pou(LX_DTB_PA, 2ul * 1024ul * 1024ul);
	LX_BC(1, 4);
	pl011_puts("HV: cache maintenance (dc cvac + ic ivau) done over Image + DTB\n");

	/* --- Guest CPU state + stage-2, both reused verbatim ------------- */
	guest_config();

	/* Same one-liner main_zephyr_qemu.c/main_zephyr.c use, same reason:
	 * booting.rst's "Architected timers" clause ("CNTVOFF must be
	 * programmed with a consistent value") — a nonzero leftover from
	 * firmware would skew the guest's view of the virtual counter/timer,
	 * which is the one Linux's arch_timer driver will actually use here
	 * (see docs/linux-guest.md: CNTHCTL_EL2 passthrough for the PHYSICAL
	 * counter is deliberately not touched, because nothing in this DTB
	 * gives Linux a reason to prefer it over the virtual PPI). */
	__asm__ volatile("msr cntvoff_el2, xzr\n\tisb" ::: "memory");

	stage2_init();
	stage2_enable();
	pl011_puts("HV: stage-2 identity map programmed + enabled (HCR_EL2.VM=1)\n");

	stage2_at_check(STAGE2_SELFTEST_IPA);
	if ((S2_AT_PAR_LO & 1u) != 0u)
		die("stage-2 AT S12E1R self-check failed (PAR_EL1.F=1)");
	pl011_puts("HV: stage-2 AT S12E1R self-check PASS (PAR_EL1.F=0)\n");
	LX_BC(1, 5);

	pl011_puts("HV: eret to EL1 -- everything after this, prefixed [guest], is Linux\n");
	pl011_puts("HV: x0(dtb)=0x");
	pl011_put_hex64(LX_DTB_PA);
	pl011_puts(" entry=0x");
	pl011_put_hex64(entry);
	pl011_puts("\n");
	LX_BC(1, 6);

	/* IRQ/FIQ are NOT masked in the SPSR kload_enter() writes (see
	 * kload.c's KLOAD_GUEST_SPSR_EL2 — D=1,A=1,I=0,F=0), which is a real,
	 * documented deviation from booting.rst's "all forms of interrupts
	 * must be masked in PSTATE.DAIF" requirement. It is inert on THIS
	 * target only because nothing arms a physical interrupt source here
	 * (no gic_timer_qemu_init() call, exactly like main_zephyr_qemu.c) —
	 * see docs/linux-guest.md's entry-contract-gap table for the full
	 * argument and why it would matter the day this target grows a tick.
	 * kload_enter() is reused UNMODIFIED (kload.c is read-only for this
	 * pass); fixing this, if it is ever worth fixing, belongs to kload.c's
	 * owner, not here. */
	kload_enter(entry, LX_DTB_PA /* x0 = dtb PA, the Linux/arm64 convention */,
	            SP_EL1_PLACEHOLDER);
	for (;;)
		__asm__ volatile("wfi");
}
