/* SPDX-License-Identifier: BSD-2-Clause */

/* vblk_emmc.c — virtio-blk over virtio-mmio (modern/v2), backed by the REAL
 * eMMC via emmc_bio.c, for the FreeBSD/arm64 EL1 guest under the bzdOS EL2
 * hypervisor (Allwinner A64 / Banana Pi M64). See vblk_emmc.h for the full
 * rationale, the naming note (why this is a NEW file, not an edit of the
 * existing RAM-disk virtio_blk.c), and the register/vring layout.
 *
 * FLOW (all synchronous, on CPU0, inside the guest's MMIO trap):
 *   guest writes QueueNotify  ->  vblk_mmio_fault() -> vblk_kick()
 *     -> for each available descriptor chain:
 *          desc[0]      = 16-byte read-only request header {type,_,sector}
 *          desc[1..k-2] = data buffers (write-only for T_IN, read-only T_OUT)
 *          desc[k-1]    = 1-byte write-only status
 *        walk sectors: emmc_bio_read()/emmc_bio_write() one 512-byte block
 *        at a time, bouncing through a 512-byte HV-local buffer so a data
 *        descriptor need not be sector-aligned in length;
 *        write status byte; push (head,used_len) onto the used ring.
 *     -> set InterruptStatus.VRING, then inject VBLK_INTID by writing the
 *        real GICD_ISPENDR (IMO=0: the passed-through GICv2 delivers it to
 *        the guest, which acks/EOIs natively).
 *
 * Guest memory access: descriptors point into guest DRAM, which stage-2
 * identity-maps (IPA==PA), so a guest PA is a valid EL2 pointer. All accesses
 * happen on CPU0 with the same Normal-WB cacheability as the guest, so they
 * are cache-coherent with the guest with no explicit maintenance (barriers
 * are still used to ORDER ring reads/writes against the guest). See the
 * design doc's coherency section.
 *
 * Freestanding: <stdint.h> only.
 */
#include <stdint.h>
#include "vblk_emmc.h"
#include "emmc_bio.h"       /* emmc_bio_init / emmc_bio_read / emmc_bio_write */
#include "wdt.h"            /* wdt_note_progress / wdt_pet — keep the HW WDOG fed */
#include "flightrec.h"      /* B4: flightrec_log(FLTR_K_VIRTIO/FLTR_K_IRQ, ...) */

/* ------------------------------------------------------------------ *
 * ESR_EL2.ISS decode for a data abort (EC==0x24) — identical convention to
 * vconsole.c (see its comments); duplicated here to keep this file
 * self-contained.
 * ------------------------------------------------------------------ */
#define ESR_EC_SHIFT      26
#define ESR_EC_MASK       0x3Fu
#define ESR_EC_DABT_LOWER 0x24u
#define ESR_ISV_BIT       (1u << 24)
#define ESR_SAS_SHIFT     22
#define ESR_SAS_MASK      0x3u
#define ESR_SRT_SHIFT     16
#define ESR_SRT_MASK      0x1Fu
#define ESR_WNR_BIT       (1u << 6)
#define SRT_XZR           31u

/* ------------------------------------------------------------------ *
 * Breadcrumb window (DRAM, survives a warm WDT reset, readable via `md`/bc).
 * 0x50005000 — matches the slot the existing virtio.c reserved (VIRTIO_BC_BASE),
 * chosen distinct from every other lane in smp.h's map.
 *   [0] magic "VBK1"   [1] status reg     [2] queue0 ready
 *   [3] desc_pa lo     [4] avail_pa lo    [5] used_pa lo
 *   [6] reads          [7] writes         [8] last sector
 *   [9] last status    [10] irq injections  [11] emmc_ready
 *   [12] last emmc rc  [13] fault count
 *
 * Extra per-completion diagnostics for the LAST request vblk_request()
 * finished (added while chasing the "guest sees hard error, HV breadcrumbs
 * look fine" bug -- see vblk_diag()):
 *   [14] head desc index        [15] chain length walked (n)
 *   [16] data bytes served      [17] status-byte PA lo32
 *   [18] status-byte PA hi32    [19] used->idx AFTER push
 *   [20] VBLK_MAX_CHAIN-truncation event count (should stay 0; see
 *        vblk_request()'s `truncated` handling)
 *
 * ROADMAP C2 (async CPU2 offload — see the "ASYNC I/O OFFLOAD" section below):
 *   [24] mailbox posts (CPU0 -> CPU2 handoffs accepted)
 *   [25] mailbox completions (CPU2 finished + injected IRQ)
 *   [26] synchronous fallbacks (mailbox busy / chain too long for the slot,
 *        or g_vblk_async_ready==0 — always correct, just not accelerated)
 *
 * D2/D5 diagnostics (code-review fixes; see each spot's own comment):
 *   [27] gmem_read/gmem_write/serve_data rejects of an out-of-DRAM-range
 *        guest PA (D2 — should stay 0 against a real, well-behaved guest)
 *   [28] QueueNum writes refused for not being a power of two (D5(b) — should
 *        stay 0; FreeBSD always negotiates a power-of-two size)
 *   [29] descriptor W/R flag mismatches observed, NOT enforced (D5(d),
 *        diagnostic only — see vblk_request()'s comment)
 *   [30] QueueNotify seen before DRIVER_OK, NOT enforced (D5(e), diagnostic
 *        only — see vblk_kick()'s comment)
 * ------------------------------------------------------------------ */
/* Address owned by hv_addrmap.h (via vblk_emmc.h). Was 0x50005000, INSIDE the
 * 64 KiB vconsole ring, where console output clobbered these words. */
#define VBLK_BC_BASE   HVMAP_VBLK_BC
#define VBLK_BC_MAGIC  0x56424B31u   /* "VBK1" */

static inline void vblk_bc(uint32_t idx, uint32_t v)
{
	volatile uint32_t *p = (volatile uint32_t *)(VBLK_BC_BASE + idx * 4u);
	*p = v;
	__asm__ volatile("dc civac, %0\n\tdsb sy" :: "r"(p) : "memory");
}

/* ------------------------------------------------------------------ *
 * Single device instance + a private 512-byte bounce block.
 * ------------------------------------------------------------------ */
static struct vblk_dev g_blk;
static uint32_t        g_reads, g_writes, g_irqs, g_faults, g_truncated;

/* D5 diagnostics (see each fix's own comment for how/why they're used).
 * Declared here (not local to their fix) so both vblk_request()/vblk_kick()
 * (which fire them) and vblk_reg_read()/vblk_reg_write() (which fire the
 * QueueNum one) can see them regardless of definition order in this file. */
static uint32_t g_bad_queue_num;      /* (b) non-power-of-two QueueNum write, refused */
static uint32_t g_bad_desc_flags;     /* (d) descriptor W/R flag mismatch, NOT enforced */
static uint32_t g_kick_not_ready;     /* (e) kick seen before DRIVER_OK, NOT enforced */

/* Live debug (2026-07-27, "vtbd0 hard error, HV breadcrumbs never move" hunt):
 * unconditional counters distinguishing "guest never notified us" from "we
 * were notified but the avail ring yielded nothing new" from "we popped a
 * head but vblk_request() bailed before touching serve_data()". Unlike
 * g_reads/g_writes (only bumped on a request that reaches the T_IN/T_OUT
 * branch) these increment on EVERY QueueNotify / EVERY successful
 * vq_pop_avail(), so they can catch a lost-kick or avail-ring-desync bug
 * that the existing diagnostics are structurally blind to. bc[31]/bc[32]
 * were unused (VBLK_BC_SIZE=0x100 leaves room to bc[63]). */
static uint32_t g_kicks_seen;         /* every vblk_kick() call, unconditional */
static uint32_t g_heads_popped;       /* every successful vq_pop_avail() */
static uint32_t g_indirect_seen;      /* VRING_DESC_F_INDIRECT early-exit fired */
static uint32_t g_short_chain;        /* n<2 early-exit fired */

/* ROADMAP C2 async I/O offload readiness handshake (see vblk_emmc.h and the
 * "ASYNC I/O OFFLOAD" section below). Zero-initialized (.bss) in EVERY build;
 * only vblk_async_cpu2_run() (vblk_async.c, linked ONLY into DBG_OBJS) ever
 * sets it to 1. This is the real on/off switch for the whole async path. */
volatile uint32_t g_vblk_async_ready;

/* One-sector HV-local bounce buffer. uint64_t-aligned (emmc_bio wants at least
 * uint32_t alignment of the PA it is handed). Kept in .bss (Normal-WB), so its
 * PA is a plain EL2 address suitable to pass to emmc_bio_read/write. */
static uint64_t g_bounce_q[VBLK_SECTOR_BYTES / 8];   /* 512 bytes */
#define BOUNCE_PA  ((uint64_t)(uintptr_t)&g_bounce_q[0])
static inline uint8_t *bounce(void) { return (uint8_t *)(uintptr_t)&g_bounce_q[0]; }

/* ------------------------------------------------------------------ *
 * D2 fix: guest-PA range check.
 *
 * Every descriptor/register field below (desc.addr, avail/used ring PAs,
 * the status-byte PA, a data buffer's PA) is a value the GUEST wrote into
 * shared memory or an MMIO register — EL2 has no stage-2 protection here
 * (identity map, and even where stage-2 traps a window, EL2 itself reads
 * these as plain PAs, bypassing stage-2 entirely). A corrupt/wild/malicious
 * value could point at HV .text/.data or a real MMIO peripheral instead of
 * guest DRAM. Reject (never dereference) any PA/length that falls CLEARLY
 * outside guest DRAM before touching it.
 *
 * Bounds: guest DRAM is stage-2-identity-mapped over
 * [STAGE2_DRAM_BASE, STAGE2_DRAM_BASE + STAGE2_DRAM_SIZE) — see stage2.h
 * (0x40000000 / 0x40000000, i.e. [0x40000000, 0x80000000), 1 GiB, as of this
 * writing). Duplicated here as plain constants (not #include "stage2.h") per
 * this file's self-containment convention — the same one that already
 * duplicates gmem_cmo/gmem_read/gmem_write against vnet_emac.c rather than
 * sharing a common header. MUST stay in lockstep with stage2.h's
 * STAGE2_DRAM_BASE/STAGE2_DRAM_SIZE if either ever changes.
 *
 * IMPORTANT — what this does NOT do: it does NOT protect the hv-image
 * (0x42000000+) or hv-scratch/breadcrumb (0x50000000+) windows carved out of
 * this SAME DRAM range (see stage2.c's HVIMG_L2_IDX/HVSCR_L2_IDX exclusions).
 * A guest descriptor pointing AT one of those (still nominally "in DRAM")
 * passes this check and would still corrupt HV state. Closing that gap needs
 * real stage-2/DMA isolation for the guest (ROADMAP milestone A1) — out of
 * scope for this fix, which only catches PAs that are clearly, unambiguously
 * outside ALL of guest DRAM (e.g. a wild pointer at 0x0 or 0x1_00000000).
 * ------------------------------------------------------------------ */
