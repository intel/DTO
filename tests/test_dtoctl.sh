#!/bin/bash
# SPDX-License-Identifier: MIT
#
# Tests for dtoctl's option handling, library resolution and environment setup.
# No DSA hardware is needed: most checks read dtoctl's --dry-run output or the
# environment the program receives, and the end-to-end check reads libdto's own
# configuration trace, which libdto prints even when it falls back to the CPU.
#
# Usage: test_dtoctl.sh <dtoctl> <libdto.so>

DTOCTL="$1"
LIB=$(realpath "$2")
LIBDIR=$(dirname "$LIB")
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
failures=0

# Start from a clean environment so the caller's DTO setup cannot leak in.
unset LD_PRELOAD
for var in $(env | sed -n 's/^\(DTO_[A-Za-z0-9_]*\)=.*/\1/p'); do
    unset "$var"
done

# run <command...>: capture the command's output and exit status
run() {
    out=$("$@" 2>&1)
    rc=$?
    cmd="$*"
}

fail() {
    echo "FAIL: $1"
    echo "    command: $cmd"
    printf '%s\n' "$out" | sed 's/^/    | /'
    failures=$((failures + 1))
}

# has <description> <pattern>: the last output has a line matching pattern
has() {
    if printf '%s\n' "$out" | grep -q -- "$2"; then echo "PASS: $1"; else fail "$1"; fi
}

# lacks <description> <pattern>: no line of the last output matches pattern
lacks() {
    if printf '%s\n' "$out" | grep -q -- "$2"; then fail "$1"; else echo "PASS: $1"; fi
}

# status <description> <code>: the last command exited with code
status() {
    if [ "$rc" -eq "$2" ]; then echo "PASS: $1"; else fail "$1 (exit $rc, expected $2)"; fi
}

# --- every option sets its variable ---
run "$DTOCTL" -l "$LIB" --dry-run -w umwait -b 64K -c 0.25 -n 2 -q 'wq0.0;wq2.0' \
    --umwait-delay 5000 --overlapping-memmove=dsa --no-memcpy --no-memmove \
    --no-memset --no-memcmp --no-cache-control --no-auto-adjust --stdc-only \
    --log-file=/tmp/dto.log --log-level 1 --stats-file=/tmp/dto.stats prog 'a b'
status "dry-run succeeds" 0
for v in DTO_WAIT_METHOD=umwait DTO_MIN_BYTES=65536 DTO_CPU_SIZE_FRACTION=0.25 \
         DTO_IS_NUMA_AWARE=2 "DTO_WQ_LIST='wq0.0;wq2.0'" DTO_UMWAIT_DELAY=5000 \
         DTO_OVERLAPPING_MEMMOVE_ACTION=1 DTO_DSA_MEMCPY=0 DTO_DSA_MEMMOVE=0 \
         DTO_DSA_MEMSET=0 DTO_DSA_MEMCMP=0 DTO_DSA_CC=0 DTO_AUTO_ADJUST_KNOBS=0 \
         DTO_USESTDC_CALLS=1 DTO_LOG_FILE=/tmp/dto.log DTO_LOG_LEVEL=1 \
         DTO_STATS_FILE=/tmp/dto.stats DTO_COLLECT_STATS=1; do
    has "option sets $v" "^$v  # option\$"
done
has "dry-run shows the library" "^library: $LIB (--library)\$"
has "dry-run shows LD_PRELOAD" "^LD_PRELOAD=$LIB\$"
has "dry-run shows the command" "^command: prog 'a b'\$"

# --- size suffixes ---
for s in 4096:4096 4k:4096 1M:1048576 2G:2147483648; do
    run "$DTOCTL" -l "$LIB" --dry-run -b "${s%%:*}" true
    has "-b ${s%%:*} is ${s##*:} bytes" "^DTO_MIN_BYTES=${s##*:}  # option\$"
done

