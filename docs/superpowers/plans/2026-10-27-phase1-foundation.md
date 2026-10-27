# SCDA Phase 1: Foundation (TU3 recompile, patched image, diagnostics) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Recompile the single-player executable from the TU3-patched image, run it from a flat patched image, and find out exactly where the game hangs, inside a GPL-3.0 git repo with tests.

**Architecture:** A C++ STFS reader pulls `SC4_Offline.xexp` out of the user's TU file; `xex_dump` applies it with XenonUtils' `XexPatcher` and writes the flat image `SC4_Offline.img`. XenonRecomp regenerates `generated/` from the patched XEX. The runtime loads the `.img` directly, verifies its hash, and gains a watchdog that walks every guest thread's stack when the game stops making progress. This is phase 1 of several; the GPU, audio, input, XAM and installer phases each get their own plan, written from what this phase's diagnostics show.

**Tech Stack:** C++20, clang-cl 23 (LLVM), VS 2022 Build Tools CMake 3.31 + Ninja, XenonRecomp/XenonUtils @ `ddd128b` + `tools/patches/xenonrecomp_scda.patch`, Python 3.15 stdlib (`unittest`), git.

**Spec:** `docs/superpowers/specs/2026-10-27-scda-playable-port-design.md`

## Global Constraints

- Project license: GPL-3.0. UnleashedRecomp code may be adapted with attribution; TTT2 (`external/TTT2`) code is reference only, never copied.
- Never commit game data: `*.iso`, `*.xex`, `*.xexp`, `*.img`, `*.bin` images, the TU file `TU_1AL61UN_000000C000000.00000000000O3`, `assets/`, `_install/`, `_run/`, `generated/` (recompiled game code).
- Never commit or print `.env`.
- Title `555307D7`, media `3FA11EB0`. Base `SC4_Offline.xex` SHA-256 `86d32713b57feb6631f0810a9416046beb3a2157084498e61f5b59b669e82a93`, version 0.0.0.3. TU3 `SC4_Offline.xexp` targets 0.0.3.3 from 0.0.0.3.
- Recompiler acceptance: 0 `Unrecognized instruction`, 0 `Switch case ... outside function`, 0 `// ERROR` in `generated/`.
- XenonRecomp joins TOML paths by string concatenation with the TOML's directory: all paths in `config/*.toml` stay relative.
- Windows 10/11 x64 only. Build scripts: `tools/build_xenonrecomp.bat`, `tools/build_runtime.bat`, `tools/xex_dump/build.bat`.
- Save/settings/log root for later phases: `%LOCALAPPDATA%\SCDARecomp`.

## Review Focus

- **STFS chains that start in the first hash-table group (blocks 0..0xA9):** the hash entry lives before the first data block; a reader using unsigned offsets loses it (TTT2 hit this). Expected: every file in the TU reads back at its listed size. Test added to Task 2.
- **Wrong title update** (different title, or an xexp that does not target 0.0.0.3 -> 0.0.3.3): expected a clear error naming the version, no output written. Test added to Task 3.
- **Paths with spaces and parentheses** (default install dir is under `C:\Program Files`): expected every tool and the runtime to work. Test added to Task 3 and Task 5.
- **Stale or foreign image** (`.img` from the unpatched XEX or another game next to a TU3 build): expected the runtime to refuse with a message naming the expected SHA-256, not crash later. Test added to Task 5.
- **Thread hung in a host wait with no import call in progress:** expected the watchdog to still report that thread's guest stack. Test added to Task 6.

---

### Task 1: Repository, license and test harness

**Files:**
- Create: `.gitignore` (replace current 3-line file), `LICENSE`, `THIRD_PARTY_NOTICES.md`, `tools/tests/__init__.py`, `tools/tests/test_repo_hygiene.py`, `runtime/tests/test_main.cpp`, `runtime/tests/check.h`
- Modify: `runtime/CMakeLists.txt` (add `scda_tests` target + `enable_testing()`/`add_test`)

