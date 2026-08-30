#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cfr/blackjack.h"
#include "cfr/evaluation.h"
#include "cfr/info_store.h"
#include "cfr/trainer.h"

typedef enum {
    CLI_PARSE_READY,
    CLI_PARSE_HELP,
    CLI_PARSE_ERROR
} CliParseResult;

typedef enum {
    CLI_EXIT_SUCCESS = 0,
    CLI_EXIT_RUNTIME_ERROR = 1,
    CLI_EXIT_USAGE_ERROR = 2
} CliExitCode;

typedef struct {
    size_t iterations;
    size_t report_every;
    bool evaluate;
} CliOptions;

static bool print_usage(FILE *stream, const char *program_name) {
    if (stream == NULL || program_name == NULL)
        return false;

    if (fprintf(stream,
                "Uso: %s --iterations N [--report-every N] [--evaluate]\n"
                "       %s --help\n"
                "\n"
                "Opciones:\n"
                "  --iterations N     Iteraciones de entrenamiento; N debe "
                "ser positivo.\n"
                "  --report-every N   Iteraciones entre informes; si se "
                "omite, solo se informa al final.\n"
                "  --evaluate         Evalúa el perfil medio después del "
                "entrenamiento.\n"
                "  --help, -h         Muestra esta ayuda.\n"
                "\n"
                "Aviso: cada recorrido enumera el árbol completo de "
                "blackjack. Empiece con --iterations 1.\n"
                "La evaluación también enumera el árbol y puede necesitar "
                "mucho tiempo y memoria.\n"
                "\n"
                "Códigos de salida:\n"
                "  0  Ejecución correcta o ayuda.\n"
                "  1  Fallo operativo, de biblioteca, reloj o escritura.\n"
                "  2  Argumentos inválidos.\n",
                program_name, program_name) < 0) {
        return false;
    }

    return fflush(stream) == 0 && !ferror(stream);
}

static bool parse_positive_size(const char *text, size_t *value_out) {
    const unsigned char *current;
    char *end;
    uintmax_t value;

    if (text == NULL || value_out == NULL || text[0] == '\0')
        return false;

    current = (const unsigned char *)text;
    while (*current != '\0') {
        if (*current < (unsigned char)'0' || *current > (unsigned char)'9')
            return false;
        current += 1;
    }

    errno = 0;
    end = NULL;
    value = strtoumax(text, &end, 10);
    if (errno == ERANGE || end == text || *end != '\0' || value == 0 ||
        value > SIZE_MAX) {
        return false;
    }

    *value_out = (size_t)value;
    return true;
}

