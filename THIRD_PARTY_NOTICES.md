# Third-party notices

SCDA Recomp is licensed under GPL-3.0 (see `LICENSE`). It builds on or adapts:

| Project | License | Use | URL |
|-|-|-|-|
| XenonRecomp / XenonAnalyse / XenonUtils | MIT | PPC recompiler, XEX loader (patched: `tools/patches/xenonrecomp_scda.patch`) | https://github.com/hedge-dev/XenonRecomp |
| XenosRecomp | MIT | Shader microcode translator, forked into `runtime/src/gpu/xenos/` (modified) | https://github.com/hedge-dev/XenosRecomp |
| UnleashedRecomp | GPL-3.0 | Reference runtime; kernel/XAM HLE patterns adapted | https://github.com/hedge-dev/UnleashedRecomp |
| Xenia | BSD-3-Clause | XEX patching code inside XenonUtils (`xex_patcher`); format reference | https://github.com/xenia-project/xenia |
| simde | MIT | SIMD intrinsics used by recompiled code | https://github.com/simd-everywhere/simde |
| libmspack | LGPL-2.1 | LZX decompression inside XenonUtils | https://www.cabextract.org.uk/libmspack/ |
| tiny-AES-c | Unlicense | XEX decryption inside XenonUtils | https://github.com/kokke/tiny-AES-c |
| fmt | MIT | Formatting in XenonRecomp | https://github.com/fmtlib/fmt |
| toml++ | MIT | Config parsing in XenonRecomp | https://github.com/marzer/tomlplusplus |

The Tekken Tag Tournament 2 PC port (https://github.com/shihabal3amri/tekken-tag-tournament-2-pc)
is used as a design reference only. No code from it is included: it has no license.

Splinter Cell: Double Agent is the property of Ubisoft. This project contains no game data.