**Interfaces:**
- Produces: `python -I -m unittest discover -s tools/tests -t .` (Python tests); `ctest --test-dir runtime/build` (C++ tests). `check.h` provides `CHECK(expr)` and `CHECK_EQ(a, b)` that print file:line and set a global failure flag; `test_main.cpp` provides `TEST(name)` registration and returns nonzero on any failure. Later tasks add `runtime/tests/test_*.cpp` files to `scda_tests`.

- [ ] **Step 1: Write the failing test** `tools/tests/test_repo_hygiene.py`

```python
class RepoHygiene(unittest.TestCase):
    FORBIDDEN = (".iso", ".xex", ".xexp", ".img", ".env")
    def tracked(self):
        return subprocess.run(["git", "ls-files"], capture_output=True, text=True, check=True, cwd=ROOT).stdout.split()
    def test_no_game_data_tracked(self):
        bad = [f for f in self.tracked() if f.lower().endswith(self.FORBIDDEN) or f.startswith(("assets/", "_install/", "_run/", "generated/")) or f.startswith("TU_")]
        self.assertEqual(bad, [])
    def test_license_is_gpl3(self):
        self.assertIn("GNU GENERAL PUBLIC LICENSE", (ROOT / "LICENSE").read_text())
        self.assertIn("Version 3", (ROOT / "LICENSE").read_text())
    def test_notices_name_dependencies(self):
        text = (ROOT / "THIRD_PARTY_NOTICES.md").read_text()
        for name in ("XenonRecomp", "UnleashedRecomp", "Xenia", "simde", "libmspack", "tiny-AES-c"):
            self.assertIn(name, text)
```

- [ ] **Step 2: Run it to verify it fails**

Run: `python -I -m unittest discover -s tools/tests -t .`
Expected: FAIL (`not a git repository` / missing `LICENSE`).

- [ ] **Step 3: Create the repo**

`git init -b main`. `.gitignore` covers: `.env`, `*.iso`, `*.xex`, `*.xexp`, `*.img`, `*.bin`, `TU_*`, `assets/`, `_install/`, `_run/`, `generated/`, `.venv/`, `external/`, `runtime/build/`, `tools/**/*.exe`, `tools/**/*.obj`, `tools/**/*.res`, `*.log`, `__pycache__/`. `LICENSE` = verbatim GPL-3.0 text (copy `external/UnleashedRecomp/COPYING`). `THIRD_PARTY_NOTICES.md`: one section per dependency with license name and URL: XenonRecomp (MIT), UnleashedRecomp (GPL-3.0), Xenia (BSD-3-Clause, via XenonUtils `xex_patcher`), simde (MIT), libmspack (LGPL-2.1), tiny-AES-c (Unlicense), fmt (MIT), tomlplusplus (MIT). External tools stay out of git: `external/` is rebuilt from `STATE.md` instructions.

- [ ] **Step 4: Add the C++ harness**

`runtime/tests/check.h` + `runtime/tests/test_main.cpp` as described in Interfaces. A placeholder test `TEST(harness_runs) { CHECK_EQ(1 + 1, 2); }`. CMake: `add_executable(scda_tests tests/test_main.cpp)`, `enable_testing()`, `add_test(NAME scda_tests COMMAND scda_tests)`.

- [ ] **Step 5: Run both suites**

Run: `python -I -m unittest discover -s tools/tests -t .` then `cmd //c tools\\build_runtime.bat` and `ctest --test-dir runtime/build --output-on-failure`
Expected: Python OK (3 tests); ctest `100% tests passed`.

- [ ] **Step 6: Commit**

```bash
git add .gitignore LICENSE THIRD_PARTY_NOTICES.md CLAUDE.md STATE.md BRIDGE_LOG.md .mcp.json .claude config docs tools runtime icon.jpg installer.png
git status --short   # confirm no game data, no .env
git commit -m "chore: initial repo, GPL-3.0 license, test harnesses"
```

