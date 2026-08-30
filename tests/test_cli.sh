#!/bin/sh

set -eu

fail() {
    printf 'CLI integration test failure: %s\n' "$1" >&2
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

temporary_directory=$(mktemp -d "${TMPDIR:-/tmp}/cfr-cli-tests.XXXXXX") ||
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
    expected_status=$1
    if [ "$case_status" -ne "$expected_status" ]; then
        fail "$case_name exited with $case_status; expected $expected_status"
    fi
}

require_empty() {
    if [ -s "$1" ]; then
        fail "$case_name wrote unexpected content to $1"
    fi
}

require_text() {
    required_file=$1
    required_text=$2
    if ! grep -F -- "$required_text" "$required_file" >/dev/null; then
        fail "$case_name does not contain the expected text: $required_text"
    fi
}

check_usage_error() {
    usage_name=$1
    usage_message=$2
    shift 2

    run_case "$usage_name" "$@"
    require_status 2
    require_empty "$case_output"
    require_text "$case_error" "error:"
    require_text "$case_error" "$usage_message"
    require_text "$case_error" "Usage:"
    require_text "$case_error" "Invalid arguments."
}

validate_reports() {
    reports_file=$1
    expected_iterations=$2

    if ! awk '
        function is_decimal(text) {
            return text ~ /^[+-]?([0-9]+([.][0-9]*)?|[.][0-9]+)([eE][+-]?[0-9]+)?$/
        }

        /^report / {
            count += 1
            if (NF != 6 || $2 !~ /^iterations=/ ||
                $3 !~ /^average_value_player_0=/ ||
                $4 !~ /^exploitability=/ ||
                $5 !~ /^information_sets=/ ||
                $6 !~ /^seconds=/) {
                exit 1
            }
            iteration_parts = split($2, iterations, "=")
            value_parts = split($3, value, "=")
            exploitability_parts = split($4, exploitability, "=")
            information_set_parts = split($5, information_sets, "=")
            second_parts = split($6, seconds, "=")
            if (iteration_parts != 2 || value_parts != 2 ||
                exploitability_parts != 2 || information_set_parts != 2 ||
                second_parts != 2 || iterations[2] !~ /^[0-9]+$/ ||
                information_sets[2] !~ /^[0-9]+$/ ||
                !is_decimal(value[2]) || !is_decimal(exploitability[2]) ||
                !is_decimal(seconds[2]) || iterations[2] + 0 <= 0 ||
                information_sets[2] + 0 != 12 ||
                exploitability[2] + 0 < 0 || seconds[2] + 0 < 0 ||
                tolower(value[2]) ~ /nan|inf/ ||
                tolower(exploitability[2]) ~ /nan|inf/ ||
                tolower(seconds[2]) ~ /nan|inf/) {
                exit 1
            }
            sequence = sequence (count == 1 ? "" : " ") iterations[2]
        }
        END {
            if (count == 0)
                exit 1
            print sequence
        }
    ' "$reports_file" >"$temporary_directory/report-sequence"; then
        fail "$case_name contains an invalid report"
    fi

    actual_iterations=$(sed -n '1p' "$temporary_directory/report-sequence")
    if [ "$actual_iterations" != "$expected_iterations" ]; then
        sequence_error="$case_name reported at '$actual_iterations'"
        fail "$sequence_error; expected '$expected_iterations'"
    fi
}

validate_only_reports() {
    expected_iterations=$1

    require_status 0
    require_empty "$case_error"
    if grep -v '^report ' "$case_output" | grep -q .; then
        fail "$case_name wrote lines other than reports"
    fi
    validate_reports "$case_output" "$expected_iterations"
}

run_case long_help --help
require_status 0
require_empty "$case_error"
require_text "$case_output" "Usage:"
require_text "$case_output" "--iterations N"
require_text "$case_output" "--report-every N"
require_text "$case_output" "--print-strategy"
require_text "$case_output" "--cfr-plus"
require_text "$case_output" "--load FILE"
require_text "$case_output" "--save FILE"
require_text "$case_output" "--evaluate"
require_text "$case_output" "--export-strategy FILE"
require_text "$case_output" "0  Successful execution or help."
require_text "$case_output" \
    "1  Operation, library, clock, or write failure."
