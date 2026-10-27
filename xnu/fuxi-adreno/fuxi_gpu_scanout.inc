/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 NativeMetal contributors
 * Ported from UniXNU drivers/NativeMetal/GpuScanoutClient.h (MIT).
 * The CPU pixel oracle is adapted below; Fuxi display ownership is original.
 * Finished-frame copy/fence design: NativeMetal/GpuScanoutPresenter.h (MIT).
 * See LICENSE.NativeMetal.
 */
/* GPU-SCANOUT: Fuxi command-mode display ownership. Original implementation.
 * No panel/DSI reset or firmware dependency; optional reserved-buffer flips.
 * fuxipanel120=1 optionally sends the panel's timing-switch commands once,
 * only after ownership and inherited transfer drain. Default does no DSI IO.
 * Register meanings already used by the board's inherited-display probe:
 * INTF1 AUTOREFRESH_CONFIG +0x2b4 bit31; TEAR_LINE_COUNT +0x2b0 low16
 * write line; CTL START +0x1c bit0. Never copy into a buffer a transfer reads.
 * Tear-check block per the SM8550 SDE driver (sde_hw_intf.c): +0x280
 * MDP_VSYNC_SEL, +0x284 TEAR_CHECK_EN, +0x288 SYNC_CONFIG_VSYNC, +0x28c
 * SYNC_CONFIG_HEIGHT, +0x294 VSYNC_INIT_VAL, +0x298 INT_COUNT_VAL (read
 * pointer: high16 TE frame count, low16 line), +0x29c SYNC_THRESH, +0x2a0
 * START_POS, +0x2a8 WR_PTR_IRQ. +0x2a8 is a static setting (reads 1 on
 * device), not the read pointer; earlier logs labelled it rd.
 * fuxiscante=1 (opt-in) replaces the fixed floor with a TE gate: after each
 * START, wait for a TE (frame count change) and some lines past it before
 * the START/write-line checks. A frame count that never changes falls back
 * to the floor for the rest of the boot.
 */
#define FG_SCAN_CTL 0x0ae16000u
#define FG_SCAN_INTF 0x0ae36000u
#define FG_SCAN_DRAIN_US 20000u
#define FG_SCAN_TE_MARGIN 64u     /* lines after TE before testing the write line */
#define FG_SCAN_TE_STALL_US 40000u /* no TE this long after START: counter unusable */
#define FG_SCAN_DSI 0x0ae94000u
#define FG_SCAN_DSI_TIMEOUT_US 20000u
static struct fg_present_times fg_scanout_stamps;
static unsigned fg_scanout_panel120_attempted, fg_scanout_panel120_active;
static unsigned fg_scanout_period_us = 16667u, fg_scanout_explicit_drain;
static unsigned fg_scanout_owned, fg_scanout_failed, fg_scanout_writable;
static unsigned fg_scanout_reserved, fg_scanout_checks, fg_scanout_checked;
static uintptr_t fg_scanout_pixels, fg_scanout_source;
static unsigned fg_scanout_pitch, fg_scanout_sx, fg_scanout_sy;
static unsigned fg_scanout_dx, fg_scanout_dy, fg_scanout_w, fg_scanout_h;
static uint64_t fg_scanout_after, fg_scanout_timeout;
/* Floor after each START before the transfer status is tested. Boot-arg
 * fuxiscandrain=<us> (4000..20000) lowers it; the START/write-line checks
 * still gate every copy. Wait polls are counted per reason. */
static unsigned fg_scanout_drain_us = FG_SCAN_DRAIN_US;
static unsigned fg_scanout_wait_ring, fg_scanout_wait_floor, fg_scanout_wait_xfer, fg_scanout_starts;
static unsigned fg_scanout_te_mode, fg_scanout_te_frame, fg_scanout_te_init, fg_scanout_wait_te;
static unsigned fg_scanout_te_window, fg_scanout_te_frames, fg_scanout_iv[4];
static uint64_t fg_scanout_started, fg_scanout_last_start, fg_scanout_window_start;
static int fg_abi_observe(void);
static unsigned fg_abi_start; /* fuxigpuncimport bit 8192: panel120 START floor of 3/4 period; the framebuffer polls START retries at 1/64 tick */
/* Measurement only. xfer: START write to the first poll that sees CTL START clear and the write line idle (quantized by the poll
 * interval); sampled on every acquire, also inside the floor. r2s: newest back buffer ready to START, split by the reason each
 * blocked poll returned (the interval since the previous poll is charged to that poll's reason). */
enum { FG_WHY_RING, FG_WHY_FLOOR, FG_WHY_TE, FG_WHY_XFER };
static const unsigned fg_scanout_xfer_edges[7] = { 5000, 6000, 7000, 7500, 8000, 9000, 12000 };
static unsigned fg_scanout_xfer_open, fg_scanout_xfer_n, fg_scanout_xfer_hist[8], fg_scanout_last_why;
static uint64_t fg_scanout_xfer_sum, fg_scanout_xfer_max, fg_scanout_ready_at, fg_scanout_charged_at, fg_scanout_charge[4], fg_scanout_r2s, fg_scanout_r2s_n;
static void fg_scanout_charge_to(unsigned why)
{
 if (!fg_scanout_ready_at) return;
 uint64_t now = rsr("cntpct_el0");
 if (!fg_scanout_charged_at) fg_scanout_charged_at = fg_scanout_ready_at;
 if (now > fg_scanout_charged_at) fg_scanout_charge[why] += now - fg_scanout_charged_at;
 fg_scanout_charged_at = now; fg_scanout_last_why = why;
}
static int fg_scanout_read(unsigned pa, unsigned *value);
static void fg_scanout_xfer_sample(void)
{
 unsigned line, start;
 if (!fg_scanout_xfer_open || !fg_scanout_read(FG_SCAN_INTF+0x2b0, &line) || !fg_scanout_read(FG_SCAN_CTL+0x1c, &start)) return;
 if ((start & 1) || ((line & 65535) != 0 && (line & 65535) < 2400)) return;
 uint64_t hz = rsr("cntfrq_el0"), us = (rsr("cntpct_el0") - fg_scanout_started)*1000000/hz;
 unsigned bin = 0;
 while (bin < 7 && us >= fg_scanout_xfer_edges[bin]) bin++;
 fg_scanout_xfer_hist[bin]++; fg_scanout_xfer_n++; fg_scanout_xfer_sum += us;
 if (us > fg_scanout_xfer_max) fg_scanout_xfer_max = us;
 fg_scanout_xfer_open = 0;
}
static unsigned fg_scanout_double_active, fg_scanout_double_initialized, fg_scanout_front;
static unsigned fg_scanout_back_ready, fg_scanout_flips, fg_scanout_ready_drops, fg_scanout_retries;
static unsigned fg_scanout_current_match, fg_scanout_current_mismatch, fg_scanout_reentries;
static uintptr_t fg_scanout_pixels_b, fg_scanout_pixels_c;
static unsigned fg_scanout_ready, fg_scanout_copy, fg_scanout_buffer_flips[3];
static unsigned fg_scanout_target(void)
{
 for (unsigned i = 0; i < (fg_scanout_triple_requested ? 3u : 2u); i++)
  if (i != fg_scanout_front && (!fg_scanout_back_ready || i != fg_scanout_ready)) return i;
 return fg_scanout_front ^ 1u;
}
static unsigned fg_scanout_pa(unsigned buffer) { return 0xb8000000u + buffer*0xa00000u; }
static uintptr_t fg_scanout_pixels_at(unsigned buffer) { return buffer == 2 ? fg_scanout_pixels_c : buffer ? fg_scanout_pixels_b : fg_scanout_pixels; }
static int fg_scanout_acquire(unsigned publication);
static int fg_scanout_stop(const char *reason);

