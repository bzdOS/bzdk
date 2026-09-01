# internal-note: guest vblk sustained-write wedge — diagnosis + patch proposal

**Reported:** 2026-09-01. **Status:** analysis only, no commit (tree consolidating).

## Symptom

1.4G `tar` to guest `/opt` froze all guest disk I/O at 733M written. Vblk
breadcrumbs byte-frozen 30s+. Kill of the `tar` process unwedged I/O
immediately.

## Configuration at time of wedge

- `VCPU2=1`, `VCPU1=1` → CPU2 = guest vCPU (3rd), CPU1 = guest vCPU (4th)
- `vcpu2_run()` is noreturn on CPU2 → `vblk_async_cpu2_run()` never executes
- `g_vblk_async_ready` stays 0 → every guest sector I/O takes the **sync
  fallback path** on CPU0 (vblk_emmc.c's `serve_data()`)

## Root cause: sync fallback serialization + eMMC write stalls

### The sync path (when async is off)

Each 4KB page the guest writes becomes a virtio-blk T_OUT request processed
entirely on CPU0 inside the guest's MMIO trap:

```
vblk_kick() → vblk_request() → serve_data():
  1. acquire eMMC lock (test-and-set, 6s timeout, retries up to 4x)
  2. emmc_bio_read() (partial writes: read-modify-write)
  3. gmem_read() + dsb (bounce guest data)
  4. emmc_bio_write() — CMD24, polls STAR for DATA_OVER + card-not-busy
  5. release eMMC lock
  6. gmem_cmo() — 8× dc civac + ic ivau per 512B sector
```

Steps 1-5 are serialized per-sector. Under a sustained write stream the
eMMC controller's internal garbage collection (GC) triggers, stalling the
program-done signal. Each stall burns through `EMMC_WRITE_BUSY_TIMEOUT_MS`
(4s) × `VBLK_WRITE_RETRIES` (8) = **32s worst case per sector**.

### Why the 733M stall

A 1.4G tar pushes ~360,000 4KB pages → ~360,000 sector writes. At the
400 kHz init clock (~10ms/sector I/O), the expected time is ~60 minutes
for the full tar. But at 733M (~187,000 sectors), the eMMC hits sustained
GC pressure. One or more sectors hit the write-stall timeout, and the sync
path's per-sector retry budget (8 retries × 4s = 32s) manifests as the
observed "30s+ byte-frozen" breadcrumbs.

The breadcrumbs freeze because `bc[7]` (write counter) is updated AFTER the
per-descriptor loop completes — not per-sector. A long-running
`emmc_bio_write()` inside `serve_data()` holds the breadcrumb counter
static for the entire duration of that sector's stall.

### Why kill fixes it

Killing `tar` stops the write stream. The guest's virtio-blk driver drains
remaining in-flight requests. Each sector that hasn't yet hit its timeout
completes normally. The eMMC's GC pressure drops. I/O resumes.

## Why this only happens with async off

With `VCPU2=0`: CPU2 runs `vblk_async_cpu2_run()`, draining the mailbox.
CPU0's trap returns IMMEDIATELY after posting to the mailbox (line 1505:
`return 0`), never blocking on eMMC I/O. The eMMC stall happens on CPU2,
invisible to the guest's trap latency.

With `VCPU2=1`: CPU2 is the guest vCPU. The mailbox is never drained.
`vblk_async_post()` returns 0 (line 1204: `g_vblk_async_ready==0`). Every
sector blocks CPU0 for the full eMMC transfer + retry budget.

## Patch proposal

Two independent mitigations, either helps:

### A. Reduce write retry budget (conservative, targeted)

The 8-retry budget was raised from 3 based on evidence that retries rescue
real writes. But 8 × 4s = 32s is excessive for the sustained-write case —
a card doing GC will not recover within 8 retries of the same LBA.
Reduce to 4 (16s worst case), keeping the watchdog-fed bounded wait.

```diff
--- a/vblk_emmc.c
+++ b/vblk_emmc.c
@@ -733,7 +733,7 @@
  * rescue rescued writes (write_retry_ok=20). So the mechanism works and the budget
  * was simply too small. Each attempt is bounded by the write timeouts and the
  * loop pets the watchdog between them, so the cost of a larger budget is only
  * latency on a sector that is failing anyway -- against a guest that treats one
  * failed metadata write as fatal. */
-#define VBLK_WRITE_RETRIES  8u
+#define VBLK_WRITE_RETRIES  4u
```

### B. Re-enable async I/O with time-slice (structural fix)

The real fix is that async offload should not be mutually exclusive with
CPU2-as-guest-vCPU. Two options:

**B1: Dedicated async core.** Reserve CPU3 (currently the 4th guest vCPU)
for async I/O instead. The 4th vCPU gives the guest ~2% more compute
but the async path gives the guest lower tail latency on every disk I/O.
Trade: `VCPU3=0` in the build, CPU3 runs `vblk_async_cpu2_run()`.

**B2: Hybrid on CPU2.** When the guest has pending virtio-blk requests and
the mailbox is empty, CPU2 services them instead of running guest code.
This requires preemption of the guest vCPU, which ARMv8 EL2 does not
support without a timer-based context switch — significant new complexity.

**Recommendation:** B1 is the pragmatic choice. The 4th vCPU's compute
value is marginal (FreeBSD guest rarely saturates 3 cores) while the
async I/O latency improvement is measured and significant.

## Files involved

| File | Role |
|---|---|
| `vblk_emmc.c` | Sync fallback path (serve_data, vblk_request), retry budget |
| `vblk_async.c` | Async CPU2 loop (vblk_async_cpu2_run) |
| `smp.c:752-759` | CPU2 dispatch: vcpu2_run() → vblk_async_cpu2_run() |
| `emmc_bio.c` | eMMC PIO + write stall timeout |

## Not attempted (board needed)

- Live reproduction with reduced retry budget
- Measurement of per-sector eMMC latency under GC pressure
- Confirmation that the 733M boundary matches a specific GC threshold
