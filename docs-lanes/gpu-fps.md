# Backboardd frame throughput

## Evidence and limits

The supplied device windows report roughly 48–144 draws, 4.9–11.3 ms median
GPU execution and 17–25 ms median `wall`, with two admitted flights. Panel and
CA tick periods are already about 8.27 ms. No device was accessed for this
change. The code identifies serialization and redundant recording, but does
not establish which dominates that workload or demonstrate 60 fps on A740.

`wall` in overlap mode is **commit entry to retirement after surface release**,
not a frame interval or an exclusive CPU cost. `completion_ms` additionally
includes waiting for scheduled-handler delivery. `interval` measures command
buffer creation cadence, excludes gaps >=100 ms, and includes client work.
Neither 1/median(wall) nor `gpu_exec / draws` is a throughput or shader-cost
measurement. The latter includes passes, image transitions, memory barriers,
clears, transfers, and GPU idle time between the two GPU timestamps.

## Why two flights need not double throughput

* `Frontend.mm` reserves a two-token semaphore in `reserveOverlap`, including
  on explicit `enqueue`, before placing work on the serial submit queue. Thus
  `overlap_depth=2` can mean one GPU flight and one worker waiting to submit.
  A third reservation blocks the caller. A queue created with a maximum count
  of one has only one token.
* `commit` copies each unique no-copy buffer into a CPU snapshot before it
  returns. `executeCommitted` copies the snapshot into its shared Vulkan
  buffer through `prepare`. **Any nonempty `prepare` drains both backend slots**,
  even if the pending flight touches unrelated buffers. Indexed asynchronous
  draws also drain both slots before CPU index validation. This is conservative
  ownership protection; simply deleting the drain races GPU reads/CPU writes.
* `Backend::submit` holds one device mutex across planning, translation/runtime
  variants, descriptor updates, recording, mapped-memory flushes and
  `vkQueueSubmit`. Conflicting noncoherent allocations or overlapping host
  ranges wait for retirement. `lease_lock` historically combines IOSurface
  lease acquisition and this entire backend lock/admission wait, so it was
  never just a mutex measurement. The supplied small lease times do not prove
  a large shadow drain in those particular windows.
* Deferred fence waits run on the serial retire queue **outside** the backend
  mutex. Retirement reacquires that mutex for query results, whole-allocation
  invalidation and destruction, releases IOSurface leases, then frees an
  admission token. Consequently CPU recording can delay retirement, and host
  cache maintenance can delay the next submit. The synchronous event-segment
  path still waits under the mutex. Signals/waits split a Metal command into
  synchronous segments; only its final segment gets deferred retirement.
* All referenced IOSurfaces receive an exclusive `SurfaceLease` in overlap
  mode, including sampled surfaces. Reusing any surface can wait for the
  preceding flight's lease release, even for two readers. Removing that
  exclusion requires a separate read/write lease and kernel ownership design.
* Scheduled handlers run inline on the submit queue. Completed handlers run
  on a separate serial delivery queue, possibly targeted at CA's supplied
  completion queue. `presentDrawable*` is a completed handler: it presents only
  after successful retirement and can wait behind other handlers. Delivery
  does not hold the backend mutex or admission token, but CA can wait for a
  callback/presentation before producing the next frame. `waitUntilCompleted`
  also includes completed-handler delivery. Delaying that queue can therefore
  throttle the producer without filling the GPU queue.
* FuxiMetal supplies no CAMetalLayer/`nextDrawable` implementation. That call
  and drawable-pool throttling belong to the external client/framework. There
  is no trustworthy direct `nextDrawable` timer to add here without interposing
  an external implementation. `commit_gap` measures the return-boundary-to-next-
  commit interval, which includes drawable acquisition, CA scheduling and CPU
  preparation. It cannot distinguish them by itself.
* The kernel `fuxi_gpu_abi_backend.inc` admits one client submission while
  `fg_client_busy`, `fg_busy` or `fg_scanout_reserved` is set. It requires an
  idle ring, switches the SMMU root, hands off imports, and rings the doorbell.
  Completion requires the fence, ring pointers and GPU idle observation,
  restores the root and invalidates imports before publishing success. Scanout
  shares this exclusion. Two Vulkan command slots do not remove this hardware
  admission bottleneck; Turnip submission behavior is external and unchanged.

The approximately equal `gpu_wait` and `gpu_exec` are consistent with a retire
worker starting its wait shortly after submission and little queued GPU work.
They do not prove `vkQueueSubmit` waits for execution. The new `queue_submit`
and `slot_wait` distinguish time inside that call from frontend admission.

## Where the apparent per-draw GPU cost comes from

