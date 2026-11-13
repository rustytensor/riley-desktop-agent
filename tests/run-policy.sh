#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# M1 gate: config schema and permission engine. No desktop access.
#
#   tests/run-policy.sh                  build and check the real engine; must print PASS
#   tests/run-policy.sh --plant <fault>  build the planted fault into the -fault build dir; must print FAIL,
#                                        with the cases that target the fault among the failures
#
# Checks:
#   1. xmllint: examples/config.xml and fixtures main, alt are valid to schema/config.xsd
#   2. xmllint: fixtures/invalid.xml and every tests/policy/broken/*.xml (at least 6) are rejected
#   3. the decision table has at least 40 cases, every reason code is declared and used
#   4. the driver builds against the engine
#   5. the engine's loader agrees with the schema (driver: schema rows)
#   6. every case of tests/policy/cases.xml gets its expected decision (driver: decide rows)
#
# Known planted faults: remote-allowlist-ignored (see cmake/planted-faults.cmake).

set -u
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SCRATCH=/srv/devops/scratch/riley-desktop-agent/policy-gate
BUILD=/srv/devops/forge/build/riley-desktop-agent
PLANT=""

while [ $# -gt 0 ]; do
    case "$1" in
        --plant) PLANT="${2:-}"; shift 2 || { echo "usage: $0 [--plant <fault>]"; exit 2; } ;;
        -h|--help) sed -n '3,20p' "$0"; exit 0 ;;
        *) echo "usage: $0 [--plant <fault>]"; exit 2 ;;
    esac
done
case "$PLANT" in
    ""|remote-allowlist-ignored) ;;
    *) echo "unknown planted fault: $PLANT (known: remote-allowlist-ignored)"; exit 2 ;;
esac
[ -n "$PLANT" ] && BUILD=/srv/devops/forge/build/riley-desktop-agent-fault

mkdir -p "$SCRATCH"
SCHEMA="$REPO/schema/config.xsd"
CASES="$REPO/tests/policy/cases.xml"
FAILS=0
pass() { echo "PASS $*"; }
fail() { echo "FAIL $*"; FAILS=$((FAILS + 1)); }

# 1 and 2: the schema itself, checked by libxml2's xmllint.
for f in examples/config.xml tests/policy/fixtures/main.xml tests/policy/fixtures/alt.xml; do
    if xmllint --noout --schema "$SCHEMA" "$REPO/$f" >"$SCRATCH/xmllint.log" 2>&1; then
        pass "schema accepts $f"
    else
        fail "schema accepts $f: $(grep -m1 -i error "$SCRATCH/xmllint.log")"
    fi
