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
#include "cfr/checkpoint.h"
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
    bool cfr_plus;
    const char *load_path;
    const char *save_path;
    const char *export_path;
} CliOptions;

static bool print_usage(FILE *stream, const char *program_name) {
    if (stream == NULL || program_name == NULL)
        return false;

    if (fprintf(stream,
                "Usage: %s --iterations N [--report-every N] [--evaluate] "
                "[--cfr-plus] [--save FILE] [--export-strategy FILE]\n"
                "       %s --load FILE --iterations N [--report-every N] "
                "[--evaluate] [--save FILE] [--export-strategy FILE]\n"
                "       %s --help\n"
                "\n"
                "Options:\n"
                "  --iterations N     Training iterations; N must be "
                "positive.\n"
                "  --report-every N   Iterations between reports; if omitted, "
                "only the final report is printed.\n"
                "  --evaluate         Evaluate the average profile after "
                "training.\n"
                "  --cfr-plus         Use CFR+ with Regret Matching+ and "
                "linear averaging.\n"
                "  --load FILE        Load a binary checkpoint before "
                "training.\n"
                "  --save FILE        Save a binary checkpoint after "
                "training.\n"
                "  --export-strategy FILE\n"
                "                     Export the average strategy as text.\n"
                "  --help, -h         Display this help.\n"
                "\n"
                "Warning: each traversal enumerates the complete blackjack "
                "tree. Start with --iterations 1.\n"
                "Evaluation also enumerates the tree and can require "
                "considerable time and memory.\n"
                "\n"
                "Exit codes:\n"
                "  0  Successful execution or help.\n"
                "  1  Operation, library, clock, or write failure.\n"
                "  2  Invalid arguments.\n",
                program_name, program_name, program_name) < 0) {
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
    bool cfr_plus_seen = false;
    bool load_seen = false;
    bool save_seen = false;
    bool export_seen = false;
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
            (void)fprintf(diagnostic, "error: null argument\n");
            return CLI_PARSE_ERROR;
        }

        if (strcmp(argument, "--iterations") == 0) {
            if (iterations_seen) {
                (void)fprintf(
                    diagnostic,
                    "error: --iterations was specified more than once\n");
                return CLI_PARSE_ERROR;
            }
            if (index + 1 >= argc || argv[index + 1] == NULL) {
                (void)fprintf(diagnostic,
                              "error: missing value for --iterations\n");
                return CLI_PARSE_ERROR;
            }
            index += 1;
            if (!parse_positive_size(argv[index], &options.iterations)) {
                (void)fprintf(diagnostic,
                              "error: --iterations requires a representable "
                              "positive decimal integer\n");
                return CLI_PARSE_ERROR;
            }
            iterations_seen = true;
            continue;
        }

        if (strcmp(argument, "--report-every") == 0) {
            if (report_every_seen) {
                (void)fprintf(
                    diagnostic,
                    "error: --report-every was specified more than once\n");
                return CLI_PARSE_ERROR;
            }
            if (index + 1 >= argc || argv[index + 1] == NULL) {
                (void)fprintf(diagnostic,
                              "error: missing value for --report-every\n");
                return CLI_PARSE_ERROR;
            }
            index += 1;
            if (!parse_positive_size(argv[index], &options.report_every)) {
                (void)fprintf(
                    diagnostic,
                    "error: --report-every requires a representable positive "
                    "decimal integer\n");
                return CLI_PARSE_ERROR;
            }
            report_every_seen = true;
            continue;
        }

        if (strcmp(argument, "--evaluate") == 0) {
            if (evaluate_seen) {
                (void)fprintf(
                    diagnostic,
                    "error: --evaluate was specified more than once\n");
                return CLI_PARSE_ERROR;
            }
            options.evaluate = true;
            evaluate_seen = true;
            continue;
        }

        if (strcmp(argument, "--cfr-plus") == 0) {
            if (cfr_plus_seen) {
                (void)fprintf(
                    diagnostic,
                    "error: --cfr-plus was specified more than once\n");
                return CLI_PARSE_ERROR;
            }
            options.cfr_plus = true;
            cfr_plus_seen = true;
            continue;
        }

        if (strcmp(argument, "--load") == 0 ||
            strcmp(argument, "--save") == 0 ||
            strcmp(argument, "--export-strategy") == 0) {
            bool *seen;
            const char **path;

            if (strcmp(argument, "--load") == 0) {
                seen = &load_seen;
                path = &options.load_path;
            } else if (strcmp(argument, "--save") == 0) {
                seen = &save_seen;
                path = &options.save_path;
            } else {
                seen = &export_seen;
                path = &options.export_path;
            }
            if (*seen) {
                (void)fprintf(diagnostic,
                              "error: %s was specified more than once\n",
                              argument);
                return CLI_PARSE_ERROR;
            }
            if (index + 1 >= argc || argv[index + 1] == NULL) {
                (void)fprintf(diagnostic, "error: missing value for %s\n",
                              argument);
                return CLI_PARSE_ERROR;
            }
            index += 1;
            if (argv[index][0] == '\0') {
                (void)fprintf(diagnostic,
                              "error: %s requires a nonempty path\n",
                              argument);
                return CLI_PARSE_ERROR;
            }
            *seen = true;
            *path = argv[index];
            continue;
        }

        if (strcmp(argument, "--help") == 0 || strcmp(argument, "-h") == 0) {
            (void)fprintf(
                diagnostic,
                "error: help can only be requested without other options\n");
            return CLI_PARSE_ERROR;
        }

        (void)fprintf(diagnostic, "error: unknown option: %s\n", argument);
        return CLI_PARSE_ERROR;
    }

    if (load_seen && cfr_plus_seen) {
        (void)fprintf(diagnostic,
                      "error: --cfr-plus cannot be combined with --load; "
                      "the checkpoint selects the variant\n");
        return CLI_PARSE_ERROR;
    }
    if (save_seen && export_seen &&
        strcmp(options.save_path, options.export_path) == 0) {
        (void)fprintf(diagnostic,
                      "error: --save and --export-strategy require different "
                      "paths\n");
        return CLI_PARSE_ERROR;
    }
    if (!iterations_seen) {
        (void)fprintf(diagnostic,
                      "error: missing required option --iterations\n");
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
    case CFR_STATUS_IO_ERROR:
        return "CFR_STATUS_IO_ERROR";
    case CFR_STATUS_FORMAT_ERROR:
        return "CFR_STATUS_FORMAT_ERROR";
    case CFR_STATUS_INCOMPATIBLE_GAME:
        return "CFR_STATUS_INCOMPATIBLE_GAME";
    default:
        return "CFR_STATUS_UNKNOWN";
    }
}