#define GUEST_DRAM_BASE   0x40000000ULL
#define GUEST_DRAM_SIZE   0x40000000ULL
#define GUEST_DRAM_END    (GUEST_DRAM_BASE + GUEST_DRAM_SIZE)

static uint32_t g_gmem_oob;    /* count of rejected out-of-range accesses */

static inline int gpa_in_range(uint64_t gpa, uint32_t len)
{
	if (len == 0u)
		return 1;                              /* nothing to touch */
	if ((uint64_t)len > GUEST_DRAM_SIZE)
		return 0;                              /* pathological length */
	if (gpa < GUEST_DRAM_BASE || gpa >= GUEST_DRAM_END)
		return 0;
	if ((GUEST_DRAM_END - gpa) < (uint64_t)len)
		return 0;                              /* [gpa,gpa+len) runs past DRAM top */
	return 1;
}

/* ------------------------------------------------------------------ *
 * Guest-memory helpers. IPA==PA identity map => a guest PA is an EL2 pointer.
 * Byte-wise copies keep us honest about alignment of arbitrary descriptor
 * buffers. dsb before a read of guest-produced data / after a write of
 * device-produced data orders us against the guest on the same core.
 * ------------------------------------------------------------------ */
/* Clean+invalidate every cache line covering [gpa, gpa+len) to PoC.
 *
 * WHY (2026-07-20, "hard error despite S_OK" hunt): EL2 currently runs on
 * U-Boot's inherited stage-1 tables whose exact attributes (cacheability,
 * shareability, SCTLR_EL2.C state) we do NOT control. If EL2's view of guest
 * DRAM differs from the guest's own Normal-WB Inner-Shareable view in ANY of
 * those dimensions, the same-core "EL2 writes are trivially visible to EL1"
 * argument silently breaks (mismatched-attribute accesses lose coherency
 * guarantees even on one core). Bracketing every guest-memory access with a
 * line-wise dc civac makes the HV behave like a maintenance-correct
 * non-coherent DMA master REGARDLESS of what U-Boot left us: reads always
 * refetch from PoC (any dirty line — ours or the guest's, same core — is
 * first written back by the clean half, so data is never lost), and writes
 * always land at PoC where every observer sees them. Cost: a handful of
 * cache ops per trap — noise next to the eMMC transfer itself. */
static void gmem_cmo(uint64_t gpa, uint32_t len)
{
	uint64_t p   = gpa & ~63ULL;
	uint64_t end = gpa + len;
	for (; p < end; p += 64)
		__asm__ volatile("dc civac, %0" :: "r"(p) : "memory");
	__asm__ volatile("dsb sy" ::: "memory");
	/* D-cache-only was the 2026-07-22 fix for the GPT/superblock staleness
	 * bug (see this function's callers) -- correct for that, but this same
	 * path also serves regular FILE data, including whatever the guest is
	 * about to EXECUTE (ELF text for /sbin/init, every dynamically-loaded
	 * program, etc). ARM's I-cache is not automatically coherent with the
	 * D-cache even after a clean to PoC: a physical page that previously
	 * held different code (near-certain after any real boot's worth of
	 * exec/page reuse) can leave the guest's I-cache holding STALE
	 * instructions for this address, independent of what the D-cache (and
	 * therefore any plain load) now sees. Without this, the guest can
	 * fetch-and-execute garbage instead of the freshly-read code the very
	 * moment it jumps there -- matching the live symptom found 2026-07-27
	 * (mountroot-real-bug-is-ufs-sblock-offset.md): root mounts fine, but
	 * a scatter of DIFFERENT userland programs (init, ifconfig, sysctl,
	 * date, rcorder, kenv, automount -- core files for all of them were
	 * sitting in the guest's own root directory) crash unpredictably
	 * shortly after they'd be exec'd, consistent with instruction fetch
	 * being unreliable rather than one deterministic bug. `ic ivau` +
	 * barriers is the standard ARMv8 "make freshly-written memory safely
	 * executable" sequence; cost is the same order as the dc civac loop
	 * above, i.e. still noise next to the eMMC transfer. */
	for (p = gpa & ~63ULL; p < end; p += 64)
		__asm__ volatile("ic ivau, %0" :: "r"(p) : "memory");
	__asm__ volatile("dsb ish\n\tisb" ::: "memory");
}

static void gmem_read(uint64_t gpa, void *dst, uint32_t len)
{
	if (!gpa_in_range(gpa, len)) {
		/* D2: never dereference outside guest DRAM. Leave dst untouched —
		 * every caller already treats gmem_read's output as
		 * guest/attacker-controlled ring/descriptor data, so a short-circuit
		 * here (uninitialized dst, same as if the caller hadn't zeroed it)
		 * is strictly safer than reading real HV memory or MMIO, and never
		 * less correct for any access actually inside DRAM. */
		g_gmem_oob++;
		vblk_bc(27, g_gmem_oob);
		return;
	}
	const volatile uint8_t *s = (const volatile uint8_t *)(uintptr_t)gpa;
	uint8_t *d = (uint8_t *)dst;
	gmem_cmo(gpa, len);                  /* refetch from PoC, never stale */
	for (uint32_t i = 0; i < len; i++)
		d[i] = s[i];
}

static void gmem_write(uint64_t gpa, const void *src, uint32_t len)
{
	if (!gpa_in_range(gpa, len)) {
		g_gmem_oob++;
		vblk_bc(27, g_gmem_oob);
		return;                          /* D2: drop the write, see gmem_read() */
	}
	volatile uint8_t *d = (volatile uint8_t *)(uintptr_t)gpa;
	const uint8_t *s = (const uint8_t *)src;
	for (uint32_t i = 0; i < len; i++)
		d[i] = s[i];
	gmem_cmo(gpa, len);                  /* publish to PoC for every observer */
}

static inline uint16_t gmem_ld16(uint64_t gpa)
{
	uint16_t v; gmem_read(gpa, &v, 2); return v;    /* guest is LE == our LE */
}
static inline void gmem_st16(uint64_t gpa, uint16_t v)
{
	gmem_write(gpa, &v, 2);
}

/* ------------------------------------------------------------------ *
 * eMMC-controller mutual exclusion + watchdog feeding (design §8.1).
 *
 * ONE controller, TWO cores: CPU0 (this device) and CPU1 (debug core, via a
 * dbgmon `call emmc_bio_*`). We serialize every controller transaction behind
 * a single test-and-set spinlock in a FIXED DRAM word (VBLK_EMMC_LOCK_PA) so
 * the two never poke 0x01c11000 at once. ARMv8.0 A53 (no LSE) => ldaxr/stlxr
 * exclusive loop, exactly like smp.c's g_online set. The word is plain DRAM
 * (cross-core coherent under SMPEN) and is zeroed in vblk_init() so a stale
 * "1" from a prior run can never deadlock a fresh boot.
 * ------------------------------------------------------------------ */
static inline volatile uint32_t *emmc_lock_word(void)
{
	return (volatile uint32_t *)VBLK_EMMC_LOCK_PA;
}

int vblk_emmc_trylock(void)
{
	volatile uint32_t *p = emmc_lock_word();
	uint32_t prev, status, one = 1u;
	/* status(%w1), value(%w2==one) and addr(%w3) are distinct registers —
	 * stlxr's status/value/base must not alias (else UNPREDICTABLE). If the
	 * word is already 1 we branch out (leaving the monitor to be cleared by
	 * the next exclusive) and report failure. */
	__asm__ volatile(
		"	ldaxr	%w0, [%3]\n"
		"	cbnz	%w0, 1f\n"          /* already held -> fail */
		"	stlxr	%w1, %w2, [%3]\n"   /* try to store 1 */
		"	b	2f\n"
		"1:	mov	%w1, #1\n"          /* prev!=0: report failure */
		"2:\n"
		: "=&r"(prev), "=&r"(status)
		: "r"(one), "r"(p)
		: "memory");
	if (prev == 0u && status == 0u) {
		__asm__ volatile("dsb sy" ::: "memory");
		return 1;                        /* acquired */
	}
	return 0;                            /* contended / store lost */
}

void vblk_emmc_unlock(void)
{
	volatile uint32_t *p = emmc_lock_word();
	__asm__ volatile("dsb sy" ::: "memory");
	*p = 0u;
	__asm__ volatile("dsb sy\n\tsev" ::: "memory");
}

/* ------------------------------------------------------------------ *
 * Used-ring publish lock (CPU0 sync completion vs CPU2 async completion) —
 * see VBLK_USED_LOCK_PA's comment in vblk_emmc.h for the bug this closes.
 * Same ldaxr/stlxr test-and-set as the eMMC lock above, duplicated (not
 * shared) because it protects a completely different critical section.
 * ------------------------------------------------------------------ */
static inline volatile uint32_t *used_lock_word(void)
{
	return (volatile uint32_t *)VBLK_USED_LOCK_PA;
}

