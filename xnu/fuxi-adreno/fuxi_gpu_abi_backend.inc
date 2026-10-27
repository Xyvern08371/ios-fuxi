/* Built into fuxi_adreno.c, with access to its proven SMMU/ring helpers.
 * Single privileged diagnostic client, not a security boundary for PM4.
 * Its root has no scanout or surface-cache mappings. The kernel-only runtime
 * pages below va_start are identical in both roots; only idle TTBR switches
 * are allowed. Client leaves never modify the present root.
 */
#define FG_ABI_PT (G_MEM+0x1c00000)
#define FG_ABI_PT_BYTES 0x400000u
#define FG_ABI_FENCE_OFF 128u
_Static_assert(FG_ABI_PT >= G4_DST_PA+G4_BYTES+4096, "ABI table overlap");
_Static_assert(FG_ABI_PT >= G5_META_PA+G5_META_BYTES, "ABI/zap metadata overlap");
_Static_assert(FG_ABI_PT >= FG_FW_BASE+3*FG_FW_SLOT, "ABI/cold handoff or firmware overlap");
_Static_assert(FG_ABI_PT+FG_ABI_PT_BYTES <= G_MEM+G_MEM_SIZE, "ABI tables exceed scratch");
static unsigned fg_abi_live, fg_abi_seq, fg_abi_completed, fg_abi_poisoned, fg_abi_timed_out;
static int fg_abi_drained = 1;
static int fg_abi_live_for_scanout(void) { return fg_abi_live; }
static uint64_t fg_client_deadline;
static thread_call_data_t fg_client_watchdog;
/* Every mapped import can be referenced indirectly by PM4, not just the IB
 * list. Serialize cache handoff with the same GPU lock as submit/retirement.
 * The user client owns these prepared kernel mappings until successful unmap. */
static struct { uint64_t iova, bytes; uintptr_t source; unsigned permissions; } fg_abi_imports[FG_ABI_MAX_IMPORTS];
static unsigned fg_abi_host_handoff, fg_abi_ncimport, fg_abi_cohtest, fg_abi_tlbpages;
/* fuxigpuncimport is a bitmask: 1 NC-import map-time clean, 64 coherent-import self-test (skips the per-submit
 * clean/retire invalidate only if it proves the GPU path coherent), 128 ranged TLBIVAL for a small unmap. Default 0. */
static unsigned fg_abi_coh, fg_abi_coh_tries; /* coh: 0 untested, 1 proven coherent, 2 refused */
static uint64_t fg_abi_cleaned, fg_abi_skipped, fg_abi_invalidated;
static void fg_abi_cache_region(uintptr_t source, uint64_t bytes, int complete)
{
 uintptr_t end=source+bytes;
 for (uintptr_t p=source;p<end;p+=64) {
  if (complete) { __asm__ volatile("dc ivac, %0" :: "r"(p) : "memory"); }
  else { __asm__ volatile("dc civac, %0" :: "r"(p) : "memory"); }
 }
 if (complete) fg_abi_invalidated+=bytes;
 else fg_abi_cleaned+=bytes;
}
static void fg_abi_import_cache(int complete)
{
 if (complete && !fg_abi_host_handoff) return;
 __asm__ volatile("dsb sy" ::: "memory");
 if (fg_abi_coh==1) { /* self-test proved CPU/GPU coherence for import-class mappings: no cache maintenance */
  for (unsigned i=0;i<FG_ABI_MAX_IMPORTS;i++) if (fg_abi_imports[i].iova) fg_abi_skipped+=fg_abi_imports[i].bytes;
  fg_abi_host_handoff=0;
  return;
 }
 /* A read-only PTE cannot be written by the GPU, so retirement has no stale
  * lines to drop for it; the submit-side clean still covers every import. */
 for (unsigned i=0;i<FG_ABI_MAX_IMPORTS;i++) if (fg_abi_imports[i].iova && (!complete || (fg_abi_imports[i].permissions&2))) {
  /* NativeAdreno b686b90 (MIT), adapted: GPU-written imports are already
   * NC in our SMMU. Opt-in requires NO CPU writes for their mapped lifetime,
   * not merely while a submission is in flight. Clean once at map, then
   * avoid re-cleaning unchanged CPU lines on every indirect PM4 use.
   * Permissions alone do NOT prove CPU immutability: keep default off.
   * Unlike upstream, retain completion IVAC: CPU readers (fallback/pixel
   * oracle) may have cached an older frame. Never clean over GPU writes.
   * Read-only imports may change on CPU between submissions; keep cleaning.
   * An IB list is not a resource list, so keep walking ALL mapped imports. */
  if (!complete && fg_abi_ncimport && (fg_abi_imports[i].permissions&2)) {
   fg_abi_skipped+=fg_abi_imports[i].bytes;
   continue;
  }
  fg_abi_cache_region(fg_abi_imports[i].source,fg_abi_imports[i].bytes,complete);
 }
 __asm__ volatile("dsb sy" ::: "memory");
 fg_abi_host_handoff=!complete;
}
void fuxi_gpu_import_stats(uint64_t *cleaned, uint64_t *skipped, uint64_t *invalidated,
    uint64_t *mapped, unsigned *imports)
{
 FG_GUARD;
 *cleaned=fg_abi_cleaned; *skipped=fg_abi_skipped; *invalidated=fg_abi_invalidated;
 *mapped=0; *imports=0;
 for (unsigned i=0;i<FG_ABI_MAX_IMPORTS;i++) if (fg_abi_imports[i].iova) {
  *mapped+=fg_abi_imports[i].bytes; ++*imports;
 }
}
static volatile uint64_t *fg_abi_leaf(uint64_t iova)
{
 unsigned chunk = (unsigned)((iova-FG_ABI_VA_START)>>21);
 return (volatile uint64_t *)G_PTR_RANGE(FG_ABI_PT+(4u+chunk)*4096,4096)+((iova>>12)&511);
}
/* Cached mirror of which client 4K leaves are live. Only map/unmap/init write client leaves, so the overlap test reads this
 * instead of NC page-table memory (UniXNU ArmSmmu4K: no existing-range pass for caller-owned pages). */
