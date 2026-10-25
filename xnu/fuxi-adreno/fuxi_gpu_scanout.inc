/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 NativeMetal contributors
 * Ported from UniXNU drivers/NativeMetal/GpuScanoutClient.h (MIT).
 * The CPU pixel oracle is adapted below; Fuxi display ownership is original.
 * Finished-frame copy/fence design: NativeMetal/GpuScanoutPresenter.h (MIT).
 * See LICENSE.NativeMetal.
 */
/* GPU-SCANOUT: Fuxi command-mode display ownership. Original implementation.
 * No panel/DSI reset, address flip, clock change or firmware dependency.
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
  unsigned drain = 0, te = 0;
  fg_scanout_te_mode = PE_parse_boot_argn("fuxiscante", &te, sizeof(te)) && te == 1;
  /* The TE gate needs no floor unless one is given explicitly. */
  if (fg_scanout_te_mode) fg_scanout_drain_us = 0;
  if (PE_parse_boot_argn("fuxiscandrain", &drain, sizeof(drain)) && drain >= 4000 && drain <= FG_SCAN_DRAIN_US)
   fg_scanout_drain_us = drain;
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
/* 1 writable, 0 wait, -1 held after error. No spinning or sleeping. First
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
   if (rsr("cntpct_el0") >= fg_scanout_started + hz*FG_SCAN_TE_STALL_US/1000000) {
    /* No usable TE count: the default floor (from this START) for the boot. */
    printf("FUXI SCANOUT: te counter stalled count=%#x since=%u; floor fallback drain_us=%u\n",
        count, since, FG_SCAN_DRAIN_US);
    fg_scanout_te_mode = 0;
    fg_scanout_drain_us = FG_SCAN_DRAIN_US;
    fg_scanout_after = fg_scanout_started + hz*FG_SCAN_DRAIN_US/1000000;
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
 /* START-to-START gaps in 60 Hz periods: <=1, <=2, <=3, longer (idle). */
 if (fg_scanout_last_start) {
  uint64_t gap = now - fg_scanout_last_start, tick = hz*16667/1000000;
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