done
nbroken=0
for f in "$REPO"/tests/policy/fixtures/invalid.xml "$REPO"/tests/policy/broken/*.xml; do
    [ -e "$f" ] || continue
    rel="${f#"$REPO"/}"
    [[ "$rel" == tests/policy/broken/* ]] && nbroken=$((nbroken + 1))
    if xmllint --noout --schema "$SCHEMA" "$f" >"$SCRATCH/xmllint.log" 2>&1; then
        fail "schema rejects $rel (it was accepted)"
    else
        pass "schema rejects $rel"
    fi
done
if [ "$nbroken" -ge 6 ]; then pass "broken copies: $nbroken (at least 6)"; else fail "broken copies: $nbroken (need 6)"; fi

# 3: the table's shape.
ncases=$(xmllint --xpath 'count(//case)' "$CASES" 2>/dev/null || echo 0)
if [ "${ncases%.*}" -ge 40 ] 2>/dev/null; then pass "decision table: $ncases cases (at least 40)"; else fail "decision table: $ncases cases (need 40)"; fi
declared=$(xmllint --xpath '//reasons/reason/@code' "$CASES" 2>/dev/null | sed 's/ code="\([^"]*\)"/\1\n/g' | sed '/^$/d' | sort -u)
used=$(xmllint --xpath '//case/@reason' "$CASES" 2>/dev/null | sed 's/ reason="\([^"]*\)"/\1\n/g' | sed '/^$/d' | sort -u)
undeclared=$(comm -13 <(echo "$declared") <(echo "$used") | tr '\n' ' ')
unused=$(comm -23 <(echo "$declared") <(echo "$used") | tr '\n' ' ')
if [ -z "$undeclared" ] && [ -z "$unused" ] && [ -n "$declared" ]; then
    pass "reason codes: every declared code used, every used code declared"
else
    fail "reason codes: undeclared [$undeclared] unused [$unused]"
fi
targeted=$(xmllint --xpath '//case[@targets="remote-allowlist-ignored"]/@id' "$CASES" 2>/dev/null \
    | sed 's/ id="\([^"]*\)"/\1 /g' | tr '\n' ' ' | tr -s ' ')
if [ -n "$targeted" ]; then pass "cases targeting remote-allowlist-ignored: $targeted"; else fail "no case targets remote-allowlist-ignored"; fi

# 4: build the driver against the engine (the planted fault only ever into the -fault build dir).
DRIVER="$BUILD/tests/policy/tst_policy"
BUILT=0
if [ ! -f "$REPO/CMakeLists.txt" ]; then
    fail "build: no top-level CMakeLists.txt (the engine is not written yet)"
elif ! cmake -S "$REPO" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DRILEY_PLANTED_FAULT="$PLANT" \
        >"$SCRATCH/configure.log" 2>&1; then
    fail "build: cmake configure failed (see $SCRATCH/configure.log)"
elif ! cmake --build "$BUILD" --target tst_policy -j8 >"$SCRATCH/build.log" 2>&1; then
    fail "build: tst_policy did not build (see $SCRATCH/build.log)"
elif [ ! -x "$DRIVER" ]; then
    fail "build: $DRIVER missing"
else
    pass "build: tst_policy${PLANT:+ with planted fault $PLANT}"
    BUILT=1
fi

# 5 and 6: run the driver.
FAILED_CASES=""
if [ "$BUILT" -eq 1 ]; then
    OUT="$SCRATCH/tst_policy.txt"
    "$DRIVER" -o "$OUT,txt" >/dev/null 2>&1
    if grep -q '^FAIL!  : tst_Policy::initTestCase' "$OUT"; then
        fail "driver: initTestCase: $(grep -m1 -A1 '^FAIL!  : tst_Policy::initTestCase' "$OUT" | tr -s ' \n' ' ')"
    fi
    nschema=0
    while IFS= read -r line; do
        case "$line" in
            "PASS   : tst_Policy::schema("*)
                row="${line#*schema(}"; pass "loader: ${row%%)*}"; nschema=$((nschema + 1)) ;;
            "FAIL!  : tst_Policy::schema("*)
                row="${line#*schema(}"; detail=$(grep -F -A3 "$line" "$OUT" | sed -n '2,3p' | tr -s ' \n' ' ')
                fail "loader: ${row%%)*}:${row#*)} $detail"; nschema=$((nschema + 1)) ;;
        esac
    done <"$OUT"
    nrows=0
    while IFS= read -r line; do
        case "$line" in
            "PASS   : tst_Policy::decide("*)
                id="${line#*decide(}"; id="${id%%)*}"; pass "case $id"; nrows=$((nrows + 1)) ;;
            "FAIL!  : tst_Policy::decide("*)
                id="${line#*decide(}"; id="${id%%)*}"
                detail=$(grep -F -A3 "$line" "$OUT" | sed -n '2,3p' | tr -s ' \n' ' ')
                fail "case $id:${line#*)} $detail"; FAILED_CASES="$FAILED_CASES $id "; nrows=$((nrows + 1)) ;;
        esac
    done <"$OUT"
    if [ "$nschema" -ge 11 ]; then pass "loader rows run: $nschema"; else fail "loader rows run: $nschema (need at least 11)"; fi
    if [ "$nrows" -eq "${ncases%.*}" ]; then pass "case rows run: $nrows of $ncases"; else fail "case rows run: $nrows of $ncases"; fi
else
    fail "driver: not run (the build failed)"
fi

if [ -n "$PLANT" ]; then
    missed=""
    for id in $targeted; do
        [[ "$FAILED_CASES" == *" $id "* ]] || missed="$missed $id"
    done
    if [ -n "$FAILED_CASES" ] && [ -z "$missed" ]; then
        echo "PLANTED FAULT $PLANT CAUGHT by:$FAILED_CASES"
    else
        echo "PLANTED FAULT $PLANT NOT CAUGHT (targeted cases still passing:${missed:- none failed at all})"
    fi
fi

if [ "$FAILS" -eq 0 ]; then echo "PASS"; exit 0; else echo "FAIL ($FAILS checks failed)"; exit 1; fi
