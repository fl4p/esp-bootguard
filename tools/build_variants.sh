#!/bin/bash
# Build every crash-loop variant the board test flashes, each in its own build dir.
#   source $IDF_PATH/export.sh && tools/build_variants.sh [variant ...]
set -u
cd "$(dirname "$0")/../examples/crashloop" || exit 2

all="healthy abort stack intwdt restart twocrash abort-halt"
variants="${*:-$all}"

for v in $variants; do
    case "$v" in
        healthy)    frags="" ;;
        abort)      frags="sdkconfig.kind_abort" ;;
        stack)      frags="sdkconfig.kind_stack" ;;
        intwdt)     frags="sdkconfig.kind_intwdt" ;;
        restart)    frags="sdkconfig.kind_restart" ;;
        twocrash)   frags="sdkconfig.kind_twocrash" ;;
        abort-halt) frags="sdkconfig.kind_abort;sdkconfig.action_halt" ;;
        *) echo "unknown variant $v"; exit 2 ;;
    esac
    defaults="sdkconfig.defaults${frags:+;$frags}"
    rm -f "build-$v/sdkconfig"   # re-apply the defaults
    echo "== $v ($defaults)"
    if ! idf.py -B "build-$v" -D SDKCONFIG="build-$v/sdkconfig" -D SDKCONFIG_DEFAULTS="$defaults" build > "build-$v.log" 2>&1; then
        grep -E "error:|Error" "build-$v.log" | head -10
        echo "FAILED: $v (build-$v.log)"
        exit 1
    fi
    ls -l "build-$v/bootloader/bootloader.bin" "build-$v/crashloop.bin" | awk '{print "   " $5, $9}'
done
echo "all built"