---

### Task 2: STFS reader (title update packages)

**Files:**
- Create: `tools/stfs/stfs.h`, `tools/stfs/stfs.cpp`, `tools/stfs/tu_extract.cpp` (CLI), `tools/stfs/build.bat`, `runtime/tests/test_stfs.cpp`
- Modify: `runtime/CMakeLists.txt` (add `tools/stfs/stfs.cpp` to `scda_tests`)

**Interfaces:**
- Produces (namespace-free, `stfs.h`):
  - `struct StfsEntry { std::string path; uint32_t size; uint32_t startBlock; uint32_t blockCount; bool isDir; };`
  - `class StfsPackage { public: bool Open(const std::filesystem::path& file, std::string* error); uint32_t TitleId() const; uint32_t MediaId() const; uint32_t ContentType() const; const std::vector<StfsEntry>& Entries() const; bool Read(const StfsEntry& e, std::vector<uint8_t>& out, std::string* error) const; };` `path` uses `\` separators relative to the package root (e.g. `Texts\ENG\Menu_text.ENG` — the exact nesting comes from the file table's parent indices).
  - CLI: `tu_extract.exe <tu file> <out dir>` writes every file, exit 0/1.
- Reference for format only: `external/TTT2/port/tools/stfs.py` (read, do not copy); Xenia `stfs_container_device.cc` (BSD) may be consulted.

- [ ] **Step 1: Write the failing test** `runtime/tests/test_stfs.cpp` (skips with a message if the TU file is absent)

```cpp
TEST(stfs_tu3_contents) {
    StfsPackage p; std::string err;
    CHECK(p.Open(SCDA_ROOT "/TU_1AL61UN_000000C000000.00000000000O3", &err));
    CHECK_EQ(p.TitleId(), 0x555307D7u);
    CHECK_EQ(p.MediaId(), 0x3FA11EB0u);
    CHECK_EQ(p.ContentType(), 0xB0000u);
    auto find = [&](const char* name) { for (auto& e : p.Entries()) if (e.path.ends_with(name)) return &e; return (const StfsEntry*)nullptr; };
    CHECK_EQ(find("SC4_Offline.xexp")->size, 4096u);
    CHECK_EQ(find("default.xexp")->size, 774144u);
    CHECK_EQ(find("Menu_text.ENG")->size, 144624u);
    CHECK_EQ(find("ChallengeDefinitionsDC.ini")->size, 18140u);
}
TEST(stfs_every_file_reads_full_size) {   // Review Focus: chains starting in blocks 0..0xA9
    StfsPackage p; std::string err; std::vector<uint8_t> buf;
    CHECK(p.Open(SCDA_ROOT "/TU_1AL61UN_000000C000000.00000000000O3", &err));
    for (auto& e : p.Entries()) if (!e.isDir) { CHECK(p.Read(e, buf, &err)); CHECK_EQ(buf.size(), (size_t)e.size); }
}
TEST(stfs_rejects_non_package) {
    StfsPackage p; std::string err;
    CHECK(!p.Open(SCDA_ROOT "/icon.jpg", &err));
    CHECK(!err.empty());
}
```
`SCDA_ROOT` is a compile definition set by CMake to the repo root.

- [ ] **Step 2: Run to verify it fails**

Run: build `scda_tests`, `ctest --test-dir runtime/build --output-on-failure`
Expected: compile error, `StfsPackage` undefined.

- [ ] **Step 3: Implement `StfsPackage`** in `tools/stfs/stfs.cpp`

Magic `LIVE`/`PIRS`/`CON `; header fields at their XContent offsets (title ID 0x360, media ID 0x354, content type 0x344, volume descriptor 0x379). Block-to-offset and hash-table placement depend on the descriptor's table-size shift; compute block offsets in signed 64-bit so a chain's hash entry for blocks 0..0xA9 (stored before the first data block) is found. Follow chains through hash entries' next-block field, not by assuming contiguity.

- [ ] **Step 4: Implement the `tu_extract` CLI** and `tools/stfs/build.bat` (vcvars64 + clang-cl `/MT`, same shape as `tools/xex_dump/build.bat`).

- [ ] **Step 5: Run tests**

Run: `ctest --test-dir runtime/build --output-on-failure`, then `tools/stfs/tu_extract.exe TU_1AL61UN_000000C000000.00000000000O3 assets/tu3` and `ls -la assets/tu3`
Expected: all tests pass; `assets/tu3/SC4_Offline.xexp` is 4096 bytes.

- [ ] **Step 6: Commit**

```bash
git add tools/stfs runtime/tests/test_stfs.cpp runtime/CMakeLists.txt
git commit -m "feat: STFS reader and tu_extract for title update packages"
```

---

### Task 3: Patched flat image (`SC4_Offline.img`)

**Files:**
- Modify: `tools/xex_dump/main.cpp`, `tools/xex_dump/build.bat`
- Create: `tools/tests/test_xex_dump.py`

**Interfaces:**
- Consumes: `assets/tu3/SC4_Offline.xexp` (Task 2).
- Produces: CLI `xex_dump.exe <in.xex> [--patch <in.xexp>] [--out-xex <patched.xex>] [--out-img <image.img>]`. Prints `base=`, `size=`, `entry=`, `version=<a.b.c.d>` (from the patched XEX execution info, opt header `0x00040006`), the eight save/restore helper lines (existing), and `sha256=<hex>` of the written image. On a version mismatch prints `ERROR: patch targets <v>, expected 0.0.3.3` and exits 2 without writing. Writes the image zero-padded to the full `imageSize` (0x1300000), fixing the existing 64 KB short write.
- Produces: `assets/SC4_Offline_tu3.xex`, `assets/SC4_Offline.img`, and the image SHA-256, which Task 4 records in `config/expected_image.txt` and Task 5 bakes into the runtime.

- [ ] **Step 1: Write the failing test** `tools/tests/test_xex_dump.py`

```python
def run(*args):
    return subprocess.run([str(ROOT / "tools/xex_dump/xex_dump.exe"), *map(str, args)], capture_output=True, text=True)

