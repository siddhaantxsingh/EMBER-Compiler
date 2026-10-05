#!/bin/sh
# Enforce the dependency layering of src/:
#   common  <- bc <- vm
#   common  <- bc <- compiler (front, sema, ir, codegen, driver)
#   tools and cli may use everything.
# In particular the VM must never include compiler headers, and the
# compiler must never include the VM.
set -eu
cd "$(dirname "$0")/../src"
status=0
check() {  # dir allowed-prefixes...
    dir=$1; shift
    for f in "$dir"/*.h "$dir"/*.cc; do
        [ -e "$f" ] || continue
        grep -o '#include "[^"]*"' "$f" | sed 's/#include "\(.*\)"/\1/' | while read -r inc; do
            ok=0
            for p in "$@"; do
                case "$inc" in "$p"/*) ok=1 ;; esac
            done
            if [ $ok -eq 0 ]; then
                echo "layering violation: $f includes $inc"
                echo x >> /tmp/ember-layering.$$
            fi
        done
    done
}
rm -f /tmp/ember-layering.$$
check common common
check bc bc common
check vm vm bc common
for d in front sema ir codegen driver; do
    check $d front sema ir codegen driver bc common
done
if [ -e /tmp/ember-layering.$$ ]; then
    rm -f /tmp/ember-layering.$$
    exit 1
fi
echo "layering ok"
