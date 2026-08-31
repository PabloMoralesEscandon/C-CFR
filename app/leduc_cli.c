#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cfr/checkpoint.h"
#include "cfr/evaluation.h"
#include "cfr/game.h"
#include "cfr/info_store.h"
#include "cfr/leduc_poker.h"
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
    bool print_strategy;
    bool cfr_plus;
    bool mccfr;
    uint64_t seed;
    bool evaluate;
    const char *load_path;
    const char *save_path;
    const char *export_path;
} CliOptions;

static bool print_usage(FILE *stream, const char *program_name) {
    if (stream == NULL || program_name == NULL)
        return false;

    if (fprintf(stream,
                "Usage: %s --iterations N [--report-every N] "
                "[--print-strategy] [--cfr-plus | --mccfr [--seed N]] "
                "[--save FILE] "
                "[--export-strategy FILE]\n"
                "       %s --load FILE --iterations N [--report-every N] "
                "[--print-strategy] [--save FILE] "
                "[--export-strategy FILE]\n"
                "       %s --load FILE --evaluate [--print-strategy] "
                "[--export-strategy FILE]\n"
                "       %s --help\n"
                "\n"
                "Options:\n"
                "  --iterations N     Training iterations; N must be "
                "positive.\n"
                "  --report-every N   Iterations between reports; if omitted, "
                "only the final report is printed.\n"
                "  --print-strategy   Print the final average strategy.\n"
                "  --cfr-plus         Use CFR+ with Regret Matching+ and linear "
                "averaging.\n"
                "  --mccfr            Use external-sampling Monte Carlo CFR.\n"
                "  --seed N           MCCFR random seed; defaults to zero.\n"
                "  --load FILE        Load a binary checkpoint before running.\n"
                "  --save FILE        Save a binary checkpoint after training.\n"
                "  --evaluate         Evaluate a loaded checkpoint without "
                "training.\n"
                "  --export-strategy FILE\n"
                "                     Export the average strategy as text; "
                "FILE must not exist.\n"
                "  --help, -h         Display this help.\n"
                "\n"
                "Exit codes:\n"
                "  0  Successful execution or help.\n"
                "  1  Operation, library, clock, or write failure.\n"
                "  2  Invalid arguments.\n",
                program_name, program_name, program_name, program_name) < 0) {
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

static bool parse_u64(const char *text, uint64_t *value_out) {
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
    if (errno == ERANGE || end == text || *end != '\0' || value > UINT64_MAX)
        return false;
    *value_out = (uint64_t)value;
    return true;
}

static CliParseResult parse_options(int argc, char *const argv[],
                                    FILE *diagnostic, CliOptions *options_out) {
    CliOptions options = {0};
    bool iterations_seen = false;
    bool report_every_seen = false;
    bool print_strategy_seen = false;
    bool cfr_plus_seen = false;
    bool mccfr_seen = false;
    bool seed_seen = false;
    bool evaluate_seen = false;
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

        if (strcmp(argument, "--print-strategy") == 0) {
            if (print_strategy_seen) {
                (void)fprintf(
                    diagnostic,
                    "error: --print-strategy was specified more than once\n");
                return CLI_PARSE_ERROR;
            }
            options.print_strategy = true;
            print_strategy_seen = true;
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

        if (strcmp(argument, "--mccfr") == 0) {
            if (mccfr_seen) {
                (void)fprintf(diagnostic,
                              "error: --mccfr was specified more than once\n");
                return CLI_PARSE_ERROR;
            }
            options.mccfr = true;
            mccfr_seen = true;
            continue;
        }

        if (strcmp(argument, "--seed") == 0) {
            if (seed_seen) {
                (void)fprintf(diagnostic,
                              "error: --seed was specified more than once\n");
                return CLI_PARSE_ERROR;
            }
            if (index + 1 >= argc || argv[index + 1] == NULL) {
                (void)fprintf(diagnostic, "error: missing value for --seed\n");
                return CLI_PARSE_ERROR;
            }
            index += 1;
            if (!parse_u64(argv[index], &options.seed)) {
                (void)fprintf(diagnostic,
                              "error: --seed requires an unsigned 64-bit "
                              "decimal integer\n");
                return CLI_PARSE_ERROR;
            }
            seed_seen = true;
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

    if (options.evaluate && !load_seen) {
        (void)fprintf(diagnostic,
                      "error: --evaluate requires --load FILE\n");
        return CLI_PARSE_ERROR;
    }
    if (options.evaluate && iterations_seen) {
        (void)fprintf(diagnostic,
                      "error: --evaluate cannot be combined with "
                      "--iterations\n");
        return CLI_PARSE_ERROR;
    }
    if (options.evaluate && report_every_seen) {
        (void)fprintf(diagnostic,
                      "error: --evaluate cannot be combined with "
                      "--report-every\n");
        return CLI_PARSE_ERROR;
    }
    if (options.evaluate && cfr_plus_seen) {
        (void)fprintf(diagnostic,
                      "error: --evaluate cannot be combined with --cfr-plus\n");
        return CLI_PARSE_ERROR;
    }
    if (options.evaluate && (mccfr_seen || seed_seen)) {
        (void)fprintf(diagnostic,
                      "error: --evaluate cannot be combined with --mccfr or "
                      "--seed\n");
        return CLI_PARSE_ERROR;
    }
    if (options.evaluate && save_seen) {
        (void)fprintf(diagnostic,
                      "error: --evaluate cannot be combined with --save\n");
        return CLI_PARSE_ERROR;
    }
    if (load_seen && cfr_plus_seen) {
        (void)fprintf(diagnostic,
                      "error: --cfr-plus cannot be combined with --load; "
                      "the checkpoint selects the variant\n");
        return CLI_PARSE_ERROR;
    }
    if (load_seen && (mccfr_seen || seed_seen)) {
        (void)fprintf(diagnostic,
                      "error: --mccfr and --seed cannot be combined with "
                      "--load; the checkpoint selects the variant and random "
                      "stream\n");
        return CLI_PARSE_ERROR;
    }
    if (cfr_plus_seen && mccfr_seen) {
        (void)fprintf(diagnostic,
                      "error: --cfr-plus cannot be combined with --mccfr\n");
        return CLI_PARSE_ERROR;
    }
    if (seed_seen && !mccfr_seen) {
        (void)fprintf(diagnostic, "error: --seed requires --mccfr\n");
        return CLI_PARSE_ERROR;
    }
    if (save_seen && export_seen &&
        strcmp(options.save_path, options.export_path) == 0) {
        (void)fprintf(diagnostic,
                      "error: --save and --export-strategy require different "
                      "paths\n");
        return CLI_PARSE_ERROR;
    }
    /*
     * --save may replace the loaded checkpoint: that is how training resumes.
     * --export-strategy writes text that cannot be loaded, so it must never
     * name the checkpoint. run_training also checks the target before training
     * and claims it exclusively, which catches aliases this comparison cannot
     * detect.
     */
    if (load_seen && export_seen &&
        strcmp(options.load_path, options.export_path) == 0) {
        (void)fprintf(diagnostic,
                      "error: --export-strategy cannot write to the "
                      "--load checkpoint\n");
        return CLI_PARSE_ERROR;
    }
    if (!options.evaluate && !iterations_seen) {
        (void)fprintf(diagnostic,
                      "error: missing required option --iterations\n");
        return CLI_PARSE_ERROR;
    }

    if (!options.evaluate && !report_every_seen)
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

static bool print_report(FILE *stream, size_t completed_iterations,
                         const EvaluationMetrics *metrics,
                         const InfoStoreStats *store_stats, double seconds) {
    if (stream == NULL || metrics == NULL || store_stats == NULL ||
        !isfinite(seconds) || seconds < 0.0) {
        return false;
    }

    if (fprintf(stream,
                "report iterations=%zu average_value_player_0=%.17g "
                "exploitability=%.17g information_sets=%zu seconds=%.6f\n",
                completed_iterations, metrics->profile_value_player_0,
                metrics->exploitability, store_stats->size, seconds) < 0) {
        return false;
    }

    return fflush(stream) == 0 && !ferror(stream);
}

static bool print_evaluation(FILE *stream, size_t training_iterations,
                             const EvaluationMetrics *metrics,
                             const InfoStoreStats *store_stats) {
    if (stream == NULL || metrics == NULL || store_stats == NULL)
        return false;

    if (fprintf(
            stream,
            "evaluation training_iterations=%zu "
            "profile_value_player_0=%.17g profile_value_player_1=%.17g "
            "best_response_value_player_0=%.17g "
            "best_response_value_player_1=%.17g improvement_player_0=%.17g "
            "improvement_player_1=%.17g nash_conv=%.17g "
            "exploitability=%.17g information_sets=%zu\n",
            training_iterations, metrics->profile_value_player_0,
            metrics->profile_value_player_1,
            metrics->best_response_value_player_0,
            metrics->best_response_value_player_1,
            metrics->improvement_player_0, metrics->improvement_player_1,
            metrics->nash_conv, metrics->exploitability, store_stats->size) <
        0) {
        return false;
    }

    return fflush(stream) == 0 && !ferror(stream);
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
    stream = NULL;
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

/* Fails early when an export cannot claim a new destination safely. */
static bool export_target_is_available(const char *path, FILE *diagnostic) {
    FILE *stream;

    if (diagnostic == NULL)
        return false;
    if (path == NULL)
        return true;

    errno = 0;
    stream = fopen(path, "rb");
    if (stream != NULL) {
        if (fclose(stream) != 0) {
            (void)print_status_error(diagnostic, "inspect the export target",
                                     CFR_STATUS_IO_ERROR);
            return false;
        }
        (void)fprintf(diagnostic,
                      "error: --export-strategy target '%s' already exists; "
                      "choose a new path or remove it first\n",
                      path);
        return false;
    }
    if (errno != ENOENT) {
        (void)print_status_error(diagnostic, "inspect the export target",
                                 CFR_STATUS_IO_ERROR);
        return false;
    }
    return true;
}

typedef struct {
    InfoSetKey key;
    Player player;
    LeducPokerCard private_card;
    LeducPokerCard public_card;
    LeducPokerPhase phase;
    LeducPokerAction
        history[CFR_LEDUC_POKER_PUBLIC_HISTORY_CAPACITY];
    size_t history_count;
    size_t round_start_index;
    Action actions[CFR_LEDUC_POKER_MAX_POSSIBLE_ACTIONS];
    size_t action_count;
} StrategyRow;

static const char *card_name(LeducPokerCard card) {
    switch (card) {
    case CFR_LEDUC_POKER_CARD_NOT_DEALT:
        return "none";
    case CFR_LEDUC_POKER_CARD_JACK:
        return "J";
    case CFR_LEDUC_POKER_CARD_QUEEN:
        return "Q";
    case CFR_LEDUC_POKER_CARD_KING:
        return "K";
    default:
        return NULL;
    }
}

static const char *action_name(Action action) {
    switch ((LeducPokerAction)action) {
    case CFR_LEDUC_POKER_ACTION_CHECK:
        return "check";
    case CFR_LEDUC_POKER_ACTION_BET:
        return "bet";
    case CFR_LEDUC_POKER_ACTION_FOLD:
        return "fold";
    case CFR_LEDUC_POKER_ACTION_CALL:
        return "call";
    case CFR_LEDUC_POKER_ACTION_RAISE:
        return "raise";
    default:
        return NULL;
    }
}

static bool strategy_row_exists(const StrategyRow *rows, size_t row_count,
                                InfoSetKey key) {
    size_t index;

    for (index = 0; index < row_count; index++) {
        if (rows[index].key == key)
            return true;
    }
    return false;
}

static Status collect_strategy_rows(const Game *game, LeducPokerState *state,
                                    StrategyRow *rows, size_t capacity,
                                    size_t *row_count) {
    GameState *game_state;
    const GameState *const_state;
    Action actions[CFR_LEDUC_POKER_MAX_POSSIBLE_ACTIONS];
    size_t action_count = 0;
    bool terminal;
    Actor actor;
    Status status;
    size_t index;

    if (game == NULL || state == NULL || rows == NULL || row_count == NULL)
        return CFR_STATUS_INVALID_ARGUMENT;
    game_state = cfr_leduc_poker_state_as_game_state(state);
    const_state = cfr_leduc_poker_state_as_game_state_const(state);
    status = cfr_game_is_terminal(game, const_state, &terminal);
    if (status != CFR_STATUS_SUCCESS || terminal)
        return status;
    status = cfr_game_current_actor(game, const_state, &actor);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    status = cfr_game_legal_actions(game, const_state, actions,
                                    CFR_LEDUC_POKER_MAX_POSSIBLE_ACTIONS,
                                    &action_count);
    if (status != CFR_STATUS_SUCCESS)
        return status;

    if (actor.kind == CFR_ACTOR_PLAYER) {
        InfoSetKey key;

        status = cfr_game_information_set_key(game, const_state, &key);
        if (status != CFR_STATUS_SUCCESS)
            return status;
        if (!strategy_row_exists(rows, *row_count, key)) {
            StrategyRow *row;

            if (*row_count >= capacity)
                return CFR_STATUS_BUFFER_TOO_SMALL;
            row = &rows[*row_count];
            *row = (StrategyRow){0};
            row->key = key;
            row->player = actor.player;
            row->private_card = state->private_cards[actor.player];
            row->public_card = state->public_card;
            row->phase = state->phase;
            row->history_count = state->public_action_count;
            row->round_start_index = state->round_start_index;
            row->action_count = action_count;
            for (index = 0; index < state->public_action_count; index++)
                row->history[index] = state->public_actions[index];
            for (index = 0; index < action_count; index++)
                row->actions[index] = actions[index];
            *row_count += 1;
        }
    } else if (actor.kind != CFR_ACTOR_CHANCE) {
        return CFR_STATUS_INVALID_ARGUMENT;
    }

    for (index = 0; index < action_count; index++) {
        Status undo_status;

        status = cfr_game_apply_action(game, game_state, actions[index]);
        if (status != CFR_STATUS_SUCCESS)
            return status;
        status = collect_strategy_rows(game, state, rows, capacity, row_count);
        undo_status = cfr_game_undo_action(game, game_state);
        if (undo_status != CFR_STATUS_SUCCESS)
            return undo_status;
        if (status != CFR_STATUS_SUCCESS)
            return status;
    }
    return CFR_STATUS_SUCCESS;
}

static int compare_strategy_rows(const void *left, const void *right) {
    const StrategyRow *left_row = left;
    const StrategyRow *right_row = right;

    if (left_row->key < right_row->key)
        return -1;
    if (left_row->key > right_row->key)
        return 1;
    return 0;
}

static Status print_strategy_row(FILE *stream, const InfoStore *store,
                                 const StrategyRow *row) {
    Probability strategy[CFR_LEDUC_POKER_MAX_POSSIBLE_ACTIONS];
    size_t strategy_count = 0;
    const char *private_name;
    const char *public_name;
    int round;
    size_t index;
    Status status;

    private_name = card_name(row->private_card);
    public_name = card_name(row->public_card);
    round = row->phase == CFR_LEDUC_POKER_PHASE_FIRST_BETTING ? 1 : 2;
    if (private_name == NULL || public_name == NULL ||
        (row->phase != CFR_LEDUC_POKER_PHASE_FIRST_BETTING &&
         row->phase != CFR_LEDUC_POKER_PHASE_SECOND_BETTING)) {
        return CFR_STATUS_INVALID_ARGUMENT;
    }
    status = cfr_evaluation_average_strategy(
        store, row->key, strategy, CFR_LEDUC_POKER_MAX_POSSIBLE_ACTIONS,
        &strategy_count);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    if (strategy_count != row->action_count)
        return CFR_STATUS_INVALID_ARGUMENT;

    if (fprintf(stream,
                "strategy player_%d_private_%s_public_%s_round_%d_history_",
                (int)row->player, private_name, public_name, round) < 0) {
        return CFR_STATUS_IO_ERROR;
    }
    if (row->history_count == 0) {
        if (fputs("none", stream) == EOF)
            return CFR_STATUS_IO_ERROR;
    } else {
        for (index = 0; index < row->history_count; index++) {
            const char *name = action_name(row->history[index]);

            if (name == NULL)
                return CFR_STATUS_INVALID_ARGUMENT;
            if (index > 0 && fputc(index == row->round_start_index ? '|' : '-',
                                  stream) == EOF) {
                return CFR_STATUS_IO_ERROR;
            }
            if (fputs(name, stream) == EOF)
                return CFR_STATUS_IO_ERROR;
        }
    }
    for (index = 0; index < row->action_count; index++) {
        const char *name = action_name(row->actions[index]);

        if (name == NULL)
            return CFR_STATUS_INVALID_ARGUMENT;
        if (fprintf(stream, " %s=%.17g", name, strategy[index]) < 0)
            return CFR_STATUS_IO_ERROR;
    }
    return fputc('\n', stream) == EOF ? CFR_STATUS_IO_ERROR
                                      : CFR_STATUS_SUCCESS;
}

static Status print_final_strategy(FILE *stream, const Game *game,
                                   LeducPokerState *state,
                                   const InfoStore *store) {
    InfoStoreStats stats;
    StrategyRow *rows;
    size_t row_count = 0;
    size_t index;
    Status status;

    if (stream == NULL || game == NULL || state == NULL || store == NULL)
        return CFR_STATUS_INVALID_ARGUMENT;
    status = cfr_info_store_get_stats(store, &stats);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    if (stats.size == 0)
        return CFR_STATUS_INVALID_ARGUMENT;
    if (stats.size > SIZE_MAX / sizeof(*rows))
        return CFR_STATUS_OUT_OF_MEMORY;
    rows = malloc(stats.size * sizeof(*rows));
    if (rows == NULL)
        return CFR_STATUS_OUT_OF_MEMORY;
    status = collect_strategy_rows(game, state, rows, stats.size, &row_count);
    if (status == CFR_STATUS_SUCCESS && row_count != stats.size)
        status = CFR_STATUS_INVALID_ARGUMENT;
    if (status == CFR_STATUS_SUCCESS)
        qsort(rows, row_count, sizeof(*rows), compare_strategy_rows);
    for (index = 0; status == CFR_STATUS_SUCCESS && index < row_count;
         index++) {
        status = print_strategy_row(stream, store, &rows[index]);
    }
    free(rows);
    return status;
}

static Status evaluate_trainer(const Game *game, GameState *state,
                               const InfoStore *store,
                               const Trainer *trainer,
                               EvaluationMetrics *metrics) {
    if (trainer->variant == CFR_TRAINER_VARIANT_MCCFR_EXTERNAL) {
        return cfr_evaluation_metrics_with_unvisited_uniform(
            game, state, store, metrics);
    }
    return cfr_evaluation_metrics(game, state, store, metrics);
}

static int run_training(const CliOptions *options, FILE *output,
                        FILE *diagnostic) {
    LeducPokerState state = {0};
    InfoStore store = {0};
    Trainer trainer = {0};
    const Game *game;
    GameState *game_state;
    struct timespec start = {0};
    size_t completed = 0;
    bool store_initialized = false;
    int result = CLI_EXIT_RUNTIME_ERROR;
    Status status;

    if (options == NULL || output == NULL || diagnostic == NULL)
        return CLI_EXIT_RUNTIME_ERROR;

    if (!export_target_is_available(options->export_path, diagnostic))
        goto cleanup;

    status = cfr_leduc_poker_state_init(&state);
    if (status != CFR_STATUS_SUCCESS) {
        (void)print_status_error(diagnostic, "initialize the Leduc Poker state",
                                 status);
        goto cleanup;
    }

    game = cfr_leduc_poker_descriptor();
    game_state = cfr_leduc_poker_state_as_game_state(&state);
    if (game == NULL || game_state == NULL) {
        (void)fprintf(diagnostic,
                      "error: the Leduc Poker adapter is unavailable\n");
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
        else if (options->mccfr)
            status = cfr_trainer_init_mccfr(
                &trainer, game, game_state, &store, options->seed);
        else
            status = cfr_trainer_init(&trainer, game, game_state, &store);
        if (status != CFR_STATUS_SUCCESS) {
            (void)print_status_error(diagnostic, "initialize the trainer",
                                     status);
            goto cleanup;
        }
    }

    if (options->evaluate) {
        EvaluationMetrics metrics;
        InfoStoreStats store_stats;

        status =
            evaluate_trainer(game, game_state, &store, &trainer, &metrics);
        if (status != CFR_STATUS_SUCCESS) {
            (void)print_status_error(diagnostic, "evaluate metrics", status);
            goto cleanup;
        }
        status = cfr_info_store_get_stats(&store, &store_stats);
        if (status != CFR_STATUS_SUCCESS) {
            (void)print_status_error(diagnostic, "query the store", status);
            goto cleanup;
        }
        if (!print_evaluation(output, trainer.training_iterations, &metrics,
                              &store_stats)) {
            (void)fprintf(diagnostic,
                          "error: could not write the evaluation\n");
            goto cleanup;
        }
    } else {
        if (timespec_get(&start, TIME_UTC) != TIME_UTC) {
            (void)fprintf(diagnostic,
                          "error: could not obtain the initial timestamp\n");
            goto cleanup;
        }

        while (completed < options->iterations) {
            const size_t remaining = options->iterations - completed;
            const size_t block = options->report_every < remaining
                                     ? options->report_every
                                     : remaining;
            EvaluationMetrics metrics;
            InfoStoreStats store_stats;
            struct timespec current;
            double seconds;

            status = cfr_trainer_run(&trainer, block);
            if (status != CFR_STATUS_SUCCESS) {
                (void)print_status_error(diagnostic, "train", status);
                goto cleanup;
            }
            completed += block;

            status =
                evaluate_trainer(game, game_state, &store, &trainer, &metrics);
            if (status != CFR_STATUS_SUCCESS) {
                (void)print_status_error(diagnostic, "evaluate metrics", status);
                goto cleanup;
            }

            status = cfr_info_store_get_stats(&store, &store_stats);
            if (status != CFR_STATUS_SUCCESS) {
                (void)print_status_error(diagnostic, "query the store", status);
                goto cleanup;
            }

            if (timespec_get(&current, TIME_UTC) != TIME_UTC ||
                !elapsed_seconds(&start, &current, &seconds)) {
                (void)fprintf(diagnostic,
                              "error: could not calculate elapsed time\n");
                goto cleanup;
            }

            if (!print_report(output, trainer.training_iterations, &metrics,
                              &store_stats, seconds)) {
                (void)fprintf(diagnostic,
                              "error: could not write the report\n");
                goto cleanup;
            }
        }
    }

    if (options->print_strategy) {
        status = print_final_strategy(output, game, &state, &store);
        if (status != CFR_STATUS_SUCCESS) {
            (void)print_status_error(diagnostic, "print the average strategy",
                                     status);
            goto cleanup;
        }
        if (fflush(output) != 0 || ferror(output)) {
            (void)fprintf(diagnostic,
                          "error: could not write the average strategy\n");
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
        FILE *export_stream;

        /*
         * Exclusive creation is the replacement guard. It is atomic with
         * respect to every spelling of an existing path, including symbolic
         * links and files created while training was running.
         */
        errno = 0;
        export_stream = fopen(options->export_path, "wbx");
        if (export_stream == NULL) {
            if (errno == EEXIST) {
                (void)fprintf(diagnostic,
                              "error: --export-strategy target '%s' already "
                              "exists; choose a new path or remove it first\n",
                              options->export_path);
            } else {
                (void)print_status_error(diagnostic,
                                         "create the strategy export",
                                         CFR_STATUS_IO_ERROR);
            }
            goto cleanup;
        }
        status = cfr_strategy_write_text(export_stream, &trainer);
        if (fclose(export_stream) != 0 && status == CFR_STATUS_SUCCESS)
            status = CFR_STATUS_IO_ERROR;
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
    const char *program_name = "cfr-leduc";

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