class XexDump(unittest.TestCase):
    def test_tu3_patch_produces_0033_image(self):
        with tempfile.TemporaryDirectory(prefix="scda dump (test) ") as d:   # Review Focus: spaces/parens
            img = Path(d) / "SC4_Offline.img"
            r = run(ROOT / "assets/SC4_Offline.xex", "--patch", ROOT / "assets/tu3/SC4_Offline.xexp", "--out-img", img)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertIn("version=0.0.3.3", r.stdout)
            self.assertIn("entry=0x", r.stdout)
            self.assertEqual(img.stat().st_size, 0x1300000)
            self.assertRegex(r.stdout, r"sha256=[0-9a-f]{64}")
    def test_unpatched_reports_0003(self):
        r = run(ROOT / "assets/SC4_Offline.xex")
        self.assertIn("version=0.0.0.3", r.stdout)
    def test_wrong_patch_refused(self):          # Review Focus: wrong title update
        with tempfile.TemporaryDirectory() as d:
            img = Path(d) / "x.img"
            r = run(ROOT / "assets/SC4_Offline.xex", "--patch", ROOT / "assets/tu3/default.xexp", "--out-img", img)
            self.assertEqual(r.returncode, 2)
            self.assertIn("ERROR", r.stdout)
            self.assertFalse(img.exists())