static bool print_status_error(FILE *stream, const char *operation,
                               Status status) {
    if (stream == NULL || operation == NULL)
        return false;

    return fprintf(stream, "error: %s failed: %s\n", operation,
                   status_name(status)) >= 0;
}

typedef Status (*TrainerStreamWriter)(FILE *stream, const Trainer *trainer);

static Status write_trainer_file(const char *path, const Trainer *trainer,
                                 TrainerStreamWriter writer) {
    enum { TEMPORARY_ATTEMPTS = 1000, TEMPORARY_SUFFIX_CAPACITY = 16 };
    const size_t path_length = path == NULL ? 0 : strlen(path);
    char *temporary_path;
    FILE *stream = NULL;
    Status status;

    if (path == NULL || path_length == 0 || trainer == NULL || writer == NULL)
        return CFR_STATUS_INVALID_ARGUMENT;
    if (path_length > SIZE_MAX - TEMPORARY_SUFFIX_CAPACITY)
        return CFR_STATUS_OUT_OF_MEMORY;
    temporary_path = malloc(path_length + TEMPORARY_SUFFIX_CAPACITY);
    if (temporary_path == NULL)
        return CFR_STATUS_OUT_OF_MEMORY;
    for (unsigned int attempt = 0; attempt < TEMPORARY_ATTEMPTS; attempt += 1) {
        const int printed = snprintf(temporary_path,
                                     path_length + TEMPORARY_SUFFIX_CAPACITY,
                                     "%s.tmp.%u", path, attempt);

        if (printed < 0 || (size_t)printed >=
                               path_length + TEMPORARY_SUFFIX_CAPACITY) {
            free(temporary_path);
            return CFR_STATUS_OUT_OF_MEMORY;
        }
        errno = 0;
        stream = fopen(temporary_path, "wbx");
        if (stream != NULL || errno != EEXIST)
            break;
    }
    if (stream == NULL) {
        free(temporary_path);
        return CFR_STATUS_IO_ERROR;
    }
    status = writer(stream, trainer);
    if (fclose(stream) != 0 && status == CFR_STATUS_SUCCESS)
        status = CFR_STATUS_IO_ERROR;
    if (status == CFR_STATUS_SUCCESS && rename(temporary_path, path) != 0)
        status = CFR_STATUS_IO_ERROR;
    if (status != CFR_STATUS_SUCCESS)
        (void)remove(temporary_path);
    free(temporary_path);
    return status;
}

static Status load_checkpoint_file(const char *path, const Game *game,
                                   GameState *state, InfoStore *store,
                                   Trainer *trainer) {
    FILE *stream;
    Status status;

    if (path == NULL)
        return CFR_STATUS_INVALID_ARGUMENT;
    stream = fopen(path, "rb");
    if (stream == NULL)
        return CFR_STATUS_IO_ERROR;
    status = cfr_checkpoint_read(stream, game, state, store, trainer);
    if (fclose(stream) != 0 && status == CFR_STATUS_SUCCESS)
        status = CFR_STATUS_IO_ERROR;
    return status;
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
                "report iterations=%zu traversals=%zu "
                "visited_nodes=%zu information_sets=%zu seconds=%.6f\n",
                trainer->iterations, trainer->traversals,
                trainer->visited_nodes, store->size, seconds) < 0) {
        return false;
    }

    return fflush(stream) == 0 && !ferror(stream);
}