void fuxi_gpu_scanout_configure(uintptr_t pixels, unsigned enabled, unsigned check)
{
 /* AppleCLCD calls once, before fuxi_gpu_start publishes the GPU. */
 if (fg_scanout_double_initialized || (fg_scanout_double_requested && __atomic_load_n(&fg_scanout_owned, __ATOMIC_ACQUIRE))) {
  FG_GUARD;
  (void)fg_scanout_stop(enabled == 1 ? "reconfigure while owned" : "disabled");
  return;
 }
 fg_scanout_pixels = pixels;
 fg_scanout_requested = enabled == 1;
 fg_scanout_checks = fg_scanout_requested && check == 1 ? 4 : 0;
 if (fg_scanout_requested)
{
  unsigned drain = 0, te = 0, panel120 = 0, double_buffer = 0, triple_buffer = 0;
  fg_scanout_double_requested = PE_parse_boot_argn("fuxiscandouble", &double_buffer, sizeof(double_buffer)) && double_buffer == 1;
  unsigned triple = PE_parse_boot_argn("fuxiscantriple", &triple_buffer, sizeof(triple_buffer)) && triple_buffer == 1;
  fg_scanout_triple_requested = fg_scanout_double_requested && triple;
  fg_scanout_panel120_requested = PE_parse_boot_argn("fuxipanel120", &panel120, sizeof(panel120)) && panel120 == 1;
  fg_scanout_te_mode = PE_parse_boot_argn("fuxiscante", &te, sizeof(te)) && te == 1;
  /* The TE gate needs no floor unless one is given explicitly. */
  if (fg_scanout_te_mode) fg_scanout_drain_us = 0;
  if (PE_parse_boot_argn("fuxiscandrain", &drain, sizeof(drain)) && drain >= 4000 && drain <= FG_SCAN_DRAIN_US) {
   fg_scanout_drain_us = drain;
   fg_scanout_explicit_drain = 1;
  }
  printf("FUXI SCANOUT: requested, fence then manual transfer, drain_us=%u te_gate=%u check=%u\n",
      fg_scanout_drain_us, fg_scanout_te_mode, fg_scanout_checks);
 }
}
void fuxi_gpu_scanout_stamps(struct fg_present_times *times)
{
 FG_GUARD;
 *times = fg_scanout_stamps;
}
int fuxi_gpu_scanout_owned(void)
{
 return __atomic_load_n(&fg_scanout_owned, __ATOMIC_ACQUIRE);
}
static int fg_scanout_read(unsigned pa, unsigned *value)
{
 uint64_t v = 0;
 if (fg_access(pa, &v, 0, 0)) return 0;
 *value = (unsigned)v;
 return 1;
}
static int fg_scanout_write(unsigned pa, unsigned value)
{
 uint64_t v = value;
 return !fg_access(pa, &v, 1, 0);
}
/* Opt-in measurement (fuxigpuncimport bit 16384): after every 8th START a thread call spins on CTL START and the INTF write line
 * (about 10 us apart, at most 20 ms) and stamps, relative to the START write: the first TE count change ("edge"), the first
 * START-bit-clear ("clear"), the first write-line idle after it was active ("lineend", 0 when never seen active) and "done" = the
 * later of clear and lineend, the same condition acquire uses. The worker only stores results; accounting runs under FG_GUARD at the
 * next publish, so a window is consistent. Read-only MMIO, no behaviour change. Not yet observed on device. */
static unsigned fg_abi_probe, fg_abi_spin;
/* Opt-in (fuxigpuncimport bit 32768), panel120 + double/triple only. The 1080x2400 transfer measures ~8.0 ms from the TE edge in an 8.33 ms
 * period, so a START can only catch the next TE if it is written in the ~0.3 ms between transfer-done and that TE; the 0.5-1 ms
 * display poll (plus timer slack) lands there rarely (71% of gaps were two periods). After each START a delayed thread call wakes at
 * 93% of a period and spins at most 600 us on CTL START / the write line; at the first idle it publishes the waiting back buffer through
 * the same fg_scanout_publish() the retry path uses (every gate unchanged: ring, TE, transfer status). Nothing is published when no
 * back buffer is ready or another START intervened; the display path stays the fallback. */
