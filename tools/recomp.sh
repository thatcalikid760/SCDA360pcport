#!/bin/sh
# Regenerate generated/ from config/scda_config.toml. Log -> docs/recomp_SC4_Offline.log
cd "$(dirname "$0")/.." || exit 1
# XenonRecomp skips unchanged files, so stale ones are not deleted here.
external/XenonRecomp/build/XenonRecomp/XenonRecomp.exe config/scda_config.toml external/XenonRecomp/XenonUtils/ppc_context.h > docs/recomp_SC4_Offline.log 2>&1
rc=$?
L=docs/recomp_SC4_Offline.log
echo "rc=$rc unrec=$(grep -c Unrecognized $L) switcherr=$(grep -c 'Switch case' $L) badsw=$(grep 'Switch case' $L | awk '{print $5}' | sort -u | wc -l) funcs=$(grep -c ', sub_' generated/ppc_func_mapping.cpp)"
grep -v 'Unrecognized\|Switch case\|^Recompiling' $L | sort | uniq -c | sort -rn | head -5
