#!/bin/sh

set -eu

fail() {
    printf 'Fallo en la prueba de la CLI de blackjack: %s\n' "$1" >&2
    exit 1
}

if [ "$#" -ne 1 ]; then
    fail "se esperaba la ruta del ejecutable"
fi

cli_binary=$1
case "$cli_binary" in
/*) ;;
*) cli_binary=$PWD/$cli_binary ;;
esac

if [ ! -x "$cli_binary" ]; then
    fail "el ejecutable no existe o no tiene permiso de ejecución: $cli_binary"
fi

temporary_directory=$(mktemp -d "${TMPDIR:-/tmp}/cfr-blackjack-cli.XXXXXX") ||
    fail "no se pudo crear el directorio temporal"
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
        fail "$case_name terminó con $case_status; se esperaba $expected_status"
    fi
}

require_empty() {
    if [ -s "$1" ]; then
        fail "$case_name escribió contenido inesperado en $1"
    fi
}

require_text() {
    required_file=$1
    required_text=$2
    if ! grep -F -- "$required_text" "$required_file" >/dev/null; then
        fail "$case_name no contiene el texto esperado: $required_text"
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
    require_text "$case_error" "Uso:"
    require_text "$case_error" "Argumentos inválidos."
}

run_case ayuda_larga --help
require_status 0
require_empty "$case_error"
require_text "$case_output" "Uso:"
require_text "$case_output" "--iterations N"
require_text "$case_output" "--report-every N"
require_text "$case_output" "--evaluate"
require_text "$case_output" "Empiece con --iterations 1."
require_text "$case_output" "0  Ejecución correcta o ayuda."
require_text "$case_output" \
    "1  Fallo operativo, de biblioteca, reloj o escritura."
require_text "$case_output" "2  Argumentos inválidos."

run_case ayuda_corta -h
require_status 0
require_empty "$case_error"
if ! cmp -s "$temporary_directory/ayuda_larga.out" "$case_output"; then
    fail "--help y -h no muestran la misma ayuda"
fi

check_usage_error sin_argumentos "falta la opción obligatoria --iterations"
check_usage_error opcion_desconocida "opción desconocida" --desconocida
check_usage_error iteraciones_sin_valor "falta el valor de --iterations" \
    --iterations
check_usage_error informe_sin_valor "falta el valor de --report-every" \
    --iterations 1 --report-every
check_usage_error falta_iteraciones "falta la opción obligatoria --iterations" \
    --report-every 1
check_usage_error iteraciones_cero "entero decimal positivo representable" \
    --iterations 0
check_usage_error informe_cero "entero decimal positivo representable" \
    --iterations 1 --report-every 0
check_usage_error signo_negativo "entero decimal positivo representable" \
    --iterations -1
check_usage_error signo_positivo "entero decimal positivo representable" \
    --iterations +1
check_usage_error sufijo_invalido "entero decimal positivo representable" \
    --iterations 1abc
check_usage_error fuera_de_rango "entero decimal positivo representable" \
    --iterations 9999999999999999999999999999999999999999
check_usage_error iteraciones_repetidas "--iterations está repetida" \
    --iterations 1 --iterations 2
check_usage_error informe_repetido "--report-every está repetida" \
    --iterations 1 --report-every 1 --report-every 1
check_usage_error evaluacion_repetida "--evaluate está repetida" \
    --iterations 1 --evaluate --evaluate
check_usage_error ayuda_combinada "la ayuda solo puede solicitarse" \
    --iterations 1 --help

if [ -c /dev/full ] && [ -w /dev/full ]; then
    case_name=fallo_de_escritura_inicial
    case_error=$temporary_directory/$case_name.err
    set +e
    "$cli_binary" --iterations 1 >/dev/full 2>"$case_error"
    case_status=$?
    set -e
    require_status 1
    require_text "$case_error" "no se pudo escribir el inicio"
fi

printf 'Todas las pruebas de la CLI de blackjack terminaron correctamente.\n'