#define FG_SCAN_SPIN_LEAD_PCT 93u
#define FG_SCAN_SPIN_US 600u
static thread_call_data_t fg_scanout_spin_call;
static unsigned fg_scanout_spin_setup, fg_scanout_spin_busy, fg_scanout_spin_serial, fg_scanout_async_published;
static unsigned fg_scanout_spin_stat[4]; /* wakes, published, no back buffer, window expired */
#define FG_PROBE_MAX 128
static thread_call_data_t fg_scanout_probe_call;
static unsigned fg_scanout_probe_setup, fg_scanout_probe_busy, fg_scanout_probe_ready, fg_scanout_probe_te0;
static uint64_t fg_scanout_probe_t0, fg_scanout_probe_res[4]; /* edge, clear, lineend, done (ticks; 0 = not seen) */
static unsigned fg_scanout_probe_n, fg_scanout_probe_timeouts, fg_scanout_probe_edges, fg_scanout_probe_hist[18];
static unsigned fg_scanout_probe_done[FG_PROBE_MAX], fg_scanout_probe_te2d[FG_PROBE_MAX];
static uint64_t fg_scanout_probe_sum[4];
static void fg_scanout_probe_run(thread_call_param_t a, thread_call_param_t b)
{
 (void)a; (void)b;
 uint64_t hz = rsr("cntfrq_el0"), t0 = fg_scanout_probe_t0, limit = t0 + hz/50, res[4] = {0, 0, 0, 0};
 unsigned active = 0;
 for (;;) {
  unsigned count, line, start;
  uint64_t now = rsr("cntpct_el0");
  if (!fg_scanout_read(FG_SCAN_INTF+0x298, &count) || !fg_scanout_read(FG_SCAN_INTF+0x2b0, &line) ||
      !fg_scanout_read(FG_SCAN_CTL+0x1c, &start)) break;
  if (!res[0] && (count >> 16) != fg_scanout_probe_te0) res[0] = now - t0 + 1;
  if (!res[1] && !(start & 1)) res[1] = now - t0 + 1;
  if ((line & 65535) != 0 && (line & 65535) < 2400) active = 1;
  else if (active && !res[2]) res[2] = now - t0 + 1;
  /* START clears ~17 us after the write, before the MDP starts writing at the next TE: finishing on "clear and never active" ended
   * nearly every probe early. Wait for the TE edge, then 0.5 ms for the line to go active (otherwise the START was a no-op). */
  if (res[1] && res[0] && (res[2] || (!active && now - t0 >= res[0] - 1 + hz/2000))) { res[3] = res[1] > res[2] ? res[1] : res[2]; break; }
  if (now >= limit) break;
  delay(10);
 }
 for (unsigned i = 0; i < 4; i++) fg_scanout_probe_res[i] = res[i];
 __atomic_store_n(&fg_scanout_probe_ready, 1, __ATOMIC_RELEASE);
}
static int fg_scanout_publish(void);
static void fg_scanout_spin_run(thread_call_param_t a, thread_call_param_t b)
{
 (void)a; (void)b;
 uint64_t hz = rsr("cntfrq_el0"), end = rsr("cntpct_el0") + hz*FG_SCAN_SPIN_US/1000000;
 unsigned serial = fg_scanout_spin_serial, result = 2; /* 2 = window expired */
 fg_scanout_spin_stat[0]++;
 for (;;) {
  unsigned line, start;
  if (!fg_scanout_read(FG_SCAN_INTF+0x2b0, &line) || !fg_scanout_read(FG_SCAN_CTL+0x1c, &start)) break;
  if (!(start & 1) && ((line & 65535) == 0 || (line & 65535) >= 2400)) {
   FG_GUARD;
   if (fg_scanout_starts != serial || fg_scanout_failed || !fg_scanout_back_ready || fg_busy) { result = fg_scanout_back_ready ? 3 : 1; }
   else if (fg_scanout_publish() == 1) { fg_scanout_async_published = 1; result = 0; }
   else result = 3;
   break;
  }
  if (rsr("cntpct_el0") >= end) break;
  delay(2);
 }
 { FG_GUARD; fg_scanout_spin_stat[result == 0 ? 1 : result == 1 ? 2 : 3]++; fg_scanout_spin_busy = 0; }
}
/* Called under FG_GUARD right after a START (and its arm). */
static void fg_scanout_spin_kick(void)
{
 if (!fg_abi_spin || fg_scanout_spin_busy || !fg_scanout_panel120_active || !fg_scanout_double_active) return;
 if (!fg_scanout_spin_setup) {
  thread_call_setup_with_options(&fg_scanout_spin_call, fg_scanout_spin_run, NULL, THREAD_CALL_PRIORITY_KERNEL, THREAD_CALL_OPTIONS_ONCE);
  fg_scanout_spin_setup = 1;
 }
 uint64_t lead;
 nanoseconds_to_absolutetime((uint64_t)fg_scanout_period_us*FG_SCAN_SPIN_LEAD_PCT*10, &lead);
 fg_scanout_spin_busy = 1; fg_scanout_spin_serial = fg_scanout_starts;
 (void)thread_call_enter_delayed(&fg_scanout_spin_call, mach_absolute_time() + lead);
}
static void fg_scanout_probe_kick(unsigned te_frame)
{
 if (!fg_abi_probe || fg_scanout_probe_busy || (fg_scanout_starts & 7)) return;
 if (!fg_scanout_probe_setup) {
  thread_call_setup_with_options(&fg_scanout_probe_call, fg_scanout_probe_run, NULL, THREAD_CALL_PRIORITY_KERNEL, THREAD_CALL_OPTIONS_ONCE);
  fg_scanout_probe_setup = 1;
 }
 fg_scanout_probe_busy = 1; fg_scanout_probe_te0 = te_frame;
 (void)thread_call_enter(&fg_scanout_probe_call);
}
static unsigned fg_scanout_probe_us(uint64_t ticks) { return (unsigned)(ticks ? (ticks - 1)*1000000/rsr("cntfrq_el0") : 0); }
static void fg_scanout_probe_account(void)
{
 if (!fg_scanout_probe_busy || !__atomic_load_n(&fg_scanout_probe_ready, __ATOMIC_ACQUIRE)) return;
 if (!fg_scanout_probe_res[3]) fg_scanout_probe_timeouts++;
 else if (fg_scanout_probe_n < FG_PROBE_MAX) {
  unsigned done = fg_scanout_probe_us(fg_scanout_probe_res[3]), edge = fg_scanout_probe_us(fg_scanout_probe_res[0]);
  unsigned bin = done < 6000 ? 0 : done >= 10000 ? 17 : 1 + (done - 6000)/250;
  fg_scanout_probe_hist[bin]++;
  fg_scanout_probe_done[fg_scanout_probe_n] = done;
  fg_scanout_probe_te2d[fg_scanout_probe_n] = fg_scanout_probe_res[0] && done >= edge ? done - edge : 0;
  fg_scanout_probe_edges += fg_scanout_probe_res[0] != 0;
  fg_scanout_probe_n++;
  for (unsigned i = 0; i < 4; i++) fg_scanout_probe_sum[i] += fg_scanout_probe_us(fg_scanout_probe_res[i]);
 }
 __atomic_store_n(&fg_scanout_probe_ready, 0, __ATOMIC_RELEASE);
 fg_scanout_probe_busy = 0;
}
static unsigned fg_scanout_probe_pct(unsigned *v, unsigned n, unsigned pct)
{
 for (unsigned i = 1; i < n; i++) { unsigned x = v[i], j = i; while (j && v[j-1] > x) { v[j] = v[j-1]; j--; } v[j] = x; }
 return n ? v[(n - 1)*pct/100] : 0;
}
static void fg_scanout_probe_report(void)
{
 if (!fg_abi_probe) return;
 unsigned n = fg_scanout_probe_n, d[FG_PROBE_MAX], t[FG_PROBE_MAX], dmax = 0, tmax = 0, tn = 0;
 uint64_t tsum = 0;
 for (unsigned i = 0; i < n; i++) {
  d[i] = fg_scanout_probe_done[i]; if (d[i] > dmax) dmax = d[i];
  if (fg_scanout_probe_te2d[i]) { t[tn++] = fg_scanout_probe_te2d[i]; tsum += fg_scanout_probe_te2d[i]; if (t[tn-1] > tmax) tmax = t[tn-1]; }
 }
 uint64_t div = n ? n : 1, tdiv = tn ? tn : 1, ediv = fg_scanout_probe_edges ? fg_scanout_probe_edges : 1;
 printf("FUXI SCANOUT: dsi_xfer n=%u timeouts=%u start_done_us avg=%llu p50=%u p90=%u max=%u clear_avg=%llu lineend_avg=%llu"
     " te_edge_avg=%llu te_edges=%u te_done_us avg=%llu p50=%u p90=%u max=%u hist<6/6.25../9.75/>=10ms=%u",
     n, fg_scanout_probe_timeouts, (unsigned long long)(fg_scanout_probe_sum[3]/div), fg_scanout_probe_pct(d, n, 50), fg_scanout_probe_pct(d, n, 90), dmax,
     (unsigned long long)(fg_scanout_probe_sum[1]/div), (unsigned long long)(fg_scanout_probe_sum[2]/div),
     (unsigned long long)(fg_scanout_probe_sum[0]/ediv), fg_scanout_probe_edges, (unsigned long long)(tsum/tdiv),
     fg_scanout_probe_pct(t, tn, 50), fg_scanout_probe_pct(t, tn, 90), tmax, fg_scanout_probe_hist[0]);
 for (unsigned i = 1; i < 18; i++) printf("/%u", fg_scanout_probe_hist[i]);
 printf("\n");
 fg_scanout_probe_n = fg_scanout_probe_timeouts = fg_scanout_probe_edges = 0;
 for (unsigned i = 0; i < 18; i++) fg_scanout_probe_hist[i] = 0;
 for (unsigned i = 0; i < 4; i++) fg_scanout_probe_sum[i] = 0;
}
/* Poll brackets the edge between the last old count and the first new count.
 * Two narrow, consecutive brackets establish a measured period. An IIR (1/8
 * period, 1/4 phase) rejects polling jitter without following the software
 * timer's phase. Never extrapolate an advancing counter through a stall.
 * Estimator writes belong to the caller. Only a double-mode MMIO failure
 * takes FG_GUARD to stop and restore A; healthy observation remains lock-free. */
