/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 NativeMetal contributors
 * Ported from UniXNU drivers/NativeMetal/GpuScanoutClient.h (MIT).
 * The CPU pixel oracle is adapted below; Fuxi display ownership is original.
 * Finished-frame copy/fence design: NativeMetal/GpuScanoutPresenter.h (MIT).
 * See LICENSE.NativeMetal.
 */
/* GPU-SCANOUT: Fuxi command-mode display ownership. Original implementation.
 * No panel/DSI reset, address flip, clock change or firmware dependency.
 * fuxipanel120=1 optionally sends the panel's timing-switch commands once,
 * only after ownership and inherited transfer drain. Default does no DSI IO.
 * Register meanings already used by the board's inherited-display probe:
 * INTF1 AUTOREFRESH_CONFIG +0x2b4 bit31; TEAR_LINE_COUNT +0x2b0 low16
 * write line; CTL START +0x1c bit0. Do not copy while a transfer can read RAM.
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

void fuxi_gpu_scanout_configure(uintptr_t pixels, unsigned enabled, unsigned check)
{
 /* AppleCLCD calls once, before fuxi_gpu_start publishes the GPU. */
 fg_scanout_pixels = pixels;
 fg_scanout_requested = enabled == 1;
 fg_scanout_checks = fg_scanout_requested && check == 1 ? 4 : 0;
 if (fg_scanout_requested)
{
  unsigned drain = 0, te = 0, panel120 = 0;
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
/* Poll brackets the edge between the last old count and the first new count.
 * Two narrow, consecutive brackets establish a measured period. An IIR (1/8
 * period, 1/4 phase) rejects polling jitter without following the software
 * timer's phase. Never extrapolate an advancing counter through a stall.
 * All estimator writes belong to the caller, so this API needs no FG_GUARD. */
int fuxi_gpu_scanout_te(struct fg_te_observer *te)
{
 unsigned count;
 if (!fuxi_gpu_scanout_owned() || !fg_scanout_read(FG_SCAN_INTF+0x298, &count)) {
  te->initialized = te->samples = 0;
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
 fg_scanout_after = now + hz*fg_scanout_drain_us/1000000;
 fg_scanout_timeout = fg_scanout_after + hz/10;
 fg_scanout_started = now;
}
static int fg_scanout_stop(const char *reason)
{
 if (!fg_scanout_failed)
  printf("FUXI SCANOUT: stopped reason=%s; panel held, no framebuffer writes\n", reason);
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
int fuxi_gpu_scanout_acquire(void)
{
 FG_GUARD;
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
  unsigned rd = 0, wr = 0, st = 0, tc[8] = {0};
  (void)fg_scanout_read(FG_SCAN_INTF+0x298, &rd);
  (void)fg_scanout_read(FG_SCAN_INTF+0x2b0, &wr);
  (void)fg_scanout_read(FG_SCAN_CTL+0x1c, &st);
  static const unsigned tear[8] = {0x280, 0x284, 0x288, 0x28c, 0x294, 0x29c, 0x2a0, 0x2a8};
  for (unsigned i = 0; i < 8; i++) (void)fg_scanout_read(FG_SCAN_INTF+tear[i], &tc[i]);
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
 if (fg_busy) { fg_scanout_wait_ring++; return 0; }
 if (rsr("cntpct_el0") < fg_scanout_after) { fg_scanout_wait_floor++; return 0; }
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
   fg_scanout_wait_te++;
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
  fg_scanout_wait_xfer++;
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
 __asm__ volatile("dsb sy" ::: "memory");
 fg_scanout_writable = 1;
 return 1;
}
/* The caller has acquired scanout and finished ALL CPU writes or observed the
 * GPU copy's cache-clean fence. Generic console/display kicks are suppressed
 * while owned. Autorefresh stays off; only this function starts transfers.
 */
int fuxi_gpu_scanout_publish(void)
{
 if (!fuxi_gpu_scanout_owned()) return 0;
 FG_GUARD;
 if (fg_scanout_failed || fg_busy || !fg_scanout_writable) return -1;
 unsigned ar, start;
 if (!fg_scanout_read(FG_SCAN_INTF+0x2b4, &ar) || (ar & 0x80000000u) ||
     !fg_scanout_read(FG_SCAN_CTL+0x1c, &start) || (start & 1))
  return fg_scanout_stop("publish transfer status");
 __asm__ volatile("dsb sy" ::: "memory");
 fg_scanout_writable = 0;
 if (!fg_scanout_write(FG_SCAN_CTL+0x1c, 1)) return fg_scanout_stop("start transfer");
 /* Read pointer after START: the TE gate then waits for a TE that follows
  * this read, so a TE racing the START write only makes it wait longer. */
 unsigned count = 0;
 __asm__ volatile("dsb sy" ::: "memory");
 if (!fg_scanout_read(FG_SCAN_INTF+0x298, &count)) return fg_scanout_stop("read te count");
 uint64_t now = rsr("cntpct_el0"), hz = rsr("cntfrq_el0");
 /* START-to-START gaps in actual panel periods: <=1, <=2, <=3, longer. */
 if (fg_scanout_last_start) {
  uint64_t gap = now - fg_scanout_last_start, tick = hz*fg_scanout_period_us/1000000;
  fg_scanout_iv[gap <= tick + tick/8 ? 0 : gap <= 2*tick + tick/8 ? 1 : gap <= 3*tick + tick/8 ? 2 : 3]++;
 }
 fg_scanout_last_start = now;
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
  fg_scanout_wait_ring = fg_scanout_wait_floor = fg_scanout_wait_xfer = fg_scanout_wait_te = 0;
  fg_scanout_iv[0] = fg_scanout_iv[1] = fg_scanout_iv[2] = fg_scanout_iv[3] = 0;
  fg_scanout_window_start = now;
  fg_scanout_te_window = fg_scanout_te_frames;
 }
 __asm__ volatile("dsb sy" ::: "memory");
 fg_scanout_arm(count);
 return 1;
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
 if (fg_abi_live_for_scanout() && fg_abi_observe() != 0) return 0;
 if (fg_client_busy) return 0;
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
  const volatile uint32_t *dst = (const volatile uint32_t *)(fg_scanout_pixels +
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
