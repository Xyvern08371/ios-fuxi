# GPU-SCANOUT: finished IOSurface to command-mode panel

2026-10-23. Offline implementation and host qualification. No phone access,
firmware, device log, deployment, push or CI run. Rebuild **both the kernel and
FuxiMetal** for this option. NativeAdreno ABI 2, the external translator pin and
external Mesa build remain unchanged. No Mesa/translator/GPL code is added.

## Exact current path and cost

The relevant final sources are `tools/fuxi-metal/Frontend.mm`,
`tools/fuxi-metal/Backend.hpp`, `xnu/fuxi-adreno/fuxi_gpu_abi_backend.inc`,
`xnu/fuxi-scanout/SweetSurface.cpp` and
`xnu/fuxi-unixnu/SweetFramebuffer.cpp`. The framebuffer/surface baseline comes
from patches 0068, 0079, 0082, 0087, 0093, 0096, 0098 and 0106.

1. CA/backboardd wraps its composed BGRA IOSurface with
   `newTextureWithDescriptor:iosurface:plane:`. FuxiMetal retains it and imports
   its pages as a Vulkan linear image. This path has **no full-frame upload or
   readback memcpy**. No-copy *input* buffers can separately use M9 shadows;
   those are not the composed IOSurface-to-panel copy.
2. `FuxiCommandBuffer::commit` synchronously calls `Backend::submit`, including
   `vkQueueSubmit`, a bounded `vkWaitForFences` and host invalidation. Drawable
   presentation callbacks run after completion. Turnip submits through ABI 2.
   Imported mappings are prepared/wired. The kernel uses CIVAC over every mapped
   import before submit, then IVAC after CP fence + both-thread idle, before
   returning fence success; watchdog retirement performs the same handoff.
3. CLCD selector 5 copies the 0x46c-byte swap record into a **latest-frame** slot.
   Selector 6 SwapWait is still a stub, not a display fence. A nominal 60 Hz
   workloop tick consumes the newest record; `sweetfb_hz` can reduce this rate.
   It crops logical 1125x2436 to panel 1080x2400, source origin **22,18** for a
   full logical frame. Pitch comes from the surface; no scaling or swizzle.
4. The existing `sweet_surface_gpu_copy` already supports a one-layer BGRA
   blit, with four prepared backing-cache entries and a retained in-flight
   source. However, the default Metal wrapper holds a writable IOSurface lock
   for its **entire texture lifetime**. `gpuWriters != 0` rejects that blit.
   A busy Turnip client also rejects it. Both cases normally reach the CPU
   path. Therefore “backboardd rendered on GPU” alone does not establish GPU
   presentation; the per-frame/summary path counters below establish the copy.
5. CPU fallback runs **2400 row memcpys of 4320 bytes** for a full panel. Source
   is ordinary cached IOSurface RAM; destination is the existing firmware
   framebuffer at physical 0xb8000000, mapped write-combined/Normal NC by
   SweetSurface. A full copy reads and writes **10,368,000 bytes each**:
   at least **20,736,000 bytes of traffic/frame**, excluding cache/padding and
   other rendering work. That is 62.208–559.872 MB/s at 3–27 frames/s, or
   1.24416 GB/s at 60. A 1125x2436 source has 10,962,000 visible bytes before
   cropping; row/allocation padding is not copied. CPU cost cannot be derived
   from CPU GHz alone. For *illustrative effective aggregate bandwidth* of
   2/5/10 GB/s, that traffic alone costs 10.368/4.147/2.074 ms. These are
   estimates, not measured Fuxi bandwidth or device copy times.
6. The old GPU path maps prepared source pages into four 16 MiB SMMU windows,
   reuses generation-checked mappings, force-cleans intersecting CPU cache
   lines to PoC, then emits one batch. Pitch units require interleaved row
   classes (at least two, at most sixteen); both pitches must become 64-byte
   multiples. A normal aligned full-panel pitch needs two rectangle blits.
   Resolve/CCU/UCHE clean events precede its timestamp fence. Poll observes
   fence, RPTR/WPTR and idle; 100 ms failure uses the existing isolation and
   quarantine policy. Its old wait measurement includes up to a 16.667 ms
   display-tick observation delay. The existing 600-frame split statistics
   already report pin, PFN lookup, map/TLBI, clean, submit, wait and CPU runtime.
