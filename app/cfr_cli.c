#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cfr/evaluation.h"
#include "cfr/game.h"
#include "cfr/info_store.h"
#include "cfr/kuhn_poker.h"
#include "cfr/trainer.h"

#define STRATEGY_PATH_CAPACITY 3

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
} CliOptions;

typedef struct {
    const char *label;
    Player expected_player;
    Action path[STRATEGY_PATH_CAPACITY];
    size_t path_length;
} StrategyRow;

static bool print_usage(FILE *stream, const char *program_name) {
    if (stream == NULL || program_name == NULL)
        return false;

    if (fprintf(stream,
                "Usage: %s --iterations N [--report-every N] "
                "[--print-strategy] [--cfr-plus]\n"
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
                "  --help, -h         Display this help.\n"
                "\n"
                "Exit codes:\n"
                "  0  Successful execution or help.\n"
                "  1  Operation, library, clock, or write failure.\n"
                "  2  Invalid arguments.\n",
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
    bool print_strategy_seen = false;
    bool cfr_plus_seen = false;
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

        if (strcmp(argument, "--help") == 0 || strcmp(argument, "-h") == 0) {
            (void)fprintf(
                diagnostic,
                "error: help can only be requested without other options\n");
            return CLI_PARSE_ERROR;
        }

        (void)fprintf(diagnostic, "error: unknown option: %s\n", argument);
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
    default:
        return "CFR_STATUS_UNKNOWN";
    }
}

static const char *action_name(Action action) {
    switch ((KuhnPokerAction)action) {
    case CFR_KUHN_POKER_ACTION_CHECK:
        return "check";
    case CFR_KUHN_POKER_ACTION_BET:
        return "bet";
    case CFR_KUHN_POKER_ACTION_FOLD:
        return "fold";
    case CFR_KUHN_POKER_ACTION_CALL:
        return "call";
    case CFR_KUHN_POKER_ACTION_NONE:
    case CFR_KUHN_POKER_ACTION_JQ:
    case CFR_KUHN_POKER_ACTION_JK:
    case CFR_KUHN_POKER_ACTION_QJ:
    case CFR_KUHN_POKER_ACTION_QK:
    case CFR_KUHN_POKER_ACTION_KJ:
    case CFR_KUHN_POKER_ACTION_KQ:
    default:
        return NULL;
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

static bool print_status_error(FILE *stream, const char *operation,
                               Status status) {
    if (stream == NULL || operation == NULL)
        return false;

    return fprintf(stream, "error: %s failed: %s\n", operation,
                   status_name(status)) >= 0;
}

static Status print_strategy_row(FILE *stream, const Game *game,
                                 const InfoStore *store,
                                 const StrategyRow *row) {
    KuhnPokerState state = {0};
    GameState *game_state;
    const GameState *const_game_state;
    Action actions[CFR_KUHN_POKER_MAX_POSSIBLE_ACTIONS];
    Probability strategy[CFR_KUHN_POKER_MAX_POSSIBLE_ACTIONS];
    size_t action_count = 0;
    size_t strategy_count = 0;
    InfoSetKey key;
    Actor actor;
    size_t index;
    Status status;

    if (stream == NULL || game == NULL || store == NULL || row == NULL ||
        row->label == NULL || row->path_length == 0 ||
        row->path_length > STRATEGY_PATH_CAPACITY) {
        return CFR_STATUS_INVALID_ARGUMENT;
    }

    status = cfr_kuhn_poker_state_init(&state);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    game_state = cfr_kuhn_poker_state_as_game_state(&state);
    if (game_state == NULL)
        return CFR_STATUS_INVALID_ARGUMENT;

    for (index = 0; index < row->path_length; index += 1) {
        status = cfr_game_apply_action(game, game_state, row->path[index]);
        if (status != CFR_STATUS_SUCCESS)
            return status;
    }

    const_game_state = cfr_kuhn_poker_state_as_game_state_const(&state);
    if (const_game_state == NULL)
        return CFR_STATUS_INVALID_ARGUMENT;

    status = cfr_game_current_actor(game, const_game_state, &actor);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    if (actor.kind != CFR_ACTOR_PLAYER || actor.player != row->expected_player)
        return CFR_STATUS_INVALID_ARGUMENT;

    status = cfr_game_information_set_key(game, const_game_state, &key);
    if (status != CFR_STATUS_SUCCESS)
        return status;

    status = cfr_game_legal_actions(game, const_game_state, actions,
                                    CFR_KUHN_POKER_MAX_POSSIBLE_ACTIONS,
                                    &action_count);
    if (status != CFR_STATUS_SUCCESS)
        return status;

    status = cfr_evaluation_average_strategy(
        store, key, strategy, CFR_KUHN_POKER_MAX_POSSIBLE_ACTIONS,
        &strategy_count);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    if (action_count == 0 ||
        action_count > CFR_KUHN_POKER_MAX_POSSIBLE_ACTIONS ||
        action_count != strategy_count) {
        return CFR_STATUS_INVALID_ARGUMENT;
    }

    /*
     * Status represents only CFR operations. The caller detects stream errors
     * through fflush and ferror.
     */
    if (fprintf(stream, "strategy %s", row->label) < 0)
        return CFR_STATUS_SUCCESS;
    for (index = 0; index < action_count; index += 1) {
        const char *name = action_name(actions[index]);

        if (name == NULL)
            return CFR_STATUS_INVALID_ARGUMENT;
        if (fprintf(stream, " %s=%.17g", name, strategy[index]) < 0)
            return CFR_STATUS_SUCCESS;
    }
    (void)fprintf(stream, "\n");
    return CFR_STATUS_SUCCESS;
}

static Status print_final_strategy(FILE *stream, const Game *game,
                                   const InfoStore *store) {
    static const StrategyRow rows[] = {
        {"player_0_open_card_J",
         CFR_PLAYER_0,
         {CFR_KUHN_POKER_ACTION_JQ},
         1},
        {"player_0_open_card_Q",
         CFR_PLAYER_0,
         {CFR_KUHN_POKER_ACTION_QJ},
         1},
        {"player_0_open_card_K",
         CFR_PLAYER_0,
         {CFR_KUHN_POKER_ACTION_KJ},
         1},
        {"player_1_after_check_card_J",
         CFR_PLAYER_1,
         {CFR_KUHN_POKER_ACTION_QJ, CFR_KUHN_POKER_ACTION_CHECK},
         2},
        {"player_1_after_check_card_Q",
         CFR_PLAYER_1,
         {CFR_KUHN_POKER_ACTION_JQ, CFR_KUHN_POKER_ACTION_CHECK},
         2},
        {"player_1_after_check_card_K",
         CFR_PLAYER_1,
         {CFR_KUHN_POKER_ACTION_JK, CFR_KUHN_POKER_ACTION_CHECK},
         2},
        {"player_1_facing_open_bet_card_J",
         CFR_PLAYER_1,
         {CFR_KUHN_POKER_ACTION_QJ, CFR_KUHN_POKER_ACTION_BET},
         2},
        {"player_1_facing_open_bet_card_Q",
         CFR_PLAYER_1,
         {CFR_KUHN_POKER_ACTION_JQ, CFR_KUHN_POKER_ACTION_BET},
         2},
        {"player_1_facing_open_bet_card_K",
         CFR_PLAYER_1,
         {CFR_KUHN_POKER_ACTION_JK, CFR_KUHN_POKER_ACTION_BET},
         2},
        {"player_0_facing_check_bet_card_J",
         CFR_PLAYER_0,
         {CFR_KUHN_POKER_ACTION_JQ, CFR_KUHN_POKER_ACTION_CHECK,
          CFR_KUHN_POKER_ACTION_BET},
         3},
        {"player_0_facing_check_bet_card_Q",
         CFR_PLAYER_0,
         {CFR_KUHN_POKER_ACTION_QJ, CFR_KUHN_POKER_ACTION_CHECK,
          CFR_KUHN_POKER_ACTION_BET},
         3},
        {"player_0_facing_check_bet_card_K",
         CFR_PLAYER_0,
         {CFR_KUHN_POKER_ACTION_KJ, CFR_KUHN_POKER_ACTION_CHECK,
          CFR_KUHN_POKER_ACTION_BET},
         3},
    };
    size_t index;

    if (stream == NULL || game == NULL || store == NULL)
        return CFR_STATUS_INVALID_ARGUMENT;

    for (index = 0; index < sizeof(rows) / sizeof(rows[0]); index += 1) {
        Status status = print_strategy_row(stream, game, store, &rows[index]);

        if (status != CFR_STATUS_SUCCESS)
            return status;
        if (ferror(stream))
            return CFR_STATUS_SUCCESS;
    }

    return CFR_STATUS_SUCCESS;
}

static int run_training(const CliOptions *options, FILE *output,
                        FILE *diagnostic) {
    KuhnPokerState state = {0};
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

    status = cfr_kuhn_poker_state_init(&state);
    if (status != CFR_STATUS_SUCCESS) {
        (void)print_status_error(diagnostic, "initialize the Kuhn Poker state",
                                 status);
        goto cleanup;
    }

    game = cfr_kuhn_poker_descriptor();
    game_state = cfr_kuhn_poker_state_as_game_state(&state);
    if (game == NULL || game_state == NULL) {
        (void)fprintf(diagnostic,
                      "error: the Kuhn Poker adapter is unavailable\n");
        goto cleanup;
    }

    status = cfr_info_store_init(&store);
    if (status != CFR_STATUS_SUCCESS) {
        (void)print_status_error(diagnostic, "initialize the store", status);
        goto cleanup;
    }
    store_initialized = true;

    if (options->cfr_plus)
        status = cfr_trainer_init_plus(&trainer, game, game_state, &store);
    else
        status = cfr_trainer_init(&trainer, game, game_state, &store);
    if (status != CFR_STATUS_SUCCESS) {
        (void)print_status_error(diagnostic, "initialize the trainer", status);
        goto cleanup;
    }

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

        status = cfr_evaluation_metrics(game, game_state, &store, &metrics);
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
            (void)fprintf(
                diagnostic,
                "error: could not calculate elapsed time\n");
            goto cleanup;
        }

        if (!print_report(output, completed, &metrics, &store_stats, seconds)) {
            (void)fprintf(diagnostic, "error: could not write the report\n");
            goto cleanup;
        }
    }

    if (options->print_strategy) {
        status = print_final_strategy(output, game, &store);
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
    const char *program_name = "cfr-kuhn";

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