int vblk_used_trylock(void)
{
	volatile uint32_t *p = used_lock_word();
	uint32_t prev, status, one = 1u;
	__asm__ volatile(
		"	ldaxr	%w0, [%3]\n"
		"	cbnz	%w0, 1f\n"
		"	stlxr	%w1, %w2, [%3]\n"
		"	b	2f\n"
		"1:	mov	%w1, #1\n"
		"2:\n"
		: "=&r"(prev), "=&r"(status)
		: "r"(one), "r"(p)
		: "memory");
	if (prev == 0u && status == 0u) {
		__asm__ volatile("dsb sy" ::: "memory");
		return 1;
	}
	return 0;
}

void vblk_used_unlock(void)
{
	volatile uint32_t *p = used_lock_word();
	__asm__ volatile("dsb sy" ::: "memory");
	*p = 0u;
	__asm__ volatile("dsb sy\n\tsev" ::: "memory");
}

/* Bounded spin: the critical section is a handful of word writes (no real
 * hardware wait involved, unlike the eMMC controller lock), so a small spin
 * count is plenty — this is only ever contended for a few instructions'
 * worth of time against the OTHER core's own publish step. */
static void vblk_used_lock_acquire(void)
{
	for (uint32_t i = 0; i < 100000u; i++) {
		if (vblk_used_trylock())
			return;
		__asm__ volatile("yield" ::: "memory");
	}
	/* Never expected to be reached: the critical section is a handful of word
	 * writes (no hardware wait), so a 100k-yield standoff means the word is
	 * almost certainly stale garbage, not a live holder. Break it — but by
	 * RE-ACQUIRING, not merely clearing. The old code did `*word = 0; return`,
	 * which left the caller running UNLOCKED: a concurrent vblk_used_trylock()
	 * then succeeded immediately, reopening the exact CPU0-vs-CPU2 race this
	 * lock exists to close. Force-clear the stale word, then take it. */
	*used_lock_word() = 0u;
	__asm__ volatile("dsb sy" ::: "memory");
	for (uint32_t i = 0; i < 100000u; i++) {
		if (vblk_used_trylock())
			return;
		__asm__ volatile("yield" ::: "memory");
	}
	/* Still contended after a forced clear (truly pathological) — proceed with
	 * the word held-set so at least the OTHER core's trylock fails, rather than
	 * both cores running free into the ring. */
	*used_lock_word() = 1u;
	__asm__ volatile("dsb sy" ::: "memory");
}

/* D4 fix: CNTPCT_EL0-based time helpers (mirrors emmc_bio.c's/wdt.c's own
 * pattern) so the eMMC-lock acquire budget below can be expressed in real
 * elapsed time rather than a raw spin count, whose wall-clock cost per
 * iteration is not actually fixed (a "yield" instruction's latency is not
 * architecturally bounded). */
static inline uint64_t read_cntpct(void)
{
	uint64_t v;
	__asm__ volatile("isb\n\tmrs %0, cntpct_el0" : "=r"(v));
	return v;
}

static inline uint64_t read_cntfrq(void)
{
	uint64_t v;
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
	return v;
}

static inline uint64_t vblk_ms_to_ticks(uint32_t ms)
{
	uint64_t f = read_cntfrq();
	if (f == 0)
		f = 24000000ull;   /* A64 arch timer default, matches wdt.c's fallback */
	return (f * (uint64_t)ms) / 1000ull;
}

/* Acquire the eMMC lock from CPU0's trap path, BOUNDED so a CPU1/CPU2 holder
 * can never stall the guest's core past the ~16 s HW watchdog. On each spin
 * we feed the watchdog (note-progress + pet) so a legitimately long wait
 * stays resident, and cap the WAIT in real elapsed time (D4 fix — was a raw
 * 2,000,000-iteration spin count, which at a few instructions per iteration
 * can amount to only single-digit milliseconds of real wall time: far
 * shorter than a legitimate emmc_bio_write() holder can now take under its
 * own new time caps (up to ~1 s DATA_OVER + ~4 s CARD_BUSY + sub-1s FIFO
 * push, see emmc_bio.c's EMMC_WRITE_DATA_TIMEOUT_MS/EMMC_WRITE_BUSY_TIMEOUT_MS
 * — worst case a little under 6 s). The old budget could therefore give up
 * on a perfectly healthy in-progress write and hand the guest a spurious
 * S_IOERR. VBLK_EMMC_LOCK_TIMEOUT_MS is chosen to comfortably exceed that
 * worst case while staying well inside the 16 s HW WDOG window and the
 * 180 s software progress window (wdt.c). A wedged holder still yields a
 * clean S_IOERR (return 0) instead of hanging CPU0 forever. */
#define VBLK_EMMC_LOCK_TIMEOUT_MS  6000u
static int emmc_lock_acquire_bounded(void)
{
	uint64_t start = read_cntpct();
	uint64_t cap = vblk_ms_to_ticks(VBLK_EMMC_LOCK_TIMEOUT_MS);
	uint32_t i = 0;

	for (;;) {
		if (vblk_emmc_trylock())
			return 1;
		if ((i++ & 0xFFFFu) == 0u) {       /* periodically keep the HW WDOG fed */
			wdt_note_progress();
			wdt_pet();
		}
		if (read_cntpct() - start > cap)
			return 0;                       /* contended too long — give up */
		__asm__ volatile("yield" ::: "memory");
	}
}

/* Per-sector watchdog feed inside a long multi-sector transfer. el2_trap pets
 * the WDT only on trap ENTRY; a single QueueNotify can drain many sectors at
 * the 400 kHz init clock (~10 ms/block => ~1600 sectors before a 16 s fire),
 * so we re-feed here to guarantee CPU0 is never declared wedged mid-transfer.
 * emmc_bio's own polls are iteration-capped, so every call returns in bounded
 * time — this only extends the outer WDOG window, it cannot mask a real hang. */
static inline void vblk_pet_wdt(void)
{
	wdt_note_progress();
	wdt_pet();
}

/* ------------------------------------------------------------------ *
 * Virtqueue engine (split ring, VIRTIO 1.x).
 * ------------------------------------------------------------------ */

/* Read descriptor `idx` of queue 0 into host-endian *out. */
static void vq_read_desc(struct vblk_vq *vq, uint16_t idx, struct vblk_desc *out)
{
	struct vring_desc d;
	uint64_t p = vq->desc + (uint64_t)idx * sizeof(struct vring_desc);
	gmem_read(p, &d, sizeof(d));
	out->addr  = d.addr;
	out->len   = d.len;
	out->flags = d.flags;
	out->next  = d.next;
}

/* Pop the next available head from queue 0's avail ring. Returns 1 + *head,
 * or 0 if nothing new. avail ring layout: [0]=flags,[1]=idx,[2..]=ring[num]. */
static int vq_pop_avail(struct vblk_vq *vq, uint16_t *head)
{
	uint16_t avail_idx;

	if (!vq->ready || vq->num == 0)
		return 0;

	/* avail->idx is at byte offset 2 (after le16 flags). */
	avail_idx = gmem_ld16(vq->avail + 2u);
	if (vq->last_avail == avail_idx)
		return 0;                          /* caught up */

	/* ring[] starts at byte offset 4; entries are le16, modulo queue size. */
	{
		uint16_t slot = (uint16_t)(vq->last_avail % vq->num);
		*head = gmem_ld16(vq->avail + 4u + (uint64_t)slot * 2u);
	}
	vq->last_avail++;
	return 1;
}

/* Publish a completed chain to queue 0's used ring and bump used->idx.
 * used ring layout: [0]=le16 flags,[2]=le16 idx,[4..]= { le32 id; le32 len }
 * Returns the used->idx value AFTER the bump (diagnostic convenience for
 * vblk_diag() -- see the breadcrumb block comment). */
static uint16_t vq_push_used(struct vblk_vq *vq, uint16_t head, uint32_t used_len)
{
	/* Guard against a concurrent driver reset (VBLK_R_STATUS=0 on CPU0 clears
	 * num/used) landing between an async request's dispatch and CPU2's
	 * completion: with num==0 the modulo below is a div-by-zero (AArch64 UDIV
	 * yields 0, no trap) and used==0 would send the ring write to PA 0x2/0x4 —
	 * silent low-memory corruption. After a reset the guest has torn this ring
	 * down anyway, so dropping the stale completion is correct, not just safe. */
	if (vq->num == 0u || vq->used == 0u)
		return 0;
	uint16_t used_idx = gmem_ld16(vq->used + 2u);
	uint16_t slot = (uint16_t)(used_idx % vq->num);
	uint64_t e = vq->used + 4u + (uint64_t)slot * 8u;   /* &ring[slot] */
	uint32_t id = head;
	uint16_t new_idx = (uint16_t)(used_idx + 1u);

	gmem_write(e + 0u, &id, 4u);
	gmem_write(e + 4u, &used_len, 4u);
	__asm__ volatile("dsb sy" ::: "memory");            /* entry before idx */
	gmem_st16(vq->used + 2u, new_idx);
	__asm__ volatile("dsb sy" ::: "memory");
	return new_idx;
}

/* ------------------------------------------------------------------ *
 * IRQ injection (IMO=0): raise VBLK_INTID as pending in the REAL GICD so the
 * passed-through GICv2 delivers it to the guest EL1. GICD_ISPENDR is a bitmap:
 * word (intid/32), bit (intid%32).
 * ------------------------------------------------------------------ */