int fuxi_gpu_scanout_te(struct fg_te_observer *te)
{
 unsigned count;
 if (!fuxi_gpu_scanout_owned() || !fg_scanout_read(FG_SCAN_INTF+0x298, &count)) {
  te->initialized = te->samples = 0;
  if (fg_scanout_double_active && fuxi_gpu_scanout_owned()) {
   FG_GUARD;
   (void)fg_scanout_stop("read TE observer");
  }
  return 0;
 }
 uint64_t now = mach_absolute_time();
 unsigned frame = count >> 16, nominal = fg_scanout_period_us;
 unsigned delta = (frame - te->frame) & 0xffffu;
 if (!te->initialized || te->nominal_us != nominal || now < te->poll ||
     (!delta && now - te->change > te->period + te->period/4)) {
  nanoseconds_to_absolutetime((uint64_t)nominal*1000, &te->period);
  te->nominal_us = nominal;
  te->frame = frame;
  te->poll = te->change = now;
  te->raw_edge = te->edge = 0;
  te->initialized = 1;
  te->samples = 0;
  return 0;
 }
 uint64_t previousPoll = te->poll;
 uint64_t bracket = now - te->poll;
 uint64_t raw = te->poll + bracket/2;
 te->poll = now;
 if (!delta) return te->samples >= 2;
 te->frame = frame;
 te->change = now;
 if (delta != 1 || bracket > te->period/4) {
  /* A slow copy can straddle TE. Retain an established phase only when
   * the counter confirms up to four edges and the prediction fits the
   * observation bracket (allow one polling interval of phase error).
   * Do not feed this uncertain sample back into the period filter. */
  uint64_t predicted = te->edge + delta*te->period;
  uint64_t tolerance = te->period/16;
  if (te->samples >= 2 && delta <= 4 &&
      predicted + tolerance >= previousPoll && predicted <= now + tolerance) {
   te->edge = predicted < now ? predicted : now;
   return 1;
  }
  /* Reset, implausible jump or ambiguous acquisition: reacquire. */
  te->samples = 0;
  te->raw_edge = 0;
  return 0;
 }
 if (te->samples) {
  unsigned frames = (frame - te->raw_frame) & 0xffffu;
  uint64_t measured = frames ? (raw - te->raw_edge)/frames : 0;
  if (frames > 8 || measured < te->period*3/4 || measured > te->period*5/4) {
   te->samples = 0;
  } else {
   int64_t error = (int64_t)measured - (int64_t)te->period;
   te->period += error/8;
  }
 }
 if (te->samples >= 2) {
  uint64_t predicted = te->edge + te->period;
  int64_t error = (int64_t)raw - (int64_t)predicted;
  te->edge = predicted + error/4;
  if (te->edge > now) te->edge = now;
 } else te->edge = raw;
 te->raw_edge = raw;
 te->raw_frame = frame;
 if (te->samples < 2) te->samples++;
 return te->samples >= 2;
}
/* Arm the per-START gates. Called with the current read pointer value. */
static void fg_scanout_arm(unsigned count)
{
 uint64_t now = rsr("cntpct_el0"), hz = rsr("cntfrq_el0");
 fg_scanout_te_frame = count >> 16;
 unsigned drain = fg_scanout_drain_us;
 /* panel120 default floor (10 ms) exceeds the 8.33 ms period: a START finishing its transfer just before a TE waited out the floor
  * and missed that TE. Bit 8192 lowers it to 3/4 period (the transfer-done and TE checks still gate every START). */
 if (fg_abi_start && fg_scanout_panel120_active && !fg_scanout_explicit_drain && !fg_scanout_te_mode && drain == FG_SCAN_DRAIN_US/2)
  drain = fg_scanout_period_us*3/4;
 fg_scanout_after = now + hz*drain/1000000;
 fg_scanout_timeout = fg_scanout_after + hz/10;
 fg_scanout_started = now; fg_scanout_xfer_open = 1;
}
/* Preserve other pending flush bits; VIG0 is CTL_FLUSH bit0. */
static int fg_scanout_address(unsigned pa)
{
 unsigned flush;
 if (!fg_scanout_read(FG_SCAN_CTL+0x18, &flush) ||
     !fg_scanout_write(0xae05014, pa) || !fg_scanout_write(FG_SCAN_CTL+0x18, flush | 1u)) return 0;
 __asm__ volatile("dsb sy" ::: "memory");
 return 1;
}
static int fg_scanout_stop(const char *reason)
{
 if (!fg_scanout_failed)
  printf("FUXI SCANOUT: stopped reason=%s; panel held, no framebuffer writes\n", reason);
 /* Restore the fixed-address shadow even on failure; never hand back ownership
  * or START if restoration/transfer/DMA state is uncertain. */
 if (!fg_scanout_failed && (fg_scanout_double_initialized || (fg_scanout_double_requested && fuxi_gpu_scanout_owned())) &&
     !fg_scanout_address(0xb8000000u))
  printf("FUXI SCANOUT: restore A failed; panel held, reboot required\n");
 fg_scanout_back_ready = 0;
 fg_scanout_failed = 1;
 fg_scanout_writable = 0;
 return -1;
}
/* Original register-mode implementation from hardware facts described by
 * Qualcomm dsi_ctrl_hw_cmn.c (kickoff_fifo_command, reset_cmd_fifo),
 * dsi_ctrl_reg.h, dsi_ctrl_hw_2_2.c (DMA trigger selection), and
 * sde_hw_intf.c (tear-check setup). No implementation text is imported.
 * DSI0 offsets: STATUS 008 (DMA busy bit1, MDP busy bit2), DMA_CTRL 03c,
 * LENGTH 04c, TRIG_CTRL 084 (DMA low nibble, MDP bits6:4), DMA_SW_TRIGGER
 * 090, INT_CTRL 110 (DMA_DONE W1C bit0; enables at odd bits), TPG_CTRL 15c,
 * FIFO_DATA 17c, FIFO_RESET 1ec. 094 is MDP_SW_TRIGGER: never use it for DCS.
 */
static int fg_panel120_poll(unsigned offset, unsigned mask, unsigned want, const char *phase)
{
 uint64_t deadline = rsr("cntpct_el0") + rsr("cntfrq_el0")*FG_SCAN_DSI_TIMEOUT_US/1000000;
 unsigned status = 0;
 /* Both a wall-clock deadline and a finite sample count, even if the clock stalls. */
 for (unsigned n = 0; n <= FG_SCAN_DSI_TIMEOUT_US/10; n++) {
  if (!fg_scanout_read(FG_SCAN_DSI+offset, &status)) {
   printf("FUXI SCANOUT: panel120 MMIO phase=%s offset=%#x\n", phase, offset);
   return 0;
  }
  if ((status & mask) == want) return 1;
  if (rsr("cntpct_el0") >= deadline) break;
  delay(10);
 }
 printf("FUXI SCANOUT: panel120 timeout phase=%s offset=%#x status=%#x\n", phase, offset, status);
 return 0;
}
/* One embedded FIFO batch, VC0, HS, no BTA, no per-packet delays. Long
 * header: length, 0, data type, flags; short: DCS, parameter, type, flags.
 * Flag 6 marks a long packet, flag 7 ends the batch; payload pads with ff.
 * DTB timing@1 timing-switch-command, unlike on-command, has no 05 packets.
 * DT descriptors have BATCH_COMMAND (flags 40) on the first seven packets,
 * flags 00 on the last: one 52-byte batch, not eight separate triggers.
 */
static unsigned fg_panel120_packet_words(uint32_t words[14])
{
 static const struct { uint8_t type, length, data[4]; } packets[] = {
  {0x39, 3, {0xf0, 0x5a, 0x5a}},
  {0x15, 2, {0x60, 0x00}},
  {0x39, 4, {0xb0, 0x00, 0x30, 0xbd}},
  {0x15, 2, {0xbd, 0x04}},
  {0x39, 4, {0xb0, 0x00, 0x16, 0xf2}},
  {0x39, 3, {0xf2, 0x01, 0x70}},
  {0x15, 2, {0xf7, 0x0f}},
  {0x39, 3, {0xf0, 0xa5, 0xa5}},
 };
 unsigned used = 0;
 for (unsigned i = 0; i < sizeof(packets)/sizeof(packets[0]); i++) {
  unsigned flags = i == sizeof(packets)/sizeof(packets[0])-1 ? 0x80u : 0;
  if (packets[i].type == 0x39) {
   words[used++] = packets[i].length | ((unsigned)packets[i].type << 16) | ((flags | 0x40u) << 24);
   uint32_t payload = 0xffffffffu;
   for (unsigned j = 0; j < packets[i].length; j++)
    payload = (payload & ~(0xffu << (8*j))) | ((unsigned)packets[i].data[j] << (8*j));
   words[used++] = payload;
  } else {
   words[used++] = packets[i].data[0] | ((unsigned)packets[i].data[1] << 8) |
       ((unsigned)packets[i].type << 16) | (flags << 24);
  }
 }
 /* Hardware fetches pairs of DWORDs; this word is outside DMA_CMD_LENGTH. */
 words[used] = 0;
 return used;
}
static int fg_panel120_tear(unsigned vsync, unsigned height, unsigned adjusted)
{
 /* SDE requires the single-buffer update within 2^16 vsync-clock cycles.
  * Mask interrupts only for these four MMIO writes, never for DSI polling. */
 boolean_t interrupts = ml_set_interrupts_enabled(FALSE);
 int good = fg_scanout_write(FG_SCAN_INTF+0x288, vsync & ~(1u << 19));
 __asm__ volatile("dsb sy" ::: "memory");
 if (good) good = fg_scanout_write(FG_SCAN_INTF+0x288, adjusted) &&
     fg_scanout_write(FG_SCAN_INTF+0x28c, height);
 __asm__ volatile("dsb sy" ::: "memory");
 if (good) good = fg_scanout_write(FG_SCAN_INTF+0x288, adjusted | (1u << 19));
 __asm__ volatile("dsb sy" ::: "memory");
 (void)ml_set_interrupts_enabled(interrupts);
 return good;
}
/* Return 0 for a refusal before transmission (keep 60 Hz), 1 on success,
 * -1 on timeout/MMIO/uncertain state (hold until reboot).
 * Called under FG_GUARD, with AR off, CTL START idle and write-line drained.
 */