static uint64_t fg_abi_used[(FG_ABI_VA_SIZE>>12)/64];
static inline unsigned fg_abi_pg(uint64_t iova) { return (unsigned)((iova-FG_ABI_VA_START)>>12); }
static inline int fg_abi_used_get(unsigned pg) { return (fg_abi_used[pg>>6]>>(pg&63))&1; }
static inline void fg_abi_used_put(unsigned pg, int on)
{ if (on) fg_abi_used[pg>>6]|=UINT64_C(1)<<(pg&63); else fg_abi_used[pg>>6]&=~(UINT64_C(1)<<(pg&63)); }
/* Ranged invalidate of a small unmap (opt-in value 128): TLBIVAL (+0x620, AArch64 format VA[55:12]|ASID<<48) per removed
 * 4K leaf, then TLBSYNC. Leaf-only: the L1/L2 tables are fixed for the life of the root. Same status codes as g4_tlb. */
#define FG_ABI_TLB_PAGE_LIMIT 512u
static int g3_sw64(unsigned off, uint64_t v) { return !fg_access_failed && !fg_access(GPU_SMMU_BASE+off,&v,1,1); }
static int fg_abi_tlb_pages(uint64_t iova, uint64_t bytes)
{
 unsigned cb=g_cb(g3_bank);
 __asm__ volatile("dsb sy" ::: "memory");
 for (uint64_t off=0;off<bytes;off+=4096)
  if (!g3_sw64(cb+0x620,(((iova+off)>>12)&((UINT64_C(1)<<44)-1))|((uint64_t)g3_asid<<48))) return 0xe01;
 if (!g3_sw(cb+0x7f0,0)) return 0xe01;
 for (unsigned i=0;i<1000;i++) {
  uint32_t st=g3_sr(cb+0x7f4);
  if (fg_access_failed) return 0xe01;
  if (!(st&1)) return 0;
  delay(1);
 }
 return 0xe03;
}
static int fg_abi_switch(uint64_t root)
{
 /* Caller proves ring and both threads idle. Invalidate the common ASID
  * before permitting another doorbell; preserved ring/SQE PTEs are equal. */
 unsigned cb = g_cb(g3_bank);
 uint64_t ttbr = root | ((uint64_t)g3_asid<<48);
 __asm__ volatile("dsb sy" ::: "memory");
 if (!g3_sw(cb+0x20,(uint32_t)ttbr) || !g3_sw(cb+0x24,ttbr>>32)) return 0;
 uint32_t tlb[128] = {0};
 return !g4_tlb(tlb) && !fg_access_failed &&
     g3_sr(cb+0x20)==(uint32_t)ttbr && g3_sr(cb+0x24)==(uint32_t)(ttbr>>32);
}
/* One bounded, read-only snapshot before isolation destroys CP/SMMU evidence.
 * GPU offsets are A6XX/A7XX dwords from Mesa a6xx.xml; SMMU offsets are
 * bytes (same context-bank registers as g3_diagnostics). No indexed-port
 * walks, retry loops, status clears or fault-resume writes on this path. */