static void vblk_inject_irq(void)
{
	uint32_t intid = VBLK_INTID;
	uint32_t word  = intid / 32u;
	uint32_t bit   = intid % 32u;
	volatile uint32_t *ispendr =
	    (volatile uint32_t *)(VBLK_GICD_BASE + VBLK_GICD_ISPENDR + word * 4u);

	/* TODO(board): this assumes the guest has already ENABLED VBLK_INTID at the
	 * distributor (GICD_ISENABLER) and set its target/priority as part of
	 * bus_setup_intr() on the virtio-mmio node. If a completion can occur
	 * before the driver attaches (it cannot in practice — the guest must kick
	 * the queue first, which means it has attached), the pending bit simply
	 * latches until the guest enables it. We ONLY set-pending; we never touch
	 * GICC — the guest acks (GICC_IAR) and deactivates (GICC_EOIR) natively.
	 *
	 * TODO(board): confirm on hardware that a software GICD_ISPENDR set on an
	 * EDGE-configured SPI (see DTB node / design doc) is delivered exactly once
	 * and self-clears on the guest's IAR read. If the SPI ends up configured
	 * LEVEL (GIC_SPI ... 4), a software set-pending may re-fire because no real
	 * wire deasserts it — in that case switch the DTB to edge (…1) OR clear the
	 * pending bit here after a bounded delay. */
	__asm__ volatile("dsb sy" ::: "memory");
	*ispendr = (1u << bit);
	__asm__ volatile("dsb sy" ::: "memory");

	g_irqs++;
	vblk_bc(10, g_irqs);

	/* B4 flight recorder: one event per virtio-blk completion IRQ raised —
	 * covers both callers (vblk_kick's synchronous path and
	 * vblk_async_poll's CPU2 completion path). a0=intid, a1=running count
	 * (cheap way to see gaps/storms in the timeline without a 2nd lookup). */
	flightrec_log(FLTR_K_IRQ, intid, g_irqs);
}

/* ------------------------------------------------------------------ *
 * Request processing.
 * ------------------------------------------------------------------ */

/* Serve `len` bytes at guest PA `gpa` for a request whose data starts at eMMC
 * byte offset described by `*sector` (in 512-byte sectors, advanced as we go).
 * `is_read` picks emmc_bio_read (device->guest) vs emmc_bio_write. Returns 0
 * on success, negative on an eMMC error or misalignment we can't serve.
 *
 * Robust to a descriptor whose length or start is not a whole sector: we bounce
 * every touched 512-byte block through g_bounce and copy only the overlapping
 * bytes to/from the guest buffer. A virtio-blk request's data region is always
 * a whole number of sectors IN TOTAL, but a single descriptor may split a
 * sector — the running `carry` handling below stitches partial sectors across
 * descriptor boundaries.
 *
 * NOTE (skeleton): the common FreeBSD case is page-granular, 512-aligned data
 * descriptors, for which this degenerates to one emmc_bio call per sector with
 * a full-block copy. The partial-sector stitching is the correctness backstop;
 * it is marked TODO where it needs a hardware soak test. */
static int serve_data(uint32_t is_read, uint64_t gpa, uint32_t len,
                      uint64_t *sector, uint32_t *sector_fill)
{
	/* *sector_fill = bytes already accumulated into the current partial
	 * sector's bounce buffer from a previous descriptor (0 in the aligned
	 * fast path). This function only fully implements the ALIGNED fast path;
	 * the unaligned stitch is a TODO backstop. */
	if (*sector_fill != 0 || (len % VBLK_SECTOR_BYTES) != 0) {
		/* TODO(board/correctness): implement cross-descriptor partial-sector
		 * stitching. In practice FreeBSD's virtio_blk hands whole-sector,
		 * 512-aligned segments, so this branch should not be exercised; if a
		 * guest ever trips it, fail the request cleanly rather than corrupt
		 * data. */
		return -100;
	}

	uint32_t nsec = len / VBLK_SECTOR_BYTES;
	for (uint32_t s = 0; s < nsec; s++) {
		uint64_t buf_gpa = gpa + (uint64_t)s * VBLK_SECTOR_BYTES;
		uint32_t lba = (uint32_t)(*sector + s);
		int rc;

		/* D2 fix: emmc_bio_read()/emmc_bio_write() take buf_gpa as a raw PA
		 * and dereference it directly (a real DMA-like buffer handoff, NOT
		 * routed through gmem_read()/gmem_write() the way every other guest-
		 * memory touch in this file is) — so THIS is the one place that
		 * needs its own explicit range check; a corrupt/wild data-descriptor
		 * PA here would otherwise let the guest make emmc_bio_read() scribble
		 * over HV .text/.data or a real MMIO peripheral. Checked before
		 * acquiring the eMMC lock so a rejected request doesn't even briefly
		 * hold the controller. See gpa_in_range()'s block comment for the
		 * exact bounds and what this does/doesn't protect. */
		if (!gpa_in_range(buf_gpa, VBLK_SECTOR_BYTES)) {
			g_gmem_oob++;
			vblk_bc(27, g_gmem_oob);
			return -300;                 /* guest PA outside DRAM -> S_IOERR */
		}

		/* Serialize this single controller transaction against CPU1 (design
		 * §8.1). Bounded acquire: if the debug core holds the eMMC too long we
		 * fail the request cleanly rather than stall CPU0 past the watchdog. */
		if (!emmc_lock_acquire_bounded())
			return -200;                 /* controller busy -> S_IOERR */

		if (is_read) {
			/* Read one eMMC block straight into the guest buffer. buf_gpa is a
			 * guest PA == EL2 PA (identity), 512-aligned here, Normal-WB. */
			rc = emmc_bio_read(lba, buf_gpa);
			/* PUBLISH TO PoC (root cause of "336 clean reads but GEOM finds no
			 * GPT / mount error 19", found live 2026-07-22). emmc_bio_read fills
			 * buf_gpa via CACHED EL2 stores (SCTLR_EL2.C=1) and only dsb's — it
			 * never cleans them to PoC. FreeBSD's virtio_blk treats vtbd0 as a
			 * NON-coherent DMA device: on completion it does a POSTREAD cache
			 * invalidate (dc ivac) on the buffer, expecting a real device wrote
			 * to main memory. That ivac DISCARDS our still-dirty write-back
			 * lines, so the guest refetches STALE data from PoC — every sector
			 * (incl. the GPT/superblock) comes back wrong even though the
			 * transfer "succeeded" (S_OK, IRQ delivered). Clean+invalidate the
			 * destination to PoC here so the guest's refetch sees the real
			 * sector. Exact mirror of gmem_write()'s trailing gmem_cmo(). */
			if (rc == 0)
				gmem_cmo(buf_gpa, VBLK_SECTOR_BYTES);
		} else {
			/* Bounce guest bytes into our block, then push to eMMC. Using the
			 * bounce (rather than emmc_bio_write(lba, buf_gpa) directly) keeps
			 * the eMMC FIFO source in an EL2-private, definitely-aligned buffer
			 * and isolates the guest buffer's cache state from the write path.
			 * TODO(perf): the direct form is valid too and saves a copy. */
			gmem_read(buf_gpa, bounce(), VBLK_SECTOR_BYTES);
			__asm__ volatile("dsb sy" ::: "memory");
			rc = emmc_bio_write(lba, BOUNCE_PA);
		}
		vblk_emmc_unlock();
		vblk_bc(12, (uint32_t)rc);

		/* Feed the HW watchdog between sectors: a big transfer at 400 kHz can
		 * run for seconds inside this one trap, and el2_trap only pets on entry. */
		vblk_pet_wdt();

		if (rc != 0)
			return rc;
	}
	*sector += nsec;
	return 0;
}

/* Extra completion diagnostics (breadcrumb words 14..19) for the LAST request
 * vblk_request() finished. Cheap plain stores via vblk_bc() -- purely so a
 * live session can correlate a guest-reported "hard error" against exactly
 * what the HV thinks it did: which head/chain, which PA the status byte
 * landed at, and where used->idx ended up. See the breadcrumb block comment
 * near VBLK_BC_BASE for the field list. */
static inline void vblk_diag(uint16_t head, uint32_t chain_len,
                              uint32_t data_bytes, uint64_t status_pa,
                              uint16_t used_idx_after)
{
	vblk_bc(14, head);
	vblk_bc(15, chain_len);
	vblk_bc(16, data_bytes);
	vblk_bc(17, (uint32_t)(status_pa & 0xFFFFFFFFu));
	vblk_bc(18, (uint32_t)(status_pa >> 32));
	vblk_bc(19, used_idx_after);
}

/* ------------------------------------------------------------------ *
 * ASYNC I/O OFFLOAD (ROADMAP milestone C2): single-slot mailbox handed to
 * CPU2, so the guest's QueueNotify trap (CPU0) can return immediately for
 * the common T_IN/T_OUT case instead of blocking on the eMMC PIO.
 *
 * PROTOCOL: single producer (CPU0, vblk_request() below) / single consumer
 * (CPU2, vblk_async_poll(), called from vblk_async.c's dedicated loop), one
 * fixed slot — "only ONE request in flight" is an explicit, accepted
 * simplification for this first cut (see vblk_async.h). A plain state flag
 * with `dsb sy` bracketing is therefore sufficient (no ldaxr/stlxr exclusive
 * loop needed, unlike the eMMC controller lock below, which genuinely has
 * more than one writer): CPU0 is the only core that ever transitions
 * EMPTY->POSTED, CPU2 is the only core that ever transitions POSTED->EMPTY,
 * so there is no read-modify-write race on the flag itself, only an
 * ordering requirement on the PAYLOAD around it:
 *
 *   CPU0 (producer): write payload fields -> dsb sy -> state = POSTED -> dsb sy.
 *     The first dsb ensures CPU2 can never observe POSTED with a
 *     half-written payload (no store can be reordered past it to become
 *     visible after the flag). The second is belt-and-suspenders so the
 *     flag write itself is not still in-flight when the trap returns and
 *     the vCPU resumes running on the SAME core (matches this file's
 *     existing convention of dsb-bracketing every guest-visible state
 *     change, e.g. vq_push_used()).
 *   CPU2 (consumer): dsb sy -> read payload -> ... -> dsb sy -> state = EMPTY
 *     -> dsb sy. The leading dsb is the mirror-image acquire: it orders
 *     CPU2's payload reads after CPU0's writes are guaranteed visible (both
 *     cores are cache-coherent under SMPEN — see smp.h — so this is ordering,
 *     not a cache-visibility flush). The trailing dsb+store ensures CPU0
 *     never observes EMPTY (and posts a new request) before every one of
 *     THIS request's completion side effects — status byte, used-ring push,
 *     IRQ injection — has actually landed.
 *
 * FALLBACK: if the mailbox is busy, the chain has more data descriptors than
 * the slot holds, or g_vblk_async_ready==0 (no build with vblk_async.o
 * linked and running — e.g. the gdb build, see vblk_emmc.h), vblk_async_post()
 * returns 0 and the caller (vblk_request()) finishes the request the OLD
 * way, synchronously, inline. This is ALWAYS correct — the async path is a
 * pure best-effort accelerator layered on top of the original behaviour,
 * never a hard dependency.
 * ------------------------------------------------------------------ */
