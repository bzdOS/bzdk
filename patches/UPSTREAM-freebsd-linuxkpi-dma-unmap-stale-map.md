# FreeBSD: `linux_dma_unmap_sg_attrs()` panics on a map that failed to be created

**File:** `sys/compat/linuxkpi/common/src/linux_pci.c`
**Found:** 2026-08-18, on a Banana Pi M64 (Allwinner A64, aarch64) running
FreeBSD 15.1 as an EL2 guest, driving a Mali-400 through drm-kmod + Mesa/lima.
**Status:** patched locally, kernel rebuilt, NOT submitted (needs the maintainer's
own account).

## Symptom

Kernel panic during GL context creation:

```
far: 0x0000000000000001      esr: 0x0000000096000004   (read, translation fault, level 0)
elr: 0xffff0000009205bc      bounce_bus_dmamap_sync + 0x34c   (busdma_bounce.c:1079)
lr : 0xffff0000007b6de0      linux_dma_unmap_sg_attrs + 0xac
panic: vm_fault failed
```

`far = 0x1` — a pointer whose value is literally `1` was dereferenced at offset 0.

## Why

`linux_dma_unmap_sg_attrs()` uses `sgl->dma_map` **without validating it**:

```c
	priv = dev->dma_priv;
	DMA_PRIV_LOCK(priv);
	...
	case DMA_BIDIRECTIONAL:
		bus_dmamap_sync(priv->dmat, sgl->dma_map, BUS_DMASYNC_POSTREAD);
```

Meanwhile `linux_dma_map_sg_attrs()` returns `0` on failure (Linux semantics) and
leaves `sgl->dma_map` in one of three bad states:

1. **`bus_dmamap_create()` fails early.** `bounce_bus_dmamap_create()` returns
   `ENOMEM` from the `dmat->segments` allocation **without writing `*mapp` at
   all**, so `sgl->dma_map` keeps whatever the caller's memory already held.
2. **`alloc_dmamap()` fails.** `*mapp` is set to `NULL` — better, but unmap still
   dereferences it.
3. **`_bus_dmamap_load_phys()` fails.** The map is `bus_dmamap_unload()`ed and
   `bus_dmamap_destroy()`ed, and `sgl->dma_map` is left **pointing at the
   destroyed map**.

A caller that runs its teardown path after a failed map — an ordinary shape, and
what drm/lima does here — then reaches `linux_dma_unmap_sg_attrs()` with a stale,
NULL, or dangling pointer. Nothing checks, and busdma dereferences it.

Case 1 matches the observed `far = 0x1` exactly: the field held a leftover `1`.

## Evidence chain

Not inferred from the panic string alone. The faulting call site was identified by
disassembling the deployed kernel:

```
+0xa0:  ldr x1, [x19, #24]      ; x19 = sgl, +24 = dma_map  (offset confirmed by DWARF:
+0xa4:  ldr x8, [x8, #112]      ;  struct scatterlist { page_link 0, offset 8, length 12,
+0xa8:  blr x8                  ;  dma_address 16, dma_map 24 }, sizeof = 32)
```

`w2 = 2` on the faulting call and `w2 = 1` on the next
(`BUS_DMASYNC_POSTREAD` then `PREREAD`) pins the path to the
`DMA_BIDIRECTIONAL` arm of the switch, matching the source line for line.

An EL2 hardware breakpoint on the call site captured `x1` — the map actually being
passed — across ~10 non-crashing calls: always a valid kernel pointer. So the bad
value is specific to the failure path, not a general layout misunderstanding.

Corroborating: the same underlying map failure otherwise surfaces harmlessly as
`EGL_BAD_ALLOC` from `eglCreateContext`. Whether it panics depends only on whether
the caller's cleanup reaches the unmap. And running with `LIMA_DEBUG=all` (which
includes `nobocache`) changes the allocation pattern enough that the map succeeds
and the whole pipeline runs — GP and PP shaders compile, a job is submitted, and
`glReadPixels` returns.

## Patch

Clear the field on every failure path, and refuse to dereference NULL:

```diff
 	/* create common DMA map in the first S/G entry */
 	if (bus_dmamap_create(priv->dmat, 0, &sgl->dma_map) != 0) {
+		sgl->dma_map = NULL;
 		DMA_PRIV_UNLOCK(priv);
 		return (0);
 	}
@@
 			bus_dmamap_unload(priv->dmat, sgl->dma_map);
 			bus_dmamap_destroy(priv->dmat, sgl->dma_map);
+			sgl->dma_map = NULL;
 			DMA_PRIV_UNLOCK(priv);
 			return (0);
@@ linux_dma_unmap_sg_attrs()
 	priv = dev->dma_priv;
+	if (sgl->dma_map == NULL)
+		return;
 	DMA_PRIV_LOCK(priv);
@@
 	bus_dmamap_unload(priv->dmat, sgl->dma_map);
 	bus_dmamap_destroy(priv->dmat, sgl->dma_map);
+	sgl->dma_map = NULL;	/* a double unmap becomes a no-op, not a UAF */
 	DMA_PRIV_UNLOCK(priv);
```

Unmapping something that was never mapped is the caller's bug. Turning it into a
panic when the guard costs one branch is the kernel's.

Arguably `bounce_bus_dmamap_create()` should also set `*mapp = NULL` on its early
`ENOMEM` return, so no caller can inherit a stale value. Left out of this patch to
keep it to one file and one behaviour change.

## Not the same as the earlier "non-PCI device" theory

An earlier note here reasoned that because the panic is in `linux_pci.c` and
Mali-400 is a platform device, the whole DMA path must be missing a platform
implementation — "upstream-scale work in drm-kmod". That was wrong. LinuxKPI's
`linux_pci.c` hosts the generic Linux DMA-API shims regardless of bus, the path is
exercised correctly, and the defect is a missing error-path cleanup of four lines.
