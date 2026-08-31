#!/bin/sh

set -eu

fail() {
    printf 'Blackjack CLI test failure: %s\n' "$1" >&2
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

temporary_directory=$(mktemp -d "${TMPDIR:-/tmp}/cfr-blackjack-cli.XXXXXX") ||
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
        fail "$case_name does not contain expected text: $required_text"
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

run_case long_help --help
require_status 0
require_empty "$case_error"
require_text "$case_output" "Usage:"
require_text "$case_output" "--iterations N"
require_text "$case_output" "--report-every N"
require_text "$case_output" "--evaluate"
require_text "$case_output" "--cfr-plus"
require_text "$case_output" "--load FILE"
require_text "$case_output" "--save FILE"
require_text "$case_output" "--export-strategy FILE"
require_text "$case_output" "--deal R,R,R"
require_text "$case_output" "--full-tree"
require_text "$case_output" "Exactly one of --deal and --full-tree is required."
require_text "$case_output" "Chance draws use fixed basic-strategy probabilities"
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
    --iterations 1abc
check_usage_error out_of_range "representable positive decimal integer" \
    --iterations 9999999999999999999999999999999999999999
check_usage_error duplicate_iterations \
    "--iterations was specified more than once" \
    --iterations 1 --iterations 2
check_usage_error duplicate_report \
    "--report-every was specified more than once" \
    --iterations 1 --report-every 1 --report-every 1
check_usage_error duplicate_evaluation \
    "--evaluate was specified more than once" \
    --iterations 1 --evaluate --evaluate
check_usage_error duplicate_cfr_plus \
    "--cfr-plus was specified more than once" \
    --iterations 1 --cfr-plus --cfr-plus
check_usage_error load_without_value "missing value for --load" \
    --iterations 1 --load
check_usage_error save_without_value "missing value for --save" \
    --iterations 1 --save
check_usage_error export_without_value "missing value for --export-strategy" \
    --iterations 1 --export-strategy
check_usage_error duplicate_load "--load was specified more than once" \
    --iterations 1 --load first --load second
check_usage_error duplicate_save "--save was specified more than once" \
    --iterations 1 --save first --save second
check_usage_error duplicate_export \
    "--export-strategy was specified more than once" \
    --iterations 1 --export-strategy first --export-strategy second
check_usage_error load_with_cfr_plus \
    "--cfr-plus cannot be combined with --load" \
    --load checkpoint --iterations 1 --cfr-plus
check_usage_error overlapping_outputs \
    "--save and --export-strategy require different paths" \
    --iterations 1 --save output --export-strategy output
check_usage_error combined_help "help can only be requested" \
    --iterations 1 --help

# The caller must choose a visible deal or the root before the initial draw.
check_usage_error missing_scope "missing required option --deal RANKS" \
    --iterations 1
check_usage_error scope_conflict \
    "--deal cannot be combined with --full-tree" \
    --iterations 1 --deal 5,10,6 --full-tree
check_usage_error duplicate_deal "--deal was specified more than once" \
    --iterations 1 --deal 5,10,6 --deal 5,10,6
check_usage_error duplicate_full_tree \
    "--full-tree was specified more than once" \
    --iterations 1 --full-tree --full-tree
check_usage_error deal_without_value "missing value for --deal" \
    --iterations 1 --deal
check_usage_error deal_too_short "three comma-separated ranks from 1 to 10" \
    --iterations 1 --deal 5,10
check_usage_error deal_too_long "three comma-separated ranks from 1 to 10" \
    --iterations 1 --deal 5,10,6,10
check_usage_error deal_zero_rank "three comma-separated ranks from 1 to 10" \
    --iterations 1 --deal 0,10,6
check_usage_error deal_high_rank "three comma-separated ranks from 1 to 10" \
    --iterations 1 --deal 11,10,6
check_usage_error deal_negative_rank \
    "three comma-separated ranks from 1 to 10" \
    --iterations 1 --deal -5,10,6
check_usage_error deal_padded "three comma-separated ranks from 1 to 10" \
    --iterations 1 --deal " 5,10,6"
check_usage_error deal_separator "three comma-separated ranks from 1 to 10" \
    --iterations 1 --deal 5.10.6
check_usage_error deal_empty "three comma-separated ranks from 1 to 10" \
    --iterations 1 --deal ""

# A dealt hand completes, which is the workflow the executable supports.
run_case deal_training --deal 5,10,6 --iterations 4 --report-every 2 \
    --evaluate
require_status 0
require_empty "$case_error"
require_text "$case_output" \
    "start game=blackjack scope=deal:5,10,6 requested_iterations=4"
if ! awk '
    function is_decimal(text) {
        return text ~ /^[+-]?([0-9]+([.][0-9]*)?|[.][0-9]+)([eE][+-]?[0-9]+)?$/
    }
    /^report / {
        reports += 1
        if (NF != 6 || $2 != "iterations=" reports * 2 ||
            $3 != "traversals=" reports * 2 || $4 !~ /^visited_nodes=[0-9]+$/ ||
            $5 !~ /^information_sets=[0-9]+$/ || $6 !~ /^seconds=/) {
            exit 1
        }
        split($4, nodes, "=")
        split($5, sets, "=")
        if (nodes[2] + 0 == 0 || sets[2] + 0 == 0)
            exit 1
    }
    /^evaluation / {
        evaluations += 1
        if (NF != 5)
            exit 1
        for (field = 2; field <= 4; field += 1) {
            split($field, value, "=")
            if (!is_decimal(value[2]) || tolower(value[2]) ~ /nan|inf/)
                exit 1
        }
    }
    END { if (reports != 2 || evaluations != 1) exit 1 }
' "$case_output"; then
    fail "deal_training did not complete a valid training and evaluation run"
fi

run_case deal_training_cfr_plus --deal 10,6,9 --iterations 2 --cfr-plus
require_status 0
require_empty "$case_error"
require_text "$case_output" "variant=cfr+"
require_text "$case_output" "iterations=2 traversals=2"

# A natural blackjack becomes terminal after the hidden hole-card chance node.
# Visiting the root plus all ten available ranks proves that --deal did not
# condition training on one unobservable hole card.
run_case deal_natural --deal 1,10,10 --iterations 1
require_status 0
require_empty "$case_error"
require_text "$case_output" "visited_nodes=11 information_sets=0"

run_case missing_checkpoint --deal 5,10,6 \
    --load "$temporary_directory/missing.cfr" --iterations 1
require_status 1
require_empty "$case_output"
require_text "$case_error" "load the checkpoint failed: CFR_STATUS_IO_ERROR"

checkpoint_path=$temporary_directory/model.cfr
strategy_path=$temporary_directory/strategy.txt
run_case checkpoint_save --deal 10,6,9 --iterations 2 \
    --save "$checkpoint_path" --export-strategy "$strategy_path"
require_status 0
require_empty "$case_error"
if [ ! -s "$checkpoint_path" ] || [ ! -s "$strategy_path" ]; then
    fail "checkpoint_save did not create both output files"
fi

run_case checkpoint_resume --deal 10,6,9 --load "$checkpoint_path" \
    --iterations 1
require_status 0
require_empty "$case_error"
require_text "$case_output" \
    "scope=deal:10,6,9 requested_iterations=1 starting_iterations=2"
require_text "$case_output" "iterations=3 traversals=3"

if [ -c /dev/full ] && [ -w /dev/full ]; then
    case_name=initial_write_failure
    case_error=$temporary_directory/$case_name.err
    set +e
    "$cli_binary" --deal 5,10,6 --iterations 1 >/dev/full 2>"$case_error"
    case_status=$?
    set -e
    require_status 1
    require_text "$case_error" "could not write the start report"

    case_name=initial_cfr_plus_write_failure
    case_error=$temporary_directory/$case_name.err
    set +e
    "$cli_binary" --deal 5,10,6 --iterations 1 --cfr-plus >/dev/full \
        2>"$case_error"
    case_status=$?
    set -e
    require_status 1
    require_text "$case_error" "could not write the start report"

    # --full-tree must reach the start report; its expensive traversal is
    # deliberately not run in the CLI argument test.
    case_name=full_tree_write_failure
    case_error=$temporary_directory/$case_name.err
    set +e
    "$cli_binary" --full-tree --iterations 1 >/dev/full 2>"$case_error"
    case_status=$?
    set -e
    require_status 1
    require_text "$case_error" "could not write the start report"

    case_name=checkpoint_options_write_failure
    case_error=$temporary_directory/$case_name.err
    set +e
    "$cli_binary" --deal 5,10,6 --iterations 1 \
        --save "$temporary_directory/model.cfr" \
        --export-strategy "$temporary_directory/strategy.txt" \
        >/dev/full 2>"$case_error"
    case_status=$?
    set -e
    require_status 1
    require_text "$case_error" "could not write the start report"
fi

printf 'All blackjack CLI tests passed.\n'