```

- [ ] **Step 2: Run to verify it fails**

Run: `python -I -m unittest tools.tests.test_xex_dump -v`
Expected: FAIL (unknown option `--patch`, no `version=` line).

- [ ] **Step 3: Implement** in `tools/xex_dump/main.cpp`

Apply the patch with `XexPatcher::apply(xexBytes, size, patchBytes, size, outBytes, false)`; on any non-`Success` result print `ERROR: <result name>` and exit 2. Read the version from the patched XEX's execution-info optional header (`0x00040006`, version field at +4, decode as `v>>28 . (v>>24)&0xF . (v>>8)&0xFFFF . v&0xFF`); refuse anything but 0.0.3.3 when `--patch` is given. SHA-256 with Windows `BCrypt` (link `bcrypt.lib`).

- [ ] **Step 4: Run tests and produce the real outputs**

Run: `python -I -m unittest tools.tests.test_xex_dump -v` then
`tools/xex_dump/xex_dump.exe assets/SC4_Offline.xex --patch assets/tu3/SC4_Offline.xexp --out-xex assets/SC4_Offline_tu3.xex --out-img assets/SC4_Offline.img`
Expected: 3 tests OK; the real run prints `version=0.0.3.3` and a `sha256=` line. Note whether the eight helper addresses differ from `config/scda_config.toml`.

- [ ] **Step 5: Commit**

```bash
git add tools/xex_dump tools/tests/test_xex_dump.py
git commit -m "feat: xex_dump applies TU xexp, writes full patched image with hash"
```

---

### Task 4: Recompile from the TU3 image

**Files:**
- Modify: `config/scda_config.toml`, `config/scda_switches.toml` (regenerated), `tools/recomp.sh`, `tools/list_imports.py` (input image argument already exists), `runtime/src/import_vars.inc` (regenerated)
- Create: `tools/check_recomp_acceptance.py`, `tools/tests/test_recomp_acceptance.py`, `config/expected_image.txt`

**Interfaces:**
- Consumes: `assets/SC4_Offline_tu3.xex`, `assets/SC4_Offline.img` and its SHA-256 (Task 3).
- Produces: `generated/` from the 0.0.3.3 image; `config/expected_image.txt` containing exactly one line, the 64-hex SHA-256 of `assets/SC4_Offline.img`; `python -I tools/check_recomp_acceptance.py docs/recomp_SC4_Offline.log` exits 0 only when the Global Constraints acceptance numbers hold, printing `unrecognized=N switch_errors=N error_sites=N functions=N`.

- [ ] **Step 1: Write the failing test** `tools/tests/test_recomp_acceptance.py`

```python
class Acceptance(unittest.TestCase):
    def test_counts_bad_log(self):
        with tempfile.TemporaryDirectory() as d:
            log = Path(d) / "r.log"
            log.write_text("Unrecognized instruction at 0x82000000: vfoo\nERROR: Switch case at 82000000 is trying to jump outside function: 82000010\n")
            r = subprocess.run([sys.executable, "-I", str(ROOT / "tools/check_recomp_acceptance.py"), str(log)], capture_output=True, text=True)
            self.assertNotEqual(r.returncode, 0)
            self.assertIn("unrecognized=1 switch_errors=1", r.stdout)
    def test_real_recomp_is_clean(self):
        r = subprocess.run([sys.executable, "-I", str(ROOT / "tools/check_recomp_acceptance.py"), str(ROOT / "docs/recomp_SC4_Offline.log")], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout)
    def test_generated_is_from_tu3_image(self):
        self.assertTrue((ROOT / "config/scda_config.toml").read_text().count("SC4_Offline_tu3.xex") >= 1)
```

- [ ] **Step 2: Run to verify it fails**

Run: `python -I -m unittest tools.tests.test_recomp_acceptance -v`
Expected: FAIL (script missing).

- [ ] **Step 3: Implement `tools/check_recomp_acceptance.py`**; scans the log plus `generated/ppc_recomp.*.cpp` for `// ERROR`.

- [ ] **Step 4: Switch the config to the patched XEX**