#define VBLK_ASYNC_MAX_DESC   (VBLK_MAX_CHAIN - 2u)   /* data descs only */

#define VBLK_MBOX_EMPTY   0u
#define VBLK_MBOX_POSTED  1u

struct vblk_async_req {
	volatile uint32_t state;
	uint16_t head;
	uint16_t is_read;
	uint64_t sector;
	uint64_t status_gpa;
	uint32_t ndesc;
	uint64_t data_addr[VBLK_ASYNC_MAX_DESC];
	uint32_t data_len[VBLK_ASYNC_MAX_DESC];
};

/* .bss, zero-initialized -> state starts VBLK_MBOX_EMPTY at boot. */
static struct vblk_async_req g_async;
static uint32_t g_async_posts, g_async_completes, g_async_fallbacks;

/* CPU0 side. Returns 1 if the mailbox accepted the request (caller MUST NOT
 * touch the used ring / status byte / IRQ for this head — CPU2 owns
 * completion now), 0 if the caller should fall back to the synchronous path. */
static int vblk_async_post(uint16_t head, uint32_t is_read, uint64_t sector,
                            uint64_t status_gpa, struct vblk_desc *chain,
                            uint32_t first, uint32_t last_excl)
{
	uint32_t n = last_excl - first;
	uint32_t i;

	if (!g_vblk_async_ready)
		return 0;                       /* no CPU2 loop draining this build */
	if (g_async.state != VBLK_MBOX_EMPTY)
		return 0;                       /* CPU2 still finishing the last one */
	if (n > VBLK_ASYNC_MAX_DESC)
		return 0;                       /* unusually long chain: fall back  */

	g_async.head       = head;
	g_async.is_read     = (uint16_t)is_read;
	g_async.sector      = sector;
	g_async.status_gpa  = status_gpa;
	g_async.ndesc        = n;
	for (i = 0; i < n; i++) {
		g_async.data_addr[i] = chain[first + i].addr;
		g_async.data_len[i]  = chain[first + i].len;
	}

	__asm__ volatile("dsb sy" ::: "memory");   /* payload before flag */
	g_async.state = VBLK_MBOX_POSTED;
	__asm__ volatile("dsb sy" ::: "memory");   /* CPU2 busy-polls; no sev needed */

	g_async_posts++;
	vblk_bc(24, g_async_posts);
	return 1;
}

/* CPU2 side — called in a tight bounded loop from vblk_async_cpu2_run()
 * (vblk_async.c). Does AT MOST one mailbox request's worth of work per call
 * and returns immediately if the mailbox is empty. */
void vblk_async_poll(void)
{
	uint16_t head;
	uint32_t is_read, ndesc, i;
	uint64_t sector, status_gpa;
	uint8_t  status = VIRTIO_BLK_S_OK;
	uint32_t used_len = 0, data_bytes;
	uint32_t sfill = 0;
	uint16_t new_idx;

	if (g_async.state != VBLK_MBOX_POSTED)
		return;

	__asm__ volatile("dsb sy" ::: "memory");   /* acquire: see CPU0's payload */

	head       = g_async.head;
	is_read    = g_async.is_read;
	sector     = g_async.sector;
	status_gpa = g_async.status_gpa;
	ndesc      = g_async.ndesc;

	for (i = 0; i < ndesc; i++) {
		uint64_t addr = g_async.data_addr[i];
		uint32_t len  = g_async.data_len[i];
		uint64_t end_sec = sector + (len / VBLK_SECTOR_BYTES);

		if (end_sec > g_blk.capacity) { status = VIRTIO_BLK_S_IOERR; break; }
		if (serve_data(is_read, addr, len, &sector, &sfill) != 0) {
			status = VIRTIO_BLK_S_IOERR;
			break;
		}
		if (is_read)
			used_len += len;
	}
	if (is_read) g_reads++; else g_writes++;
	vblk_bc(6, g_reads);
	vblk_bc(7, g_writes);

	gmem_write(status_gpa, &status, 1);
	vblk_bc(9, status);
	{	/* mirror vblk_request()'s bc[23] ack-byte readback diagnostic */
		uint8_t rb = 0xEE;
		gmem_read(status_gpa, &rb, 1);
		vblk_bc(23, rb);
	}

	data_bytes = used_len;
	used_len += 1;
	/* Locked: vblk_request()/vblk_kick() (CPU0) can be publishing a
	 * DIFFERENT completion into this same used ring concurrently — see
	 * VBLK_USED_LOCK_PA's comment in vblk_emmc.h (this is the race that
	 * caused it). */
	vblk_used_lock_acquire();
	new_idx = vq_push_used(&g_blk.vq[VBLK_QUEUE], head, used_len);
	vblk_diag(head, ndesc + 2u, data_bytes, status_gpa, new_idx);

	g_blk.int_status |= VBLK_INT_VRING;
	vblk_inject_irq();
	vblk_used_unlock();

	g_async_completes++;
	vblk_bc(25, g_async_completes);

	/* Release the slot only AFTER every completion side effect above (status
	 * byte, used-ring push, IRQ) is visible, so CPU0 can never see EMPTY and
	 * post a new request while this one's tail is still landing. */
	__asm__ volatile("dsb sy" ::: "memory");
	g_async.state = VBLK_MBOX_EMPTY;
	__asm__ volatile("dsb sy" ::: "memory");
}

/* ------------------------------------------------------------------ *
 * D1 fix: bounded drain of the CPU2 async mailbox.
 *
 * Two CPU0 call sites need this, both BEFORE they act on it:
 *   - VBLK_R_STATUS=0 (driver reset) / VBLK_R_QUEUE_READY=0, right before
 *     vblk_reg_write() tears down vq->num/desc/avail/used (or clears ready).
 *     vq_push_used()'s num==0||used==0 guard is the last-resort backstop for
 *     a completion that lands DURING/AFTER teardown despite this drain (e.g.
 *     if CPU2 is truly wedged and the bound below is hit); this drain makes
 *     that backstop the RARE case instead of the ONLY thing standing between
 *     a reset and a half-torn-down-ring write, by actually waiting for any
 *     request already accepted by CPU2 to finish landing first.
 *   - VIRTIO_BLK_T_FLUSH (D3 fix): FLUSH used to complete unconditionally,
 *     synchronously, the instant it was seen — but an async T_OUT write can
 *     be genuinely in flight on CPU2 at that moment (the whole point of the
 *     ROADMAP C2 offload is that the vCPU keeps running while CPU2 does the
 *     PIO), so "nothing is buffered" is no longer true. Waiting here for the
 *     mailbox to empty before completing FLUSH means: by the time the guest
 *     sees the FLUSH's S_OK, every write CPU0 has ever handed to CPU2 has
 *     actually reached the eMMC (emmc_bio_write() itself is synchronous
 *     CMD24 — no further buffering below CPU2 either).
 *
 * Bounded via CNTPCT (same helpers as emmc_lock_acquire_bounded()) rather
 * than by re-checking vblk_emmc_trylock(): this isn't waiting on the eMMC
 * controller lock itself, it's waiting on CPU2's whole request lifecycle
 * (lock acquire + PIO + status/used-ring/IRQ publish), which can legitimately
 * take as long as emmc_bio_write()'s own new time caps allow (worst case a
 * little under 6 s — see emmc_bio.c). The same budget as the eMMC lock
 * acquire is used for consistency. On a timeout we give up and return
 * anyway (never hang the guest's trap): the caller's own guard (vq_push_used
 * for the reset case; emmc_bio_write() being synchronous for the FLUSH case)
 * keeps this SAFE even in that pathological case, just not perfectly
 * synchronized. */
static void vblk_async_drain_bounded(void)
{
	uint64_t start, cap;
	uint32_t i = 0;

	if (!g_vblk_async_ready)
		return;                       /* no CPU2 loop -> mailbox can't be POSTED */
	if (g_async.state == VBLK_MBOX_EMPTY)
		return;                       /* fast path: nothing in flight */

	start = read_cntpct();
	cap = vblk_ms_to_ticks(VBLK_EMMC_LOCK_TIMEOUT_MS);
	while (g_async.state != VBLK_MBOX_EMPTY) {
		if ((i++ & 0xFFFFu) == 0u) {
			wdt_note_progress();
			wdt_pet();
		}
		if (read_cntpct() - start > cap)
			break;                    /* give up — see the block comment above */
		__asm__ volatile("yield" ::: "memory");
	}
	__asm__ volatile("dsb sy" ::: "memory");   /* acquire: see CPU2's writeback */
}

/* Handle one descriptor-chain request. Returns 1 if the request was
 * completed synchronously within this call (the caller's batch IRQ below
 * must fire), 0 if it was handed off to CPU2's mailbox (which injects its
 * OWN completion IRQ later, asynchronously — must NOT be double-counted). */