static int fg_panel120_switch(void)
{
 unsigned dma, trigger, irq, tpg, length, ctrl, vsync, height, init, enable;
 fg_scanout_panel120_attempted = 1; /* Never retry a partial sequence. */
 if (!fg_panel120_poll(0x008, 0x1f, 0, "idle before switch")) {
  printf("FUXI SCANOUT: panel120 aborted before commands; keeping 60 Hz\n");
  return fg_scanout_stop("panel120 controller not idle");
 }
 if (!fg_scanout_read(FG_SCAN_DSI+0x004, &ctrl) ||
     !fg_scanout_read(FG_SCAN_DSI+0x03c, &dma) ||
     !fg_scanout_read(FG_SCAN_DSI+0x084, &trigger) ||
     !fg_scanout_read(FG_SCAN_DSI+0x110, &irq) ||
     !fg_scanout_read(FG_SCAN_DSI+0x15c, &tpg) ||
     !fg_scanout_read(FG_SCAN_DSI+0x04c, &length) ||
     !fg_scanout_read(FG_SCAN_INTF+0x288, &vsync) ||
     !fg_scanout_read(FG_SCAN_INTF+0x28c, &height) ||
     !fg_scanout_read(FG_SCAN_INTF+0x294, &init) ||
     !fg_scanout_read(FG_SCAN_INTF+0x284, &enable)) {
  printf("FUXI SCANOUT: panel120 aborted reading setup; keeping 60 Hz\n");
  return fg_scanout_stop("panel120 setup MMIO");
 }
 /* Require the inherited command engine, enabled tear counter, and idle TPG.
  * Do not enable/reset a controller or guess at an unrelated panel setup. */
 if ((ctrl & 7) != 5 || tpg || (irq & (1u << 24)) || !(vsync & (1u << 19)) ||
     (vsync & 0x7ffffu) < 2 || !enable) {
  printf("FUXI SCANOUT: panel120 refused ctrl=%#x tpg=%#x irq=%#x vsync=%#x en=%#x; keeping 60 Hz\n",
      ctrl, tpg, irq, vsync, enable);
  return 0;
 }
 uint32_t words[14];
 unsigned used = fg_panel120_packet_words(words);
 unsigned masks = irq & 0xaaaaaaaau;
 printf("FUXI SCANOUT: panel120 begin dma=%#x trigger=%#x irq=%#x vsync=%#x height=%#x init=%#x\n",
     dma, trigger, irq, vsync, height, init);
 /* Select software DMA trigger 4, preserving the MDP/TE trigger fields.
  * Clear DMA_TRG_MUX bit16 and BLOCK_DMA_WITHIN_FRAME bit12 while idle. */
 if (!fg_scanout_write(FG_SCAN_DSI+0x084, (trigger & ~0x1100fu) | 4) ||
     !fg_scanout_write(FG_SCAN_DSI+0x110, (masks & ~2u) | 1u) ||
     !fg_panel120_poll(0x110, 1, 0, "clear stale done") ||
     !fg_scanout_write(FG_SCAN_DSI+0x1ec, 1)) goto uncertain;
 __asm__ volatile("dsb sy" ::: "memory");
 delay(1);
 if (!fg_scanout_write(FG_SCAN_DSI+0x1ec, 0) ||
     !fg_scanout_write(FG_SCAN_DSI+0x15c, 0x00030006)) goto uncertain;
 for (unsigned i = 0; i < used + (used & 1); i++)
  if (!fg_scanout_write(FG_SCAN_DSI+0x17c, words[i])) goto uncertain;
 /* Embedded register FIFO, unicast HS: broadcast/master/LP all clear. */
 if (!fg_scanout_write(FG_SCAN_DSI+0x03c, (dma & ~0xc4000000u) | 0x10000000u) ||
     !fg_scanout_write(FG_SCAN_DSI+0x04c, used*4)) goto uncertain;
 __asm__ volatile("dsb sy" ::: "memory");
 if (!fg_scanout_write(FG_SCAN_DSI+0x090, 1) ||
     !fg_panel120_poll(0x110, 1, 1, "DMA done") ||
     !fg_panel120_poll(0x008, 0x1f, 0, "idle after commands")) goto uncertain;
 unsigned done;
 if (!fg_scanout_read(FG_SCAN_DSI+0x110, &done) || (done & (1u << 24))) goto uncertain;
 /* Completion is certain: reset only the command FIFO and restore ABL's
  * command settings before any MDP START. No MDP/frame trigger is issued. */
 if (!fg_scanout_write(FG_SCAN_DSI+0x110, (masks & ~2u) | 1u) ||
     !fg_scanout_write(FG_SCAN_DSI+0x15c, 0) ||
     !fg_scanout_write(FG_SCAN_DSI+0x1ec, 1)) goto uncertain;
 __asm__ volatile("dsb sy" ::: "memory");
 delay(1);
 if (!fg_scanout_write(FG_SCAN_DSI+0x1ec, 0) ||
     !fg_scanout_write(FG_SCAN_DSI+0x03c, dma) ||
     !fg_scanout_write(FG_SCAN_DSI+0x04c, length) ||
     !fg_scanout_write(FG_SCAN_DSI+0x084, trigger) ||
     !fg_scanout_write(FG_SCAN_DSI+0x110, masks)) goto uncertain;
 /* Same vsync clock and line geometry: count120=floor(count60*60/120).
  * Observed 0x180085: count=133 -> 66, resulting config 0x180042.
  * Bit19 is VSYNC_COUNTER_EN, bit20 external TE; preserve other flags.
  * SDE sequence: stop the counter before writing single-buffer registers. */
 unsigned disabled = vsync & ~(1u << 19);
 unsigned adjusted = (disabled & ~0x7ffffu) | ((vsync & 0x7ffffu)/2);
 if (!fg_panel120_tear(vsync, height, adjusted)) goto uncertain;
 unsigned after, height_after, init_after;
 if (!fg_scanout_read(FG_SCAN_INTF+0x288, &after) || after != (adjusted | (1u << 19)) ||
     !fg_scanout_read(FG_SCAN_INTF+0x28c, &height_after) || height_after != height ||
     !fg_scanout_read(FG_SCAN_INTF+0x294, &init_after) || init_after != init) goto uncertain;
 fg_scanout_panel120_active = 1;
 fg_scanout_period_us = 8333;
 if (!fg_scanout_explicit_drain && !fg_scanout_te_mode) fg_scanout_drain_us = FG_SCAN_DRAIN_US/2;
 fg_scanout_te_init = init & 0xffff;
 printf("FUXI SCANOUT: panel120 switched vsync=%#x->%#x height=%#x->%#x init=%#x->%#x period_us=%u drain_us=%u\n",
     vsync, after, height, height_after, init, init_after, fg_scanout_period_us, fg_scanout_drain_us);
 return 1;
uncertain:
 /* A timed-out DMA may have sent 60 00 already. Keep 60 Hz accounting,
  * never claim success or send rollback/START into an uncertain transfer. */
 printf("FUXI SCANOUT: panel120 aborted; panel state uncertain, reboot required\n");
 return fg_scanout_stop("panel120 switch incomplete");
}
/* 1 writable, 0 wait, -1 held after error. Normal gates do not spin or sleep;
 * the one-time panel120 switch uses bounded polling before granting writes.
 * First
 * disable AR, then allow a complete 60 Hz interval plus margin before testing
 * the write-line and CTL idle indicators. The same guard follows every START.
 * This deliberately conservative policy is not a TE/vblank interrupt driver;
 * fuxiscante=1 polls the TE frame count instead of the fixed floor.
 */