7. ABL leaves INTF1 command-mode tear autorefresh on. The generic
   `fuxi_display_kick` skips manual CTL START in that case (0087). Thus an old
   in-place CPU **or GPU** copy can be read by the display before completion;
   merely delaying that kick does not establish display ownership. NC scanout
   needs no CPU writeback; DSB orders writes but cannot stop autorefresh.

`fuxi_unixnu_diff=1` can replace eligible CPU copies with the already documented
shadow-diff stores; it is a separate default-off option. Neither that option
nor the new frame logs converts the SwapWait stub into a retirement protocol.

## New option and ownership sequence

`fuxigpuscanout=1`, with **`sweetfb_noconsole=1`**, enables the new handoff.
Continue using the existing loader GPU reservation, `fuxigpu=1 fuxigpuabi=1`,
backboardd admission/configuration and `fuxibbgpu=1` (or survey mode 2). No root
image, userland policy, GPU vote, firmware or build pin is changed here.
Missing quiet-console opt-in logs `unavailable` and keeps the existing path.
Before GPU readiness the old path remains available. With the option absent,
all existing presentation behavior and lifetime surface locking remain.

The new FuxiMetal wrapper unlocks an imported surface after successful import,
while retaining its use count, memory and object. It takes a deduplicated writable
IOSurface lease for every participating imported texture/view across each
synchronous submission, including completion/cache retirement. CPU texture
transfers take the same lease. Normal completion and pre-submit failure release
it **before callbacks**. Unknown GPU ownership preserves the lock and allocation.
The surface writer trap waits on the producer thread for any in-flight display
copy; the display callback never sleeps. A retained texture is consequently
eligible for display copying between submissions. A matching new bundle matters:
an older bundle's permanent writable locks can defer this lane indefinitely.

After GPU readiness, CLCD claims the inherited display:

- Suppress generic console/manual kicks, clear INTF1 AUTOREFRESH_CONFIG bit 31,
  preserve the other bits, and allow **20 ms** for the inherited transfer.
- Only acquire the framebuffer when CTL START is idle and the INTF1 write-line
  value is zero or at least 2400. Check that autorefresh remains disabled.
  This uses the same register facts as the existing Fuxi inherited-display
  probe; no GPL implementation is copied. There is **no new DSI command,
  panel reset, address switch or display SMMU takeover**.
- Under the surface lock reserve the idle GPU ring across cache lookup,
  preparation, mapping/TLBI and submission. Observe a completed client first,
  including its cache retirement/root restoration. A live producer/client
  defers the latest swap rather than triggering a CPU scan copy. Client submit
  returns busy while the short reservation is held, then the normal in-flight
  copy flag excludes client work until the copy fence retires.
- GPU blit the existing prepared pages into the unchanged scanout buffer using
  the existing BGRA/stride/crop encoder, PoC handoff and clean-before-fence
  epilogue. Retain the exact source through retirement and optional comparison.
- After successful copy-fence retirement, DSB and issue **one manual CTL START**.
  The display is allowed to read only now. Wait another 20 ms plus idle-status
  observation before any next framebuffer write. CPU fallback and power-off
  black fill use the same acquire/publish guard. Black is explicitly sent once.

While copying/draining, poll at roughly 1.04 ms (nominal vsync period / 16).
Vsync counts/notifications retain their separate 60 Hz deadlines; fast polls do
not manufacture extra vsyncs. The demand-timer option still disarms when idle.
An uncertain display drain/change/error holds the panel and stops framebuffer
writes. Failed GPU DMA drain also holds it; no CPU fallback over uncertain DMA.
A confirmed GPU drain can replay through CPU. These paths do not restore AR or
retry panel initialization; a reboot without the new option restores baseline.

The 20 ms guard is deliberately conservative, **not a qualified TE/done IRQ**.
Even with a very fast copy, it limits throughput to below 50 presentations/s;
GPU completion/poll and scheduling reduce that further. It must be timed and
visually qualified on Fuxi. This source increment establishes no FPS, latency or
power improvement. Direct scanout of the composed allocation is not selected:
its pages can be noncontiguous, its pitch/crop differs, and there is no qualified
DPU buffer-address/mapping/latch/retirement API in this driver.