static unsigned fg_abi_hang_dumped;
static void fg_abi_hang_dump(const char *why)
{
 if (fg_abi_hang_dumped) return;
 fg_abi_hang_dumped=1;
 printf("FUXI GPU HANG: %s seq %u completed %u busy %u expected-wptr 0x%x\n",
     why,fg_abi_seq,fg_abi_completed,fg_client_busy,g3_wptr);
 static const struct { const char *name; unsigned reg; } regs[] = {
  {"CP_RB_RPTR",0x806},{"CP_RB_WPTR",0x807},
  {"CP_IB1_BASE_LO",0x928},{"CP_IB1_BASE_HI",0x929},
  {"CP_IB1_REM_SIZE",0x92a},{"CP_IB1_INIT_SIZE",0x943},
  {"CP_STATUS_1",0x825},{"CP_HW_FAULT",0x821},
  {"CP_INTERRUPT_STATUS",0x823},{"CP_PROTECT_STATUS",0x824},
  {"RBBM_STATUS",0x210},{"RBBM_STATUS3",0x213},{"RBBM_INT_0_STATUS",0x201},
 };
 for (unsigned i=0;i<sizeof(regs)/sizeof(regs[0]);i++)
  printf("FUXI GPU HANG: %s [0x%x]=0x%x\n",regs[i].name,regs[i].reg,g3_gr(regs[i].reg));
 unsigned cb=g_cb(g3_bank);
 printf("FUXI GPU HANG: SMMU GPU CB %u TTBR 0x%x:%08x FSR 0x%x FAR 0x%x:%08x FSYNR0 0x%x FSYNR1 0x%x GFSR 0x%x\n",
     g3_bank,g3_sr(cb+0x24),g3_sr(cb+0x20),g3_sr(cb+0x58),
     g3_sr(cb+0x64),g3_sr(cb+0x60),g3_sr(cb+0x68),g3_sr(cb+0x6c),g3_sr(0x48));
 printf("FUXI GPU HANG: MMIO-access-failed %u; isolating\n",fg_access_failed);
}
static int fg_abi_fault(const char *why)
{
 fg_abi_hang_dump(why);
 fg_abi_poisoned = 1;
 fg_fail(why,0x701);
 fg_abi_drained = fg_isolate();
 if (!fg_abi_drained && fg_scanout_owned) (void)fg_scanout_stop("GPU DMA drain failed");
 if (fg_abi_drained) fg_abi_import_cache(1);
 fg_client_busy = 0;
 return fg_abi_drained ? -1 : -2;
}
/* No spinning: one observation. The watchdog runs even if the client exits,
 * never waits, or asks WaitFence(timeout_ns=0) indefinitely. */