`Backend::submit` records **one Vulkan command buffer and one vkQueueSubmit per
nonempty event segment**, not one per encoder. Descriptors are allocated and
updated per draw unless `fuxigpusubmit=1` finds an identical set; unique buffer
offsets and resources can still yield one set per draw. These updates cost CPU,
not GPU shader execution. Mapped-memory flushes occur once per unique resource
per submit, not per draw.

Without merging each draw starts/ends a render pass. Existing
`fuxigpumerge=1` groups safe draws within a known Metal encoder, preserving the
first load/clear and final store/discard, but previously always split different
encoders. Every resulting run has a broad ALL_COMMANDS memory barrier. Each
image transition also emits a broad image barrier even when its layout is
unchanged; merged runs deduplicate these only within that run. Render passes
have broad external dependencies as well. Around 25–30 remaining passes plus
repeated sample barriers are plausible GPU stalls; the trace alone cannot
assign 70 microseconds to an individual draw.

Default Turnip setup explicitly requests `TU_DEBUG=sysmem,nocb` (preserving any
explicit environment override). `fuxigpugmem=1` already removes `sysmem`; it
allows Turnip's choice rather than forcing GMEM on every pass. This remains an
independent experiment: existing bring-up notes record A740 GMEM hangs. Neither
Turnip nor its sources are changed here.

Clears and loads reflect Metal attachment actions. Intermediate CLEAR,
DontCare or discarded contents must not disappear. Undefined partial writes
also need their existing initialization. This change does not remove those
operations or the general inter-operation memory barrier.