The physical default framebuffer IOSurface remains exported for ABI compatibility.
This lane assumes CA uses its offscreen composed surface and that no other client
writes directly to scanout. It does not revoke existing user mappings or control
uncooperative direct writers; those remain outside this ownership contract.

## Diagnostics and exact log forms

- `fuxigpuscanoutlog=1`: per-copy logs and a cumulative summary every **120**
  completed copies, independently of the new handoff. Use this alone for the
  existing-path baseline. The handoff enables these logs automatically.
- `fuxigpuscanoutcheck=1`: with the handoff, compare **all pixels of the first
  four successful GPU copies** against the exact CPU row-copy oracle. It reads
  the retained source at its crop/pitch and scanout with aligned volatile 32-bit
  loads, including alpha. It does not overwrite scanout with a CPU result.
  Mismatch disables the GPU, drains/quarantines and never publishes that GPU
  result; a confirmed drain permits CPU replay. No other check value enables it.

Successful full-panel forms (`N`/`T`/`P`/`A`/`M`/`D` are runtime numbers):

```text
FUXI MTL: GPU scanout surface locks per submission
FUXI SCANOUT: requested, fence then manual transfer, drain_us=20000 check=4
FUXI SCANOUT: owned, autorefresh disabled; draining inherited transfer
FUXI SCANOUT: pixel frame=1 pixels=2592000 mismatches=0 first=0,0 check_us=T
FUXI SCANOUT: frame=N path=GPU bytes=10368000 copy_us=T prepare_us=P
FUXI SCANOUT: summary frames=120 GPU=N CPU=N copy_avg_us=A copy_max_us=M deferred=D
```

Pixel lines repeat for frames 2, 3 and 4 only. Without the checker the request
line ends `check=0` and there are no pixel lines. Baseline/valid fallback copies
log `path=CPU ... prepare_us=0`. `frame` and summary count completed copies,
not proof that the panel displayed them. GPU `copy_us` is elapsed wall time
from doorbell return through observed retirement (including observation delay
and optional comparison); `prepare_us` is the preceding surface/map/clean/submit
call. CPU `copy_us` measures the CPU scan copy/diff operation. The guard wait,
compositor GPU render time, latest-frame queue delay and transfer duration are
not included in copy time. `deferred` counts callback deferrals, not dropped
frames; consult existing `SWEET FB: stats` for replaced swaps. `check_us` exposes
the optional slow device-memory readback cost; disable checks for benchmarks.

The ready line labels the programmed GPU vote and uses GPU-CLOCK’s selected
rate in kHz. With the default level:

```text
FUXI GPU: ready, BGRA strided rects + one fence, 4 cached backings, requested 220000 kHz GPU vote (not measured), async one-in-flight, no timer sleep
```

Refusal/failure forms:

```text
FUXI SCANOUT: unavailable, requires sweetfb_noconsole=1; existing present path
FUXI SCANOUT: stopped reason=transfer drain timeout; panel held, no framebuffer writes
FUXI SCANOUT: stopped reason=autorefresh changed; panel held, no framebuffer writes
FUXI SCANOUT: stopped reason=GPU DMA drain failed; panel held, no framebuffer writes
FUXI GPU: disabled permanently: scanout pixel mismatch (0x604), CPU present
```

Read/disable/status/START access failures also print the same stopped form with
`read autorefresh`, `disable autorefresh`, `read transfer status`,
`publish transfer status`, or `start transfer`. Do not treat a completed-copy
line or a zero-mismatch readback alone as panel/DSC qualification.

## Provenance, patch ownership and checks

Read-only reference: UniXNU superproject
`fb6aad6ab1f50199a4d99d2da1d0ca403b067633` and its populated NativeAdreno and
NativeMetal sources. `GpuScanoutPresenter.h` imports a finished BGRA frame and
scanout, GPU-copies matching/strided rows and waits for completion;
`GpuScanoutClient.h` verifies pixels and completes/abandons the offered frame.
Those files declare **MIT**, despite the superproject's BSD-2-Clause-Patent
licence. The adapted comparator retains its SPDX/copyright/provenance, with the
MIT licence in `xnu/fuxi-adreno/LICENSE.NativeMetal` and generated kernel patch.
The local NativeMobileDisplay directory is empty; its earlier pinned source
contract is recorded in [P-UNIXNU](unixnu-port.md), whose BSD notices remain.
No new upstream display source was fetched or executed. NativeAdreno's existing
ABI/fence/import machinery is reused. Fuxi's panel gate and SurfaceLease are
original implementation; no sheng register ownership or private ABI is assumed.

