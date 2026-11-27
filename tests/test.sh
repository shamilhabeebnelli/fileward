#!/bin/sh
# End-to-end tests for fileward. Run from the repo root: sh tests/test.sh
set -u
FW=${FW:-./fileward}
T=$(mktemp -d)
trap 'kill $(jobs -p) 2>/dev/null || true; rm -rf "$T"' EXIT
fails=0
say() { printf '%s\n' "$*"; }
check() { # name expected actual
  if [ "$2" = "$3" ]; then say "ok   - $1"; else say "FAIL - $1 (expected [$2], got [$3])"; fails=$((fails+1)); fi
}
wait_for() { # file contents timeout_seconds
  n=0; while [ $n -lt "$3" ]; do grep -q "$2" "$1" 2>/dev/null && return 0; sleep 1; n=$((n+1)); done; return 1
}

# 1. a change to a watched file runs the command once
: > "$T/ran1"
: > "$T/watched.txt"
"$FW" -q -d 200 -p "$T/watched.txt" -- sh -c "echo ran >> '$T/ran1'" & WP=$!
sleep 1; echo hello > "$T/watched.txt"
wait_for "$T/ran1" ran 8; check "change runs the command" 0 $?
kill $WP 2>/dev/null; wait $WP 2>/dev/null

# 2. rapid bursts settle into a single run
: > "$T/ran2"; echo x > "$T/f2"
"$FW" -q -d 400 -p "$T/f2" -- sh -c "echo ran >> '$T/ran2'" & WP=$!
sleep 1
for i in 1 2 3 4 5; do echo $i >> "$T/f2"; sleep 0.1; done
wait_for "$T/ran2" ran 8; sleep 1
check "5 quick edits = 1 run" 1 "$(wc -l < "$T/ran2" | tr -d ' ')"
kill $WP 2>/dev/null; wait $WP 2>/dev/null

# 3. recursive watching picks up a new subdirectory created later
mkdir "$T/tree"; : > "$T/ran3"
"$FW" -q -r -d 200 -p "$T/tree" -- sh -c "echo ran >> '$T/ran3'" & WP=$!
sleep 1; mkdir "$T/tree/newdir"; sleep 1; echo hi > "$T/tree/newdir/deep.txt"
wait_for "$T/ran3" ran 8; check "recursive: new subdir file triggers" 0 $?
kill $WP 2>/dev/null; wait $WP 2>/dev/null

# 4. atomic replace (write temp + rename) still triggers and watching continues
echo orig > "$T/f4"; : > "$T/ran4"
"$FW" -q -d 200 -p "$T/f4" -- sh -c "echo ran >> '$T/ran4'" & WP=$!
sleep 1
echo new > "$T/f4.tmp"; mv "$T/f4.tmp" "$T/f4"
wait_for "$T/ran4" ran 8; c1=$(wc -l < "$T/ran4" | tr -d ' ')
echo newer > "$T/f4.tmp"; mv "$T/f4.tmp" "$T/f4"
sleep 2; c2=$(wc -l < "$T/ran4" | tr -d ' ')
check "atomic replace triggers twice" 2 "$c2"
kill $WP 2>/dev/null; wait $WP 2>/dev/null

# 5. -k kills a running command on new changes and restarts it
: > "$T/ran5"; echo x > "$T/f5"
"$FW" -q -k -d 200 -p "$T/f5" -- sh -c "echo start >> '$T/ran5'; sleep 30" & WP=$!
sleep 1; echo y >> "$T/f5"   # starts run 1
sleep 2; echo z >> "$T/f5"   # should kill run 1 and start run 2
sleep 2
starts=$(grep -c start "$T/ran5")
check "-k restarts a running command" 2 "$starts"
kill $WP 2>/dev/null; wait $WP 2>/dev/null

# 6. -s runs once at startup
: > "$T/ran6"; echo x > "$T/f6"
"$FW" -q -s -d 200 -p "$T/f6" -- sh -c "echo ran >> '$T/ran6'" & WP=$!
wait_for "$T/ran6" ran 8; check "-s runs at startup" 0 $?
kill $WP 2>/dev/null; wait $WP 2>/dev/null

# 7. bad usage is rejected
"$FW" -- true 2>/dev/null; check "missing -p is an error" 2 $?
"$FW" -p "$T/f6" 2>/dev/null; check "missing command is an error" 2 $?

say ""
if [ $fails -eq 0 ]; then say "all tests passed"; else say "$fails test(s) FAILED"; exit 1; fi
