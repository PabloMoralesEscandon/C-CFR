#!/bin/sh
#
# Integration test for the decision-tree exporter.
#
# The test trains short runs of both games, walks each tree, and checks the
# document against facts that hold independently of the exporter: the node and
# information-set counts implied by the rules, and the profile value that the
# library's own evaluator reports for the same checkpoint.

set -eu

fail() {
    printf 'poker-tree-export integration test failure: %s\n' "$1" >&2
    exit 1
}

if [ "$#" -ne 3 ]; then
    fail "expected the exporter, Kuhn CLI, and Leduc CLI paths"
fi

absolute() {
    case "$1" in
    /*) printf '%s\n' "$1" ;;
    *) printf '%s/%s\n' "$PWD" "$1" ;;
    esac
}

exporter=$(absolute "$1")
kuhn_cli=$(absolute "$2")
leduc_cli=$(absolute "$3")

for binary in "$exporter" "$kuhn_cli" "$leduc_cli"; do
    [ -x "$binary" ] || fail "not executable: $binary"
done

temporary_directory=$(mktemp -d "${TMPDIR:-/tmp}/cfr-tree-export.XXXXXX") ||
    fail "could not create the temporary directory"
trap 'rm -rf -- "$temporary_directory"' EXIT HUP INT TERM

case_status=0
case_name=""

run_case() {
    case_name=$1
    shift
    set +e
    "$exporter" "$@" >"$temporary_directory/out" 2>"$temporary_directory/err"
    case_status=$?
    set -e
}

require_status() {
    if [ "$case_status" -ne "$1" ]; then
        fail "$case_name exited with $case_status; expected $1"
    fi
}

# ---------------------------------------------------------- usage errors ---

run_case help --help
require_status 0
grep -q -- "--game NAME" "$temporary_directory/out" ||
    fail "help does not describe --game"

run_case no-game --output /dev/null
require_status 2

run_case bad-game --game omaha
require_status 2

run_case both-sources --game kuhn --load a --strategy b
require_status 2

run_case missing-checkpoint --game kuhn --load "$temporary_directory/absent"
require_status 1

# ------------------------------------------------------------ tree shape ---

# The Kuhn tree is small enough to state exactly: six deals, and under each a
# root decision leading to five betting sequences, of which two are terminal
# after one action pair.
run_case kuhn-uniform --game kuhn --output "$temporary_directory/kuhn-uniform.json"
require_status 0

check_field() {
    file=$1
    field=$2
    expected=$3
    actual=$(tr ',{}' '\n\n\n' <"$file" | sed -n "s/^\"$field\":\\(.*\\)$/\\1/p" |
        head -n 1)
    if [ "$actual" != "$expected" ]; then
        fail "$file: $field is $actual; expected $expected"
    fi
}

check_field "$temporary_directory/kuhn-uniform.json" nodes 55
check_field "$temporary_directory/kuhn-uniform.json" terminals 30
check_field "$temporary_directory/kuhn-uniform.json" playerNodes 24
check_field "$temporary_directory/kuhn-uniform.json" depth 4
check_field "$temporary_directory/kuhn-uniform.json" source '"uniform"'

# Without a strategy every information set falls back to uniform, so the
# document reports that it is not backed by trained data.
check_field "$temporary_directory/kuhn-uniform.json" complete false

# ------------------------------------------------ agreement with the library ---

# Compares the root expected utility written by the exporter with the profile
# value the library's evaluator reports for the same checkpoint.
compare_root_value() {
    document=$1
    reported=$2
    awk -v document="$document" -v reported="$reported" '
        BEGIN {
            while ((getline line < document) > 0) text = text line
            close(document)
            index_of = index(text, "\"stats\"")
            tail = substr(text, 1, index_of)
            position = 0
            while (match(tail, /"ev":-?[0-9.eE+-]+/)) {
                position = RSTART
                value = substr(tail, RSTART + 5, RLENGTH - 5)
                tail = substr(tail, RSTART + RLENGTH)
            }
            difference = value - reported
            if (difference < 0) difference = -difference
            if (difference > 1e-9) {
                printf "root ev %s differs from the evaluator value %s\n",
                    value, reported > "/dev/stderr"
                exit 1
            }
        }
    ' || fail "$document does not agree with the library evaluator"
}

"$kuhn_cli" --iterations 2000 --save "$temporary_directory/kuhn.bin" \
    >"$temporary_directory/kuhn-train.out" 2>&1 ||
    fail "could not train Kuhn Poker"
kuhn_value=$(sed -n 's/.*average_value_player_0=\([^ ]*\).*/\1/p' \
    "$temporary_directory/kuhn-train.out" | tail -n 1)
[ -n "$kuhn_value" ] || fail "the Kuhn CLI did not report a profile value"

run_case kuhn-checkpoint --game kuhn --load "$temporary_directory/kuhn.bin" \
    --output "$temporary_directory/kuhn.json"
require_status 0
check_field "$temporary_directory/kuhn.json" nodes 55
check_field "$temporary_directory/kuhn.json" complete true
check_field "$temporary_directory/kuhn.json" source '"checkpoint"'
compare_root_value "$temporary_directory/kuhn.json" "$kuhn_value"

# ------------------------------------------------- text export and Leduc ---

"$kuhn_cli" --load "$temporary_directory/kuhn.bin" --evaluate \
    --export-strategy "$temporary_directory/kuhn.txt" >/dev/null 2>&1 ||
    fail "could not export the Kuhn strategy as text"

run_case kuhn-text --game kuhn --strategy "$temporary_directory/kuhn.txt" \
    --output "$temporary_directory/kuhn-text.json"
require_status 0
check_field "$temporary_directory/kuhn-text.json" source '"export"'
compare_root_value "$temporary_directory/kuhn-text.json" "$kuhn_value"

# A strategy from another game must be refused rather than silently ignored.
run_case wrong-schema --game leduc --strategy "$temporary_directory/kuhn.txt"
require_status 1
grep -q "does not match" "$temporary_directory/err" ||
    fail "the schema mismatch was not reported"

"$leduc_cli" --iterations 20 --save "$temporary_directory/leduc.bin" \
    >"$temporary_directory/leduc-train.out" 2>&1 ||
    fail "could not train Leduc Poker"
leduc_value=$(sed -n 's/.*average_value_player_0=\([^ ]*\).*/\1/p' \
    "$temporary_directory/leduc-train.out" | tail -n 1)
[ -n "$leduc_value" ] || fail "the Leduc CLI did not report a profile value"

run_case leduc-checkpoint --game leduc --load "$temporary_directory/leduc.bin" \
    --output "$temporary_directory/leduc.json"
require_status 0
check_field "$temporary_directory/leduc.json" nodes 1936
check_field "$temporary_directory/leduc.json" depth 10
check_field "$temporary_directory/leduc.json" complete true
compare_root_value "$temporary_directory/leduc.json" "$leduc_value"

# ------------------------------------------------------------- the page ---

viewer=$(dirname -- "$0")/poker_tree_view.html
[ -f "$viewer" ] || fail "the viewer page is missing: $viewer"
grep -q "__CFR_TREE_DATA__" "$viewer" ||
    fail "the viewer page lost its data placeholder"

printf 'poker-tree-export integration test passed\n'
