# vcpu2, attempt 2: the stuck Active is gone, and what is left is a LIVELOCK

Build `vcpu2try2` = master at `733e56e` (vcpu2 fix + CPU2's own CNTP tick +
CPU2/CPU3 vgic breadcrumb lanes), `VCPU1=1 VCPU2=1`, `cpu@2` in the DTB.

## What changed from attempt 1

`INTID 137` (virtio-blk, SPI 105) now reads **`act=0`**. In attempt 1 it was
stuck `act=1` — acknowledged, never deactivated. Porting `vcpu1.c`'s
`gic_timer_arm_preserving_cntvoff()` tick onto CPU2 removed that, which also
confirms the mechanism: without its own tick CPU2 had no `vtimer_mask_watchdog()`
rescue for the CNTV mask self-latch, so its vCPU stopped servicing and the
HW=1 List Register it held was never EOI'd.

## What is left, measured rather than inferred

CPU2's vgic lane (new window `HVMAP_VGIC_BC_HI`, 0x5009E500) sampled four times
1.5 s apart:

```
inject_count=681281 inject_ok=681281 drop=0 pendq=0 ovf=0 cntv_gate=0
inject_count=683040 inject_ok=683040 drop=0 pendq=0 ovf=0 cntv_gate=0
inject_count=684802 inject_ok=684802 drop=0 pendq=0 ovf=0 cntv_gate=0
inject_count=686564 inject_ok=686564 drop=0 pendq=0 ovf=0 cntv_gate=0
```

**~1174 injections per second on CPU2, every one succeeding, zero drops, empty
pending queue.** Zero drops and an empty queue mean the List Registers keep
being freed — so the guest vCPU on CPU2 *is* consuming and EOI-ing every one.
CPU2 is not wedged. It is livelocked: it does nothing but service interrupts.

That reframes the whole failure. FreeBSD's AP rendezvous after `Release
APs...done.` never completes because one AP makes no forward progress, and
CPU0/CPU1's own unconsumed `vINTID 27` (both lanes show LR=0x1000001b) is a
*consequence* of them waiting, not a fault of their own. CPU0 injected 71
times, CPU1 three: neither is storming, both are parked.

Incidental but useful: CPU2's lane could not be read with
`read_words_stable()` at all — it never returns two identical samples, because
the core updates it continuously. CPU3's lane at 0x5009E580 reads instantly and
is all zero. A read that *fails* where its neighbour succeeds was itself the
first evidence that CPU2 was busy rather than dead.

## Leading hypothesis, to test rather than assume

`CNTVOFF_EL2` is banked per-PE, and `vgic_init()` deliberately writes it to 0
(see `vcpu1.c`'s comment at the call site). If CPU0's `CNTVOFF_EL2` is NOT zero
— `gic_timer_arm_preserving_cntvoff()` exists precisely because something must
not clobber it — then CPU2's guest sees a different virtual timebase from
CPU0's. A `CNTV_CVAL_EL0` the guest computed on one timebase is already in the
past on the other, so the PPI re-asserts immediately after every EOI: a storm
bounded only by how fast the loop runs, which is what 1174/s looks like.

**The measurement that settles it**: read `CNTVOFF_EL2` on CPU0 and on CPU2
(banked — needs a read executed on each core, not one debug-channel read), plus
`CNTV_CVAL_EL0`/`CNTV_CTL_EL0` from CPU2's vCPU. If the two offsets disagree,
that is the bug. If they agree, the storm's source is something else and the
next question is which INTID `inject_count` is counting.

## Also worth noting

`vcpu2.h`/`vcpu1.h` both describe CNTVOFF handling; check whether the
"preserving" tick variant and `vgic_init()`'s zeroing are actually consistent
for a *secondary* core, or only for CPU0 where the original ordering was
designed.
