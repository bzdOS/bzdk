# FreeBSD LinuxKPI: three DMA defects that make drm-kmod unusable on non-coherent arm64

**Subsystem:** `sys/compat/linuxkpi/common/src/linux_pci.c` (LinuxKPI's Linux
DMA-API shims — despite the file name these are generic, not PCI-specific).
**Affects:** every LinuxKPI consumer on an architecture without coherent DMA, i.e.
essentially every arm64 SoC GPU driven through drm-kmod. Invisible on x86, whose
DMA is coherent by architecture.
**Found:** 2026-08-18/19 on a Banana Pi M64 (Allwinner A64, Cortex-A53, FreeBSD
15.1 aarch64 as an EL2 guest) driving a Mali-400 through drm-kmod + Mesa 26.2 lima.
**Status:** all three patched and hardware-verified locally. **Not yet submitted**
— needs the maintainer's own account.

The three are independent bugs but they present as one symptom, which is why they
took a while to separate: OpenGL context creation failed with `EGL_BAD_ALLOC`, and
sometimes panicked the kernel instead. With all three fixed the GPU renders.

---

## 1. `dma_alloc_coherent()` returns CACHEABLE memory

`linux_dma_alloc_coherent()`:

```c
	mem = kmem_alloc_contig(size, flag & GFP_NATIVE_MASK, 0, high,
	    align, 0, VM_MEMATTR_DEFAULT);
```

`sys/arm64/include/vm.h`:

```c
#define	VM_MEMATTR_UNCACHEABLE		1
#define	VM_MEMATTR_WRITE_BACK		2
...
#define	VM_MEMATTR_DEFAULT	VM_MEMATTR_WRITE_BACK
```

So the one allocator whose entire contract is *"no cache maintenance is required"*
hands out write-back cacheable memory and then gives its physical address to a
device. On a non-coherent architecture the CPU's stores sit in cache, the device
reads DRAM and sees stale contents, and nothing flushes anything anywhere —
correctly so, because callers of this API are entitled to assume they need not.

FreeBSD's own busdma does the right thing in the same situation.
`bounce_bus_dmamem_alloc()`:

```c
	else if ((flags & BUS_DMA_COHERENT) != 0 &&
	    (dmat->bounce_flags & BF_COHERENT) == 0)
		attr = VM_MEMATTR_UNCACHEABLE;
	else
		attr = VM_MEMATTR_DEFAULT;
```

### Evidence

lima allocates its Mali GPU page tables through `dma_alloc_coherent()`. The GPU's
own MMU then page-faulted on an address whose PTE the CPU could read back
perfectly well:

```
lima_platform_driver0: mmu page fault at 0x286000 from bus id 0 of type read on gpmmu
lima_platform_driver0:   -> PTE = 0x649371df: the page IS mapped, so the GPU is
                            not seeing the table (coherency or stale TLB)
```

That verdict comes from a probe added to lima's fault handler for this
investigation, walking the live page tables with the same macros as the mapping
path. Before reaching for a coherency explanation, the mapping code, the TLB-zap
implementation and both of its call sites were each verified byte-identical to
upstream Linux 6.12 — the tables were correct and simply not visible to the device.

### Patch

```diff
+#if defined(__aarch64__) || defined(__arm__) || defined(__riscv)
+#define	LKPI_DMA_COHERENT_MEMATTR	VM_MEMATTR_UNCACHEABLE
+#else
+#define	LKPI_DMA_COHERENT_MEMATTR	VM_MEMATTR_DEFAULT
+#endif
@@ linux_dma_alloc_coherent()
 	mem = kmem_alloc_contig(size, flag & GFP_NATIVE_MASK, 0, high,
-	    align, 0, VM_MEMATTR_DEFAULT);
+	    align, 0, LKPI_DMA_COHERENT_MEMATTR);
```

Left as `VM_MEMATTR_DEFAULT` where DMA really is coherent, so no platform gives up
write-back caching for nothing.

### A larger alternative worth considering instead

Routing coherent allocations through `bus_dmamem_alloc(..., BUS_DMA_COHERENT, ...)`
rather than `kmem_alloc_contig()` would get the attribute right *and* let busdma set
`DMAMAP_COHERENT` on the resulting map — which the current code path
(`linux_dma_map_phys_common()` → `bus_dmamap_load_phys()`) does not do. See defect
2: `DMAMAP_COHERENT`'s absence is exactly what activates the sync-list accounting.
The minimal patch above fixes the visibility bug; that refactor would fix the flag
too. Deliberately not attempted here — it is a bigger behavioural change than a
bug report should carry.

---

## 2. `nsegments = 1` makes a multi-page `dma_map_sg()` impossible

`linux_dma_tag_init()` creates the tag every LinuxKPI DMA mapping goes through:

```c
	error = bus_dma_tag_create(bus_get_dma_tag(dev->bsddev),
	    ...
	    1,				/* nsegments */
```

The consequence is not the obvious one. `nsegments` is not only the segment-list
bound; on arm64 the bounce backend also uses it to bound the per-map **sync list**,
and `map->sync_count` **accumulates across calls on the same map**
(`sys/arm64/arm64/busdma_bounce.c`, `bounce_bus_dmamap_load_phys()`):

```c
	} else if ((map->flags & DMAMAP_COHERENT) == 0) {
		if (map->sync_count == 0 || curaddr != sl_end) {
			if (++map->sync_count > dmat->common.nsegments)
				break;		/* -> buflen != 0 -> EFBIG */
```