Canonical GPU sources and new include/licence are synchronized into **0098**
by `sync-fuxi-metal.py` (0096 remains standalone); **0100** remains synchronized.
Canonical framebuffer changes regenerate **0106** with `sync-fuxi-unixnu.py`.
**0108** contains the final SweetSurface changes and guarded console kick,
regenerated/checked by `sync-fuxi-scanout.py`. `--check` validates their generated
bytes and patch application. There is no Mesa patch to regenerate.

Host commands (warnings as errors and ASan/UBSan where applicable):

```sh
python3 tools/sync-fuxi-adreno-shared.py --check
python3 tools/sync-fuxi-unixnu.py --check
python3 tools/sync-fuxi-scanout.py --check
python3 tools/test-fuxi-gpu-scanout.py
python3 tools/test-fuxi-adreno.py
python3 tools/test-fuxi-metal.py
python3 tools/test-fuxi-hostimport.py
python3 tools/test-fuxi-gpu-k1.py
python3 tools/test-fuxi-unixnu.py
python3 tools/test-fuxi-metal-frontend.py
```

The scanout suite compiles the actual gate, final surface cache and framebuffer
methods: first/subsequent transfer drain, no START before fence, no generic kick
while owned, busy/reservation exclusion, no write during deferral/blanking,
fail-closed access/timeout/AR changes, full 1080x2400 centered CPU pixel oracle,
alpha/first/last mismatch, padded small rectangles, first-four bound and summary.
The existing submit suite independently decodes actual PM4 and checks 1022
pixel-exact copies, pitches/phases, noncontiguous mappings, cache ranges,
fence/deadline and quarantine. Portable SurfaceLease tests cover opt-in parsing,
deduplication, rollback, cap and poison retention. Mac `ScanoutFrontendTests.mm`
is wired into both normal and compile-only harness modes; it covers actual
import/view/transfer/commit/callback selectors with modeled surface locks.

Optional `--xnu-buildtree "$XNU_BUILD"` on the scanout suite checks complete
`fuxi_adreno.c`, `SweetSurface.cpp` and `pe_serial.c` against saved XNU
headers/flags, applying only required console text deltas in a temporary tree.
The unixnu suite's same option checks the full framebuffer. These checks passed
on Linux with the saved 8796 tree; no saved-tree script or reference was run.
Native Mac frontend compilation/execution, complete kernel link/build, real
GPU/cache/DPU/TE/DSC behavior and timing remain unverified.

## Maintainer device qualification plan (not performed here)

1. Run the complete Mac frontend harness normally and with `--compile-only`,
   build/sign the matching kernel and FuxiMetal, retain existing ABI-2 Turnip
   and external translator. Keep the existing backboardd admission, strike
   handling and survey-first policy; do not admit SpringBoard.
2. Baseline boot with the new handoff **absent** and
   `fuxigpuscanoutlog=1`, normal existing GPU/Metal arguments and quiet console.
   Record copy path counts/times, rendering `gpu_ms`, late frames, dropped swaps,
   CPU/power/thermal behavior over equivalent interaction. Confirm the default
   lifetime-lock path still works. Do not interpret CPU GHz as GPU MHz.
3. Fresh boot with `fuxigpuscanout=1 fuxigpuscanoutcheck=1
   sweetfb_noconsole=1` and existing GPU/backboardd arguments. Require the Metal
   per-submission lock line, ownership line, **four zero-mismatch full-panel
   comparisons**, increasing `path=GPU` copies and 120-copy summaries. Inspect
   text/alpha/gradients, animation, rotation/crop edges and static DSC stability.
   A stuck/mismatched/held panel, timeout, permanent deferral or any GPU fault
   fails qualification. Reboot without the option; do not blindly re-enable AR
   or repeat manual START while a transfer may be active.
4. Reboot with the checker absent for sustained timing/power comparison. Account
   for the 20 ms guard and roughly 1 ms active polling. Require a meaningful CPU
   cost reduction and acceptable cadence; do not promise 60 fps with this guard.
   Validate 60 Hz notification counts separately from active polling. Exercise
   lock/unlock, power-off black transfer, rapid wake/swap replacement, idle
   demand timer, texture reuse/destruction and producer/display contention.