static int vblk_request(struct vblk_dev *d, uint16_t head)
{
	struct vblk_vq *vq = &d->vq[VBLK_QUEUE];
	struct vblk_desc chain[VBLK_MAX_CHAIN];
	uint32_t n = 0;
	uint16_t idx = head;
	int truncated = 0;

	/* Collect the chain (bounded by VBLK_MAX_CHAIN). */
	for (;;) {
		if (n >= VBLK_MAX_CHAIN) {
			truncated = 1;
			break;
		}
		vq_read_desc(vq, idx, &chain[n]);
		uint16_t flags = chain[n].flags;
		uint16_t next  = chain[n].next;
		/* TODO: VRING_DESC_F_INDIRECT not yet handled — FreeBSD's virtio_blk
		 * only uses indirect when the ring is small/segmented; advertise it
		 * OFF (we don't set the feature bit) so the driver never sends one.
		 * Defensive: treat an indirect desc as a malformed request. */
		if (flags & VRING_DESC_F_INDIRECT) {
			vblk_bc(33, ++g_indirect_seen);
			vblk_used_lock_acquire();   /* CPU2 may publish concurrently */
			vq_push_used(vq, head, 0);
			vblk_used_unlock();
			return 1;
		}
		n++;
		if (!(flags & VRING_DESC_F_NEXT))
			break;
		idx = next;
	}

	if (truncated) {
		/* BUG FIXED HERE: a chain longer than VBLK_MAX_CHAIN used to fall
		 * through with chain[n-1] silently (mis-)treated as the status/ack
		 * descriptor. chain[n-1] is actually whatever descriptor #31 of the
		 * REAL chain is (almost certainly a DATA buffer, since the real
		 * status descriptor lives further down the chain we never got to
		 * read) — so the old code wrote our computed status byte into the
		 * middle of guest data (corruption) while the REAL ack byte, still
		 * holding FreeBSD's poison value (vtblk sets vbr_ack = -1 before
		 * every request), was never written at all -> guaranteed hard
		 * error. Never guess at the wrong descriptor: treat it exactly like
		 * the already-handled INDIRECT case (zero-length completion, touch
		 * nothing else) and count it so a live test can see this path fire.
		 * Not believed to be the failure seen with the current guest (which
		 * negotiates no VIRTIO_BLK_F_SEG_MAX, so FreeBSD caps every request
		 * to a single page-aligned data segment -- n==3 always; see
		 * vtblk_maximum_segments()/VTBLK_FLAG_BUSDMA_ALIGN), but it is a
		 * real, silent-corruption-capable bug worth closing regardless. */
		g_truncated++;
		vblk_bc(20, g_truncated);
		vblk_used_lock_acquire();           /* CPU2 may publish concurrently */
		vq_push_used(vq, head, 0);
		vblk_used_unlock();
		return 1;
	}
	if (n < 2) {                          /* need header + status at least */
		vblk_bc(34, ++g_short_chain);
		vblk_used_lock_acquire();           /* CPU2 may publish concurrently */
		vq_push_used(vq, head, 0);
		vblk_used_unlock();
		return 1;
	}

	/* desc[0] = 16-byte request header (read-only). */
	struct { uint32_t type; uint32_t reserved; uint64_t sector; } hdr;
	gmem_read(chain[0].addr, &hdr, sizeof(hdr));

	/* Live debug (2026-07-27): unconditional record of every request's own
	 * type/sector as soon as we've read the header, regardless of which
	 * branch handles it below — lets a live session see exactly what request
	 * number N was even if it falls into a path that doesn't otherwise touch
	 * g_reads, the async counters, or vblk_diag. bc[35]=type, bc[36]=sector
	 * lo32, bc[37]=running count of requests reaching this point (distinct
	 * from g_heads_popped, which also counts the indirect/truncated/
	 * short-chain early exits that never get here). */
	{
		static uint32_t g_hdrs_read;
		vblk_bc(35, hdr.type);
		vblk_bc(36, (uint32_t)hdr.sector);
		vblk_bc(37, ++g_hdrs_read);
	}

	/* desc[n-1] = 1-byte status (write-only). */
	struct vblk_desc *stdesc = &chain[n - 1];

	/* D5(d), DIAGNOSTIC ONLY (deliberately NOT enforced — see the task's own
	 * caution about (d)/(e): with no hardware access in this task to confirm
	 * FreeBSD's virtio_blk always sets these exactly per spec on every code
	 * path, rejecting on a mismatch risks turning a working guest into a
	 * hard-erroring one. Count violations instead of acting on them; a live
	 * session can check bc[29] and decide whether promoting this to a real
	 * reject is warranted). Expected: desc[0] (header) READ-ONLY, desc[n-1]
	 * (status) device-WRITABLE. */
	if ((chain[0].flags & VRING_DESC_F_WRITE) || !(stdesc->flags & VRING_DESC_F_WRITE))
		vblk_bc(29, ++g_bad_desc_flags);

	uint8_t  status   = VIRTIO_BLK_S_OK;
	uint32_t used_len = 0;               /* bytes device WROTE (T_IN data)   */
	uint64_t sector   = hdr.sector;
	uint32_t sfill    = 0;

	vblk_bc(8, (uint32_t)hdr.sector);

	if (!d->emmc_ready) {
		status = VIRTIO_BLK_S_IOERR;
	} else if (hdr.type == VIRTIO_BLK_T_IN || hdr.type == VIRTIO_BLK_T_OUT) {
		uint32_t is_read = (hdr.type == VIRTIO_BLK_T_IN);
		int posted;

		/* Live debug (2026-07-27): bc[38]=g_vblk_async_ready at the moment
		 * of this decision, bc[39]=vblk_async_post()'s own return value —
		 * distinguishes "never tried async" / "async rejected, fell to
		 * sync" / "async accepted" for whichever request bc[35..37] just
		 * captured, since g_async_posts/g_async_fallbacks alone can't tell
		 * a live session whether THIS specific request moved them. */
		vblk_bc(38, g_vblk_async_ready);
		posted = vblk_async_post(head, is_read, sector, stdesc->addr,
		                          chain, 1, n - 1);
		vblk_bc(39, (uint32_t)posted);
		if (posted) {
			/* Handed off to CPU2 (ROADMAP C2): it will do the PIO, write
			 * the status byte, push the used-ring entry and inject the
			 * completion IRQ on its own. Return immediately WITHOUT
			 * touching any of that here — the vCPU keeps running without
			 * waiting for the eMMC transfer. */
			return 0;
		}

		/* Fallback: mailbox busy (a request is already in flight on CPU2),
		 * an unusually long chain that doesn't fit the single async slot,
		 * or no CPU2 loop draining this build at all. Finish it the OLD
		 * way, synchronously, right here — always correct, just not
		 * accelerated. */
		g_async_fallbacks++;
		vblk_bc(26, g_async_fallbacks);
		for (uint32_t i = 1; i < n - 1; i++) {
			struct vblk_desc *dd = &chain[i];
			/* D5(d) diagnostic (see the comment above chain[0]/stdesc's own
			 * check — same "count, don't enforce" rationale): a T_IN data
			 * descriptor should be device-writable, a T_OUT one should not. */
			uint32_t want_write = is_read ? VRING_DESC_F_WRITE : 0u;
			if ((dd->flags & VRING_DESC_F_WRITE) != want_write)
				vblk_bc(29, ++g_bad_desc_flags);
			/* Bounds-check against advertised capacity. */
			uint64_t end_sec = sector + (dd->len / VBLK_SECTOR_BYTES);
			if (end_sec > d->capacity) {
				status = VIRTIO_BLK_S_IOERR;
				break;
			}
			if (serve_data(is_read, dd->addr, dd->len, &sector, &sfill) != 0) {
				status = VIRTIO_BLK_S_IOERR;
				break;
			}
			if (is_read)
				used_len += dd->len;     /* device wrote these bytes          */
		}
		if (is_read) g_reads++; else g_writes++;
		vblk_bc(6, g_reads);
		vblk_bc(7, g_writes);
	} else if (hdr.type == VIRTIO_BLK_T_FLUSH) {
		/* D3 fix: emmc_bio_write() itself is synchronous (a CMD24 that waits
		 * for DATA_OVER + card-not-busy before returning) and nothing is
		 * buffered BELOW that — but the ROADMAP C2 async offload means a
		 * T_OUT write CPU0 already handed off can still be genuinely IN
		 * FLIGHT on CPU2 at the moment this FLUSH is seen (that is the
		 * entire point of the offload: CPU0 returns before the PIO is done).
		 * The old comment claiming "nothing is buffered" predates that
		 * milestone and is stale. Drain the mailbox first (bounded — see
		 * vblk_async_drain_bounded()) so FLUSH cannot report success while a
		 * write is still in flight underneath it. */
		vblk_async_drain_bounded();
	} else if (hdr.type == VIRTIO_BLK_T_GET_ID) {
		/* Answer with a fixed 20-byte identifier instead of S_UNSUPP, so
		 * FreeBSD's vtblk_ident() stops logging "vtblk_poll_request: IO
		 * error: 45" / "error getting device identifier: 45" (cosmetic
		 * noise: 45==ENOTSUP from VIRTIO_BLK_S_UNSUPP). No eMMC access at
		 * all here — this path never touches the eMMC cross-core lock. */
		static const uint8_t ident[VBLK_ID_BYTES] = {
			'b','z','d','o','s','-','e','m','m','c',
			 0,   0,  0,  0,  0,  0,  0,  0,  0,  0
		};
		uint32_t off = 0;
		for (uint32_t i = 1; i < n - 1 && off < sizeof(ident); i++) {
			struct vblk_desc *dd = &chain[i];
			uint32_t avail = (uint32_t)sizeof(ident) - off;
			uint32_t cpy   = (dd->len < avail) ? dd->len : avail;
			if (cpy == 0)
				continue;
			gmem_write(dd->addr, &ident[off], cpy);
			used_len += cpy;
			off += cpy;
		}
	} else {
		status = VIRTIO_BLK_S_UNSUPP;
	}

	/* Status byte into the final (write-only) descriptor, then account it. */
	gmem_write(stdesc->addr, &status, 1);
	vblk_bc(9, status);
	{	/* bc[23]: read the ack byte BACK from PoC (gmem_read civacs first) —
		 * proves what every observer, guest included, will see there. */
		uint8_t rb = 0xEE;
		gmem_read(stdesc->addr, &rb, 1);
		vblk_bc(23, rb);
	}

	{
		uint32_t data_bytes = used_len;      /* before the +1 for the status
		                                       * byte itself, for vblk_diag */
		uint16_t new_idx;

		used_len += 1;
		/* Locked: vblk_async_poll() (CPU2) can be publishing a DIFFERENT
		 * completion into this same used ring concurrently — see
		 * VBLK_USED_LOCK_PA's comment in vblk_emmc.h. */
		vblk_used_lock_acquire();
		new_idx = vq_push_used(vq, head, used_len);
		vblk_used_unlock();
		vblk_diag(head, n, data_bytes, stdesc->addr, new_idx);
	}
	return 1;
}