`linux_dma_map_sg_attrs()` deliberately loads every S/G entry into one shared map
(`sgl->dma_map`, created in the first entry). So on a non-coherent map, entry 0
takes the single sync-list slot and entry 1 fails with `EFBIG` — a scatterlist of
more than one discontiguous page can never be mapped. A coherent map never touches
that counter, which is why this only bites non-coherent devices.

### Evidence

With a diagnostic `printf` on the failure path:

```
lkpi dma_map_sg: load_phys failed err=27 i=1/64 phys=0x54c02000 len=4096 nseg=-1
```

`err=27` is `EFBIG`; failure on the **second** entry of a 64-entry list, on every
run. The caller only ever sees `dma_map_sg()` returning 0, i.e. Linux's generic
"failed", with no way to tell why — which is what made the userspace
`EGL_BAD_ALLOC` unexplainable.

### Patch

`nsegments` also sizes the map allocation
(`sizeof(*map) + sizeof(struct sync_list) * nsegments`), so this is a real cost per
map rather than a free constant. It is affordable because the sync list only grows
on a **discontiguity** — physically adjacent pages extend the current entry instead
of adding one:

```diff
-	    1,				/* nsegments */
+	    LINUX_DMA_TAG_NSEGMENTS,	/* nsegments -- NOT 1, see the #define */
```

with `LINUX_DMA_TAG_NSEGMENTS 1024` (32 KiB of sync list per map, covering a fully
fragmented 4 MiB mapping). The value is a judgement call and a maintainer may
prefer a different one, or to derive it from the tag's `maxsize`.

---

## 3. `linux_dma_unmap_sg_attrs()` dereferences a map that was never created

`linux_dma_unmap_sg_attrs()` uses `sgl->dma_map` with no validation at all:

```c
	priv = dev->dma_priv;
	DMA_PRIV_LOCK(priv);
	...
	case DMA_BIDIRECTIONAL:
		bus_dmamap_sync(priv->dmat, sgl->dma_map, BUS_DMASYNC_POSTREAD);
```

`linux_dma_map_sg_attrs()` returns 0 on failure (Linux semantics) and leaves that
field bad in three distinct ways:

1. **`bus_dmamap_create()` fails early.** `bounce_bus_dmamap_create()` returns
   `ENOMEM` from the `dmat->segments` allocation **without writing `*mapp` at
   all**, so `sgl->dma_map` keeps whatever the caller's memory already held.
2. **`alloc_dmamap()` fails.** `*mapp` is `NULL` — and unmap still dereferences it.
3. **`_bus_dmamap_load_phys()` fails.** The map is unloaded and
   `bus_dmamap_destroy()`ed, and `sgl->dma_map` is left pointing at the destroyed
   map.

A caller that runs its teardown after a failed map — an ordinary shape, and what
drm/lima does — then reaches unmap with a stale, NULL or dangling pointer.
Unmapping something that was never mapped is the caller's bug; turning it into a
panic when the guard costs one branch is the kernel's.

### Evidence

```
far: 0x0000000000000001      esr: 0x0000000096000004   (read, translation fault, level 0)
elr: 0xffff0000009205bc      bounce_bus_dmamap_sync + 0x34c   (busdma_bounce.c:1079)
lr : 0xffff0000007b6de0      linux_dma_unmap_sg_attrs + 0xac
panic: vm_fault failed
```

`far = 0x1` — a pointer whose value is literally `1`, matching case 1 above. The
faulting call site was identified by disassembling the deployed kernel rather than
inferred from the panic text:

```
+0xa0:  ldr x1, [x19, #24]      ; x19 = sgl, +24 = dma_map
+0xa4:  ldr x8, [x8, #112]
+0xa8:  blr x8
```

The `dma_map` offset of 24 and the 32-byte stride were confirmed against DWARF
(`gdb -batch -ex 'ptype /o struct scatterlist' kernel.debug`), not computed by
hand. `w2 = 2` on the faulting call and `w2 = 1` on the next
(`BUS_DMASYNC_POSTREAD` then `PREREAD`) pins the path to the `DMA_BIDIRECTIONAL`
arm of the switch, matching the source line for line. An EL2 hardware breakpoint on
that exact call captured the map being passed across ~10 non-crashing calls —
always a valid pointer — so the bad value belongs to the failure path specifically.

### Patch

```diff
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

Arguably `bounce_bus_dmamap_create()` should also set `*mapp = NULL` on its early
`ENOMEM` return so no caller can inherit a stale value. Left out to keep the change
to one file.

---

## Result

With all three applied, on the same hardware that previously could not create a GL
context:

```
GL_VENDOR   = Mesa      GL_RENDERER = Mali400
GL_VERSION  = OpenGL ES 2.0 Mesa 26.2.0

PASS stage A -- the PP executed a job
PASS stage B -- GP built a tile list and PP rasterised it
[B] triangle -> centre pixel = 0 255 0 255   (want 0 255 0 255)
```

**5/5 runs, zero GPU MMU faults.** Defect 3 independently verified by six
consecutive runs with no panic where the same sequence previously killed the board.

## What is NOT fixed, and is not a FreeBSD bug

The above needs `LIMA_DEBUG=nogrowheap`. lima's growable-heap BO
(`LIMA_BO_FLAG_HEAP`) returns `ENOSYS` because `lima_heap_alloc()` is stubbed in
this port of the driver — a separate, driver-side gap, not a LinuxKPI defect.

## Reproducing without a GPU

Defects 2 and 3 need no Mali. Any non-coherent arm64 device whose driver calls
`dma_map_sg()` on a scatterlist with two or more discontiguous pages will hit
defect 2 deterministically, and any caller that unmaps after that failure will hit
defect 3. A small test module is the honest way to demonstrate both to a maintainer,
and is worth writing before submitting.
