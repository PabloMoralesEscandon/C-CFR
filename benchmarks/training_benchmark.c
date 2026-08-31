#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cfr/info_store.h"
#include "cfr/kuhn_poker.h"
#include "cfr/leduc_poker.h"
#include "cfr/trainer.h"

typedef union {
    KuhnPokerState kuhn;
    LeducPokerState leduc;
} BenchmarkState;

typedef enum {
    BENCHMARK_GAME_KUHN,
    BENCHMARK_GAME_LEDUC
} BenchmarkGame;

static void usage(const char *program) {
    fprintf(stderr,
            "usage: %s {kuhn|leduc} {cfr|plus|mccfr} iterations samples\n",
            program);
}

static int parse_size(const char *text, size_t *result) {
    char *end = NULL;
    uintmax_t value;

    errno = 0;
    value = strtoumax(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value == 0 ||
        value > SIZE_MAX) {
        return 0;
    }
    *result = (size_t)value;
    return 1;
}

static Status initialize_game(BenchmarkGame selected, BenchmarkState *state,
                              const Game **game_out,
                              GameState **state_out) {
    if (selected == BENCHMARK_GAME_KUHN) {
        Status status = cfr_kuhn_poker_state_init(&state->kuhn);

        if (status != CFR_STATUS_SUCCESS)
            return status;
        *game_out = cfr_kuhn_poker_descriptor();
        *state_out = cfr_kuhn_poker_state_as_game_state(&state->kuhn);
        return CFR_STATUS_SUCCESS;
    }

    Status status = cfr_leduc_poker_state_init(&state->leduc);

    if (status != CFR_STATUS_SUCCESS)
        return status;
    *game_out = cfr_leduc_poker_descriptor();
    *state_out = cfr_leduc_poker_state_as_game_state(&state->leduc);
    return CFR_STATUS_SUCCESS;
}

static Status initialize_trainer(const char *variant, Trainer *trainer,
                                 const Game *game, GameState *state,
                                 InfoStore *store) {
    if (strcmp(variant, "cfr") == 0)
        return cfr_trainer_init(trainer, game, state, store);
    if (strcmp(variant, "plus") == 0)
        return cfr_trainer_init_plus(trainer, game, state, store);
    if (strcmp(variant, "mccfr") == 0)
        return cfr_trainer_init_mccfr(trainer, game, state, store, UINT64_C(42));
    return CFR_STATUS_INVALID_ARGUMENT;
}

static double elapsed_seconds(const struct timespec *start,
                              const struct timespec *finish) {
    return (double)(finish->tv_sec - start->tv_sec) +
           (double)(finish->tv_nsec - start->tv_nsec) / 1000000000.0;
}

int main(int argc, char **argv) {
    BenchmarkGame selected;
    size_t iterations;
    size_t samples;

    if (argc != 5) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }
    if (strcmp(argv[1], "kuhn") == 0) {
        selected = BENCHMARK_GAME_KUHN;
    } else if (strcmp(argv[1], "leduc") == 0) {
        selected = BENCHMARK_GAME_LEDUC;
    } else {
        usage(argv[0]);
        return EXIT_FAILURE;
    }
    if ((strcmp(argv[2], "cfr") != 0 && strcmp(argv[2], "plus") != 0 &&
         strcmp(argv[2], "mccfr") != 0) ||
        !parse_size(argv[3], &iterations) || !parse_size(argv[4], &samples)) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    puts("sample,seconds,iterations_per_second,visited_nodes");
    for (size_t sample = 0; sample <= samples; sample += 1) {
        BenchmarkState state;
        const Game *game = NULL;
        GameState *opaque_state = NULL;
        InfoStore store = {0};
        Trainer trainer = {0};
        TrainerStats stats = {0};
        struct timespec start;
        struct timespec finish;
        Status status = initialize_game(selected, &state, &game, &opaque_state);

        if (status == CFR_STATUS_SUCCESS)
            status = cfr_info_store_init(&store);
        if (status == CFR_STATUS_SUCCESS)
            status = initialize_trainer(argv[2], &trainer, game, opaque_state,
                                        &store);
        if (status == CFR_STATUS_SUCCESS &&
            clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
            status = CFR_STATUS_IO_ERROR;
        }
        if (status == CFR_STATUS_SUCCESS)
            status = cfr_trainer_run(&trainer, iterations);
        if (status == CFR_STATUS_SUCCESS &&
            clock_gettime(CLOCK_MONOTONIC, &finish) != 0) {
            status = CFR_STATUS_IO_ERROR;
        }
        if (status == CFR_STATUS_SUCCESS)
            status = cfr_trainer_get_stats(&trainer, &stats);
        if (status != CFR_STATUS_SUCCESS) {
            fprintf(stderr, "benchmark failed with status %d\n", (int)status);
            (void)cfr_info_store_destroy(&store);
            return EXIT_FAILURE;
        }

        if (sample != 0) {
            const double elapsed = elapsed_seconds(&start, &finish);

            printf("%zu,%.9f,%.3f,%zu\n", sample, elapsed,
                   (double)iterations / elapsed, stats.visited_nodes);
        }
        if (cfr_info_store_destroy(&store) != CFR_STATUS_SUCCESS)
            return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