`file_path = "../assets/SC4_Offline_tu3.xex"` (already patched, so no `patch_file_path`). Regenerate switches: `external/XenonRecomp/build/XenonAnalyse/XenonAnalyse.exe assets/SC4_Offline_tu3.xex config/scda_switches.toml`. Update the eight helper addresses if Task 3 reported different ones. Clear the `# BEGIN functions` block (old addresses may have moved). Write `config/expected_image.txt`.

- [ ] **Step 5: Run the recompile loop until clean**

Run: `sh tools/recomp.sh`, then `python -I tools/fix_switch_bounds.py`, repeat until `switcherr=0`. Then handle the bctr reported as `Found a switch jump table at ... with no switch table entry present` by adding its `[[switch]]` entry to `config/scda_switches.toml` (base, register, labels read from the image at that address), and rerun.
Expected: `python -I tools/check_recomp_acceptance.py docs/recomp_SC4_Offline.log` prints `unrecognized=0 switch_errors=0 error_sites=0`.

- [ ] **Step 6: Regenerate data-import table and stubs**

Run: `python -I tools/list_imports.py assets/SC4_Offline_tu3.xex assets/SC4_Offline.img` then `python -I tools/gen_import_stubs.py`
Expected: `216 records, 10 variable imports` or a changed count that is then noted in `STATE.md`.

- [ ] **Step 7: Run all Python tests, commit**

Run: `python -I -m unittest discover -s tools/tests -t .`
Expected: OK.

```bash
git add config tools/check_recomp_acceptance.py tools/tests/test_recomp_acceptance.py tools/recomp.sh runtime/src/import_vars.inc runtime/src/imports_stubs.cpp
git commit -m "feat: recompile from TU3 (0.0.3.3) image, acceptance check"
```

---

### Task 5: Runtime loads the flat patched image

**Files:**
- Modify: `runtime/src/loader.cpp`, `runtime/src/main.cpp`, `runtime/CMakeLists.txt` (bake `config/expected_image.txt` as `SCDA_EXPECTED_IMAGE_SHA256`)
- Create: `runtime/tests/test_loader.cpp`

**Interfaces:**
- Consumes: `config/expected_image.txt` (Task 4), `generated/` (Task 4).
- Produces:
  - `enum class ImageLoadResult { Ok, NotFound, WrongSize, WrongHash };`
  - `ImageLoadResult LoadFlatImage(const std::filesystem::path& img, uint8_t* guestBase, std::string* actualSha256);` copies the file to `guestBase + 0x82000000`; size must be 0x1300000; SHA-256 must equal `SCDA_EXPECTED_IMAGE_SHA256`.
  - `scda.exe` argument: path to `SC4_Offline.img`; default `<exe dir>\game\SC4_Offline.img`. `g_gameDir` = the image's directory. The XenonUtils XEX loader and `scda_loader` library are removed from `scda.exe`.
  - On a non-`Ok` result: message box + log line `[fatal] SC4_Offline.img: <reason>, expected sha256 <hex>, got <hex>`; exit code 2.

- [ ] **Step 1: Write the failing test** `runtime/tests/test_loader.cpp`