/* QueueNotify handler: drain every available request, then raise one IRQ for
 * whatever completed SYNCHRONOUSLY within this call. A request handed off to
 * CPU2's async mailbox (ROADMAP C2) is NOT counted here — it injects its own
 * completion IRQ later, independently, once the PIO actually finishes. */
static void vblk_kick(struct vblk_dev *d, uint32_t qidx)
{
	struct vblk_vq *vq = &d->vq[VBLK_QUEUE];
	uint16_t head;
	int served = 0;

	vblk_bc(31, ++g_kicks_seen);

	if (qidx != VBLK_QUEUE)
		return;

	/* D5(e), DIAGNOSTIC ONLY (same caution as (d) above — not enforced: a
	 * spec-compliant driver always reaches DRIVER_OK, which itself implies
	 * FEATURES_OK already passed, before its first QueueNotify, but this
	 * task has no hardware access to confirm that holds on every FreeBSD
	 * code path, and a wrong hard-reject here would break disk I/O outright.
	 * Count instead of gating; see vnet_emac_rx_frame()'s existing DRIVER_OK
	 * check for the one place in this tree that DOES hard-gate on it today,
	 * left unchanged. */
	if (!(d->status & VBLK_S_DRIVER_OK))
		vblk_bc(30, ++g_kick_not_ready);

	while (vq_pop_avail(vq, &head)) {
		vblk_bc(32, ++g_heads_popped);
		if (vblk_request(d, head))
			served = 1;
	}

	/* B4 flight recorder: one event per QueueNotify (the guest's virtio-blk
	 * "kick") — a0=qidx, a1=served (1 if anything completed synchronously
	 * within this call, 0 if every request went to CPU2's async mailbox or
	 * the queue was empty). This is the dispatch entry, not the per-byte PIO
	 * loop (serve_data()), so cost is one log call per kick, not per sector. */
	flightrec_log(FLTR_K_VIRTIO, qidx, (uint64_t)served);

	if (served) {
		/* Locked against vblk_async_poll()'s (CPU2) own int_status update +
		 * IRQ injection for the same reason vq_push_used() above is. */
		vblk_used_lock_acquire();
		d->int_status |= VBLK_INT_VRING;
		vblk_inject_irq();
		vblk_used_unlock();
	}
}

/* ------------------------------------------------------------------ *
 * MMIO register emulation.
 * ------------------------------------------------------------------ */

/* Interrupt-path diagnostics (bc [21]/[22]): how many times the guest ISR
 * actually read InterruptStatus and wrote InterruptACK. One pair per
 * completion IRQ proves the ISR runs and sees VBLK_INT_VRING. */
static uint32_t g_isr_status_reads;
static uint32_t g_isr_acks;

/* Compute the value returned for a guest READ of register `off`. */
static uint32_t vblk_reg_read(struct vblk_dev *d, uint32_t off)
{
	struct vblk_vq *vq = &d->vq[VBLK_QUEUE];

	switch (off) {
	case VBLK_R_MAGIC_VALUE:   return VBLK_MMIO_MAGIC;
	case VBLK_R_VERSION:       return VBLK_MMIO_VERSION;
	case VBLK_R_DEVICE_ID:     return 2u;                 /* virtio-blk */
	case VBLK_R_VENDOR_ID:     return VBLK_MMIO_VENDOR;
	case VBLK_R_DEVICE_FEATURES:
		/* Modern: only VIRTIO_F_VERSION_1 (word 1, bit 0). Word 0:
		 * VIRTIO_BLK_F_SEG_MAX only (no RO, no BLK_SIZE, no MQ — keep the
		 * device otherwise minimal). Without SEG_MAX, FreeBSD's
		 * vtblk_maximum_segments() falls back to a single data segment and
		 * forces VTBLK_FLAG_BUSDMA_ALIGN, which caps every I/O at 4KB *and*
		 * requires it to land in one page-aligned physically-contiguous
		 * buffer — busdma bounces (or fails outright) any bio that doesn't,
		 * which FreeBSD reports as a generic "hard error" with no HV-visible
		 * MMIO trap at all (the request never reaches the virtqueue).
		 * Advertising seg_max=VBLK_CONFIG_SEG_MAX below removes both the
		 * size cap and the alignment requirement. */
		if (d->dev_feat_sel == VBLK_FEATWORD_HI)
			return VBLK_F_VERSION_1_BIT;
		return VBLK_F_SEG_MAX_BIT;
	case VBLK_R_QUEUE_NUM_MAX:
		/* D5(a): only queue 0 exists for virtio-blk; a nonexistent queue_sel
		 * must read back 0 (spec: QueueNumMax==0 tells the driver the queue
		 * is not available), not the same 256 every real queue advertises. */
		return (d->queue_sel == VBLK_QUEUE) ? VBLK_QUEUE_MAX : 0u;
	case VBLK_R_QUEUE_READY:   return vq->ready;
	case VBLK_R_INTERRUPT_STATUS:
		vblk_bc(21, ++g_isr_status_reads);
		return d->int_status;
	case VBLK_R_STATUS:        return d->status;
	case VBLK_R_CONFIG_GENERATION: return d->config_gen;
	default:
		/* Config space is handled by vblk_config_read() directly in
		 * vblk_mmio_fault() (D5(c) — needs the access width). Any other
		 * offset: 0. */
		return 0u;
	}
}

/* D5(c): virtio-blk config-space read, SAS-aware — exact template copy of
 * vnet_config_read() (vnet_emac.c), per the task brief. Two fields are ever
 * nonzero: `capacity` (le64 at config+0) and, since the SEG_MAX fix,
 * `seg_max` (le32 at config+12 — struct virtio_blk_config leaves +8..+11 as
 * `size_max`, unused since VIRTIO_BLK_F_SIZE_MAX isn't offered). Every other
 * config offset still reads 0 regardless of width.
 * FreeBSD's vtblk_read_config() happens to always read capacity as two full
 * 32-bit halves today (sas==2 both times — see the comment this replaces),
 * so this is a defensive correctness fix, not a behavior change for the
 * current guest: for sas==2 it returns byte-for-byte what the old two-line
 * special case did. */
#define VBLK_CONFIG_SEG_MAX  16u  /* data segments/request; well under VBLK_MAX_CHAIN */
#define VBLK_CONFIG_LEN      16u  /* capacity(8) + size_max(4, unused=0) + seg_max(4) */

static uint32_t vblk_config_read(struct vblk_dev *d, uint32_t byte_off, uint32_t sas)
{
	uint32_t nbytes = (sas == 0u) ? 1u : (sas == 1u) ? 2u : 4u;
	uint32_t v = 0;

	for (uint32_t i = 0; i < nbytes; i++) {
		uint32_t idx = byte_off + i;
		uint8_t b;
		if (idx < 8u)
			b = (uint8_t)(d->capacity >> (8u * idx));
		else if (idx >= 12u && idx < 16u)
			b = (uint8_t)(VBLK_CONFIG_SEG_MAX >> (8u * (idx - 12u)));
		else
			b = 0u;
		v |= (uint32_t)b << (8u * i);
	}
	return v;
}