require_text "$case_output" "2  Invalid arguments."

run_case short_help -h
require_status 0
require_empty "$case_error"
if ! cmp -s "$temporary_directory/long_help.out" "$case_output"; then
    fail "--help and -h do not display the same help"
fi

check_usage_error no_arguments "missing required option --iterations"
check_usage_error unknown_option "unknown option" --unknown
check_usage_error iterations_without_value "missing value for --iterations" \
    --iterations
check_usage_error report_without_value "missing value for --report-every" \
    --iterations 1 --report-every
check_usage_error missing_iterations "missing required option --iterations" \
    --report-every 1
check_usage_error zero_iterations "representable positive decimal integer" \
    --iterations 0
check_usage_error zero_report "representable positive decimal integer" \
    --iterations 1 --report-every 0
check_usage_error negative_sign "representable positive decimal integer" \
    --iterations -1
check_usage_error positive_sign "representable positive decimal integer" \
    --iterations +1
check_usage_error invalid_suffix "representable positive decimal integer" \
    --iterations 100abc
check_usage_error out_of_range "representable positive decimal integer" \
    --iterations 9999999999999999999999999999999999999999
check_usage_error duplicate_iterations "--iterations was specified more than once" \
    --iterations 1 --iterations 2
check_usage_error duplicate_report "--report-every was specified more than once" \
    --iterations 1 --report-every 1 --report-every 1
check_usage_error duplicate_strategy "--print-strategy was specified more than once" \
    --iterations 1 --print-strategy --print-strategy
check_usage_error duplicate_cfr_plus "--cfr-plus was specified more than once" \
    --iterations 1 --cfr-plus --cfr-plus
check_usage_error load_without_value "missing value for --load" --load
check_usage_error save_without_value "missing value for --save" \
    --iterations 1 --save
check_usage_error export_without_value "missing value for --export-strategy" \
    --iterations 1 --export-strategy
check_usage_error duplicate_load "--load was specified more than once" \
    --load first --load second --iterations 1
check_usage_error duplicate_save "--save was specified more than once" \
    --iterations 1 --save first --save second
check_usage_error duplicate_export \
    "--export-strategy was specified more than once" \
    --iterations 1 --export-strategy first --export-strategy second
check_usage_error duplicate_evaluate "--evaluate was specified more than once" \
    --load model --evaluate --evaluate
check_usage_error evaluate_without_load "--evaluate requires --load FILE" \
    --evaluate
check_usage_error evaluate_with_iterations \
    "--evaluate cannot be combined with --iterations" \
    --load model --evaluate --iterations 1
check_usage_error evaluate_with_report \
    "--evaluate cannot be combined with --report-every" \
    --load model --evaluate --report-every 1
check_usage_error evaluate_with_plus \
    "--evaluate cannot be combined with --cfr-plus" \
    --load model --evaluate --cfr-plus
check_usage_error evaluate_with_save \
    "--evaluate cannot be combined with --save" \
    --load model --evaluate --save output
check_usage_error load_with_plus \
    "--cfr-plus cannot be combined with --load" \
    --load model --iterations 1 --cfr-plus
check_usage_error same_output_path \
    "--save and --export-strategy require different paths" \
    --iterations 1 --save output --export-strategy output
check_usage_error combined_help "help can only be requested" \
    --iterations 1 --help

run_case default_report --iterations 5
validate_only_reports "5"

run_case larger_interval --iterations 5 --report-every 10
validate_only_reports "5"

run_case equal_interval --iterations 5 --report-every 5
validate_only_reports "5"

run_case smaller_divisor_interval --iterations 6 --report-every 2
validate_only_reports "2 4 6"

run_case nondivisor_interval --iterations 5 --report-every 2
validate_only_reports "2 4 5"

run_case short_cfr_plus --iterations 5 --report-every 2 --cfr-plus
validate_only_reports "2 4 5"

