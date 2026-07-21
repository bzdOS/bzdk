# Chimp — beyond bring-up: capability roadmap

Speculative/forward-looking notes, separate from PROGRESS.md (which tracks the
current bring-up state) and DEBUG_RULES.md (which governs how to work day to
day). This file is about what the trap-and-emulate substrate is *for*, once
FreeBSD boots reliably — ranked by how directly it builds on code already in
this tree, not by how impressive it sounds.

## Tier 1 — cheap, direct extensions of what already exists

1. **Network-backed root filesystem via virtio-blk.** `virtio.c`/`virtio_blk.c`
   already exist in the tree (see PROGRESS.md's "delivered modules awaiting
   integration"), just not wired up. Finishing this means FreeBSD can mount
   root from a file on the host over the network — no SD card writes during
   iteration. Highest ratio of (boot-speed win) to (new code needed).
2. **Generalize vconsole's trap-and-log pattern to arbitrary MMIO ranges.**
   `vconsole_handle_fault`/`gtrace_handle_sysreg` are each a clean
   "syndrome → record + emulate" function. Add a config table of
   (PA-range → logger) instead of one hardcoded UART case, and you get
   free, no-guest-recompile instrumentation of any driver's register
   traffic — this is what made the two UART bugs findable this session; make
   it general-purpose.
3. **Fix the CNTP timer-sharing bug properly** (already delegated to a
   sub-agent this session — see its report). Prerequisite for anything below
   that needs the guest's clock/callout subsystem to actually work
   post-cold-boot.

## Tier 2 — needs new infrastructure, but is a natural next step

4. **Record/replay of the full trap stream.** Once traps are logged
   generically (#2), persist the sequence (addr, dir, value, cycle count) to
   a ring or over the network. Replaying it byte-for-byte reproduces an
   intermittent boot bug deterministically, on a machine that doesn't even
   have the board attached. Foundation for #5 and #7.
5. **Symbolic execution over a recorded trap trace.** Feed the recorded
   sequence from #4 into something like `angr`: "what if THR read X instead
   of Y — which code path does the guest take?" — answered offline, without
   touching hardware. This is the direct fix for this session's actual
   pain point (every hypothesis cost a board cycle to check).
6. **Live kernel patching via stage-2 unmap.** `stage2_unmap_guest_vector()`
   already proves the technique (unmap a guest page, catch execution on it,
   redirect). Generalize it to redirect a *specific hot function's* page to
   a hypervisor-resident replacement — a from-scratch, minimal DTrace/eBPF
   for the guest, no FreeBSD source changes.
7. **Fault injection for driver robustness testing.** Once #2 exists,
   deliberately return corrupted/delayed/error responses from emulated
   devices and watch whether FreeBSD's drivers degrade gracefully — testable
   failure modes real silicon can't be made to produce on demand.

## Tier 3 — the actual research-frontier bets

8. **Agentic debugging loop.** What happened in this session — read
   breadcrumb rings, form a hypothesis, verify against exact source, patch,
   reflash, repeat — done by an agent that decides *where to look next*
   itself, using #2's generic instrumentation and #5's offline
   hypothesis-checking to avoid burning board cycles on dead ends. The
   structured (ring-buffer, not free-text-log) interface to guest state is
   what makes this tractable here; most bring-up projects only have serial
   console text to feed a model.
9. **Verify the trap-handler perimeter, not the whole hypervisor.** Full
   seL4-style verification of everything is not in scope. But each handler
   (`vconsole_handle_fault`, `gtrace_handle_sysreg`, …) is already a small,
   pure "syndrome in → emulated ARM-architectural effect out" function —
   right-sized for a modern model checker to prove against the ARM ARM's
   actual encoding tables, one handler at a time. Cheap relative to full
   hypervisor verification because the TCB is already small by construction.
10. **Capability-hardware framing (CHERI-style), as a design lens, not a
    port.** The A64 has no capability hardware — this isn't buildable here.
    But it's worth periodically asking, per new trap handler: "would this
    trap disappear if pointers carried hardware-enforced bounds/permissions
    instead of being caught after the fact by stage-2?" — useful for judging
    which traps are inherent to the emulation and which are compensating for
    the CPU's lack of finer-grained hardware isolation.
11. **Debug-transparent hypervisor as a first-class security property, not a
    hole.** Confidential computing (SEV-SNP/TDX/ARM RME) hides the guest
    from the host. The inverse, interesting problem: give a guest's own
    developer full, audited transparency into *this specific* hypervisor's
    behavior on demand, without weakening isolation for anyone else. Nobody
    builds this bottom-up the way this project incidentally already has the
    scaffolding for (breadcrumb rings are, structurally, an audit log).

## Sequencing note

Tier 1 items are worth doing regardless of which Tier 3 bet (if any) pans
out — they're useful on their own and each is a prerequisite for at least one
Tier 2/3 item. Don't start on Tier 3 before at least #2 and #3 are done;
everything past that point assumes generic trap logging and a working guest
clock.