static int fg_scanout_acquire(unsigned publication)
{
 if (!fg_scanout_requested) return 1;
 if (fg_scanout_failed) return -1;
 if (!fuxi_gpu_scanout_owned()) {
  if (!fuxi_gpu_ready()) return 1; /* startup/unsupported GPU keeps old path */
  unsigned ar;
  if (!fg_scanout_read(FG_SCAN_INTF+0x2b4, &ar)) return fg_scanout_stop("read autorefresh");
  /* Block all generic START callers before changing display ownership. */
  __atomic_store_n(&fg_scanout_owned, 1, __ATOMIC_RELEASE);
  if (!fg_scanout_write(FG_SCAN_INTF+0x2b4, ar & ~0x80000000u))
   return fg_scanout_stop("disable autorefresh");
  __asm__ volatile("dsb sy" ::: "memory");
  if (fg_scanout_double_requested) {
   unsigned src;
   if (!fg_scanout_read(0xae05014, &src) || src != 0xb8000000u)
    return fg_scanout_stop("double initial SRC0");
  }
  unsigned rd = 0, wr = 0, st = 0, tc[8] = {0};
  int status = fg_scanout_read(FG_SCAN_INTF+0x298, &rd) &&
      fg_scanout_read(FG_SCAN_INTF+0x2b0, &wr) && fg_scanout_read(FG_SCAN_CTL+0x1c, &st);
  if (!status && fg_scanout_double_requested) return fg_scanout_stop("double inherited status");
  static const unsigned tear[8] = {0x280, 0x284, 0x288, 0x28c, 0x294, 0x29c, 0x2a0, 0x2a8};
  for (unsigned i = 0; i < 8; i++) {
   int good = fg_scanout_read(FG_SCAN_INTF+tear[i], &tc[i]);
   if (!good && fg_scanout_double_requested) return fg_scanout_stop("double tear setup");
  }
  fg_scanout_te_init = tc[4] & 0xffff;
  fg_scanout_arm(rd);
  fg_scanout_window_start = rsr("cntpct_el0");
  fg_scanout_te_window = rd >> 16;
  printf("FUXI SCANOUT: owned, autorefresh disabled; draining inherited transfer ar=%#x rd=%#x wr=%#x start=%#x\n",
      ar, rd, wr, st);
  printf("FUXI SCANOUT: tear sel=%#x en=%#x vsync=%#x height=%#x init=%#x thresh=%#x start_pos=%#x wr_irq=%#x\n",
      tc[0], tc[1], tc[2], tc[3], tc[4], tc[5], tc[6], tc[7]);
  return 0;
 }
 unsigned ar, line, start;
 if (!fg_scanout_read(FG_SCAN_INTF+0x2b4, &ar) || (ar & 0x80000000u))
  return fg_scanout_stop("autorefresh changed");
 if (fg_scanout_double_active) {
  unsigned src;
  if (!fg_scanout_read(0xae05014, &src) || src != fg_scanout_pa(fg_scanout_front))
   return fg_scanout_stop("double SRC0 changed");
 }
 if (fg_busy) { fg_scanout_wait_ring++; if (publication) fg_scanout_charge_to(FG_WHY_RING); return 0; }
 if (fg_scanout_double_active && !publication) {
  if (fg_scanout_back_ready && !fg_scanout_triple_requested) return 0;
  fg_scanout_copy = fg_scanout_target();
  fg_scanout_writable = 1;
  return 1;
 }
 fg_scanout_xfer_sample();
 if (rsr("cntpct_el0") < fg_scanout_after) { fg_scanout_wait_floor++; if (publication) fg_scanout_charge_to(FG_WHY_FLOOR); return 0; }
 if (fg_scanout_te_mode) {
  unsigned count;
  if (!fg_scanout_read(FG_SCAN_INTF+0x298, &count)) return fg_scanout_stop("read te count");
  unsigned since = ((count >> 16) - fg_scanout_te_frame) & 0xffffu;
  unsigned past = (count & 0xffffu) - fg_scanout_te_init;
  if (since == 0 || (since == 1 && (count & 0xffffu) >= fg_scanout_te_init && past < FG_SCAN_TE_MARGIN)) {
   uint64_t hz = rsr("cntfrq_el0");
   unsigned stall_us = fg_scanout_panel120_active ? FG_SCAN_TE_STALL_US/2 : FG_SCAN_TE_STALL_US;
   if (rsr("cntpct_el0") >= fg_scanout_started + hz*stall_us/1000000) {
    /* No usable TE count: the default floor (from this START) for the boot. */
    unsigned fallback = fg_scanout_panel120_active ? FG_SCAN_DRAIN_US/2 : FG_SCAN_DRAIN_US;
    printf("FUXI SCANOUT: te counter stalled count=%#x since=%u; floor fallback drain_us=%u\n",
        count, since, fallback);
    fg_scanout_te_mode = 0;
    fg_scanout_drain_us = fallback;
    fg_scanout_after = fg_scanout_started + hz*fallback/1000000;
    fg_scanout_timeout = fg_scanout_after + hz/10;
   }
   fg_scanout_wait_te++; if (publication) fg_scanout_charge_to(FG_WHY_TE);
   return 0;
  }
 }
 if (!fg_scanout_read(FG_SCAN_INTF+0x2b0, &line) ||
     !fg_scanout_read(FG_SCAN_CTL+0x1c, &start)) return fg_scanout_stop("read transfer status");
 if ((start & 1) || ((line & 65535) != 0 && (line & 65535) < 2400)) {
  if (rsr("cntpct_el0") >= fg_scanout_timeout) {
   unsigned rd = 0;
   (void)fg_scanout_read(FG_SCAN_INTF+0x298, &rd);
   printf("FUXI SCANOUT: drain timeout wr=%#x rd=%#x start=%#x\n", line, rd, start);
   return fg_scanout_stop("transfer drain timeout");
  }
  fg_scanout_wait_xfer++; if (publication) fg_scanout_charge_to(FG_WHY_XFER);
  return 0;
 }
 if (fg_scanout_panel120_requested && !fg_scanout_panel120_attempted) {
  int switched = fg_panel120_switch();
  if (switched < 0) return -1;
  if (switched) {
   unsigned count;
   if (!fg_scanout_read(FG_SCAN_INTF+0x298, &count)) return fg_scanout_stop("panel120 read te count");
   fg_scanout_arm(count);
   /* Drop the mixed-rate diagnostic window and wait with the new gates. */
   fg_scanout_window_start = rsr("cntpct_el0");
   fg_scanout_te_window = count >> 16;
   return 0;
  }
 }
 if (fg_scanout_double_requested && !fg_scanout_double_initialized) {
  fg_scanout_pixels_b = fuxi_gpu_nc_pointer(0xb8a00000u, 4320u*2400);
  if (!fg_scanout_pixels || !fg_scanout_pixels_b) return fg_scanout_stop("double buffer mapping");
  for (unsigned i = 0; i < 1080u*2400; i++)
   ((volatile uint32_t *)fg_scanout_pixels_b)[i] = ((const volatile uint32_t *)fg_scanout_pixels)[i];
  if (fg_scanout_triple_requested) {
   fg_scanout_pixels_c = fuxi_gpu_nc_pointer(0xb9400000u, 4320u*2400);
   if (!fg_scanout_pixels_c) return fg_scanout_stop("triple buffer mapping");
   for (unsigned i = 0; i < 1080u*2400; i++)
    ((volatile uint32_t *)fg_scanout_pixels_c)[i] = ((const volatile uint32_t *)fg_scanout_pixels)[i];
  }
  fg_scanout_double_initialized = fg_scanout_double_active = 1;
  printf("FUXI SCANOUT: double=1 A=0xb8000000 B=0xb8a00000 full_copy=1 flush=RMW|1%s\n",
      fg_scanout_triple_requested ? " triple=1 C=0xb9400000" : "");
 }
 __asm__ volatile("dsb sy" ::: "memory");
 if (!publication && fg_scanout_double_active) fg_scanout_copy = fg_scanout_target();
 fg_scanout_writable = 1;
 return 1;
}
int fuxi_gpu_scanout_acquire(void)
{
 FG_GUARD;
 /* A CPU frame left A as the front: GPU frames are complete, so B is free. */
 if (fg_scanout_double_initialized && !fg_scanout_double_active && !fg_scanout_failed && fuxi_gpu_scanout_owned()) {
  fg_scanout_double_active = 1; fg_scanout_front = fg_scanout_back_ready = 0;
  if (++fg_scanout_reentries <= 4) printf("FUXI SCANOUT: double re-entered after CPU frame (%u)\n", fg_scanout_reentries);
 }
 return fg_scanout_acquire(0);
}
int fuxi_gpu_scanout_double(void)
{
 return fg_scanout_double_active && !fg_scanout_failed;
}
int fuxi_gpu_scanout_triple(void)
{
 return fg_scanout_double_active && fg_scanout_triple_requested && !fg_scanout_failed;
}
int fuxi_gpu_scanout_pending(void)
{
 return fg_scanout_back_ready && !fg_scanout_failed;
}
/* CPU writers retain fPixels=A. Drain first, preserve the newest image and
 * use the single-buffer guard until the next GPU frame re-enters double mode. */