# --- inherited variables ---
run env DTO_WAIT_METHOD=umwait DTO_MIN_BYTES=4096 "$DTOCTL" -l "$LIB" --dry-run -w busypoll true
has "option overrides an inherited variable" "^DTO_WAIT_METHOD=busypoll  # option\$"
has "inherited variable is kept" "^DTO_MIN_BYTES=4096  # inherited\$"

run env DTO_MIN_BYTES=4096 DTO_USESTDC_CALLS=1 "$DTOCTL" -i -l "$LIB" --dry-run true
lacks "-i drops inherited variables" "^DTO_"

run env DTO_LIBRARY="$LIB" "$DTOCTL" -i --dry-run true
lacks "-i ignores DTO_LIBRARY" "(DTO_LIBRARY)"

# --- the program receives the environment ---
run "$DTOCTL" -l "$LIB" --stdc-only -w yield env
has "program sees DTO_WAIT_METHOD" "^DTO_WAIT_METHOD=yield\$"
has "program sees LD_PRELOAD" "^LD_PRELOAD=$LIB\$"

run "$DTOCTL" -l "$LIB" --stdc-only echo -w -l
has "options after the program are the program's" "^-w -l\$"

# --- library resolution and LD_PRELOAD ---
run env DTO_LIBRARY="$LIB" "$DTOCTL" --dry-run true
has "DTO_LIBRARY selects the library" "^library: $LIB (DTO_LIBRARY)\$"

run sh -c 'cd "$1" && shift && exec "$@"' sh "$LIBDIR" \
    "$DTOCTL" -l "./$(basename "$LIB")" --dry-run true
has "relative library path becomes absolute" "^LD_PRELOAD=$LIB\$"

run "$DTOCTL" -l "$TMP/missing/libdto.so" true
status "missing library fails" 125
has "missing library is reported" "cannot use DTO library"

run env LD_PRELOAD="$TMP/libother.so $TMP/old/libdto.so.1" "$DTOCTL" -l "$LIB" --dry-run true
has "existing LD_PRELOAD entries are kept after libdto" "^LD_PRELOAD=$LIB:$TMP/libother.so\$"
has "another libdto in LD_PRELOAD is dropped" "dropping '$TMP/old/libdto.so.1'"

# --- invalid values are rejected ---
for bad in "-w bogus" "-b 64Q" "-b abc" "-b -1" "-c 1.5" "-c nan" "-n 3" \
           "--log-level x" "--umwait-delay 0" "--overlapping-memmove=gpu"; do
    run "$DTOCTL" -l "$LIB" $bad true
    status "rejects $bad" 64
done
run "$DTOCTL" -l "$LIB" -q '' true
status "rejects an empty WQ list" 64

# --- exit status ---
run "$DTOCTL" -l "$LIB"
status "no program is a usage error" 64

run "$DTOCTL" -l "$LIB" --dry-run
status "--dry-run needs no program" 0
lacks "--dry-run without a program prints no command" "^command:"

run "$DTOCTL" -l "$LIB" "$TMP/no-such-program"
status "program not found" 127

touch "$TMP/not-executable"
run "$DTOCTL" -l "$LIB" "$TMP/not-executable"
status "program not executable" 126

run "$DTOCTL" -l "$LIB" --stdc-only sh -c 'exit 7'
status "program's exit status is returned" 7

run "$DTOCTL" --version
has "--version" "^dtoctl (DTO) "

# --- libdto applies the settings (DSA is optional) ---
run "$DTOCTL" -l "$LIB" --log-level 2 -w yield -b 8K -c 0.5 --no-auto-adjust true
has "libdto applies the options" \
    "dsa_min_size: 8192, cpu_size_fraction: 0.50, wait_method: yield, auto_adjust_knobs: 0"

echo
if [ "$failures" -ne 0 ]; then
    echo "$failures dtoctl test(s) failed"
    exit 1
fi
echo "All dtoctl tests passed"