static CliParseResult parse_options(int argc, char *const argv[],
                                    FILE *diagnostic, CliOptions *options_out) {
    CliOptions options = {0};
    bool iterations_seen = false;
    bool report_every_seen = false;
    bool evaluate_seen = false;
    int index;

    if (argv == NULL || diagnostic == NULL || options_out == NULL)
        return CLI_PARSE_ERROR;

    if (argc == 2 && argv[1] != NULL &&
        (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        return CLI_PARSE_HELP;
    }

    for (index = 1; index < argc; index += 1) {
        const char *argument = argv[index];

        if (argument == NULL) {
            (void)fprintf(diagnostic, "error: argumento nulo\n");
            return CLI_PARSE_ERROR;
        }

        if (strcmp(argument, "--iterations") == 0) {
            if (iterations_seen) {
                (void)fprintf(diagnostic,
                              "error: --iterations está repetida\n");
                return CLI_PARSE_ERROR;
            }
            if (index + 1 >= argc || argv[index + 1] == NULL) {
                (void)fprintf(diagnostic,
                              "error: falta el valor de --iterations\n");
                return CLI_PARSE_ERROR;
            }
            index += 1;
            if (!parse_positive_size(argv[index], &options.iterations)) {
                (void)fprintf(diagnostic,
                              "error: --iterations requiere un entero decimal "
                              "positivo representable\n");
                return CLI_PARSE_ERROR;
            }
            iterations_seen = true;
            continue;
        }

        if (strcmp(argument, "--report-every") == 0) {
            if (report_every_seen) {
                (void)fprintf(diagnostic,
                              "error: --report-every está repetida\n");
                return CLI_PARSE_ERROR;
            }
            if (index + 1 >= argc || argv[index + 1] == NULL) {
                (void)fprintf(diagnostic,
                              "error: falta el valor de --report-every\n");
                return CLI_PARSE_ERROR;
            }
            index += 1;
            if (!parse_positive_size(argv[index], &options.report_every)) {
                (void)fprintf(
                    diagnostic,
                    "error: --report-every requiere un entero decimal "
                    "positivo representable\n");
                return CLI_PARSE_ERROR;
            }
            report_every_seen = true;
            continue;
        }

        if (strcmp(argument, "--evaluate") == 0) {
            if (evaluate_seen) {
                (void)fprintf(diagnostic,
                              "error: --evaluate está repetida\n");
                return CLI_PARSE_ERROR;
            }
            options.evaluate = true;
            evaluate_seen = true;
            continue;
        }

        if (strcmp(argument, "--help") == 0 || strcmp(argument, "-h") == 0) {
            (void)fprintf(
                diagnostic,
                "error: la ayuda solo puede solicitarse sin otras opciones\n");
            return CLI_PARSE_ERROR;
        }

        (void)fprintf(diagnostic, "error: opción desconocida: %s\n", argument);
        return CLI_PARSE_ERROR;
    }

    if (!iterations_seen) {
        (void)fprintf(diagnostic,
                      "error: falta la opción obligatoria --iterations\n");
        return CLI_PARSE_ERROR;
    }

    if (!report_every_seen)
        options.report_every = options.iterations;

    *options_out = options;
    return CLI_PARSE_READY;
}

static const char *status_name(Status status) {
    switch (status) {
    case CFR_STATUS_SUCCESS:
        return "CFR_STATUS_SUCCESS";
    case CFR_STATUS_INVALID_ARGUMENT:
        return "CFR_STATUS_INVALID_ARGUMENT";
    case CFR_STATUS_ILLEGAL_ACTION:
        return "CFR_STATUS_ILLEGAL_ACTION";
    case CFR_STATUS_BUFFER_TOO_SMALL:
        return "CFR_STATUS_BUFFER_TOO_SMALL";
    case CFR_STATUS_NUMERIC_ERROR:
        return "CFR_STATUS_NUMERIC_ERROR";
    case CFR_STATUS_OUT_OF_MEMORY:
        return "CFR_STATUS_OUT_OF_MEMORY";
    case CFR_STATUS_NOT_FOUND:
        return "CFR_STATUS_NOT_FOUND";
    default:
        return "CFR_STATUS_UNKNOWN";
    }
}

static bool print_status_error(FILE *stream, const char *operation,
                               Status status) {
    if (stream == NULL || operation == NULL)
        return false;

    return fprintf(stream, "error: %s falló: %s\n", operation,
                   status_name(status)) >= 0;
}

static bool elapsed_seconds(const struct timespec *start,
                            const struct timespec *end, double *seconds_out) {
    double seconds;

    if (start == NULL || end == NULL || seconds_out == NULL ||
        start->tv_nsec < 0 || start->tv_nsec >= 1000000000L ||
        end->tv_nsec < 0 || end->tv_nsec >= 1000000000L ||
        end->tv_sec < start->tv_sec ||
        (end->tv_sec == start->tv_sec && end->tv_nsec < start->tv_nsec)) {
        return false;
    }

    seconds = difftime(end->tv_sec, start->tv_sec) +
              ((double)end->tv_nsec - (double)start->tv_nsec) / 1000000000.0;
    if (!isfinite(seconds) || seconds < 0.0)
        return false;

    *seconds_out = seconds;
    return true;
}

static bool print_training_report(FILE *stream, const TrainerStats *trainer,
                                  const InfoStoreStats *store,
                                  double seconds) {
    if (stream == NULL || trainer == NULL || store == NULL ||
        !isfinite(seconds) || seconds < 0.0) {
        return false;
    }

    if (fprintf(stream,
                "informe iteraciones=%zu recorridos=%zu "
                "nodos_visitados=%zu conjuntos_informacion=%zu "
                "segundos=%.6f\n",
                trainer->iterations, trainer->traversals,
                trainer->visited_nodes, store->size, seconds) < 0) {
        return false;
    }

    return fflush(stream) == 0 && !ferror(stream);
}

static bool print_start(FILE *stream, const CliOptions *options) {
    if (stream == NULL || options == NULL)
        return false;

    if (fprintf(stream,
                "inicio juego=blackjack iteraciones_solicitadas=%zu "
                "informe_cada=%zu evaluacion=%s\n",
                options->iterations, options->report_every,
                options->evaluate ? "si" : "no") < 0) {
        return false;
    }

    return fflush(stream) == 0 && !ferror(stream);
}

static bool print_evaluation(FILE *stream, const EvaluationMetrics *metrics,
                             double seconds) {
    if (stream == NULL || metrics == NULL || !isfinite(seconds) ||
        seconds < 0.0) {
        return false;
    }

    if (fprintf(stream,
                "evaluacion valor_medio_jugador_0=%.17g "
                "valor_medio_banca=%.17g explotabilidad=%.17g "
                "segundos=%.6f\n",
                metrics->profile_value_player_0,
                metrics->profile_value_player_1, metrics->exploitability,
                seconds) < 0) {
        return false;
    }

    return fflush(stream) == 0 && !ferror(stream);
}

static int run_training(const CliOptions *options, FILE *output,
                        FILE *diagnostic) {
    BlackjackState state = {0};
    InfoStore store = {0};
    Trainer trainer = {0};
    const Game *game;
    GameState *game_state;
    struct timespec start;
    size_t completed = 0;
    bool store_initialized = false;
    int result = CLI_EXIT_RUNTIME_ERROR;
    Status status;

    if (options == NULL || output == NULL || diagnostic == NULL)
        return CLI_EXIT_RUNTIME_ERROR;

    status = cfr_blackjack_state_init(&state);
    if (status != CFR_STATUS_SUCCESS) {
        (void)print_status_error(diagnostic,
                                 "inicializar el estado de blackjack", status);
        goto cleanup;
    }

    game = cfr_blackjack_descriptor();
    game_state = cfr_blackjack_state_as_game_state(&state);
    if (game == NULL || game_state == NULL) {
        (void)fprintf(diagnostic,
                      "error: el adaptador de blackjack no está disponible\n");
        goto cleanup;
    }

    status = cfr_info_store_init(&store);
    if (status != CFR_STATUS_SUCCESS) {
        (void)print_status_error(diagnostic, "inicializar el almacén", status);
        goto cleanup;
    }
    store_initialized = true;

    status = cfr_trainer_init(&trainer, game, game_state, &store);
    if (status != CFR_STATUS_SUCCESS) {
        (void)print_status_error(diagnostic, "inicializar el entrenador",
                                 status);
        goto cleanup;
    }

    if (timespec_get(&start, TIME_UTC) != TIME_UTC) {
        (void)fprintf(diagnostic,
                      "error: no se pudo obtener la marca de tiempo inicial\n");
        goto cleanup;
    }
    if (!print_start(output, options)) {
        (void)fprintf(diagnostic,
                      "error: no se pudo escribir el inicio\n");
        goto cleanup;
    }

    while (completed < options->iterations) {
        const size_t remaining = options->iterations - completed;
        const size_t block = options->report_every < remaining
                                 ? options->report_every
                                 : remaining;
        TrainerStats trainer_stats;
        InfoStoreStats store_stats;
        struct timespec current;
        double seconds;

        status = cfr_trainer_run(&trainer, block);
        if (status != CFR_STATUS_SUCCESS) {
            (void)print_status_error(diagnostic, "entrenar", status);
            goto cleanup;
        }
        completed += block;

        status = cfr_trainer_get_stats(&trainer, &trainer_stats);
        if (status != CFR_STATUS_SUCCESS) {
            (void)print_status_error(diagnostic,
                                     "consultar el entrenador", status);
            goto cleanup;
        }
        status = cfr_info_store_get_stats(&store, &store_stats);
        if (status != CFR_STATUS_SUCCESS) {
            (void)print_status_error(diagnostic, "consultar el almacén",
                                     status);
            goto cleanup;
        }

        if (timespec_get(&current, TIME_UTC) != TIME_UTC ||
            !elapsed_seconds(&start, &current, &seconds)) {
            (void)fprintf(
                diagnostic,
                "error: no se pudo calcular el tiempo transcurrido\n");
            goto cleanup;
        }
        if (!print_training_report(output, &trainer_stats, &store_stats,
                                   seconds)) {
            (void)fprintf(diagnostic,
                          "error: no se pudo escribir el informe\n");
            goto cleanup;
        }
    }

    if (options->evaluate) {
        EvaluationMetrics metrics;
        struct timespec current;
        double seconds;

        status = cfr_evaluation_metrics(game, game_state, &store, &metrics);
        if (status != CFR_STATUS_SUCCESS) {
            (void)print_status_error(diagnostic, "evaluar el perfil medio",
                                     status);
            goto cleanup;
        }
        if (timespec_get(&current, TIME_UTC) != TIME_UTC ||
            !elapsed_seconds(&start, &current, &seconds)) {
            (void)fprintf(
                diagnostic,
                "error: no se pudo calcular el tiempo transcurrido\n");
            goto cleanup;
        }
        if (!print_evaluation(output, &metrics, seconds)) {
            (void)fprintf(diagnostic,
                          "error: no se pudo escribir la evaluación\n");
            goto cleanup;
        }
    }

    result = CLI_EXIT_SUCCESS;

cleanup:
    if (store_initialized) {
        status = cfr_info_store_destroy(&store);
        if (status != CFR_STATUS_SUCCESS) {
            (void)print_status_error(diagnostic, "destruir el almacén", status);
            result = CLI_EXIT_RUNTIME_ERROR;
        }
    }
    if (fflush(diagnostic) != 0 || ferror(diagnostic))
        result = CLI_EXIT_RUNTIME_ERROR;
    return result;
}

int main(int argc, char *argv[]) {
    CliOptions options;
    CliParseResult parse_result;
    const char *program_name = "cfr-blackjack";

    if (argc > 0 && argv != NULL && argv[0] != NULL && argv[0][0] != '\0')
        program_name = argv[0];

    parse_result = parse_options(argc, argv, stderr, &options);
    if (parse_result == CLI_PARSE_HELP) {
        return print_usage(stdout, program_name) ? CLI_EXIT_SUCCESS
                                                 : CLI_EXIT_RUNTIME_ERROR;
    }
    if (parse_result == CLI_PARSE_ERROR) {
        if (ferror(stderr) || !print_usage(stderr, program_name))
            return CLI_EXIT_RUNTIME_ERROR;
        return CLI_EXIT_USAGE_ERROR;
    }

    return run_training(&options, stdout, stderr);
}
