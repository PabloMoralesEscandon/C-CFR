#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "cfr/checkpoint.h"
#include "cfr/info_store.h"
#include "cfr/kuhn_poker.h"
#include "cfr/trainer.h"

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
    size_t node_count;
    size_t samples;
    KuhnPokerState state;
    InfoStore store = {0};
    Trainer trainer = {0};
    FILE *stream;
    Status status;

    if (argc != 3 || !parse_size(argv[1], &node_count) ||
        !parse_size(argv[2], &samples)) {
        fprintf(stderr, "usage: %s information_sets samples\n", argv[0]);
        return EXIT_FAILURE;
    }

    status = cfr_kuhn_poker_state_init(&state);
    if (status == CFR_STATUS_SUCCESS)
        status = cfr_info_store_init(&store);
    for (size_t index = 0;
         status == CFR_STATUS_SUCCESS && index < node_count; index += 1) {
        InfoNode *node = NULL;

        status = cfr_info_store_get_or_create(
            &store, (InfoSetKey)index, 4, &node);
        if (status == CFR_STATUS_SUCCESS) {
            for (size_t action = 0; action < node->action_count; action += 1) {
                node->regret_sums[action] =
                    (Utility)((index + action) % 17) - 8.0;
                node->strategy_sums[action] =
                    (double)((index + action) % 23);
            }
        }
    }
    if (status == CFR_STATUS_SUCCESS) {
        status = cfr_trainer_init(
            &trainer, cfr_kuhn_poker_descriptor(),
            cfr_kuhn_poker_state_as_game_state(&state), &store);
    }
    stream = tmpfile();
    if (status != CFR_STATUS_SUCCESS || stream == NULL) {
        fprintf(stderr, "benchmark setup failed\n");
        (void)cfr_info_store_destroy(&store);
        return EXIT_FAILURE;
    }

    puts("sample,seconds,megabytes_per_second,bytes");
    for (size_t sample = 0; sample <= samples; sample += 1) {
        struct timespec start;
        struct timespec finish;
        long bytes;

        rewind(stream);
        if (clock_gettime(CLOCK_MONOTONIC, &start) != 0)
            return EXIT_FAILURE;
        status = cfr_checkpoint_write(stream, &trainer);
        if (status == CFR_STATUS_SUCCESS && fflush(stream) != 0)
            status = CFR_STATUS_IO_ERROR;
        if (clock_gettime(CLOCK_MONOTONIC, &finish) != 0)
            return EXIT_FAILURE;
        bytes = ftell(stream);
        if (status != CFR_STATUS_SUCCESS || bytes <= 0) {
            fprintf(stderr, "checkpoint write failed with status %d\n",
                    (int)status);
            fclose(stream);
            (void)cfr_info_store_destroy(&store);
            return EXIT_FAILURE;
        }
        if (sample != 0) {
            const double elapsed = elapsed_seconds(&start, &finish);

            printf("%zu,%.9f,%.3f,%ld\n", sample, elapsed,
                   ((double)bytes / (1024.0 * 1024.0)) / elapsed, bytes);
        }
    }

    fclose(stream);
    if (cfr_info_store_destroy(&store) != CFR_STATUS_SUCCESS)
        return EXIT_FAILURE;
    return EXIT_SUCCESS;
}
