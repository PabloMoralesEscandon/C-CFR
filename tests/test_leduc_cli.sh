#!/bin/sh

set -eu

fail() {
    printf 'Leduc CLI integration test failure: %s\n' "$1" >&2
    exit 1
}

if [ "$#" -ne 1 ]; then
    fail "expected the executable path"
fi

cli_binary=$1
case "$cli_binary" in
/*) ;;
*) cli_binary=$PWD/$cli_binary ;;
esac

if [ ! -x "$cli_binary" ]; then
    fail "the executable does not exist or is not executable: $cli_binary"
fi

temporary_directory=$(mktemp -d "${TMPDIR:-/tmp}/cfr-leduc-cli.XXXXXX") ||
    fail "could not create the temporary directory"
trap 'rm -rf -- "$temporary_directory"' EXIT HUP INT TERM

run_case() {
    case_name=$1
    shift
    case_output=$temporary_directory/$case_name.out
    case_error=$temporary_directory/$case_name.err
    set +e
    "$cli_binary" "$@" >"$case_output" 2>"$case_error"
    case_status=$?
    set -e
}

require_status() {
    if [ "$case_status" -ne "$1" ]; then
        fail "$case_name exited with $case_status; expected $1"
    fi
}

require_empty() {
    if [ -s "$1" ]; then
        fail "$case_name wrote unexpected content to $1"
    fi
}

require_text() {
    if ! grep -F -- "$2" "$1" >/dev/null; then
        fail "$case_name does not contain expected text: $2"
    fi
}

run_case help --help
require_status 0
require_empty "$case_error"
require_text "$case_output" "Usage:"
require_text "$case_output" "--cfr-plus"
require_text "$case_output" "--mccfr"
require_text "$case_output" "--seed N"
require_text "$case_output" "--save FILE"
require_text "$case_output" "--load FILE"
require_text "$case_output" "--evaluate"
require_text "$case_output" "--export-strategy FILE"

run_case no_arguments
require_status 2
require_empty "$case_output"
require_text "$case_error" "missing required option --iterations"

run_case bad_iterations --iterations 0
require_status 2
require_empty "$case_output"
require_text "$case_error" "positive decimal integer"

run_case unknown --iterations 1 --unknown
require_status 2
require_empty "$case_output"
require_text "$case_error" "unknown option"

run_case one_iteration --iterations 1
require_status 0
require_empty "$case_error"
if ! awk '
    /^report / {
        if (NF != 6 || $2 != "iterations=1" ||
            $3 !~ /^average_value_player_0=/ ||
            $4 !~ /^exploitability=/ ||
            $5 != "information_sets=288" || $6 !~ /^seconds=/) exit 1
        count += 1
    }
    END { if (count != 1) exit 1 }
' "$case_output"; then
    fail "one_iteration did not write a valid report"
fi

run_case reports --iterations 7 --report-every 3
require_status 0
require_empty "$case_error"
if [ "$(sed -n 's/^report iterations=\([0-9]*\).*/\1/p' "$case_output" |
    tr '\n' ' ')" != "3 6 7 " ]; then
    fail "reports did not use the requested block boundaries"
fi

checkpoint=$temporary_directory/leduc.cfr
strategy_export=$temporary_directory/leduc.txt
run_case train_save_export --iterations 7 --cfr-plus --save "$checkpoint" \
    --export-strategy "$strategy_export"
require_status 0
require_empty "$case_error"
test -s "$checkpoint" || fail "train_save_export did not create a checkpoint"
test -s "$strategy_export" || fail "train_save_export did not create text"
require_text "$strategy_export" \
    "cfr-strategy version=1 schema=cfr.leduc-poker/v1 variant=cfr-plus"
require_text "$strategy_export" "training_iterations=7 information_sets=288"
if [ "$(grep -c '^infoset ' "$strategy_export")" -ne 288 ]; then
    fail "strategy export does not contain all Leduc information sets"
fi
if grep -E 'regret|strategy_sums' "$strategy_export" >/dev/null; then
    fail "strategy export contains resumable training accumulators"
fi

run_case evaluate --load "$checkpoint" --evaluate
require_status 0
require_empty "$case_error"
require_text "$case_output" "evaluation training_iterations=7 "
require_text "$case_output" " information_sets=288"
for field in profile_value_player_0 profile_value_player_1 \
    best_response_value_player_0 best_response_value_player_1 \
    improvement_player_0 improvement_player_1 nash_conv exploitability; do
    require_text "$case_output" "$field="
done

resumed=$temporary_directory/resumed.cfr
continuous=$temporary_directory/continuous.cfr
run_case resume --load "$checkpoint" --iterations 13 --save "$resumed"
require_status 0
require_empty "$case_error"
require_text "$case_output" "report iterations=20 "
run_case continuous --iterations 20 --cfr-plus --save "$continuous"
require_status 0
require_empty "$case_error"
if ! cmp -s "$resumed" "$continuous"; then
    fail "resumed CFR+ training differs from continuous training"
fi

run_case print_strategy --iterations 1 --print-strategy
require_status 0
require_empty "$case_error"
require_text "$case_output" \
    "strategy player_0_private_J_public_none_round_1_history_none check="
require_text "$case_output" "_public_J_round_2_history_"
if [ "$(grep -c '^strategy ' "$case_output")" -ne 288 ]; then
    fail "--print-strategy did not print all Leduc information sets"
fi
if ! awk '
    /^strategy / {
        sum = 0
        if (NF < 4 || NF > 5) exit 1
        for (field = 3; field <= NF; field++) {
            split($field, value, "=")
            if (value[2] + 0 < 0 || value[2] + 0 > 1 ||
                tolower(value[2]) ~ /nan|inf/) exit 1
            sum += value[2]
        }
        difference = sum - 1
        if (difference < 0) difference = -difference
        if (difference > 1e-12) exit 1
        count += 1
    }
    END { if (count != 288) exit 1 }
' "$case_output"; then
    fail "--print-strategy wrote invalid action probabilities"
fi

run_case refuse_existing_export --load "$checkpoint" --iterations 1 \
    --export-strategy "$strategy_export"
require_status 1
require_empty "$case_output"
require_text "$case_error" "already exists"
run_case checkpoint_survived --load "$checkpoint" --evaluate
require_status 0
require_empty "$case_error"
require_text "$case_output" "evaluation training_iterations=7 "

run_case convergence --iterations 300 --cfr-plus
require_status 0
require_empty "$case_error"
if ! awk '
    /^report / {
        split($3, value, "=")
        split($4, exploitability, "=")
        difference = value[2] - (-0.0856064)
        if (difference < 0) difference = -difference
        if (difference > 0.002 || exploitability[2] + 0 > 0.005) exit 1
        valid = 1
    }
    END { if (!valid) exit 1 }
' "$case_output"; then
    fail "CFR+ did not converge to the expected Leduc value"
fi

run_case mccfr --iterations 5000 --mccfr --seed 42
require_status 0
require_empty "$case_error"
require_text "$case_output" "report iterations=5000 "
require_text "$case_output" " information_sets=288 "

printf 'All Leduc CLI integration tests completed successfully.\n'