static int fg_abi_observe(void)
{
 if (fg_abi_poisoned) return fg_abi_timed_out ? (fg_abi_drained ? -3 : -4) : (fg_abi_drained ? -1 : -2);
 if (!fuxi_gpu_ready()) return -1;
 if (fg_access_failed || g3_faulted()) return fg_abi_fault("client GPU fault");
 if (!fg_client_busy) return 0;
 if (g3_read(G3_FENCE_PA+FG_ABI_FENCE_OFF+4)==fg_abi_seq &&
     g3_gr(0x806)==g3_wptr && g3_gr(0x807)==g3_wptr && !(g3_gr(0x210)&~1u)) {
  if (!fg_abi_switch(G3_PT)) return fg_abi_fault("client restore TTBR");
  fg_abi_import_cache(1); /* before fence success, even without a user wait */
  fg_abi_completed = fg_abi_seq;
  fg_client_busy = 0;
  return 0;
 }
 if (rsr("cntpct_el0") >= fg_client_deadline) {
  fg_abi_timed_out=1; (void)fg_abi_fault("client fence deadline (100 ms)");
  return fg_abi_drained ? -3 : -4;
 }
 return 1;
}
static void fg_abi_watch(thread_call_param_t arg, thread_call_param_t unused)
{
 (void)arg; (void)unused;
 FG_GUARD;
 if (fg_abi_observe()==1) {
  uint64_t deadline;
  clock_interval_to_deadline(2,NSEC_PER_MSEC,&deadline);
  (void)thread_call_enter_delayed(&fg_client_watchdog,deadline);
 }
}
static int fg_abi_init(void)
{
 unsigned bits=0;
 (void)PE_parse_boot_argn("fuxigpuncimport",&bits,sizeof(bits));
 fg_abi_ncimport=bits&1; fg_abi_cohtest=(bits>>6)&1; fg_abi_tlbpages=(bits>>7)&1;
 fg_abi_coh=fg_abi_coh_tries=0;
 for (unsigned i=0;i<sizeof(fg_abi_used)/sizeof(fg_abi_used[0]);i++) fg_abi_used[i]=0;
 /* 772 4K pages: root, two L2s, kernel runtime L3, 768 client L3s.
  * Allocate all table storage from unused reserved NC scratch, never VM.
  * No existing GMU or present PTE is reused as mutable client storage. */
 /* Snapshot once before checking: do not short-circuit away the evidence.
  * GPU offsets are dwords; SMMU_ID2 is a byte offset. The UCHE address
  * registers are adjacent 32-bit low/high pairs, including on A740.
  * Address low words have 4K granularity: A740 reads GMEM MAX as
  * 0x12ff000 after writing inclusive end 0x12fffff. Compare page addresses;
  * high words, NC mode and the extracted HBB field remain exact. */
 static const struct { const char *name; unsigned reg; uint32_t want, mask; } checks[] = {
  {"UCHE_WRITE_THRU_BASE_LO",0xe07,0xfffff000u,0xfffff000u},
  {"UCHE_WRITE_THRU_BASE_HI",0xe08,0x1ffff,UINT32_MAX},
  {"UCHE_TRAP_BASE_LO",0xe09,0xfffff000u,0xfffff000u},
  {"UCHE_TRAP_BASE_HI",0xe0a,0x1ffff,UINT32_MAX},
  {"UCHE_GMEM_RANGE_MIN_LO",0xe0b,0x1000000,0xfffff000u},
  {"UCHE_GMEM_RANGE_MIN_HI",0xe0c,0,UINT32_MAX},
  {"UCHE_GMEM_RANGE_MAX_LO",0xe0d,0x12fffff,0xfffff000u},
  {"UCHE_GMEM_RANGE_MAX_HI",0xe0e,0,UINT32_MAX},
  {"UCHE_MODE_CNTL_HBB",0xe01,3,UINT32_MAX},
  {"RBBM_NC_MODE_CNTL",0x534,1,UINT32_MAX},
 };
 uint32_t values[sizeof(checks)/sizeof(checks[0])];
 for (unsigned i=0;i<sizeof(checks)/sizeof(checks[0]);i++) values[i]=g3_gr(checks[i].reg);
 uint32_t id2=g3_sr(SMMU_ID2);
 static unsigned dumped;
 if (!dumped) {
  dumped=1;
  for (unsigned i=0;i<sizeof(checks)/sizeof(checks[0]);i++)
   printf("FUXI GPU ABI: init regs %s [0x%x]=0x%x\n",checks[i].name,checks[i].reg,values[i]);
  printf("FUXI GPU ABI: init regs SMMU_ID2 [0x%x]=0x%x\n",SMMU_ID2,id2);
 }
 if (fg_access_failed) {
  printf("FUXI GPU ABI: init check register_access got 0x%x want 0x0\n",fg_access_failed);
  return 0;
 }
 for (unsigned i=0;i<sizeof(checks)/sizeof(checks[0]);i++) {
  uint32_t got=checks[i].reg==0xe01 ? (values[i]>>21)&3 : values[i];
  if ((got&checks[i].mask)!=(checks[i].want&checks[i].mask)) {
   printf("FUXI GPU ABI: init check %s got 0x%x want 0x%x\n",checks[i].name,got,checks[i].want);
   return 0;
  }
 }
 if (((id2>>4)&15)<1) {
  printf("FUXI GPU ABI: init check SMMU_ID2_OAS_min got 0x%x want 0x1\n",(id2>>4)&15);
  return 0;
 }
 /* SM8550 UBWC4 configuration, same NC values as stage14 (also run by
  * stage16). GX OOB stays held; fg_state is not ready, so no present can
  * race these CPU MMIO writes. CP protection applies to packets, not AHB.
  * Host PIPE bits 13:12 select BR=1/BV=2; restore default aperture last.
  * g3_gw checks access faults, not value readback, so NC/aperture writes
  * impose no exact-read assumption. Tables below are NC scratch stores,
  * not register readback. TTBR switching checks an aligned PA plus ASID. */
 static const struct { const char *name; unsigned reg; uint32_t value; } writes[] = {
  {"G4_RB_NC",G4_RB_NC,0x816},
  {"G4_TPL1_NC",G4_TPL1_NC,6},
  {"G4_SP_NC",G4_SP_NC,0x26},
  {"G3_APERTURE_HOST_BR",G3_APERTURE_HOST,1u<<12},
  {"G4_GRAS_NC_BR",G4_GRAS_NC,0x60},
  {"G3_APERTURE_HOST_BV",G3_APERTURE_HOST,2u<<12},
  {"G4_GRAS_NC_BV",G4_GRAS_NC,0x60},
  {"G3_APERTURE_HOST_DEFAULT",G3_APERTURE_HOST,0},
 };
 for (unsigned i=0;i<sizeof(writes)/sizeof(writes[0]);i++) {
  if (!g3_gw(writes[i].reg,writes[i].value)) {
   printf("FUXI GPU ABI: init write %s [0x%x]=0x%x failed\n",writes[i].name,writes[i].reg,writes[i].value);
   return 0;
  }
 }
 g_zero(FG_ABI_PT,FG_ABI_PT_BYTES);
 volatile uint64_t *root = (volatile uint64_t *)G_PTR_RANGE(FG_ABI_PT,4096);
 volatile uint64_t *l20 = (volatile uint64_t *)G_PTR_RANGE(FG_ABI_PT+4096,4096);
 volatile uint64_t *l21 = (volatile uint64_t *)G_PTR_RANGE(FG_ABI_PT+8192,4096);
 volatile uint64_t *runtime = (volatile uint64_t *)G_PTR_RANGE(FG_ABI_PT+12288,4096);
 root[0]=(FG_ABI_PT+4096)|3; root[1]=(FG_ABI_PT+8192)|3;
 l20[G3_SQE_VA>>21]=(FG_ABI_PT+12288)|3;
 for (unsigned i=0;i<0x2a000/4096;i++) {
  uint64_t pa = G3_SQE_PA+(uint64_t)i*4096;
  unsigned ap = i<0x20 ? 3u<<6 : i==0x28 ? 0 : 1u<<6;
  runtime[i]=pa|3|(3u<<8)|(1u<<10)|ap;
 }
 for (unsigned i=0;i<768;i++) {
  uint64_t va=FG_ABI_VA_START+(uint64_t)i*0x200000;
  volatile uint64_t *l2=va>>30 ? l21 : l20;
  l2[(va>>21)&511]=(FG_ABI_PT+(4u+i)*4096)|3;
 }
 g_wr(G3_FENCE_PA,FG_ABI_FENCE_OFF+4,0);
 g_wr(G3_FENCE_PA,FG_ABI_FENCE_OFF+12,0);
 __asm__ volatile("dsb sy" ::: "memory");
 thread_call_setup_with_options(&fg_client_watchdog,fg_abi_watch,NULL,
     THREAD_CALL_PRIORITY_KERNEL,THREAD_CALL_OPTIONS_ONCE);
 fg_abi_live=1;
 return 1;
}
int fuxi_gpu_abi_available(void)
{
 FG_GUARD;
 return fg_abi_live && fuxi_gpu_ready() && !fg_abi_poisoned;
}
int fuxi_gpu_abi_timestamp(uint64_t *ticks)
{
 FG_GUARD;
 if (!ticks) return -2;
 *ticks=0;
 if (!fg_abi_live || !fuxi_gpu_ready() || fg_abi_poisoned) return -1;
 /* A6XX/A7XX CP_ALWAYS_ON_COUNTER, dword indices (not CPU timer ticks).
  * High/low/high avoids torn rollover; NativeAdrenoTimer uses eight tries. */
 for (unsigned attempt=0;attempt<8;attempt++) {
  uint32_t high=g3_gr(0x981), low=g3_gr(0x980), again=g3_gr(0x981);
  if (fg_access_failed) { (void)fg_abi_fault("client timestamp MMIO"); return -2; }
  if (high==again) { *ticks=((uint64_t)high<<32)|low; return 0; }
 }
 return -3;
}
int fuxi_gpu_abi_status(uint64_t *faults, uint64_t *suspends)
{
 FG_GUARD;
 if (!faults || !suspends) return -1;
 *faults=*suspends=0;
 if (!fg_abi_live) return -1;
 /* Observe even without WaitFence; after isolation never touch powered-down
  * MMIO. Returning success with the changed count is Turnip's device loss. */
 if (fuxi_gpu_ready()) (void)fg_abi_observe();
 *faults=fg_faults; *suspends=fg_suspends;
 return 0;
}
/* 16K XNU pages: one pmap walk per page (not per 4K leaf), kept for the write pass. */
static ppnum_t fg_abi_ppns[(FG_ABI_MAX_BYTES>>PAGE_SHIFT)+2];
/* Idle test for map/unmap/submit. A pending job is observed once instead of
 * waiting for the 2 ms watchdog, so a caller that polls sees retirement at
 * once. 0 idle, 1 still running, -1 failed/poisoned. */