5. In a separately controlled fault run, verify ordinary confirmed GPU failures
   replay safely on CPU, unknown DMA/display ownership holds writes, existing
   failure/strike CPU recovery remains effective, and the next no-option boot
   restores the inherited path. No device fault test was run for this change.

## Drain floor and wait counters (`fuxiscandrain=<us>`)

OBSERVED (scripted gestures, backboardd on FuxiMetal with two frames in flight):
- CA swaps at about 34/s, but the panel shows about 24 frames/s, and about 30%
  of swaps are superseded before they are shown.
- The scanout summary counts about 8.5 deferred acquire polls per shown frame,
  at about 1 ms each.
- After every START, acquire waits a fixed 20 ms floor (`FG_SCAN_DRAIN_US`)
  before it tests CTL START and the write line. It also waits for an idle GPU
  ring (`fg_busy`).

GUESS: the floor and the ring wait, not CA rendering, set the shown frame rate.

`fuxiscandrain=<us>` (4000..20000; the default stays 20000) lowers the floor.
The START bit and write-line checks still gate every copy. Every 600 starts,
`FUXI SCANOUT: waits per 600 starts ring=… floor=… transfer=…` reports the
polls each gate held. A large `transfer` count at a low floor means the panel
transfer itself is still running; tearing would show there first.

## Tear-check registers, TE rate and opt-in TE gate (`fuxiscante=1`)

OBSERVED (code + logs): the `rd=` value in earlier `owned` lines came from
INTF+0x2a8. The SM8550 SDE driver (`sde_hw_intf.c`) names that register
`INTF_TEAR_WR_PTR_IRQ`, a static setting that `sde_encoder_phys_cmd.c` programs
to 1. Both M114 and P117 logged `rd=0x1`, which matches. The read pointer is
`INTF_TEAR_INT_COUNT_VAL` at +0x298: the high 16 bits count TE frames and the
low 16 bits are the line, which reloads to `VSYNC_INIT_VAL` (+0x294) at each TE.

New log lines:
- At ownership: `FUXI SCANOUT: tear sel= en= vsync= height= init= thresh=
  start_pos= wr_irq=`. These are the raw ABL tear-check settings.
  `vsync & 0x7ffff` is `vsync_count`, from which the configured rate is
  `19.2 MHz / (vtotal * vsync_count)`, with vtotal 2684 for this panel's DTB
  timing. This was an estimate from Android's nominal geometry, not a
  calibration of ABL's inherited counter; see the measured baseline below.
- Every 600 STARTs: `te=` (TE-gate polls), `te_hz=` (TE frames per second
  measured over the window) and `gaps=a/b/c/d`. The gaps are START-to-START
  intervals of at most 1, 2 or 3 sixty-Hz periods, or longer (idle).

With `fuxiscante=1`, acquire skips the fixed floor. After each START it waits
until the TE frame count changes and the line is at least 64 past the reload
value. Then the usual START-bit and write-line checks run. The count is read
after the START write, so a TE that races the START only adds a wait. If no
usable TE arrives within 40 ms of a START, the gate logs `te counter stalled`
and uses the default 20 ms floor for the rest of the boot. An explicit
`fuxiscandrain` still applies on top of the TE gate.

GUESS: TE counting and reload behaviour follow the SDE driver's use of these
registers. This has not been checked on the device. Boot with the default
first, and only enable the gate after `te_hz` reads 60 or 120.

## Opt-in panel 120 Hz (`fuxipanel120=1`, default off)

The supplied device baseline is **`te_hz=60.4`, `vsync=0x180085`,
`init=0x960`**: ABL leaves this panel at about 60 Hz. A fast DSI link or a
120 Hz timing in the Android tree does not establish the current panel rate.
This change has host-model coverage only; no device, full kernel build or
DSI/DSC qualification was performed for it.

Enable with `fuxipanel120=1 fuxigpuscanout=1 sweetfb_noconsole=1` plus the
existing GPU/ABI boot arguments. Only the exact value 1 enables it; it is
ignored without the scanout handoff. With the option absent or zero, DSI0
is not mapped or accessed, no switch commands or tear-check writes occur,
and the existing 60 Hz gate, floor, histogram and logs retain their behavior.
The compositor's existing nominal 60 Hz notification/workloop schedule is
unchanged; panel refresh is separate from rendered or presented frame rate.

