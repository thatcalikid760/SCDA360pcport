# SCDA Phase 2a: GPU heartbeat Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Get the game's own D3D runtime past its wait-for-GPU loop by emulating what the Xenos GPU reports back (ring read pointer, fences, interrupts), without drawing anything yet.

**Architecture:** The same first milestone TTT2 reached (`external/TTT2/port/src/runtime/kernel/kernel_video.cpp` header comment; reference only, no code copied). Vd* imports record where the game put the ring, the read-pointer writeback and the system command buffer. A command-processor thread reads the write pointer the game stores in the GPU register block, walks PM4 packets up to it, performs the packets that write memory (fences, MEM_WRITE, register writes, indirect buffers), and publishes the read pointer. An interrupt thread calls the game's graphics interrupt callback for vblank and swap. Phase 2b (window, draws, shaders, resolution scaling) is planned from what this phase's watchdog shows next.

**Tech Stack:** C++20, clang-cl, existing runtime (`runtime/`), C++ test harness (`runtime/tests/check.h`), `tools/test_runtime.bat`.

**Spec:** `docs/superpowers/specs/2026-10-27-scda-playable-port-design.md`

## Global Constraints

- GPL-3.0. TTT2 is reference only; UnleashedRecomp/Xenia may be consulted under their licenses.
- Never commit game data (`.gitignore`, `tools/tests/test_repo_hygiene.py`).
- Guest physical memory: physical address `p` is guest virtual `0xA0000000 + p` (the runtime's physical allocator range `0xA0000000-0xBFFFFFFF`, `MmGetPhysicalAddress` = `addr & 0x1FFFFFFF`).
- GPU register block: guest `0x7FC80000 + index*4`, big-endian dwords. `CP_RB_WPTR` = index `0x01C5` (guest `0x7FC80714`).
- Values observed in SCDA's boot (`runtime/build/scda.log`, 2026-10-27): `VdInitializeRingBuffer(0x12C0, 0x10)`, `VdEnableRingBufferRPtrWriteBack(0x68133C, 0x0A)`, `VdSetSystemCommandBufferGpuIdentifierAddress(0xA0681308)`, `VdSetGraphicsInterruptCallback(0x821F3620, 0x40121800)`, `XGetVideoMode(0x4010D520)`, `VdQueryVideoMode(0x4010D030)`.
- Ring size in bytes = `1 << (sizeLog2 + 3)` (Xenia's `CommandProcessor::InitializeRingBuffer`): 0x10 -> 512 KiB.
- PM4 packet header: type = `h >> 30`. Type 0: base register `h & 0x7FFF`, count `((h >> 16) & 0x3FFF) + 1` dwords, write-one-register flag bit 15. Type 1: two registers, 2 payload dwords. Type 2: no payload. Type 3: opcode `(h >> 8) & 0x7F`, count `((h >> 16) & 0x3FFF) + 1` payload dwords. Header `0` = 1-dword padding.
- Type 3 opcodes handled here: `ME_INIT 0x48`, `NOP 0x10`, `INTERRUPT 0x54`, `INDIRECT_BUFFER 0x3F` (and `0x37` PFP variant), `WAIT_REG_MEM 0x3C`, `REG_RMW 0x21`, `COND_WRITE 0x45`, `EVENT_WRITE 0x46`, `EVENT_WRITE_SHD 0x58` (payload: initiator, address, value), `EVENT_WRITE_EXT 0x59`, `MEM_WRITE 0x3D` (address, then values), `WAIT_FOR_IDLE 0x26`. Every other opcode is skipped by size and counted. Verify each number against Xenia `src/xenia/gpu/xenos.h` (BSD) before relying on it; record any mismatch as a ruling.
- Vblank rate 60 Hz. Interrupt callback signature: `callback(source, context)`; source `0` = vblank, `1` = swap/command interrupt (Xenia `GraphicsSystem::DispatchInterruptCallback`). Confirm against TTT2's notes (their source numbering differs: "source 0 swap-pending, source 1 vblank"); the test in Task 4 pins whichever the binary's callback at `0x821F3620` expects, decided by reading its first branch on r3 in `generated/`.

## Review Focus

- **Ring wrap mid-packet:** a packet whose dwords wrap past the end of the ring must be executed once, whole, and only when all its dwords are published. Test in Task 2.
- **Write pointer published before payload is written:** WPtr may be read while the game is still filling; never execute past WPtr. Test in Task 2.
- **Indirect buffer recursion and bad addresses:** an IB pointing outside guest memory or nesting more than 4 deep is skipped and logged, not a crash. Test in Task 3.
- **Guest callback re-entrancy:** the interrupt callback runs on its own host thread with its own PPCContext and stack; it must not share the main thread's context. Test in Task 4.
- **Shutdown/idle:** with no ring initialized (early boot), the command-processor thread must idle without touching memory. Test in Task 3.

---

### Task 1: Vd* state capture and video mode

**Files:**
- Create: `runtime/src/gpu_state.h`, `runtime/src/video.cpp`, `runtime/tests/test_video.cpp`
- Modify: `runtime/CMakeLists.txt` (add `src/video.cpp` to `scda` and `scda_tests`), regenerate `runtime/src/imports_stubs.cpp` via `python -I tools/gen_import_stubs.py`

**Interfaces:**
- Produces (`gpu_state.h`):
  ```cpp
  struct GpuState {
      std::atomic<uint32_t> ringBase;      // guest virtual (0xA0000000 + physical), 0 = not initialized
      std::atomic<uint32_t> ringSize;      // bytes
      std::atomic<uint32_t> rptrWriteback; // guest virtual, 0 = none
      std::atomic<uint32_t> sysCmdBuf;     // guest virtual of VdSetSystemCommandBufferGpuIdentifierAddress arg
      std::atomic<uint32_t> interruptCallback, interruptContext;
  };
  GpuState& Gpu();
  constexpr uint32_t GuestPhysical(uint32_t p) { return 0xA0000000u + (p & 0x1FFFFFFFu); }
  constexpr uint32_t kGpuRegBase = 0x7FC80000u, kRegCpRbWptr = 0x01C5u;
  ```
- Produces imports (`video.cpp`): `VdInitializeRingBuffer`, `VdEnableRingBufferRPtrWriteBack`, `VdSetSystemCommandBufferGpuIdentifierAddress`, `VdSetGraphicsInterruptCallback`, `XGetVideoMode`, `VdQueryVideoMode`, `VdQueryVideoFlags`, `VdGetCurrentDisplayInformation`, `VdGetCurrentDisplayGamma`, `VdIsHSIOTrainingSucceeded`, `VdPersistDisplay`, `VdInitializeEngines`, `VdShutdownEngines`, `VdRetrainEDRAM`, `VdRetrainEDRAMWorker`, `VdEnableDisableClockGating`, `VdSetDisplayMode`, `VdInitializeScalerCommandBuffer`, `VdCallGraphicsNotificationRoutines`, `VdGetSystemCommandBuffer`. Video mode struct (`X_VIDEO_MODE`, 0x30 bytes): width 1280, height 720, isInterlaced 0, isWidescreen 1, isHiDef 1, refreshRate 60.0f, videoStandard 1, pixelRate 0x4A, widescreenFlag 0x01, reserved 0. Field offsets from Xenia `xboxkrnl_video.cc` `X_VIDEO_MODE`; the test pins them.
  - `VdIsHSIOTrainingSucceeded` returns 1; `VdPersistDisplay(a, u32* out)` stores 0, returns 0; `VdInitializeEngines` returns 1; `VdRetrainEDRAM` returns 0; `VdGetSystemCommandBuffer(u32* p, u32* v)` stores a fixed 0x94-byte guest physical block and `0xBEEF0000`; `VdQueryVideoFlags` returns 7 (widescreen | 1080-capable | 16:9).
- `VdSetSystemCommandBufferGpuIdentifierAddress` is called twice in boot (once with r3 = 0); only a nonzero argument updates `sysCmdBuf`.

- [ ] **Step 1: Write the failing test** `runtime/tests/test_video.cpp`

```cpp
TEST(ring_init_records_physical_base_and_size) {
    PPCContext ctx{}; ctx.r3.u64 = 0x12C0; ctx.r4.u64 = 0x10;
    __imp__VdInitializeRingBuffer(ctx, g_base);
    CHECK_EQ(Gpu().ringBase.load(), 0xA00012C0u);
    CHECK_EQ(Gpu().ringSize.load(), 0x80000u);
}
TEST(rptr_writeback_and_syscmdbuf_recorded) {
    PPCContext ctx{}; ctx.r3.u64 = 0x68133C; ctx.r4.u64 = 0xA;
    __imp__VdEnableRingBufferRPtrWriteBack(ctx, g_base);
    CHECK_EQ(Gpu().rptrWriteback.load(), 0xA068133Cu);
    ctx.r3.u64 = 0; __imp__VdSetSystemCommandBufferGpuIdentifierAddress(ctx, g_base);
    ctx.r3.u64 = 0xA0681308; __imp__VdSetSystemCommandBufferGpuIdentifierAddress(ctx, g_base);
    CHECK_EQ(Gpu().sysCmdBuf.load(), 0xA0681308u);
}
TEST(interrupt_callback_recorded) {
    PPCContext ctx{}; ctx.r3.u64 = 0x821F3620; ctx.r4.u64 = 0x40121800;
    __imp__VdSetGraphicsInterruptCallback(ctx, g_base);
    CHECK_EQ(Gpu().interruptCallback.load(), 0x821F3620u);
    CHECK_EQ(Gpu().interruptContext.load(), 0x40121800u);
}
TEST(video_mode_is_720p_widescreen) {
    PPCContext ctx{}; ctx.r3.u64 = 0x1000;
    __imp__XGetVideoMode(ctx, g_base);
    CHECK_EQ(GuestLoad32(0x1000 + 0x00), 1280u);   // DisplayWidth
    CHECK_EQ(GuestLoad32(0x1000 + 0x04), 720u);    // DisplayHeight
    CHECK_EQ(GuestLoad32(0x1000 + 0x0C), 1u);      // IsWidescreen
    CHECK_EQ(GuestLoad32(0x1000 + 0x10), 1u);      // IsHiDef
}
```
The test file sets `g_base` to a 4 GB reserve with the touched pages committed (helper `TestGuestBase()` in `runtime/tests/guest_memory.h`, new, reused by Tasks 2-4: reserve `0x100000000`, commit on demand via a vectored exception handler that commits the faulting page).

- [ ] **Step 2: Run to verify it fails** — `cmd //c tools\\test_runtime.bat`. Expected: compile error, `Gpu` undefined.
- [ ] **Step 3: Implement** `gpu_state.h`, `video.cpp`, `tests/guest_memory.h`; regenerate stubs.
- [ ] **Step 4: Run tests** — `cmd //c tools\\test_runtime.bat` then `runtime/build/scda_tests.exe | tail -1`. Expected: all pass.
- [ ] **Step 5: Commit** — `git commit -m "feat: Vd* video state capture and 720p video mode"`.

---

### Task 2: PM4 ring walker

**Files:**
- Create: `runtime/src/pm4.h`, `runtime/src/pm4.cpp`, `runtime/tests/test_pm4.cpp`

**Interfaces:**
- Produces:
  ```cpp
  uint32_t Pm4PacketDwords(uint32_t header);   // total dwords including header (see Global Constraints)
  struct Pm4Ring { uint32_t base, sizeBytes; }; // guest virtual
  // Walks whole packets from readDword to writeDword (dword indices into the ring).
  // Calls exec(header, payloadDword(i)) once per packet; returns the new read index.
  // A packet not fully below writeDword is left for the next call.
  uint32_t Pm4WalkRing(const Pm4Ring& ring, uint32_t readDword, uint32_t writeDword,
                       const std::function<void(uint32_t header, const std::function<uint32_t(uint32_t)>& payload)>& exec);
  ```

- [ ] **Step 1: Write the failing tests** `runtime/tests/test_pm4.cpp`

```cpp
TEST(pm4_sizes) {
    CHECK_EQ(Pm4PacketDwords(0x00000000u), 1u);          // padding
    CHECK_EQ(Pm4PacketDwords(0x80000000u), 1u);          // type 2
    CHECK_EQ(Pm4PacketDwords(0x40000000u), 3u);          // type 1
    CHECK_EQ(Pm4PacketDwords(0xC0025800u), 4u);          // EVENT_WRITE_SHD, 3 payload
    CHECK_EQ(Pm4PacketDwords(0x00010000u | 0x2000), 3u); // type 0, 2 registers
}
TEST(pm4_walk_executes_each_packet_once) { /* ring of 64 dwords: NOP(1 payload), EVENT_WRITE_SHD; walk 0..6 -> 2 exec calls, returns 6; walk 6..6 -> 0 calls */ }
TEST(pm4_wrap_mid_packet_waits_for_whole_packet) { // Review Focus
    // EVENT_WRITE_SHD starting at dword 62 of a 64-dword ring: header 62, payload 63, 0, 1.
    // write index 0 -> 0 exec calls, read stays 62; write index 2 -> 1 call with payload(1)==addr wrapped from dword 0.
}
TEST(pm4_never_reads_past_write) { // Review Focus
    // packet needs 4 dwords, write index = read + 3 -> no call, read unchanged.
}
```
Write the bodies as the comments describe, with concrete dword values (`EVENT_WRITE_SHD` payload `{4, 0xA0681308, 0x12345678}`).

- [ ] **Step 2: Run to verify it fails** — expected compile error.
- [ ] **Step 3: Implement `pm4.cpp`**.
- [ ] **Step 4: Run tests** — all pass.
- [ ] **Step 5: Commit** — `git commit -m "feat: PM4 ring walker with wrap and write-pointer guards"`.

---

### Task 3: Command processor heartbeat

**Files:**
- Create: `runtime/src/command_processor.h`, `runtime/src/command_processor.cpp`, `runtime/tests/test_command_processor.cpp`
- Modify: `runtime/src/main.cpp` (start `CommandProcessorStart()` before `_xstart`)

**Interfaces:**
- Consumes: `Gpu()` (Task 1), `Pm4WalkRing` (Task 2).
- Produces:
  ```cpp
  struct CpStats { uint64_t packets, fences, memWrites, regWrites, indirect, skipped; };
  // One pass: read CP_RB_WPTR, walk from the current read index, execute, publish RPtr
  // (dword index, big-endian) to Gpu().rptrWriteback. Returns false if the ring is not initialized.
  bool CommandProcessorTick(CpStats* stats);
  void CommandProcessorStart();   // host thread: Tick, then Sleep(0)/yield; 1 ms sleep when idle
  uint32_t GpuRegister(uint32_t index);           // reads guest 0x7FC80000 + index*4
  ```
  Execution: type 0/1 store payload into the register block (`base` increments unless the one-register flag is set). `EVENT_WRITE_SHD`: store `value` at `address` (physical -> `GuestPhysical`, big-endian). `MEM_WRITE`: store each value from `address` onward. `INDIRECT_BUFFER` (`address`, `sizeDwords`): walk that buffer linearly with the same executor, depth limit 4, skip if the range is outside `0x00000000-0xFFFFFFFF` guest memory that is committed (check with `VirtualQuery`). `WAIT_REG_MEM`: evaluate once; if not satisfied, stop this tick before the packet (retry next tick). `INTERRUPT`: queue a source-1 interrupt (Task 4). All other opcodes: count in `skipped`.
  The CP writes only the RPtr itself; the system command buffer's completed-segment counter is written by the game's own `EVENT_WRITE_SHD` packets (TTT2 note: "the game polls the GPU completed segment counter at sysCmdBuf+0"). In Task 5, log the address `sub_821FA428` polls (`[r29+10768]`) once, and record whether it equals `Gpu().rptrWriteback` or a `sysCmdBuf`-relative address.

- [ ] **Step 1: Write the failing tests** `runtime/tests/test_command_processor.cpp`
  - `cp_idle_without_ring`: `Gpu().ringBase = 0` -> `CommandProcessorTick` returns false, no memory touched. (Review Focus)
  - `cp_fence_written_and_rptr_published`: ring at `0xA00012C0` (64 KiB in test), packets `EVENT_WRITE_SHD {4, 0x00681308, 0xCAFE0001}`; set WPtr register = 4 -> after tick `GuestLoad32(0xA0681308) == 0xCAFE0001` and `GuestLoad32(rptrWriteback) == 4`, `stats.fences == 1`.
  - `cp_mem_write_and_type0_registers`: MEM_WRITE of two values; type-0 write of 2 registers at index 0x2000 -> `GpuRegister(0x2000/0x2001)` hold them.
  - `cp_indirect_buffer_executes_and_bad_ib_skipped`: IB with a fence executes; IB at an uncommitted address increments `skipped`, no crash; IB nesting 5 deep stops at 4. (Review Focus)
  - `cp_wait_reg_mem_blocks_until_satisfied`.
- [ ] **Step 2: Run to verify it fails.**
- [ ] **Step 3: Implement**; start the thread in `main.cpp`.
- [ ] **Step 4: Run tests** — all pass.
- [ ] **Step 5: Commit** — `git commit -m "feat: PM4 command processor heartbeat (fences, mem writes, IBs, rptr)"`.

---

### Task 4: Interrupt thread (vblank + swap)

**Files:**
- Create: `runtime/src/interrupts.h`, `runtime/src/interrupts.cpp`, `runtime/tests/test_interrupts.cpp`
- Modify: `runtime/src/main.cpp` (start `InterruptsStart()`), `runtime/src/command_processor.cpp` (INTERRUPT -> `InterruptsQueue(1)`), `runtime/src/video.cpp` (`VdSwap` -> queue swap interrupt, return 0)

**Interfaces:**
- Consumes: `Gpu().interruptCallback/interruptContext` (Task 1), `InitThreadContext` (main.cpp), `WatchdogRegisterThread`.
- Produces:
  ```cpp
  void InterruptsQueue(uint32_t source);
  // Delivers queued interrupts, then a vblank (source kVblankSource) if 1/60 s elapsed.
  // Calls callback(r3 = source, r4 = context) on the interrupt thread's own PPCContext.
  void InterruptsDeliverForTest(PPCContext& ctx, uint32_t source);
  void InterruptsStart();
  constexpr uint32_t kVblankSource = /* decided in Step 1 from sub_821F3620 */;
  ```

- [ ] **Step 1: Pin the source numbering.** Read `sub_821F3620` in `generated/` (find with `grep -l "PPC_FUNC_IMPL(__imp__sub_821F3620)" generated/*.cpp`): note which r3 value takes the vblank path (look for `cmplwi cr6,r3,0` / `cmpwi` and what each branch calls). Record in the ledger.
- [ ] **Step 2: Write the failing tests** `runtime/tests/test_interrupts.cpp`
  - `interrupt_calls_callback_with_source_and_context`: register a host test function at a fake guest address (insert into the function table via `PPC_LOOKUP_FUNC`) that records `ctx.r3`, `ctx.r4`; `InterruptsDeliverForTest(ctx, 1)` -> recorded `(1, context)`.
  - `interrupt_uses_own_context`: the recorded `&ctx` differs from the main thread's context. (Review Focus)
  - `no_callback_no_call`: callback 0 -> nothing called.
- [ ] **Step 3: Run to verify it fails.**
- [ ] **Step 4: Implement**; the thread uses a 16 MiB host stack and registers with the watchdog as guest thread id 0x100.
- [ ] **Step 5: Run tests** — all pass.
- [ ] **Step 6: Commit** — `git commit -m "feat: vblank/swap interrupt thread calling the game's graphics callback"`.

---

### Task 5: Boot past the GPU wait

**Files:**
- Modify: `STATE.md`

- [ ] **Step 1: Boot with the watchdog**

Run: `cd _install && SCDA_WATCHDOG=20 timeout 70 ../runtime/build/scda.exe "<abs path>\game\SC4_Offline.img"; grep -E '\[watchdog\]|\[fs\] open|\[thread\]|\[fatal\]' ../runtime/build/scda.log | tail -40`
Expected: thread 3 is no longer in `sub_821FA428`; new files open beyond `CHECKPOINT.RAW` (game data packages, maps, sounds) or a new, different stall.

- [ ] **Step 2: Record** the new stall (watchdog block, last 10 files opened, top stub calls by count) in `STATE.md`, plus which address `sub_821FA428` actually polled (Task 3 note).

- [ ] **Step 3: Full test run and commit**

Run: `python -I -m unittest discover -s tools/tests -t .` and `cmd //c tools\\test_runtime.bat`. Expected: all OK.
`git commit -m "docs: phase 2a GPU heartbeat result"`.