```cpp
TEST(loader_accepts_tu3_image) {
    std::vector<uint8_t> mem(0x84000000ull); // covers guest 0x82000000 + 0x1300000
    std::string sha;
    CHECK(LoadFlatImage(SCDA_ROOT "/assets/SC4_Offline.img", mem.data(), &sha) == ImageLoadResult::Ok);
    CHECK_EQ(sha, std::string(SCDA_EXPECTED_IMAGE_SHA256));
}
TEST(loader_rejects_foreign_image) {   // Review Focus: stale or foreign image
    auto tmp = std::filesystem::temp_directory_path() / "scda loader (test)" / "SC4_Offline.img"; // spaces/parens
    std::filesystem::create_directories(tmp.parent_path());
    { std::ofstream f(tmp, std::ios::binary); std::vector<char> z(0x1300000); f.write(z.data(), z.size()); }
    std::vector<uint8_t> mem(0x84000000ull);
    std::string sha;
    CHECK(LoadFlatImage(tmp, mem.data(), &sha) == ImageLoadResult::WrongHash);
}
TEST(loader_rejects_short_file) {
    auto tmp = std::filesystem::temp_directory_path() / "scda_short.img";
    { std::ofstream f(tmp, std::ios::binary); f << "x"; }
    std::vector<uint8_t> mem(0x84000000ull);
    CHECK(LoadFlatImage(tmp, mem.data(), nullptr) == ImageLoadResult::WrongSize);
}
```
(Allocate the test buffer with `VirtualAlloc` reserve+commit of `0x84000000` bytes if `std::vector` is too slow; the assertion set stays the same.)

- [ ] **Step 2: Run to verify it fails**

Run: `cmd //c tools\\build_runtime.bat` then `ctest --test-dir runtime/build --output-on-failure`
Expected: compile error, `LoadFlatImage` undefined.

- [ ] **Step 3: Implement `LoadFlatImage`** in `runtime/src/loader.cpp` (BCrypt SHA-256) and switch `main.cpp` to it.

- [ ] **Step 4: Run tests and a boot**

Run: `ctest --test-dir runtime/build --output-on-failure`; copy `assets/SC4_Offline.img` to `_install/game/`; `cd _install && timeout 60 ../runtime/build/scda.exe; grep -c '\[fs\] open ' ../runtime/build/scda.log`
Expected: tests pass; the boot log shows `[loader]` with the image path and reaches the same `[thread] 3 start` line as before.

- [ ] **Step 5: Commit**

```bash
git add runtime
git commit -m "feat: runtime loads verified TU3 flat image"
```

---

### Task 6: Hang watchdog with guest stack walks

**Files:**
- Create: `runtime/src/watchdog.cpp`, `runtime/src/watchdog.h`, `runtime/tests/test_watchdog.cpp`
- Modify: `runtime/src/kernel.cpp` (register/unregister guest threads), `runtime/src/main.cpp` (register main thread, start watchdog), `runtime/src/imports.cpp` (record the current import name per thread in `ImportStub` and in every `SCDA_IMPORT` via a thread-local set by a small RAII helper used in `ImportStub`; hand-written imports that block call `WatchdogNoteBlocking("<name>")`)

**Interfaces:**
- Produces (`watchdog.h`):
  - `void WatchdogRegisterThread(uint32_t guestThreadId, PPCContext* ctx);` / `void WatchdogUnregisterThread(uint32_t guestThreadId);`
  - `void WatchdogNoteBlocking(const char* importName);` (thread-local, cleared on return by `struct WatchdogBlockScope`)
  - `std::vector<uint32_t> WalkGuestStack(const uint8_t* guestBase, uint32_t r1, uint32_t lr, int maxFrames);` returns return addresses, first = `lr`. Frame rule (Xbox 360 ABI as emitted by this title: `mflr r12; stw r12,-8(r1); stwu r1,-N(r1)`): back chain = `load32(sp)`; saved LR of the caller frame = `load32(backChain - 8)`; stop on 0, on a back chain not above `sp`, or outside guest memory.
  - `void WatchdogStart(unsigned seconds);` enabled by env `SCDA_WATCHDOG=<seconds>`; if no `[fs]`/import progress counter changes for that long, logs per thread: `[watchdog] thread <id> in <import or "(guest code)"> stack: 0x...  0x...` (max 16 frames), once per stall.

- [ ] **Step 1: Write the failing test** `runtime/tests/test_watchdog.cpp`

