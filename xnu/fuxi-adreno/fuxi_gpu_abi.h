/* NativeAdrenoABI platform bridge. All operations serialize with presents. */
#ifndef FUXI_GPU_ABI_H
#define FUXI_GPU_ABI_H
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Managed 16K host page with Normal WB or NC cache policy; osfmk owns pmap APIs. */
bool fuxi_adreno_host_page_ok(uint64_t pa);
/* 0 success, 1 ring busy, -1 disabled/drained, -2 failed DMA drain.
 * poll also returns -3 on watchdog timeout/drained, -4 timeout/quarantined. */
int fuxi_gpu_abi_map(uint64_t iova, uintptr_t source, uint64_t bytes,
    unsigned permissions, unsigned writeback, unsigned shareability);
/* Persistent kernel mapping of a prepared task descriptor; lifetime through unmap. */
int fuxi_gpu_abi_map_import(uint64_t iova, uintptr_t source, uint64_t bytes, unsigned permissions);
/* Ticks the last fuxi_gpu_abi_map spent holding the GPU lock (after taking it). */
uint64_t fuxi_gpu_abi_map_work(void);
int fuxi_gpu_abi_unmap(uint64_t iova, uint64_t bytes);
int fuxi_gpu_abi_submit(const uint64_t *iovas, const uint64_t *sizes,
    unsigned count, uint64_t *fence);
int fuxi_gpu_abi_poll(uint64_t fence);
/* CP always-on ticks: 0 success, -1 not ready, -2 MMIO error, -3 rollover timeout. */
int fuxi_gpu_abi_timestamp(uint64_t *ticks);
/* Monotonic boot-wide counts remain readable after permanent GPU failure. */
int fuxi_gpu_abi_status(uint64_t *faults, uint64_t *suspends);
int fuxi_gpu_abi_available(void);
/* Snapshot of the backend's IBs-per-submit [4], WPTR fence [4] and stall recovery [8] counters; see fuxi_gpu_abi_backend.inc. */
void fuxi_gpu_abi_sched_stats(uint32_t ibs[4], uint32_t wptr[4], uint32_t stall[8]);
/* fuxigpuncimport bit 512: submits queue behind a running job (in-order ring, one TTBR switch per chain). */
int fuxi_gpu_abi_queue_enabled(void);
int fuxi_gpu_abi_roimport(void);
int fuxi_gpu_abi_pace(void);
int fuxi_gpu_abi_start(void);
/* Queued submits, deepest chain, queue refusals for a waiting present, refusals for a full chain/ring. */
void fuxi_gpu_abi_queue_stats(uint32_t q[6]);
/* submit_split window (see fuxi_gpu_abi_backend.inc), taken and cleared by the caller's 600-submit report. */
void fuxi_gpu_abi_submit_split(uint64_t ns[8], uint32_t why[5], uint32_t n[2], uint64_t *lockmax_ns, uint64_t kib[2]);
void fuxi_gpu_abi_publish(void);
#ifdef __cplusplus
}
#endif
#endif