The kernel already walks **all mapped imports**, since the IB list is not a
resource list, cleaning before submit and invalidating before successful fence
publication. Those CPU CMOs contribute to submit/wait latency, not directly to
the GPU timestamp range (although GPU/CPU contention can affect it). Existing
`fuxigpuncimport=1` cleans GPU-writable imports once at map and skips subsequent
submission cleans, while retaining completion invalidation and read-only
cleans. It requires no CPU writes for the entire mapped lifetime, including
aliases and fallback rendering. Permissions do not prove that contract; it is
not safe to enable automatically or drop all completion CMOs. See
[cache ownership](unixnu-port.md#cache-ownership-and-diagnostics-fuxigpuncimport1).
The existing `FUXI GPU: presents` cleaned/skipped/invalidated byte deltas expose
the volume. No kernel ownership change is made in this patch.

## Opt-in reductions

One new key, `fuxigpuperf=N`, accepts an exact decimal digit 0–3. Missing,
duplicate, malformed and unknown values disable it. Defaults are unchanged.
A space plus `fuxigpuperf=3` consumes **14 command-line bytes**; check the actual
608-byte budget rather than appending a collection of additional keys.

| Value/bit | Change | Retained protection |
| --- | --- | --- |
| 1 / bit 0 | Skip SHADER_READ_ONLY_OPTIMAL → same-layout image barriers | Real layout changes, GENERAL/attachment/feedback barriers, initial host barrier and every inter-operation barrier remain. Transfers/alias writers still have the global dependency. |
| 2 / bit 1 | Allow safe runs across different known encoder IDs, with `fuxigpumerge=1` | Identical attachments/mips/area and STORE→LOAD; existing storage-write, alias, shadow-producer, attachment-sample, transfer and unknown-encoder splits remain. First clear and last store/discard survive. |
| 3 | Both reductions | Same restrictions as the individual bits. |

Compare absent/0, 1, 2, 3 with the existing workload and existing keys fixed.
Bit 0 also works without merge. Bit 1 does nothing without merge. These are
recording optimizations, not permission to overlap host writes with GPU reads.

## Reading the next device trace

All appended timing pairs are p50/p90 **milliseconds**. Existing fields and
window bounds stay intact. Frontend timing below is for overlap mode; zero
means unavailable/not observed where appropriate.

| Fields | Interpretation |
| --- | --- |
| `commit_call`, `admission_wait`, `snapshot` | Commit entry to its return boundary; semaphore wait (also measured on explicit enqueue); CPU snapshot work. Admission on an earlier enqueue need not lie within commit_call. |
| `commit_gap` | Previous commit's return boundary to this commit's entry on the same queue; idle gaps >=100 ms and overlapping callers yield zero. Boundary is immediately before releasing commitReady, since recording after return is impossible. |
| `submit_queue` | Return boundary to execution on the serial submit worker. Explicit enqueue/commit order can enlarge this. |
| `backend_lock`, `slot_wait`, `wait_reasons=drain,conflict,slots` | Device mutex acquisition vs backend condition-variable wait. Reason counters describe conditions observed before waiting, can overlap, and are not separate durations. |
| `flush`, `queue_submit` | Vulkan mapped-memory flush CPU time vs actual successful vkQueueSubmit call CPU time. Both are included in backend_submit. |
| `gpu_exec`, `gpu_wait`, `submits/frame` | GPU timestamps, host fence wait, and successful Vulkan submission count. More than one submit suggests event segmentation. |
| `retire_lock`, `invalidate`, `retire_host`, `surface_release` | Reacquiring the backend mutex after the deferred fence, mapped-memory invalidation, host retirement including invalidation/query/destruction (excluding lock wait), and IOSurface lease release. These overlap; do not add invalidate to retire_host. |
| `scheduled_handlers` | Inline scheduled-handler batch on the submit worker. A slow batch stalls later submissions and completion publication. |
| `prior_delivery`, `prior_handlers`, `prior_wait_completed` | Last observed queue delivery delay, completed-handler runtime (including drawable presentation), and explicit waitUntilCompleted duration, sampled at commit entry. These are atomic queue observations, can repeat, and are **not** attributed to the current frame. Zero can mean no observation yet. |
| `image_barriers/frame`, `read_barriers_skipped/frame`, `cross_encoder_merges/frame` | Actual image barriers emitted, read barriers omitted by bit 0, and encoder boundaries removed by bit 1. Global memory barriers are not part of image_barriers. |
| `render_passes/frame`, `merged_draws/frame`, `merged_splits`, `interval` | Check that passes and encoder splits fall without losing draws, then compare actual frame cadence, not reciprocal wall latency. |

Large commit_gap with small admission/submit-queue/backend waits points outside
submission (CA work, drawable availability, callback or tick scheduling).
Large prior_wait_completed or prior_handlers supports a completion-driven
producer. Large slot_wait plus drain/conflict reasons supports serialization
inside the frontend. Large queue_submit supports driver/kernel admission or
cache handoff cost. Reduced barriers/passes without reduced gpu_exec points
toward shader/bandwidth workload or unchanged pass boundaries; improvement in
gpu_exec without cadence improvement points back to CPU/presentation pacing.

## Host verification

The CI metal target invokes `test-fuxi-metal-frontend.py`. On Linux this builds
and runs portable C++ models under ASan/UBSan and explicitly skips native
Foundation/Metal Objective-C runtime tests; `tools/ci/build.sh` requires macOS
for its signed iOS bundle build. Native overlap tests now also assert the new
commit/admission/handler attribution, but cannot execute on this Linux host.

`test-fuxi-metal-merge.py` compares exact authored pixels against the unchanged
path for each bit independently and together, with overlap/feedback, blending,
load/clear/discard, CPU shadows, transfer and mip hazards, rollback and two
flights. Its separate Vulkan proxy counts actual image barriers/submissions,
checks render-pass compatibility and counts encoder merges. It also hides the
ordered-access extension to exercise fallback. Bad/duplicate values retain the
old policy. The benchmark keeps shader/descriptor/merge settings fixed and
varies encoder boundaries and the new mask. Host timings are observations,
not A740 predictions or pass thresholds.

Passed on Linux for this change:

* `python3 tools/test-fuxi-metal-frontend.py`
* `python3 tools/test-fuxi-metal-backend.py --headers <vulkan-headers>`
* `python3 tools/test-fuxi-metal-frametrace.py`
* `python3 tools/test-fuxi-metal-merge.py --glslang <host-validator>`
* `python3 tools/test-fuxi-metal-overlap.py --glslang <host-validator>`
* `python3 tools/test-fuxi-metal-mipfeedback.py --glslang <host-validator>`
* `python3 tools/test-fuxi-metal-m6.py --headers <vulkan-headers> --glslang <host-validator>`
* `python3 tools/test-fuxi-metal-m9m.py`, `test-fuxi-metal-m9n.py`,
  `test-fuxi-metal-m9o.py` (M9o's optional external-translator shader controls
  skipped; local memory/ownership controls passed).
* `python3 tools/test-fuxi-metal.py`, `test-fuxi-metal2.py`,
  `test-fuxi-metal2g.py`, `test-fuxi-metal2h.py`.
* `python3 tools/test-fuxi-gpu-k1.py`, `test-fuxi-gpu-clock.py`,
  `test-fuxi-gpu-probe.py`, `test-fuxi-gpu-scanout.py`.
* `python3 tools/sync-fuxi-metal.py --check` and `git diff --check`.

External-translator/Apple-input qualification suites were not run: no matching
external translator or Apple artifacts are present. The new optimization tests
use the existing authored GLSL adapter and local software Vulkan instead.
