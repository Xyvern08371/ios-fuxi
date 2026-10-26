#ifndef FUXI_ADRENO_H
#define FUXI_ADRENO_H
#define FG_CACHE_SLOTS 4u
/* Timings in mach absolute ticks, CPU uses scheduler runtime (excludes sleep).
 * Surface ownership uses SweetSurface's cache lock. When the ABI is
 * enabled, a second shared mutex serializes ring and SMMU control. */
struct fg_times {
 uint64_t pin, lookup, map, clean, submit, wait, unmap, cpu;
 unsigned attempts, hits, misses, sleeps, blocks, groups;
};
/* Full-range lookup only; zero for unpublished, wrapping or unknown PAs. */
uintptr_t fuxi_gpu_nc_pointer(uint64_t pa, uint64_t bytes);
int fuxi_gpu_ready(void);
/* Cumulative ABI cache bytes (including initial map clean), plus live imports.
 * Snapshot under the GPU lock; no counter reset or ownership change. */
void fuxi_gpu_import_stats(uint64_t *cleaned, uint64_t *skipped, uint64_t *invalidated,
    uint64_t *mapped, unsigned *imports);
/* Default-off command-mode handoff. configure is called before GPU startup.
 * acquire: 1 GPU target writable, 0 deferred, -1 held. CPU writers must use
 * cpu_acquire, which drains/restores A and permanently ends double mode.
 * publish: 1 transferred, 2 back ready/deferred, 0 old path (caller kicks), -1 held.
 * retry: same results, but 0 means no ready frame; never issue a generic kick.
 * reserve/release bracket a SweetSurface cache transaction; 0 means defer.
 */
void fuxi_gpu_scanout_configure(uintptr_t pixels, unsigned enabled, unsigned check);
int fuxi_gpu_scanout_owned(void);
/* Caller-owned, zero-initialized TE observer. Use from one serialized timer;
 * independent observers share no mutable estimator state. Healthy observation
 * takes no GPU lock or writes; double-mode MMIO failure stops/restores A.
 * On success frame (low16), edge and period describe the latest observed TE
 * in mach absolute units. On failure these outputs must not be used. */
struct fg_te_observer {
 uint64_t edge, period, poll, change, raw_edge;
 unsigned frame, raw_frame, samples, initialized, nominal_us;
};
int fuxi_gpu_scanout_te(struct fg_te_observer *te);
int fuxi_gpu_scanout_acquire(void);
int fuxi_gpu_scanout_publish(void);
int fuxi_gpu_scanout_retry(void);
int fuxi_gpu_scanout_pending(void);
int fuxi_gpu_scanout_double(void);
int fuxi_gpu_scanout_cpu_acquire(void);
int fuxi_gpu_scanout_reserve(void);
void fuxi_gpu_scanout_release(void);
void fuxi_gpu_start(uint64_t framebuffer, unsigned width, unsigned height, unsigned pitch);
int fuxi_gpu_map(unsigned slot, uint64_t generation, uintptr_t source,
    uint64_t length, struct fg_times *t);
/* False means failed DMA drain: keep backing prepared/retained until reset. */
int fuxi_gpu_unmap(unsigned slot, struct fg_times *t);
/* copy: 2 submitted, 0 fallback, -1 quarantine. poll: 0 busy, 1 done,
 * -1 failed/drained, -2 failed/quarantined. Serialized by the surface lock. */
int fuxi_gpu_poll(struct fg_times *t);
int fuxi_gpu_copy(unsigned slot, uint64_t generation, uintptr_t source, uint64_t length,
    unsigned pitch, unsigned sx, unsigned sy, unsigned dx, unsigned dy,
    unsigned width, unsigned height, struct fg_times *t);
#endif