```cpp
TEST(stack_walk_follows_back_chain) {
    std::vector<uint8_t> mem(0x10000);
    auto st = [&](uint32_t a, uint32_t v) { *(uint32_t*)&mem[a] = _byteswap_ulong(v); };
    // frame0 sp=0x8000 -> back chain 0x8100; frame1 saved LR at 0x8100-8; frame1 back chain 0x8200; frame2 saved LR at 0x8200-8; then 0
    st(0x8000, 0x8100); st(0x8100 - 8, 0x82001000);
    st(0x8100, 0x8200); st(0x8200 - 8, 0x82002000);
    st(0x8200, 0);
    auto frames = WalkGuestStack(mem.data(), 0x8000, 0x82000500, 16);
    CHECK_EQ(frames.size(), (size_t)3);
    CHECK_EQ(frames[0], 0x82000500u);
    CHECK_EQ(frames[1], 0x82001000u);
    CHECK_EQ(frames[2], 0x82002000u);
}
TEST(stack_walk_stops_on_loop) {
    std::vector<uint8_t> mem(0x10000);
    *(uint32_t*)&mem[0x8000] = _byteswap_ulong(0x8000);   // back chain points to itself
    CHECK_EQ(WalkGuestStack(mem.data(), 0x8000, 0x82000500, 16).size(), (size_t)1);
}
TEST(blocked_thread_is_reported_without_import) {   // Review Focus: hung in a host wait
    PPCContext ctx{}; ctx.r1.u64 = 0x8000; ctx.lr = 0x82000500;
    WatchdogRegisterThread(7, &ctx);
    std::string report = WatchdogReportForTest();  // formats the same text the timer logs
    CHECK(report.find("thread 7 in (guest code)") != std::string::npos);
    WatchdogUnregisterThread(7);
}
```
`WatchdogReportForTest()` is declared in `watchdog.h` under `#ifdef SCDA_TESTS` and returns the report string for the registered threads, using the same formatter as the timer.

- [ ] **Step 2: Run to verify it fails**

Run: `ctest --test-dir runtime/build --output-on-failure`
Expected: compile error, `WalkGuestStack` undefined.

- [ ] **Step 3: Implement `watchdog.cpp`** and wire registration into `GuestThreadMain` (kernel.cpp) and `main()`; the waits in `kernel.cpp` (`NtWaitFor*`, `KeWaitFor*`, `KeDelayExecutionThread`) open a `WatchdogBlockScope` with their import name.

- [ ] **Step 4: Run tests and capture the real hang**

Run: `ctest --test-dir runtime/build --output-on-failure`; then `cd _install && SCDA_WATCHDOG=15 timeout 40 ../runtime/build/scda.exe; grep '\[watchdog\]' ../runtime/build/scda.log`
Expected: tests pass; the log has one `[watchdog]` block naming each guest thread, what it is blocked in, and its guest return addresses.

- [ ] **Step 5: Record findings and commit**

Copy the `[watchdog]` block into `STATE.md` under "Boot", with the import each thread waits in and the top three guest addresses. These addresses are the input to the GPU phase plan.

```bash
git add runtime STATE.md
git commit -m "feat: hang watchdog with guest stack walks; record boot stall"
```

---

### Task 7: Phase hand-off

**Files:**
- Modify: `STATE.md`, `CLAUDE.md` (section 4 layout: add `tools/stfs`, `docs/superpowers/`, test commands)

- [ ] **Step 1: Verify everything from a clean shell**

Run: `python -I -m unittest discover -s tools/tests -t .` and `ctest --test-dir runtime/build --output-on-failure` and `git status --short`
Expected: all OK; working tree clean except ignored files.

- [ ] **Step 2: Update `STATE.md`** (max 50 lines): TU3 image hash, function count, acceptance numbers, watchdog findings, next = "write Phase 2 (GPU) plan from the watchdog findings".

- [ ] **Step 3: Commit**

```bash
git add STATE.md CLAUDE.md
git commit -m "docs: phase 1 complete, state for phase 2"
```
