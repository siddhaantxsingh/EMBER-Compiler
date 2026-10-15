#!/bin/sh
# Run the golden suite (tests/run_golden.py) with extra options added to
# every `ember run`, e.g. to check that -O0 and GC stress mode produce
# exactly the same output, errors and exit codes as the default build:
#   scripts/golden_variant.sh build/ember -O0
#   scripts/golden_variant.sh build-asan/ember --gc-stress
set -eu
ember=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
shift
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
cat > "$dir/ember" <<WRAP
#!/bin/sh
cmd=\$1; shift
exec "$ember" "\$cmd" $* "\$@"
WRAP
chmod +x "$dir/ember"
python3 "$(dirname "$0")/../tests/run_golden.py" "$dir/ember"