static int fg_abi_wait_idle(void)
{
 if (!fg_client_busy) return 0;
 int s=fg_abi_observe();
 return s<0 ? -1 : s>0 ? 1 : 0;
}
static int fg_abi_map_locked(uint64_t iova, uintptr_t source, uint64_t bytes,
    unsigned permissions, unsigned wb, unsigned sh)
{
 if (!fg_abi_live || !fuxi_gpu_ready() || fg_abi_poisoned) return -1;
 int idle=fg_abi_wait_idle();
 if (idle) return idle;
 if (!fg_abi_map_valid(iova,bytes,permissions,wb,sh,0) || !source || (source&4095) || bytes>FG_ABI_MAX_BYTES) return -1;
 /* Validate the entire range before publishing any leaf. 16K XNU pages
  * provide four distinct 4K SMMU pages, including noncontiguous BOs. A 2 MiB
  * chunk's 512 leaves are contiguous, so the table pointer is built per chunk. */
 uintptr_t base=source>>PAGE_SHIFT;
 volatile uint64_t *leaf=NULL;
 for (uint64_t off=0;off<bytes;off+=4096) {
  if (!off || !((source+off)&PAGE_MASK)) {
   ppnum_t ppn=pmap_find_phys(kernel_pmap,source+off);
   if (!ppn || ((uint64_t)ppn<<PAGE_SHIFT)>=(1ULL<<FG_ABI_PA_BITS)) return -1;
   fg_abi_ppns[((source+off)>>PAGE_SHIFT)-base]=ppn;
  }
  if (fg_abi_used_get(fg_abi_pg(iova+off))) return -1;
 }
 for (uint64_t off=0;off<bytes;off+=4096) {
  if (!leaf || !((iova+off)&0x1fffff)) leaf=fg_abi_leaf(iova+off); else leaf++;
  uint64_t pa=((uint64_t)fg_abi_ppns[((source+off)>>PAGE_SHIFT)-base]<<PAGE_SHIFT)|((source+off)&PAGE_MASK);
  *leaf=pa|3|(permissions==3 ? 1u<<6 : 3u<<6)|(wb ? 1u<<2 : 0)|sh<<8|1u<<10;
  fg_abi_used_put(fg_abi_pg(iova+off),1);
 }
 __asm__ volatile("dsb sy" ::: "memory");
 /* Root is inactive; TLBI is mandatory before its next doorbell. */
 return 0;
}
int fuxi_gpu_abi_map(uint64_t iova, uintptr_t source, uint64_t bytes,
    unsigned permissions, unsigned wb, unsigned sh)
{
 FG_GUARD;
 return fg_abi_map_locked(iova,source,bytes,permissions,wb,sh);
}
static void fg_abi_coh_probe(void);
int fuxi_gpu_abi_map_import(uint64_t iova, uintptr_t source, uint64_t bytes, unsigned permissions)
{
 FG_GUARD;
 if (!source || !bytes || ((source|bytes)&(NativeAdrenoAbiHostPageBytes-1)) ||
     bytes>FG_ABI_MAX_BYTES || source+bytes<source) return -1;
 if (fg_abi_cohtest && !fg_abi_coh) fg_abi_coh_probe(); /* first import = first use; runs before this range is mapped */
 unsigned slot;
 for (slot=0;slot<FG_ABI_MAX_IMPORTS && fg_abi_imports[slot].iova;slot++) {}
 if (slot==FG_ABI_MAX_IMPORTS) return -1;
 int s=fg_abi_map_locked(iova,source,bytes,permissions,0,2);
 if (!s) {
  if (fg_abi_ncimport && fg_abi_coh!=1 && (permissions&2)) {
   /* Before the first GPU use: publish preexisting CPU pixels/dirty lines.
    * The GPU lock prevents any submission until this handoff has finished. */
   __asm__ volatile("dsb sy" ::: "memory");
   fg_abi_cache_region(source,bytes,0);
   __asm__ volatile("dsb sy" ::: "memory");
  }
  fg_abi_imports[slot].permissions=permissions;
  fg_abi_imports[slot].source=source; fg_abi_imports[slot].bytes=bytes; fg_abi_imports[slot].iova=iova;
 }
 return s;
}
static void fg_abi_import_remove(uint64_t iova, uint64_t bytes)
{
 for (unsigned i=0;i<FG_ABI_MAX_IMPORTS;i++)
  if (fg_abi_imports[i].iova==iova && fg_abi_imports[i].bytes==bytes) fg_abi_imports[i].iova=0;
}
static int fg_abi_unmap_locked(uint64_t iova, uint64_t bytes)
{
 if (fg_abi_live && !fg_abi_poisoned && fg_abi_range(iova,bytes)) (void)fg_abi_wait_idle();
 if (fg_abi_poisoned) {
  if (!fg_abi_drained) return -2;
  fg_abi_import_remove(iova,bytes); return 0;
 }
 if (!fg_abi_live || !fg_abi_range(iova,bytes)) return -1;
 if (fg_client_busy) return 1;
 volatile uint64_t *leaf=NULL;
 for (uint64_t off=0;off<bytes;off+=4096) {
  if (!leaf || !((iova+off)&0x1fffff)) leaf=fg_abi_leaf(iova+off); else leaf++;
  *leaf=0;
  fg_abi_used_put(fg_abi_pg(iova+off),0);
 }
 __asm__ volatile("dsb sy" ::: "memory");
 /* Remove stale ASID translations before backing pages can be released; never skipped. Opt-in value 128 drops a small
  * unmap page by page instead of the whole ASID. Safe with an in-flight present: only the inactive root was changed. */
 uint32_t tlb[128]={0};
 int terr=fg_abi_tlbpages && bytes/4096<=FG_ABI_TLB_PAGE_LIMIT ? fg_abi_tlb_pages(iova,bytes) : g4_tlb(tlb);
 if (terr || fg_access_failed) return fg_abi_fault("client unmap TLB sync");
 fg_abi_import_remove(iova,bytes);
 return 0;
}
static int fg_abi_submit_locked(const uint64_t *iovas, const uint64_t *sizes,
    unsigned count, uint64_t *fence)
{
 if (!fg_abi_live || !fuxi_gpu_ready() || fg_abi_poisoned || fg_abi_seq==UINT32_MAX) return -1;
 if (fg_busy || fg_scanout_reserved) return 1;
 int idle=fg_abi_wait_idle();
 if (idle) return idle<0 ? -1 : 1;
 if (g3_faulted() || g3_gr(0x806)!=g3_wptr || g3_gr(0x807)!=g3_wptr || (g3_gr(0x210)&~1u))
  return fg_abi_fault("client pre-submit idle/fault");
 uint32_t words[160];
 unsigned n=fg_abi_frame(words,iovas,sizes,count,G3_FENCE_VA+FG_ABI_FENCE_OFF,fg_abi_seq,fg_abi_seq+1,1);
 if (!n) return -1;
 if (!fg_abi_switch(FG_ABI_PT)) return fg_abi_fault("client install TTBR");
 /* Ordinary BOs are NC on both sides. Imports preserve CPU attributes and
  * need an explicit PoC handoff; IO-coherent XNU helpers may be no-ops here.
  * No CPU access is allowed until retirement, per external synchronization.
  * PM4 prologue/epilogue handle GPU cache invalidate/writeback. */
 fg_abi_import_cache(0);
 __asm__ volatile("dsb sy" ::: "memory");
 for (unsigned i=0;i<n;i++) g3_emit(words[i]);
 g3_wptr &= 8191;
 __asm__ volatile("dsb sy" ::: "memory");
 fg_client_deadline=rsr("cntpct_el0")+rsr("cntfrq_el0")/10;
 fg_client_busy=1;
 fg_dcvs_kick();
 if (!g3_gw(0x807,g3_wptr) || fg_access_failed) return fg_abi_fault("client doorbell");
 *fence=++fg_abi_seq;
 uint64_t deadline;
 clock_interval_to_deadline(2,NSEC_PER_MSEC,&deadline);
 (void)thread_call_enter_delayed(&fg_client_watchdog,deadline);
 return 0;
}
int fuxi_gpu_abi_unmap(uint64_t iova, uint64_t bytes)
{
 FG_GUARD;
 return fg_abi_unmap_locked(iova,bytes);
}
int fuxi_gpu_abi_submit(const uint64_t *iovas, const uint64_t *sizes,
    unsigned count, uint64_t *fence)
{
 FG_GUARD;
 return fg_abi_submit_locked(iovas,sizes,count,fence);
}
/* Opt-in (fuxigpuncimport value 64) fail-closed CPU/GPU coherency self-test, run once under the GPU lock at the first import map.
 * It maps a cacheable kernel page with the exact import leaf attributes (Normal NC, outer-shareable) and runs one tiny IB:
 * up: the CPU leaves 8 lines dirty in its cache and the GPU must copy those values (no clean); down: the CPU caches 8 old
 * lines and the GPU must overwrite them and the CPU must read the new values (no invalidate). All 16 must match, otherwise
 * (stale, timeout, fault, anything unexpected) the normal per-submit clean and retire invalidate stay. Not yet observed on device. */
