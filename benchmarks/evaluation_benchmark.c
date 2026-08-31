#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cfr/evaluation.h"
#include "cfr/info_store.h"
#include "cfr/kuhn_poker.h"
#include "cfr/leduc_poker.h"
#include "cfr/trainer.h"

typedef union {
    KuhnPokerState kuhn;
    LeducPokerState leduc;
} BenchmarkState;

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

static double elapsed_seconds(const struct timespec *start,
                              const struct timespec *finish) {
    return (double)(finish->tv_sec - start->tv_sec) +
           (double)(finish->tv_nsec - start->tv_nsec) / 1000000000.0;
}

int main(int argc, char **argv) {
    size_t iterations;
    size_t evaluations;
    size_t samples;
    BenchmarkState state;
    const Game *game;
    GameState *opaque_state;
    InfoStore store = {0};
    Trainer trainer = {0};
    Status status;

    if (argc != 5 || !parse_size(argv[2], &iterations) ||
        !parse_size(argv[3], &evaluations) ||
        !parse_size(argv[4], &samples)) {
        fprintf(stderr,
                "usage: %s {kuhn|leduc} training_iterations "
                "evaluations_per_sample samples\n",
                argv[0]);
        return EXIT_FAILURE;
    }
    if (strcmp(argv[1], "kuhn") == 0) {
        status = cfr_kuhn_poker_state_init(&state.kuhn);
        game = cfr_kuhn_poker_descriptor();
        opaque_state = cfr_kuhn_poker_state_as_game_state(&state.kuhn);
    } else if (strcmp(argv[1], "leduc") == 0) {
        status = cfr_leduc_poker_state_init(&state.leduc);
        game = cfr_leduc_poker_descriptor();
        opaque_state = cfr_leduc_poker_state_as_game_state(&state.leduc);
    } else {
        fprintf(stderr, "unknown game: %s\n", argv[1]);
        return EXIT_FAILURE;
    }

    if (status == CFR_STATUS_SUCCESS)
        status = cfr_info_store_init(&store);
    if (status == CFR_STATUS_SUCCESS)
        status = cfr_trainer_init(&trainer, game, opaque_state, &store);
    if (status == CFR_STATUS_SUCCESS)
        status = cfr_trainer_run(&trainer, iterations);
    if (status != CFR_STATUS_SUCCESS) {
        fprintf(stderr, "benchmark setup failed with status %d\n",
                (int)status);
        (void)cfr_info_store_destroy(&store);
        return EXIT_FAILURE;
    }

    puts("sample,seconds,evaluations_per_second,exploitability");
    for (size_t sample = 0; sample <= samples; sample += 1) {
        EvaluationMetrics metrics;
        struct timespec start;
        struct timespec finish;

        if (clock_gettime(CLOCK_MONOTONIC, &start) != 0)
            return EXIT_FAILURE;
        for (size_t evaluation = 0; evaluation < evaluations;
             evaluation += 1) {
            status =
                cfr_evaluation_metrics(game, opaque_state, &store, &metrics);
            if (status != CFR_STATUS_SUCCESS)
                break;
        }
        if (clock_gettime(CLOCK_MONOTONIC, &finish) != 0)
            return EXIT_FAILURE;
        if (status != CFR_STATUS_SUCCESS) {
            fprintf(stderr, "evaluation failed with status %d\n", (int)status);
            (void)cfr_info_store_destroy(&store);
            return EXIT_FAILURE;
        }
        if (sample != 0) {
            const double elapsed = elapsed_seconds(&start, &finish);

            printf("%zu,%.9f,%.3f,%.17g\n", sample, elapsed,
                   (double)evaluations / elapsed, metrics.exploitability);
        }
    }

    if (cfr_info_store_destroy(&store) != CFR_STATUS_SUCCESS)
        return EXIT_FAILURE;
    return EXIT_SUCCESS;
}