checkpoint_file=$temporary_directory/strategy.cfr
strategy_export=$temporary_directory/strategy.txt
run_case checkpoint_training --iterations 7 --cfr-plus \
    --save "$checkpoint_file" --export-strategy "$strategy_export"
validate_only_reports "7"
if [ ! -s "$checkpoint_file" ]; then
    fail "checkpoint_training did not create a checkpoint"
fi
if [ ! -s "$strategy_export" ]; then
    fail "checkpoint_training did not create a strategy export"
fi
require_text "$strategy_export" \
    "cfr-strategy version=1 schema=cfr.kuhn-poker/v1 variant=cfr-plus"
require_text "$strategy_export" "training_iterations=7 information_sets=12"
if [ "$(grep -c '^infoset ' "$strategy_export")" -ne 12 ]; then
    fail "checkpoint_training did not export twelve information sets"
fi
if grep -E 'regret|strategy_sums' "$strategy_export" >/dev/null; then
    fail "checkpoint_training exported resumable accumulators as text"
fi

run_case checkpoint_evaluation --load "$checkpoint_file" --evaluate
require_status 0
require_empty "$case_error"
if ! awk '
    function is_decimal(text) {
        return text ~ /^[+-]?([0-9]+([.][0-9]*)?|[.][0-9]+)([eE][+-]?[0-9]+)?$/
    }
    /^evaluation / {
        if (NF != 11 || $2 != "training_iterations=7" ||
            $3 !~ /^profile_value_player_0=/ ||
            $4 !~ /^profile_value_player_1=/ ||
            $5 !~ /^best_response_value_player_0=/ ||
            $6 !~ /^best_response_value_player_1=/ ||
            $7 !~ /^improvement_player_0=/ ||
            $8 !~ /^improvement_player_1=/ || $9 !~ /^nash_conv=/ ||
            $10 !~ /^exploitability=/ || $11 != "information_sets=12") {
            exit 1
        }
        for (field = 3; field <= 10; field += 1) {
            split($field, value, "=")
            if (!is_decimal(value[2]) || tolower(value[2]) ~ /nan|inf/)
                exit 1
        }
        count += 1
    }
    END { if (count != 1) exit 1 }
' "$case_output"; then
    fail "checkpoint_evaluation did not print valid exact metrics"
fi

resumed_checkpoint=$temporary_directory/resumed.cfr
run_case checkpoint_resume --load "$checkpoint_file" --iterations 13 \
    --save "$resumed_checkpoint"
validate_only_reports "20"

continuous_checkpoint=$temporary_directory/continuous.cfr
run_case checkpoint_continuous --iterations 20 --cfr-plus \
    --save "$continuous_checkpoint"
validate_only_reports "20"
if ! cmp -s "$resumed_checkpoint" "$continuous_checkpoint"; then
    fail "resumed CFR+ training differs from uninterrupted training"
fi

run_case checkpoint_replace --load "$checkpoint_file" --iterations 1 \
    --save "$checkpoint_file"
validate_only_reports "8"
run_case replaced_evaluation --load "$checkpoint_file" --evaluate
require_status 0
require_empty "$case_error"
require_text "$case_output" "evaluation training_iterations=8 "

corrupt_checkpoint=$temporary_directory/corrupt.cfr
cp "$checkpoint_file" "$corrupt_checkpoint"
printf 'x' >>"$corrupt_checkpoint"
run_case corrupt_checkpoint --load "$corrupt_checkpoint" --evaluate
require_status 1
require_empty "$case_output"
require_text "$case_error" "CFR_STATUS_FORMAT_ERROR"

run_case missing_checkpoint --load "$temporary_directory/missing.cfr" --evaluate
require_status 1
require_empty "$case_output"
require_text "$case_error" "CFR_STATUS_IO_ERROR"

preserved_destination=$temporary_directory/preserved-destination
mkdir "$preserved_destination"
printf 'keep\n' >"$preserved_destination/marker"
run_case checkpoint_replace_failure --iterations 1 \
    --save "$preserved_destination"