Panel command provenance: the requested local **`dtb/` decompiled DTB is
unavailable in this worktree**, and the main checkout's `dtb/` is empty.
Consequently a source DTB filename and device-specific byte comparison cannot
be claimed. The corroborating source is
[`qcom/display/display/dsi-panel-m3-38-0c-0a-dsc-cmd.dtsi`](https://github.com/LineageOS/android_kernel_xiaomi_sm8550-devicetrees/blob/43b5ada5654ed84c25f74789bc8f6326b7bacd20/qcom/display/display/dsi-panel-m3-38-0c-0a-dsc-cmd.dtsi),
node `qcom,mdss_dsi_m3_38_0c_0a_dsc_cmd`, child
`qcom,mdss-dsi-display-timings/timing@1`, property
**`qcom,mdss-dsi-timing-switch-command`**. It matches the task's eight
120 Hz writes exactly. Its sibling
`qcom,mdss-dsi-timing-switch-command-state` is `dsi_hs_mode`.
The seven-byte descriptors are `type, legacy-last, VC, flags, wait-ms,
length-hi, length-lo`, followed by the payload:

```text
39 00 00 40 00 00 03  F0 5A 5A
15 00 00 40 00 00 02  60 00
39 00 00 40 00 00 04  B0 00 30 BD
15 00 00 40 00 00 02  BD 04
39 00 00 40 00 00 04  B0 00 16 F2
39 00 00 40 00 00 03  F2 01 70
15 00 00 40 00 00 02  F7 0F
39 00 00 00 00 00 03  F0 A5 A5
```

`0x39` is DCS long write, `0x15` is DCS short write with one parameter.
Every switch delay is **zero**. Flags `0x40` batch the first seven packets;
the final flags `0x00` terminate the batch. The 60 Hz `timing@0` property
differs only at `60 08`; `timing@2` is 90 Hz. All three timings specify four
lanes at 1,100,000,000 bit/s and `qcom,mdss-mdp-transfer-time-us = 7300`.
The per-timing **`qcom,mdss-dsi-on-command`** also contains the same frequency
select block, but includes full initialization: `0x05` DCS short writes
`11 00` (sleep out, wait byte `0x78` = 120 ms) and `29 00` (display on,
zero delay), as well as a later `0x39` `F7 0F` with wait `0x14` = 20 ms.
These initialization commands and delays are **not** replayed by this mode
switch. The timing-switch property is the source for the implemented batch.

The original implementation uses register facts from Qualcomm
[`dsi_ctrl_hw_cmn.c`](https://github.com/LineageOS/android_kernel_qcom_sm8550-modules/blob/d00477fbff4a83babbee37b2ea56fb0ded793eec/qcom/opensource/display-drivers/msm/dsi/dsi_ctrl_hw_cmn.c)
(`kickoff_fifo_command`, `reset_cmd_fifo`, DMA-done status),
`dsi_ctrl_reg.h`, `dsi_ctrl_hw_2_2.c` (trigger selection), `dsi_ctrl.c`
(embedded packet layout), `dsi_panel.c` (descriptor flags), and
[`sde_hw_intf.c`](https://github.com/LineageOS/android_kernel_qcom_sm8550-modules/blob/d00477fbff4a83babbee37b2ea56fb0ded793eec/qcom/opensource/display-drivers/msm/sde/sde_hw_intf.c)
(tear-counter sequencing). No GPL implementation is copied or vendored.

After disabling autorefresh and claiming START, acquire waits for the inherited
transfer's existing floor/TE gate, CTL START idle and drained write line. Before
the first manual START, it attempts the switch **once** under the GPU/display
lock. DSI0 (`0xae94000`) must also become idle: STATUS at +0x008 includes
command DMA busy bit1 and command MDP busy bit2. The inherited command engine
must already be on, video mode off, tear counter enabled and TPG unused.
The code clears stale DMA-done status, selects software DMA trigger 4 in
+0x084 while preserving MDP/TE fields, resets the command FIFO, writes
TPG_CTRL +0x15c = `0x30006`, and streams the embedded batch to +0x17c.
There are **52 bytes / 13 DWORDs**, plus the hardware-required zero DWORD
for an odd word count; DMA length +0x04c stays 52. The embedded headers carry
LONG and final-LAST flags; three-byte long payloads pad with `ff`.
DMA_CTRL +0x03c selects embedded, unicast HS. A barrier precedes the
**DMA trigger +0x090**; **MDP trigger +0x094 is never used for DCS**.
The code polls INT_CTRL +0x110 DMA_DONE bit0 and then DSI idle before cleaning
the FIFO and restoring the inherited DMA, length, trigger and interrupt masks.
Only DMA_DONE is acknowledged; unrelated W1C status is preserved.

Each poll is bounded by **20 ms and 2001 samples**. A timeout logs
`FUXI SCANOUT: panel120 timeout phase=... offset=... status=...`, stops the
attempt, holds scanout and never retries or issues START. Before commands are
sent the panel and tear configuration remain at 60 Hz. **After a trigger,
a timeout cannot guarantee that the panel stayed at 60 Hz:** it might already
have received `60 00`. The code leaves 60 Hz accounting, logs that panel state
is uncertain, and requires a reboot instead of sending a speculative rollback.
MMIO errors, controller error status after transmission and tear-check readback
failures also hold publication. A refused unsupported setup sends no commands and continues
with the inherited 60 Hz mode.

After a successful command transfer, tear-check programming disables
**VSYNC_COUNTER_EN, bit19 of INTF+0x288**, writes the new count and preserved
height (+0x28c), and re-enables the counter. Interrupts are masked only around
these four MMIO writes to meet SDE's single-buffer update constraint. The
TEAR_CHECK_EN register (+0x284), external TE bit20, init, thresholds and line
geometry remain intact. With the clock and geometry unchanged, line period is
proportional to the low-19-bit count, so doubling refresh uses
`floor(133 * 60 / 120) = 66`: **`0x180085 -> 0x180042`**, with `init=0x960`
unchanged. Truncation produces a local counter rate about 2.015 times the old
one; external TE supplies frame reloads. The earlier estimate of 119/59 from
19.2 MHz and nominal DTB vtotal 2684 does not match the observed ABL count 133.
This implementation uses the measured inherited count ratio, not that estimate;
the `te_hz` measurement remains the device qualification criterion.

Only after successful register readback does the active period become
**8333 us**. The default 20 ms drain/fallback floor becomes **10 ms**, keeping
the same 1.2-period margin; an explicit `fuxiscandrain=<us>` retains its units
and value across the mode switch. `fuxiscante=1` keeps its 64-line margin (half the time with the
adjusted counter) and uses a **20 ms** stall deadline instead of 40 ms. Gap
buckets become at most 1/2/3 actual panel periods, then longer. The TE-rate
window restarts after the switch so it does not mix 60 Hz and 120 Hz samples.
The new floor alone still limits presentation below 100 frames/s; this feature
does not promise 120 rendered/presented frames/s.

Verification on device, to be performed separately by the maintainer: first
record the no-option `te_hz` baseline. Boot with the opt-in arguments and look
for `FUXI SCANOUT: panel120 begin ...` followed by
`FUXI SCANOUT: panel120 switched vsync=0x180085->0x180042 ... period_us=8333`.
Require the subsequent 600-START summaries to show **`te_hz` around 120**,
then qualify DSC stability, image quality, TE gating, wake/blanking, cadence,
power and thermals. A controller-done log alone proves no physical TE rate.
An incorrect sequence can **hang DSI or blank the panel and needs a reboot**;
boot without `fuxipanel120` to recover the ABL baseline. Do not retry in place.

Host checks: `python3 tools/test-fuxi-gpu-scanout.py`,
`python3 tools/sync-fuxi-metal.py --check`,
`python3 tools/test-fuxi-unixnu.py`, and
`python3 tools/test-fuxi-adreno-mapping.py`. These exercise production C with
modeled MMIO: exact packet bytes and FIFO order/padding, boundary admission,
one attempt, stale completion, frozen-clock and busy/done timeouts, MMIO
failure, tear-counter disable/update/enable and readback, 60/120 Hz timing,
and DSI mapping only with opt-in. They cannot qualify actual hardware.