int fuxi_gpu_scanout_cpu_acquire(void)
{
 FG_GUARD;
 int writable = fg_scanout_acquire(1);
 if (writable != 1 || !fg_scanout_double_active) return writable;
 unsigned newest = fg_scanout_back_ready ? (fg_scanout_triple_requested ? fg_scanout_ready : fg_scanout_front ^ 1u) : fg_scanout_front;
 if (newest) {
  for (unsigned i = 0; i < 1080u*2400; i++)
   ((volatile uint32_t *)fg_scanout_pixels)[i] = ((const volatile uint32_t *)fg_scanout_pixels_at(newest))[i];
 }
 if (!fg_scanout_address(0xb8000000u)) return fg_scanout_stop("CPU restore A");
 if (fg_scanout_back_ready) fg_scanout_ready_drops++;
 fg_scanout_back_ready = fg_scanout_double_active = fg_scanout_front = 0;
 if (fg_scanout_reentries < 4) printf("FUXI SCANOUT: double CPU fallback, drained; single buffer A until the next GPU frame\n");
 return 1;
}
/* The caller has acquired scanout and finished ALL CPU writes or observed the
 * GPU copy's cache-clean fence. Generic console/display kicks are suppressed
 * while owned. Autorefresh stays off; only this function starts transfers.
 */
