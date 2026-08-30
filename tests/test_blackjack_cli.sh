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
require_text "$case_output" "Start with --iterations 1."
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
check_usage_error combined_help "help can only be requested" \
    --iterations 1 --help

if [ -c /dev/full ] && [ -w /dev/full ]; then
    case_name=initial_write_failure
    case_error=$temporary_directory/$case_name.err
    set +e
    "$cli_binary" --iterations 1 >/dev/full 2>"$case_error"
    case_status=$?
    set -e
    require_status 1
    require_text "$case_error" "could not write the start report"

    case_name=initial_cfr_plus_write_failure
    case_error=$temporary_directory/$case_name.err
    set +e
    "$cli_binary" --iterations 1 --cfr-plus >/dev/full 2>"$case_error"
    case_status=$?
    set -e
    require_status 1
    require_text "$case_error" "could not write the start report"
fi

printf 'All blackjack CLI tests passed.\n'