require_status 1
require_text "$case_error" "CFR_STATUS_IO_ERROR"
require_text "$preserved_destination/marker" "keep"
if find "$temporary_directory" -maxdepth 1 \
    -name 'preserved-destination.tmp.*' | grep -q .; then
    fail "checkpoint_replace_failure left a temporary checkpoint"
fi

if [ -c /dev/full ] && [ -w /dev/full ]; then
    case_name=write_failure
    case_error=$temporary_directory/$case_name.err
    set +e
    "$cli_binary" --iterations 1 >/dev/full 2>"$case_error"
    case_status=$?
    set -e
    require_status 1
    require_text "$case_error" "could not write the report"
fi

run_case first_strategy --iterations 37 --report-every 10 --print-strategy
require_status 0
require_empty "$case_error"
grep '^report ' "$case_output" >"$temporary_directory/strategy-reports"
validate_reports "$temporary_directory/strategy-reports" "10 20 30 37"

if ! awk '
    function is_decimal(text) {
        return text ~ /^[+-]?([0-9]+([.][0-9]*)?|[.][0-9]+)([eE][+-]?[0-9]+)?$/
    }

    /^strategy / {
        count += 1
        if (NF != 4) {
            exit 1
        }
        first_parts = split($3, first, "=")
        second_parts = split($4, second, "=")
        if (first_parts != 2 || second_parts != 2 ||
            !is_decimal(first[2]) || !is_decimal(second[2])) {
            exit 1
        }
        if (count <= 6) {
            if (first[1] != "check" || second[1] != "bet")
                exit 1
        } else if (first[1] != "fold" || second[1] != "call") {
            exit 1
        }
        if (first[2] + 0 < 0 || first[2] + 0 > 1 ||
            second[2] + 0 < 0 || second[2] + 0 > 1 ||
            tolower(first[2]) ~ /nan|inf/ ||
            tolower(second[2]) ~ /nan|inf/) {
            exit 1
        }
        difference = first[2] + second[2] - 1
        if (difference < 0)
            difference = -difference
        if (difference > 1e-12)
            exit 1
        print $2
    }
    END {
        if (count != 12)
            exit 1
    }
' "$case_output" >"$temporary_directory/strategy-labels"; then
    fail "the final strategy does not contain twelve valid rows"
fi

cat >"$temporary_directory/expected-labels" <<'EOF'
player_0_open_card_J
player_0_open_card_Q
player_0_open_card_K
player_1_after_check_card_J
player_1_after_check_card_Q
player_1_after_check_card_K
player_1_facing_open_bet_card_J
player_1_facing_open_bet_card_Q
player_1_facing_open_bet_card_K
player_0_facing_check_bet_card_J
player_0_facing_check_bet_card_Q
player_0_facing_check_bet_card_K
EOF

if ! cmp -s "$temporary_directory/expected-labels" \
    "$temporary_directory/strategy-labels"; then
    fail "the strategy rows do not retain the expected order"
fi

first_strategy_output=$case_output
run_case second_strategy --iterations 37 --report-every 10 --print-strategy
require_status 0
require_empty "$case_error"

sed 's/ seconds=[^ ]*$/ seconds=<time>/' "$first_strategy_output" \
    >"$temporary_directory/strategy-first-normalized"
sed 's/ seconds=[^ ]*$/ seconds=<time>/' "$case_output" \
    >"$temporary_directory/strategy-second-normalized"
if ! cmp -s "$temporary_directory/strategy-first-normalized" \
    "$temporary_directory/strategy-second-normalized"; then
    fail "identical runs differ outside the time field"
fi

run_case long_validation --iterations 100000 --report-every 100000
validate_only_reports "100000"
if ! awk '
    /^report / {
        split($3, value, "=")
        split($4, exploitability, "=")
        difference = value[2] - (-1 / 18)
        if (difference < 0)
            difference = -difference
        if (difference > 0.0001 || exploitability[2] + 0 > 0.01)
            exit 1
        valid = 1
    }
    END {
        if (!valid)
            exit 1
    }
' "$case_output"; then
    fail "the long run does not meet the documented thresholds"
fi

printf 'All CLI integration tests completed successfully.\n'