static int fg_scanout_publish(void)
{
 if (fg_scanout_failed || fg_busy || !fg_scanout_writable) return -1;
 if (fg_scanout_double_active) {
  int writable = fg_scanout_acquire(1);
  if (writable != 1) {
   if (!writable) fg_scanout_retries++;
   return writable < 0 ? -1 : 2;
  }
 }
 unsigned ar, start;
 fg_scanout_probe_account();
 if (!fg_scanout_read(FG_SCAN_INTF+0x2b4, &ar) || (ar & 0x80000000u) ||
     !fg_scanout_read(FG_SCAN_CTL+0x1c, &start) || (start & 1))
  return fg_scanout_stop("publish transfer status");
 fg_scanout_charge_to(fg_scanout_last_why); /* the last wait ran until this poll */
 __asm__ volatile("dsb sy" ::: "memory");
 fg_scanout_writable = 0;
 if (fg_scanout_double_active && !fg_scanout_address(fg_scanout_pa(fg_scanout_triple_requested ? fg_scanout_ready : fg_scanout_front ^ 1u)))
  return fg_scanout_stop("double SRC0 flush");
 if (!fg_scanout_write(FG_SCAN_CTL+0x1c, 1)) return fg_scanout_stop("start transfer");
 fg_scanout_probe_t0 = rsr("cntpct_el0");
 fg_scanout_stamps.previous_start = fg_scanout_stamps.start;
 fg_scanout_stamps.start = mach_absolute_time();
 if (fg_scanout_double_active) {
  fg_scanout_front = fg_scanout_triple_requested ? fg_scanout_ready : fg_scanout_front ^ 1u;
  fg_scanout_buffer_flips[fg_scanout_front]++;
  fg_scanout_back_ready = 0;
  fg_scanout_flips++;
 }
 /* Read pointer after START: the TE gate then waits for a TE that follows
  * this read, so a TE racing the START write only makes it wait longer. */
 unsigned count = 0;
 __asm__ volatile("dsb sy" ::: "memory");
 if (!fg_scanout_read(FG_SCAN_INTF+0x298, &count)) return fg_scanout_stop("read te count");
 fg_scanout_probe_kick(count >> 16);
 uint64_t now = rsr("cntpct_el0"), hz = rsr("cntfrq_el0");
 /* START-to-START gaps in actual panel periods: <=1, <=2, <=3, longer. */
 if (fg_scanout_last_start) {
  uint64_t gap = now - fg_scanout_last_start, tick = hz*fg_scanout_period_us/1000000;
  fg_scanout_iv[gap <= tick + tick/8 ? 0 : gap <= 2*tick + tick/8 ? 1 : gap <= 3*tick + tick/8 ? 2 : 3]++;
 }
 fg_scanout_last_start = now;
 if (fg_scanout_ready_at) { fg_scanout_r2s += now - fg_scanout_ready_at; fg_scanout_r2s_n++; }
 fg_scanout_ready_at = fg_scanout_charged_at = 0;
 fg_scanout_te_frames = count >> 16;
 if (++fg_scanout_starts % 600 == 0) {
  uint64_t span = now - fg_scanout_window_start;
  unsigned frames = (fg_scanout_te_frames - fg_scanout_te_window) & 0xffffu;
  /* TE rate in 0.1 Hz over the window (the 16-bit count wraps after ~9 min at 120 Hz). */
  unsigned te_dhz = span ? (unsigned)((uint64_t)frames*hz*10/span) : 0;
  printf("FUXI SCANOUT: waits per 600 starts ring=%u floor=%u transfer=%u te=%u drain_us=%u te_gate=%u"
      " te_hz=%u.%u gaps=%u/%u/%u/%u\n",
      fg_scanout_wait_ring, fg_scanout_wait_floor, fg_scanout_wait_xfer, fg_scanout_wait_te,
      fg_scanout_drain_us, fg_scanout_te_mode, te_dhz/10, te_dhz%10,
      fg_scanout_iv[0], fg_scanout_iv[1], fg_scanout_iv[2], fg_scanout_iv[3]);
  fg_scanout_probe_report();
  if (fg_abi_spin) {
   printf("FUXI SCANOUT: te_start spin wakes=%u published=%u no_back_buffer=%u expired_or_raced=%u\n",
       fg_scanout_spin_stat[0], fg_scanout_spin_stat[1], fg_scanout_spin_stat[2], fg_scanout_spin_stat[3]);
   fg_scanout_spin_stat[0] = fg_scanout_spin_stat[1] = fg_scanout_spin_stat[2] = fg_scanout_spin_stat[3] = 0;
  }
  {
   uint64_t n = fg_scanout_xfer_n ? fg_scanout_xfer_n : 1, r = fg_scanout_r2s_n ? fg_scanout_r2s_n : 1, cum = 0;
   unsigned p50 = 0;
   for (unsigned i = 0; i < 8; i++) { cum += fg_scanout_xfer_hist[i]; if (cum*2 >= n) { p50 = i < 7 ? fg_scanout_xfer_edges[i] : 0; break; } }
   printf("FUXI SCANOUT: xfer_done_us n=%u avg=%llu p50<%u max=%llu hist<5/6/7/7.5/8/9/12ms/more=%u/%u/%u/%u/%u/%u/%u/%u"
       " ready_to_start_us n=%llu avg=%llu floor=%llu xfer=%llu te=%llu ring=%llu start_bit=%u\n",
       fg_scanout_xfer_n, (unsigned long long)(fg_scanout_xfer_sum/n), p50, (unsigned long long)fg_scanout_xfer_max,
       fg_scanout_xfer_hist[0], fg_scanout_xfer_hist[1], fg_scanout_xfer_hist[2], fg_scanout_xfer_hist[3],
       fg_scanout_xfer_hist[4], fg_scanout_xfer_hist[5], fg_scanout_xfer_hist[6], fg_scanout_xfer_hist[7],
       (unsigned long long)fg_scanout_r2s_n, (unsigned long long)(fg_scanout_r2s*1000000/hz/r),
       (unsigned long long)(fg_scanout_charge[FG_WHY_FLOOR]*1000000/hz/r), (unsigned long long)(fg_scanout_charge[FG_WHY_XFER]*1000000/hz/r),
       (unsigned long long)(fg_scanout_charge[FG_WHY_TE]*1000000/hz/r), (unsigned long long)(fg_scanout_charge[FG_WHY_RING]*1000000/hz/r), fg_abi_start);
   fg_scanout_xfer_n = 0; fg_scanout_xfer_sum = fg_scanout_xfer_max = 0; fg_scanout_r2s = fg_scanout_r2s_n = 0;
   for (unsigned i = 0; i < 8; i++) fg_scanout_xfer_hist[i] = 0;
   for (unsigned i = 0; i < 4; i++) fg_scanout_charge[i] = 0;
  }
  if (fg_scanout_double_active) {
   unsigned current, expected = fg_scanout_pa(fg_scanout_front);
   if (!fg_scanout_read(0xae050a4, &current)) return fg_scanout_stop("read current SRC0");
   if (current == expected) fg_scanout_current_match++; else fg_scanout_current_mismatch++;
   char triple[128] = {0};
   if (fg_scanout_triple_requested) snprintf(triple, sizeof(triple), " triple=1 flips_a=%u flips_b=%u flips_c=%u drops=%u",
       fg_scanout_buffer_flips[0], fg_scanout_buffer_flips[1], fg_scanout_buffer_flips[2], fg_scanout_ready_drops);
   printf("FUXI SCANOUT: double=1 flips=%u back_ready_drops=%u publish_retries=%u current_src0=%#x expected=%#x match=%u mismatch=%u%s\n",
       fg_scanout_flips, fg_scanout_ready_drops, fg_scanout_retries, current, expected,
       fg_scanout_current_match, fg_scanout_current_mismatch, triple);
   fg_scanout_buffer_flips[0] = fg_scanout_buffer_flips[1] = fg_scanout_buffer_flips[2] = 0;
   fg_scanout_flips = fg_scanout_ready_drops = fg_scanout_retries = 0;
  }
  fg_scanout_wait_ring = fg_scanout_wait_floor = fg_scanout_wait_xfer = fg_scanout_wait_te = 0;
  fg_scanout_iv[0] = fg_scanout_iv[1] = fg_scanout_iv[2] = fg_scanout_iv[3] = 0;
  fg_scanout_window_start = now;
  fg_scanout_te_window = fg_scanout_te_frames;
 }
 __asm__ volatile("dsb sy" ::: "memory");
 fg_scanout_arm(count);
 fg_scanout_spin_kick();
 return 1;
}
int fuxi_gpu_scanout_publish(void)
{
 if (!fuxi_gpu_scanout_owned()) return 0;
 FG_GUARD;
 if (fg_scanout_failed || fg_busy || !fg_scanout_writable) return -1;
 if (fg_scanout_double_active) {
  if (fg_scanout_back_ready && fg_scanout_triple_requested) fg_scanout_ready_drops++;
  fg_scanout_ready = fg_scanout_triple_requested ? fg_scanout_copy : fg_scanout_front ^ 1u;
  fg_scanout_back_ready = 1;
  fg_scanout_ready_at = rsr("cntpct_el0"); fg_scanout_charged_at = 0;
 }
 return fg_scanout_publish();
}
int fuxi_gpu_scanout_retry(void)
{
 FG_GUARD;
 if (fg_scanout_failed) return -1;
 if (fg_scanout_async_published) { fg_scanout_async_published = 0; return 1; } /* the spin thread already STARTed the waiting buffer */
 if (!fg_scanout_back_ready || fg_busy) return 0;
 return fg_scanout_publish();
}
/* Reserve the idle ring across the surface-cache transaction. Client submit
 * returns busy until release; map/TLBI/copy cannot race a client TTBR switch.
 * Called under SweetSurface's lock, matching the existing surface -> GPU order.
 */
int fuxi_gpu_scanout_reserve(void)
{
 if (!fg_scanout_requested) return 1;
 FG_GUARD;
 if (!fuxi_gpu_ready()) return 1;
 if (fg_scanout_failed || fg_scanout_reserved || fg_busy) return 0;
 if (fg_abi_live_for_scanout() && fg_abi_observe() != 0) { if (fg_client_busy) fg_abi_present_wait = 1; return 0; }
 if (fg_client_busy) { fg_abi_present_wait = 1; return 0; }
 fg_scanout_reserved = 1;
 return 1;
}
void fuxi_gpu_scanout_release(void)
{
 if (!fg_scanout_requested) return;
 FG_GUARD;
 fg_scanout_reserved = 0;
}
/* Compare the whole copied rectangle to exactly the
 * CPU row-copy oracle, after the fence and before releasing the source pin.
 * Scalar aligned device loads only; no CPU framebuffer writes in this check.
 */
static int fg_scanout_verify(void)
{
 if (!fg_scanout_checks || fg_scanout_checked >= fg_scanout_checks) return 1;
 uint64_t began = mach_absolute_time(), ns;
 unsigned mismatch = 0, first_x = 0, first_y = 0;
 __asm__ volatile("dsb sy" ::: "memory");
 for (unsigned y = 0; y < fg_scanout_h; y++) {
  const uint32_t *src = (const uint32_t *)(fg_scanout_source +
      (uint64_t)(fg_scanout_sy+y)*fg_scanout_pitch + fg_scanout_sx*4);
  const volatile uint32_t *dst = (const volatile uint32_t *)(fg_scanout_pixels_at(fg_scanout_double_active ? (fg_scanout_triple_requested ? fg_scanout_copy : fg_scanout_front ^ 1u) : 0) +
      (uint64_t)(fg_scanout_dy+y)*4320 + fg_scanout_dx*4);
  for (unsigned x = 0; x < fg_scanout_w; x++) if (src[x] != dst[x]) {
   if (!mismatch) { first_x = x; first_y = y; }
   mismatch++;
  }
 }
 absolutetime_to_nanoseconds(mach_absolute_time()-began, &ns);
 printf("FUXI SCANOUT: pixel frame=%u pixels=%u mismatches=%u first=%u,%u check_us=%llu\n",
     ++fg_scanout_checked, fg_scanout_w*fg_scanout_h, mismatch, first_x, first_y,
     (unsigned long long)(ns/1000));
 return !mismatch;
}