/* Apply a guest WRITE of `val` to register `off`. */
static void vblk_reg_write(struct vblk_dev *d, uint32_t off, uint32_t val)
{
	struct vblk_vq *vq = &d->vq[VBLK_QUEUE];

	switch (off) {
	case VBLK_R_DEVICE_FEATURES_SEL: d->dev_feat_sel = val; break;
	case VBLK_R_DRIVER_FEATURES_SEL: d->drv_feat_sel = val; break;
	case VBLK_R_DRIVER_FEATURES:
		if (d->drv_feat_sel < 2u)
			d->driver_features |= ((uint64_t)val) << (32u * d->drv_feat_sel);
		break;
	case VBLK_R_QUEUE_SEL:
		d->queue_sel = val;             /* only queue 0 exists */
		break;
	case VBLK_R_QUEUE_NUM: {
		if (d->queue_sel == VBLK_QUEUE) {
			uint32_t v = (val > VBLK_QUEUE_MAX) ? VBLK_QUEUE_MAX : val;
			/* D5(b): QueueNum must be a power of two (or 0, queue disabled)
			 * per the VIRTIO 1.x spec. FreeBSD's virtio_blk always negotiates
			 * a power-of-two size (its segment-count-derived queue depths are
			 * themselves powers of two), so this never fires for the real
			 * guest — it exists purely to refuse an obviously non-compliant/
			 * corrupt value rather than feed it, unquestioned, into
			 * vq_pop_avail()/vq_push_used()'s `% vq->num` indexing. Leaves
			 * vq->num UNCHANGED on a bad write (does not silently substitute
			 * some OTHER size the guest never asked for — that would desync
			 * our modulo indexing from the guest's own ring layout, a worse
			 * bug than just refusing the write). */
			if (v != 0u && (v & (v - 1u)) != 0u) {
				vblk_bc(28, ++g_bad_queue_num);
				break;
			}
			vq->num = v;
			vblk_bc(2, vq->num);
		}
		break;
	}
	case VBLK_R_QUEUE_DESC_LOW:
		if (d->queue_sel == VBLK_QUEUE) { vq->desc = (vq->desc & ~0xFFFFFFFFULL) | val; vblk_bc(3, val); }
		break;
	case VBLK_R_QUEUE_DESC_HIGH:
		if (d->queue_sel == VBLK_QUEUE) vq->desc = (vq->desc & 0xFFFFFFFFULL) | ((uint64_t)val << 32);
		break;
	case VBLK_R_QUEUE_DRIVER_LOW:
		if (d->queue_sel == VBLK_QUEUE) { vq->avail = (vq->avail & ~0xFFFFFFFFULL) | val; vblk_bc(4, val); }
		break;
	case VBLK_R_QUEUE_DRIVER_HIGH:
		if (d->queue_sel == VBLK_QUEUE) vq->avail = (vq->avail & 0xFFFFFFFFULL) | ((uint64_t)val << 32);
		break;
	case VBLK_R_QUEUE_DEVICE_LOW:
		if (d->queue_sel == VBLK_QUEUE) { vq->used = (vq->used & ~0xFFFFFFFFULL) | val; vblk_bc(5, val); }
		break;
	case VBLK_R_QUEUE_DEVICE_HIGH:
		if (d->queue_sel == VBLK_QUEUE) vq->used = (vq->used & 0xFFFFFFFFULL) | ((uint64_t)val << 32);
		break;
	case VBLK_R_QUEUE_READY:
		if (d->queue_sel == VBLK_QUEUE) {
			if ((val & 1u) == 0u) {
				/* D1 fix: a QueueReady=0 write is typically followed by the
				 * driver reprogramming QueueDesc/QueueDriver/QueueDevice for a
				 * fresh negotiation (still pointing vq->desc/avail/used at the
				 * OLD, about-to-be-superseded addresses right now). Drain any
				 * CPU2 completion already accepted for THIS queue before we
				 * return from this trap, so it cannot land after the driver
				 * has moved on and started reusing/freeing that memory. */
				vblk_async_drain_bounded();
			}
			vq->ready = val & 1u;
			if (vq->ready)
				vq->last_avail = 0;      /* fresh negotiation */
		}
		break;
	case VBLK_R_QUEUE_NOTIFY:
		/* The kick. `val` is the queue index being notified. */
		vblk_kick(d, val);
		break;
	case VBLK_R_INTERRUPT_ACK:
		/* This RMW races CPU2's `int_status |= VBLK_INT_VRING` (done under the
		 * used lock): interleaved, it can clear a pending bit CPU2 just set, so
		 * the guest ISR reads InterruptStatus==0 and skips the vq scan while a
		 * completion sits unseen in the used ring. Take the same lock. */
		vblk_used_lock_acquire();
		d->int_status &= ~val;           /* driver acked these bits */
		vblk_used_unlock();
		vblk_bc(22, ++g_isr_acks);
		break;
	case VBLK_R_STATUS:
		d->status = val;
		vblk_bc(1, val);
		if (val == 0u) {
			/* D1 fix: drain any in-flight CPU2 async completion BEFORE tearing
			 * down vq->num/desc/avail/used below (bounded — see
			 * vblk_async_drain_bounded()). vq_push_used()'s own num==0||
			 * used==0 guard (kept below, unchanged) remains as the backstop
			 * for the case this drain times out on a truly wedged CPU2, but
			 * the drain makes that the rare/defensive path instead of the
			 * only thing preventing a completion from landing mid-teardown. */
			vblk_async_drain_bounded();

			/* Driver reset: clear queue state (device stays registered). Under
			 * the used lock so a concurrent CPU2 completion can't be mid
			 * vq_push_used() while num/used are torn down; with vq_push_used()'s
			 * own num/used==0 guard this closes the reset-vs-async race. */
			vblk_used_lock_acquire();
			vq->ready = 0; vq->num = 0; vq->last_avail = 0;
			vq->desc = vq->avail = vq->used = 0;
			d->int_status = 0;
			vblk_used_unlock();
		}
		break;
	default:
		/* Writes to RO/unknown registers are silently ignored. */
		break;
	}
}

/* ------------------------------------------------------------------ *
 * el2_trap dispatch entry — mirrors vconsole_handle_fault().
 * ------------------------------------------------------------------ */
int vblk_mmio_fault(struct el2_frame *frame)
{
	uint32_t esr = (uint32_t)frame->esr;
	uint32_t ec  = (esr >> ESR_EC_SHIFT) & ESR_EC_MASK;

	if (ec != ESR_EC_DABT_LOWER)
		return 0;

	/* Live debug (2026-07-27/28): unconditional counter + last-seen fault IPA,
	 * BEFORE the address-match check below — tells a live session whether the
	 * guest is still taking ANY non-UART data abort at all (proves the vCPU
	 * hasn't gone fully quiet) even when it's stopped hitting OUR window
	 * specifically. bc[40]=count, bc[41]=this fault's IPA lo32. */
	{
		static uint32_t g_dabt_seen;
		uint64_t hpfar_probe;
		__asm__ volatile("mrs %0, hpfar_el2" : "=r"(hpfar_probe));
		uint64_t addr_probe = ((hpfar_probe & 0xFFFFFFFFF0ULL) << 8) |
		                       (frame->far & 0xFFFull);
		vblk_bc(40, ++g_dabt_seen);
		vblk_bc(41, (uint32_t)addr_probe);
	}

	/* Reconstruct the faulting IPA: HPFAR_EL2[39:4] = IPA[47:12], OR the
	 * in-page offset from FAR_EL2[11:0]. (Same as vconsole/virtio.c: FAR_EL2
	 * alone is the guest VA once the guest MMU is on, which is NOT our base.) */
	uint64_t hpfar;
	__asm__ volatile("mrs %0, hpfar_el2" : "=r"(hpfar));
	uint64_t addr = ((hpfar & 0xFFFFFFFFF0ULL) << 8) | (frame->far & 0xFFFull);

	if (addr < g_blk.base || addr >= g_blk.base + VBLK_MMIO_SIZE)
		return 0;                         /* not our window */

	g_faults++;
	vblk_bc(13, g_faults);

	uint32_t isv = esr & ESR_ISV_BIT;
	if (!isv) {
		/* No instruction syndrome — we cannot decode reg/size/direction and we
		 * do not disassemble. Skip the instruction so the guest makes forward
		 * progress rather than re-faulting forever (matches vconsole). A real
		 * virtio driver access always carries ISV for these simple loads/stores,
		 * so this is a defensive corner. */
		frame->elr += 4;
		return 1;
	}

	uint32_t wnr = esr & ESR_WNR_BIT;
	uint32_t srt = (esr >> ESR_SRT_SHIFT) & ESR_SRT_MASK;
	uint32_t sas = (esr >> ESR_SAS_SHIFT) & ESR_SAS_MASK;   /* 2 == 32-bit */
	/* sas is only actually consulted below for the config-space (capacity)
	 * read path (D5(c)) — every other virtio-mmio register here is a
	 * standard 32-bit word access, exactly like vnet_emac.c's identical
	 * comment on its own vnet_mmio_fault(). */

	uint32_t off = (uint32_t)(addr - g_blk.base);

	if (wnr) {
		uint64_t val = (srt == SRT_XZR) ? 0 : frame->x[srt];
		vblk_reg_write(&g_blk, off, (uint32_t)val);
	} else {
		uint32_t val = (off >= VBLK_R_CONFIG && off < VBLK_R_CONFIG + VBLK_CONFIG_LEN)
		             ? vblk_config_read(&g_blk, off - VBLK_R_CONFIG, sas)
		             : vblk_reg_read(&g_blk, off);
		if (srt != SRT_XZR)
			frame->x[srt] = (uint64_t)val;
	}

	frame->elr += 4;   /* emulated — skip the faulting load/store */
	return 1;
}

/* ------------------------------------------------------------------ *
 * Init.
 * ------------------------------------------------------------------ */
int vblk_init(void)
{
	for (uint32_t i = 0; i < sizeof(g_blk); i++)
		((uint8_t *)&g_blk)[i] = 0;

	g_blk.base       = VBLK_MMIO_BASE;
	g_blk.config_gen = 0;
	g_reads = g_writes = g_irqs = g_faults = g_truncated = 0;

	vblk_bc(0, VBLK_BC_MAGIC);

	/* Release the eMMC-controller lock unconditionally. It lives in a fixed
	 * DRAM word (not .bss), so a warm WDT reset (which preserves DRAM) can
	 * leave it holding a stale "1" from a prior run — exactly the vconsole
	 * RX/TX-tee stale-counter class of bug. Zero it BEFORE emmc_bio_init()
	 * (which itself touches the controller) and before the guest/CPU1 exist. */
	*(volatile uint32_t *)VBLK_EMMC_LOCK_PA = 0u;
	/* Same warm-reset-safety reason: the used-ring publish lock (see
	 * VBLK_USED_LOCK_PA's comment in vblk_emmc.h). */
	*(volatile uint32_t *)VBLK_USED_LOCK_PA = 0u;
	__asm__ volatile("dsb sy" ::: "memory");

	/* Bring the eMMC up. emmc_bio_init() is idempotent and returns 0 on the
	 * card reporting ready (hardware-verified read path). */
	int rc = emmc_bio_init();
	g_blk.emmc_ready = (rc == 0) ? 1u : 0u;
	vblk_bc(11, g_blk.emmc_ready);

	/* Advertised capacity, in 512-byte sectors.
	 * TODO(board): derive from the eMMC CSD/EXT_CSD (SEC_COUNT) so the guest
	 * sees the real disk size. Until emmc_bio exposes that, advertise a
	 * conservative fixed value. The board's eMMC is 8 GiB (memory
	 * emmc-pc5-pinmux-fix): 8 GiB / 512 = 0x1000000 sectors. Using that here
	 * is safe as long as the real device is >= that; a too-large capacity
	 * would let the guest read past the end and get emmc timeouts (reported as
	 * VIRTIO_BLK_S_IOERR). Prefer reading the true count before trusting this. */
	g_blk.capacity = 15269888ULL;        /* 0xE90000 — the REAL eMMC size (mmc0:
	                                      * "memory: 15269888 blocks", see emmc-pc5-
	                                      * pinmux-fix). The old 0x01000000 (8 GiB)
	                                      * was TOO LARGE: the guest's GPT taste read
	                                      * the backup header at (cap-1)=16777215,
	                                      * BEYOND the real disk -> emmc_bio_read=-1 ->
	                                      * S_IOERR -> no vtbd0pN partitions. The GPT's
	                                      * backupLBA=15269887 also requires this exact
	                                      * mediasize to validate. THE virtio read bug. */

	return (rc == 0) ? 0 : -1;
}