static bool print_start(FILE *stream, const CliOptions *options,
                        const Trainer *trainer) {
    const char *variant;

    if (stream == NULL || options == NULL || trainer == NULL)
        return false;
    if (trainer->variant == CFR_TRAINER_VARIANT_CFR)
        variant = "cfr";
    else if (trainer->variant == CFR_TRAINER_VARIANT_CFR_PLUS)
        variant = "cfr+";
    else
        return false;

    if (fprintf(stream,
                "start game=blackjack requested_iterations=%zu "
                "starting_iterations=%zu report_every=%zu evaluation=%s "
                "variant=%s\n",
                options->iterations, trainer->training_iterations,
                options->report_every,
                options->evaluate ? "yes" : "no", variant) < 0) {
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
                "evaluation average_value_player_0=%.17g "
                "average_value_dealer=%.17g exploitability=%.17g "
                "seconds=%.6f\n",
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
        (void)print_status_error(diagnostic, "initialize the blackjack state",
                                 status);
        goto cleanup;
    }

    game = cfr_blackjack_descriptor();
    game_state = cfr_blackjack_state_as_game_state(&state);
    if (game == NULL || game_state == NULL) {
        (void)fprintf(diagnostic,
                      "error: the blackjack adapter is unavailable\n");
        goto cleanup;
    }

    if (options->load_path != NULL) {
        status = load_checkpoint_file(options->load_path, game, game_state,
                                      &store, &trainer);
        if (status != CFR_STATUS_SUCCESS) {
            (void)print_status_error(diagnostic, "load the checkpoint", status);
            goto cleanup;
        }
        store_initialized = true;
    } else {
        status = cfr_info_store_init(&store);
        if (status != CFR_STATUS_SUCCESS) {
            (void)print_status_error(diagnostic, "initialize the store", status);
            goto cleanup;
        }
        store_initialized = true;

        if (options->cfr_plus)
            status =
                cfr_trainer_init_plus(&trainer, game, game_state, &store);
        else
            status = cfr_trainer_init(&trainer, game, game_state, &store);
        if (status != CFR_STATUS_SUCCESS) {
            (void)print_status_error(diagnostic, "initialize the trainer",
                                     status);
            goto cleanup;
        }
    }

    if (timespec_get(&start, TIME_UTC) != TIME_UTC) {
        (void)fprintf(diagnostic,
                      "error: could not obtain the initial timestamp\n");
        goto cleanup;
    }
    if (!print_start(output, options, &trainer)) {
        (void)fprintf(diagnostic,
                      "error: could not write the start report\n");
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
            (void)print_status_error(diagnostic, "train", status);
            goto cleanup;
        }
        completed += block;

        status = cfr_trainer_get_stats(&trainer, &trainer_stats);
        if (status != CFR_STATUS_SUCCESS) {
            (void)print_status_error(diagnostic, "query the trainer", status);
            goto cleanup;
        }
        status = cfr_info_store_get_stats(&store, &store_stats);
        if (status != CFR_STATUS_SUCCESS) {
            (void)print_status_error(diagnostic, "query the store", status);
            goto cleanup;
        }

        if (timespec_get(&current, TIME_UTC) != TIME_UTC ||
            !elapsed_seconds(&start, &current, &seconds)) {
            (void)fprintf(
                diagnostic,
                "error: could not calculate elapsed time\n");
            goto cleanup;
        }
        if (!print_training_report(output, &trainer_stats, &store_stats,
                                   seconds)) {
            (void)fprintf(diagnostic,
                          "error: could not write the training report\n");
            goto cleanup;
        }
    }

    if (options->evaluate) {
        EvaluationMetrics metrics;
        struct timespec current;
        double seconds;

        status = cfr_evaluation_metrics(game, game_state, &store, &metrics);
        if (status != CFR_STATUS_SUCCESS) {
            (void)print_status_error(diagnostic, "evaluate the average profile",
                                     status);
            goto cleanup;
        }
        if (timespec_get(&current, TIME_UTC) != TIME_UTC ||
            !elapsed_seconds(&start, &current, &seconds)) {
            (void)fprintf(
                diagnostic,
                "error: could not calculate elapsed time\n");
            goto cleanup;
        }
        if (!print_evaluation(output, &metrics, seconds)) {
            (void)fprintf(diagnostic,
                          "error: could not write the evaluation\n");
            goto cleanup;
        }
    }

    if (options->save_path != NULL) {
        status = write_trainer_file(options->save_path, &trainer,
                                    cfr_checkpoint_write);
        if (status != CFR_STATUS_SUCCESS) {
            (void)print_status_error(diagnostic, "save the checkpoint", status);
            goto cleanup;
        }
    }

    if (options->export_path != NULL) {
        status = write_trainer_file(options->export_path, &trainer,
                                    cfr_strategy_write_text);
        if (status != CFR_STATUS_SUCCESS) {
            (void)print_status_error(diagnostic, "export the average strategy",
                                     status);
            goto cleanup;
        }
    }

    result = CLI_EXIT_SUCCESS;

cleanup:
    if (store_initialized) {
        status = cfr_info_store_destroy(&store);
        if (status != CFR_STATUS_SUCCESS) {
            (void)print_status_error(diagnostic, "destroy the store", status);
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
