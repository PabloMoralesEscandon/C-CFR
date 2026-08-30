#!/bin/sh

set -eu

fail() {
    printf 'Fallo en la prueba integral de la CLI: %s\n' "$1" >&2
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

temporary_directory=$(mktemp -d "${TMPDIR:-/tmp}/cfr-cli-tests.XXXXXX") ||
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

validate_reports() {
    reports_file=$1
    expected_iterations=$2

    if ! awk '
        function is_decimal(text) {
            return text ~ /^[+-]?([0-9]+([.][0-9]*)?|[.][0-9]+)([eE][+-]?[0-9]+)?$/
        }

        /^informe / {
            count += 1
            if (NF != 6 || $2 !~ /^iteraciones=/ ||
                $3 !~ /^valor_medio_jugador_0=/ ||
                $4 !~ /^explotabilidad=/ ||
                $5 !~ /^conjuntos_informacion=/ ||
                $6 !~ /^segundos=/) {
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
        fail "$case_name contiene un informe inválido"
    fi

    actual_iterations=$(sed -n '1p' "$temporary_directory/report-sequence")
    if [ "$actual_iterations" != "$expected_iterations" ]; then
        sequence_error="$case_name informó en '$actual_iterations'"
        fail "$sequence_error; se esperaba '$expected_iterations'"
    fi
}

validate_only_reports() {
    expected_iterations=$1

    require_status 0
    require_empty "$case_error"
    if grep -v '^informe ' "$case_output" | grep -q .; then
        fail "$case_name escribió líneas ajenas al informe"
    fi
    validate_reports "$case_output" "$expected_iterations"
}

run_case ayuda_larga --help
require_status 0
require_empty "$case_error"
require_text "$case_output" "Uso:"
require_text "$case_output" "--iterations N"
require_text "$case_output" "--report-every N"
require_text "$case_output" "--print-strategy"
require_text "$case_output" "--cfr-plus"
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
    --iterations 100abc
check_usage_error fuera_de_rango "entero decimal positivo representable" \
    --iterations 9999999999999999999999999999999999999999
check_usage_error iteraciones_repetidas "--iterations está repetida" \
    --iterations 1 --iterations 2
check_usage_error informe_repetido "--report-every está repetida" \
    --iterations 1 --report-every 1 --report-every 1
check_usage_error estrategia_repetida "--print-strategy está repetida" \
    --iterations 1 --print-strategy --print-strategy
check_usage_error cfr_plus_repetida "--cfr-plus está repetida" \
    --iterations 1 --cfr-plus --cfr-plus
check_usage_error ayuda_combinada "la ayuda solo puede solicitarse" \
    --iterations 1 --help

run_case informe_predeterminado --iterations 5
validate_only_reports "5"

run_case frecuencia_mayor --iterations 5 --report-every 10
validate_only_reports "5"

run_case frecuencia_igual --iterations 5 --report-every 5
validate_only_reports "5"

run_case frecuencia_menor_divisora --iterations 6 --report-every 2
validate_only_reports "2 4 6"

run_case frecuencia_no_divisora --iterations 5 --report-every 2
validate_only_reports "2 4 5"

run_case cfr_plus_corto --iterations 5 --report-every 2 --cfr-plus
validate_only_reports "2 4 5"

if [ -c /dev/full ] && [ -w /dev/full ]; then
    case_name=fallo_de_escritura
    case_error=$temporary_directory/$case_name.err
    set +e
    "$cli_binary" --iterations 1 >/dev/full 2>"$case_error"
    case_status=$?
    set -e
    require_status 1
    require_text "$case_error" "no se pudo escribir el informe"
fi

run_case estrategia_primera --iterations 37 --report-every 10 --print-strategy
require_status 0
require_empty "$case_error"
grep '^informe ' "$case_output" >"$temporary_directory/estrategia-informes"
validate_reports "$temporary_directory/estrategia-informes" "10 20 30 37"

if ! awk '
    function is_decimal(text) {
        return text ~ /^[+-]?([0-9]+([.][0-9]*)?|[.][0-9]+)([eE][+-]?[0-9]+)?$/
    }

    /^estrategia / {
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
            if (first[1] != "pasar" || second[1] != "apostar")
                exit 1
        } else if (first[1] != "retirarse" || second[1] != "igualar") {
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
    fail "la estrategia final no contiene doce filas válidas"
fi

cat >"$temporary_directory/expected-labels" <<'EOF'
jugador_0_apertura_carta_J
jugador_0_apertura_carta_Q
jugador_0_apertura_carta_K
jugador_1_despues_de_pasar_carta_J
jugador_1_despues_de_pasar_carta_Q
jugador_1_despues_de_pasar_carta_K
jugador_1_ante_apuesta_inicial_carta_J
jugador_1_ante_apuesta_inicial_carta_Q
jugador_1_ante_apuesta_inicial_carta_K
jugador_0_ante_pasar_apostar_carta_J
jugador_0_ante_pasar_apostar_carta_Q
jugador_0_ante_pasar_apostar_carta_K
EOF

if ! cmp -s "$temporary_directory/expected-labels" \
    "$temporary_directory/strategy-labels"; then
    fail "las filas de estrategia no conservan el orden esperado"
fi

first_strategy_output=$case_output
run_case estrategia_segunda --iterations 37 --report-every 10 --print-strategy
require_status 0
require_empty "$case_error"

sed 's/ segundos=[^ ]*$/ segundos=<tiempo>/' "$first_strategy_output" \
    >"$temporary_directory/strategy-first-normalized"
sed 's/ segundos=[^ ]*$/ segundos=<tiempo>/' "$case_output" \
    >"$temporary_directory/strategy-second-normalized"
if ! cmp -s "$temporary_directory/strategy-first-normalized" \
    "$temporary_directory/strategy-second-normalized"; then
    fail "dos ejecuciones iguales difieren fuera del campo de tiempo"
fi

run_case validacion_larga --iterations 100000 --report-every 100000
validate_only_reports "100000"
if ! awk '
    /^informe / {
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
    fail "la ejecución larga no alcanza los umbrales documentados"
fi

printf 'Todas las pruebas integrales de la CLI terminaron correctamente.\n'