#define FG_ABI_COH_IOVA (FG_ABI_VA_START+FG_ABI_VA_SIZE-0x10000)
#define FG_ABI_COH_IB (FG_ABI_PT+0x3f0000)
#define FG_ABI_COH_RES (FG_ABI_PT+0x3f1000)
_Static_assert(FG_ABI_COH_IB >= FG_ABI_PT+(4u+768u)*4096 && FG_ABI_COH_RES+4096 <= FG_ABI_PT+FG_ABI_PT_BYTES, "coherency test pages overlap tables");
static uint8_t fg_abi_coh_page[16384] __attribute__((aligned(16384)));
static void fg_abi_coh_probe(void)
{
 if (!fg_abi_live || !fuxi_gpu_ready() || fg_abi_poisoned) return;
 const uint64_t ib=FG_ABI_COH_IOVA, res=ib+0x1000, data=ib+0x4000;
 const char *why="inconclusive";
 unsigned up=0, down=0, mapped=0, definitive=0;
 volatile uint32_t *page=(volatile uint32_t *)fg_abi_coh_page;
 volatile uint32_t *rp=(volatile uint32_t *)G_PTR_RANGE(FG_ABI_COH_RES,4096);
 uint32_t *ibw=(uint32_t *)G_PTR_RANGE(FG_ABI_COH_IB,4096);
 uint64_t fence=0, ibytes=0, cleaned=fg_abi_cleaned;
 unsigned n=0;
 if (fg_abi_map_locked(ib,(uintptr_t)ibw,4096,3,0,2)) { why="map ib"; goto out; }
 mapped|=1;
 if (fg_abi_map_locked(res,(uintptr_t)rp,4096,3,0,2)) { why="map result"; goto out; }
 mapped|=2;
 if (fg_abi_map_locked(data,(uintptr_t)fg_abi_coh_page,sizeof(fg_abi_coh_page),3,0,2)) { why="map data"; goto out; }
 mapped|=4;
 for (unsigned k=0;k<16;k++) rp[k]=0;
 for (unsigned k=0;k<8;k++) { page[k*16]=0xC0DE0000u|k; page[(8+k)*16]=0x51A1E000u|k; (void)page[(8+k)*16]; }
 for (unsigned k=0;k<8;k++) {
  ibw[n++]=fg_abi_pkt7(0x73,5); ibw[n++]=0; ibw[n++]=(uint32_t)(res+4*k); ibw[n++]=0; ibw[n++]=(uint32_t)(data+64*k); ibw[n++]=0;
 }
 for (unsigned k=0;k<8;k++) {
  ibw[n++]=fg_abi_pkt7(0x3d,3); ibw[n++]=(uint32_t)(data+(8+k)*64); ibw[n++]=0; ibw[n++]=0x6E57A000u|k;
 }
 ibw[n++]=fg_abi_pkt7(0x12,0); ibw[n++]=fg_abi_pkt7(0x26,0);
 ibytes=4u*n;
 __asm__ volatile("dsb sy" ::: "memory");
 { int s=fg_abi_submit_locked(&ib,&ibytes,1,&fence); if (s) { why=s>0 ? "busy" : "submit"; goto out; } }
 { int o; while ((o=fg_abi_observe())==1) delay(20);
   if (o) { why="fault"; definitive=1; goto out; } }
 for (unsigned k=0;k<8;k++) { up+=rp[k]==(0xC0DE0000u|k); down+=page[(8+k)*16]==(0x6E57A000u|k); }
 definitive=1;
 why=up==8 && down==8 ? "pass" : "stale";
out:
 if (mapped&4) fg_abi_cache_region((uintptr_t)fg_abi_coh_page,sizeof(fg_abi_coh_page),0);
 fg_abi_cleaned=cleaned;
 if (mapped&4) (void)fg_abi_unmap_locked(data,sizeof(fg_abi_coh_page));
 if (mapped&2) (void)fg_abi_unmap_locked(res,4096);
 if (mapped&1) (void)fg_abi_unmap_locked(ib,4096);
 if (up==8 && down==8 && definitive && !fg_abi_poisoned) fg_abi_coh=1;
 else if (definitive || ++fg_abi_coh_tries>=3) fg_abi_coh=2;
 printf("FUXI GPU ABI: coherent-import self-test %s cpu_to_gpu %u/8 gpu_to_cpu %u/8 -> %s\n",why,up,down,
     fg_abi_coh==1 ? "skip import clean/invalidate" : fg_abi_coh==2 ? "keep import cache maintenance (final)" : "keep import cache maintenance (retry)");
}
int fuxi_gpu_abi_poll(uint64_t fence)
{
 FG_GUARD;
 if (!fg_abi_live || fence>fg_abi_seq) return -1;
 int err=fg_abi_observe();
 if (err<0) return err;
 return fence<=fg_abi_completed ? 0 : 1;
}
