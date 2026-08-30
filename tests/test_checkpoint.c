#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfr/checkpoint.h"
#include "cfr/evaluation.h"
#include "cfr/kuhn_poker.h"
#include "support/test_allocator.h"
#include "test_suite.h"

static int failures;

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            fprintf(stderr, "%s:%d: assertion failed: %s\n", __FILE__, __LINE__, \
                    #condition);                                               \
            failures += 1;                                                     \
        }                                                                      \
    } while (0)

static GameState *as_state(KuhnPokerState *state) {
    return cfr_kuhn_poker_state_as_game_state(state);
}

static void initialize_kuhn(KuhnPokerState *state, InfoStore *store,
                            Trainer *trainer, bool plus) {
    CHECK(cfr_kuhn_poker_state_init(state) == CFR_STATUS_SUCCESS);
    *store = (InfoStore){0};
    CHECK(cfr_info_store_init(store) == CFR_STATUS_SUCCESS);
    if (plus) {
        CHECK(cfr_trainer_init_plus(trainer, cfr_kuhn_poker_descriptor(),
                                    as_state(state), store) ==
              CFR_STATUS_SUCCESS);
    } else {
        CHECK(cfr_trainer_init(trainer, cfr_kuhn_poker_descriptor(),
                               as_state(state), store) == CFR_STATUS_SUCCESS);
    }
}

static bool files_equal(FILE *left, FILE *right) {
    int left_byte;
    int right_byte;

    rewind(left);
    rewind(right);
    do {
        left_byte = fgetc(left);
        right_byte = fgetc(right);
        if (left_byte != right_byte)
            return false;
    } while (left_byte != EOF);
    return !ferror(left) && !ferror(right);
}

static bool same_metrics(const EvaluationMetrics *left,
                         const EvaluationMetrics *right) {
    return left->profile_value_player_0 == right->profile_value_player_0 &&
           left->profile_value_player_1 == right->profile_value_player_1 &&
           left->best_response_value_player_0 ==
               right->best_response_value_player_0 &&
           left->best_response_value_player_1 ==
               right->best_response_value_player_1 &&
           left->improvement_player_0 == right->improvement_player_0 &&
           left->improvement_player_1 == right->improvement_player_1 &&
           left->nash_conv == right->nash_conv &&
           left->exploitability == right->exploitability;
}

static void test_round_trip_and_exact_continuation(bool plus) {
    const size_t first_amount = 7;
    const size_t second_amount = 13;
    const Game *game = cfr_kuhn_poker_descriptor();
    KuhnPokerState continuous_state;
    KuhnPokerState split_state;
    KuhnPokerState loaded_state;
    InfoStore continuous_store;
    InfoStore split_store;
    InfoStore loaded_store = {0};
    Trainer continuous_trainer;
    Trainer split_trainer;
    Trainer loaded_trainer = {0};
    EvaluationMetrics before;
    EvaluationMetrics after;
    FILE *intermediate = tmpfile();
    FILE *continuous_file = tmpfile();
    FILE *resumed_file = tmpfile();

    CHECK(intermediate != NULL);
    CHECK(continuous_file != NULL);
    CHECK(resumed_file != NULL);
    if (intermediate == NULL || continuous_file == NULL ||
        resumed_file == NULL) {
        if (intermediate != NULL)
            (void)fclose(intermediate);
        if (continuous_file != NULL)
            (void)fclose(continuous_file);
        if (resumed_file != NULL)
            (void)fclose(resumed_file);
        return;
    }

    initialize_kuhn(&continuous_state, &continuous_store, &continuous_trainer,
                    plus);
    initialize_kuhn(&split_state, &split_store, &split_trainer, plus);
    CHECK(cfr_kuhn_poker_state_init(&loaded_state) == CFR_STATUS_SUCCESS);
    CHECK(cfr_trainer_run(&continuous_trainer,
                          first_amount + second_amount) == CFR_STATUS_SUCCESS);
    CHECK(cfr_trainer_run(&split_trainer, first_amount) == CFR_STATUS_SUCCESS);
    CHECK(cfr_evaluation_metrics(game, as_state(&split_state), &split_store,
                                 &before) == CFR_STATUS_SUCCESS);
    CHECK(cfr_checkpoint_write(intermediate, &split_trainer) ==
          CFR_STATUS_SUCCESS);
    CHECK(fflush(intermediate) == 0);
    rewind(intermediate);
    CHECK(cfr_checkpoint_read(intermediate, game, as_state(&loaded_state),
                              &loaded_store, &loaded_trainer) ==
          CFR_STATUS_SUCCESS);
    CHECK(loaded_trainer.variant == split_trainer.variant);
    CHECK(loaded_trainer.training_iterations == first_amount);
    CHECK(loaded_trainer.stats.iterations == split_trainer.stats.iterations);
    CHECK(loaded_trainer.stats.traversals == split_trainer.stats.traversals);
    CHECK(loaded_trainer.stats.visited_nodes ==
          split_trainer.stats.visited_nodes);
    CHECK(loaded_trainer.stats.errors == split_trainer.stats.errors);
    CHECK(cfr_evaluation_metrics(game, as_state(&loaded_state), &loaded_store,
                                 &after) == CFR_STATUS_SUCCESS);
    CHECK(same_metrics(&before, &after));

    CHECK(cfr_trainer_run(&loaded_trainer, second_amount) ==
          CFR_STATUS_SUCCESS);
    CHECK(cfr_checkpoint_write(continuous_file, &continuous_trainer) ==
          CFR_STATUS_SUCCESS);
    CHECK(cfr_checkpoint_write(resumed_file, &loaded_trainer) ==
          CFR_STATUS_SUCCESS);
    CHECK(fflush(continuous_file) == 0);
    CHECK(fflush(resumed_file) == 0);
    CHECK(files_equal(continuous_file, resumed_file));

    CHECK(cfr_info_store_destroy(&continuous_store) == CFR_STATUS_SUCCESS);
    CHECK(cfr_info_store_destroy(&split_store) == CFR_STATUS_SUCCESS);
    CHECK(cfr_info_store_destroy(&loaded_store) == CFR_STATUS_SUCCESS);
    CHECK(fclose(intermediate) == 0);
    CHECK(fclose(continuous_file) == 0);
    CHECK(fclose(resumed_file) == 0);
}

static unsigned char *read_file(FILE *stream, size_t *length_out) {
    long length;
    unsigned char *bytes;

    CHECK(fflush(stream) == 0);
    CHECK(fseek(stream, 0, SEEK_END) == 0);
    length = ftell(stream);
    CHECK(length >= 0);
    if (length < 0)
        return NULL;
    CHECK(fseek(stream, 0, SEEK_SET) == 0);
    bytes = malloc((size_t)length + 1);
    CHECK(bytes != NULL);
    if (bytes == NULL)
        return NULL;
    CHECK(fread(bytes, 1, (size_t)length, stream) == (size_t)length);
    bytes[length] = '\0';
    *length_out = (size_t)length;
    return bytes;
}

static FILE *file_from_bytes(const unsigned char *bytes, size_t length,
                             bool add_trailing_byte) {
    FILE *stream = tmpfile();

    CHECK(stream != NULL);
    if (stream == NULL)
        return NULL;
    CHECK(fwrite(bytes, 1, length, stream) == length);
    if (add_trailing_byte)
        CHECK(fputc(0x5a, stream) != EOF);
    CHECK(fflush(stream) == 0);
    rewind(stream);
    return stream;
}

static uint32_t crc32(const unsigned char *bytes, size_t length) {
    uint32_t crc = UINT32_MAX;

    for (size_t index = 0; index < length; index += 1) {
        crc ^= bytes[index];
        for (unsigned int bit = 0; bit < 8; bit += 1) {
            const uint32_t mask = (uint32_t)-(int32_t)(crc & UINT32_C(1));

            crc = (crc >> 1) ^ (UINT32_C(0xedb88320) & mask);
        }
    }
    return ~crc;
}

static void replace_checksum(unsigned char *bytes, size_t length) {
    const uint32_t checksum = crc32(bytes, length - 4);

    for (size_t index = 0; index < 4; index += 1)
        bytes[length - 4 + index] =
            (unsigned char)(checksum >> (index * 8));
}

static void store_u64_le(unsigned char *bytes, uint64_t value) {
    for (size_t index = 0; index < 8; index += 1)
        bytes[index] = (unsigned char)(value >> (index * 8));
}

static void expect_format_error(const unsigned char *bytes, size_t length,
                                const Game *game, KuhnPokerState *state) {
    InfoStore output_store = {0};
    Trainer output_trainer = {.training_iterations = 99};
    FILE *stream = file_from_bytes(bytes, length, false);

    if (stream == NULL)
        return;
    CHECK(cfr_checkpoint_read(stream, game, as_state(state), &output_store,
                              &output_trainer) == CFR_STATUS_FORMAT_ERROR);
    CHECK(output_store.entries == NULL);
    CHECK(output_trainer.training_iterations == 99);
    CHECK(fclose(stream) == 0);
}

static void test_rejects_bad_input_transactionally(void) {
    const Game *game = cfr_kuhn_poker_descriptor();
    KuhnPokerState source_state;
    KuhnPokerState output_state;
    InfoStore source_store;
    Trainer source_trainer;
    FILE *valid = tmpfile();
    unsigned char *bytes;
    size_t length = 0;

    CHECK(valid != NULL);
    if (valid == NULL)
        return;
    initialize_kuhn(&source_state, &source_store, &source_trainer, true);
    CHECK(cfr_trainer_run(&source_trainer, 3) == CFR_STATUS_SUCCESS);
    CHECK(cfr_checkpoint_write(valid, &source_trainer) == CFR_STATUS_SUCCESS);
    bytes = read_file(valid, &length);
    CHECK(length > 12);
    CHECK(cfr_kuhn_poker_state_init(&output_state) == CFR_STATUS_SUCCESS);

    if (bytes != NULL && length > 12) {
        InfoStore output_store = {0};
        Trainer output_trainer = {.training_iterations = 99};
        FILE *corrupt;

        bytes[0] ^= 1;
        corrupt = file_from_bytes(bytes, length, false);
        if (corrupt != NULL) {
            CHECK(cfr_checkpoint_read(corrupt, game, as_state(&output_state),
                                      &output_store, &output_trainer) ==
                  CFR_STATUS_FORMAT_ERROR);
            CHECK(output_store.entries == NULL);
            CHECK(output_trainer.training_iterations == 99);
            CHECK(fclose(corrupt) == 0);
        }
        bytes[0] ^= 1;

        bytes[length - 1] ^= 1;
        expect_format_error(bytes, length, game, &output_state);
        bytes[length - 1] ^= 1;

        bytes[8] = 2;
        expect_format_error(bytes, length, game, &output_state);
        bytes[8] = 1;

        {
            const size_t header_length =
                8 + 4 + 4 + strlen(game->strategy_schema_id) + 4 + 6 * 8;
            const size_t first_regret = header_length + 8 + 8;
            const size_t second_key = header_length + 48;
            unsigned char saved[8];

            CHECK(second_key + 8 < length - 4);
            if (second_key + 8 < length - 4) {
                memcpy(saved, bytes + first_regret, sizeof(saved));
                store_u64_le(bytes + first_regret,
                             UINT64_C(0x7ff8000000000000));
                replace_checksum(bytes, length);
                expect_format_error(bytes, length, game, &output_state);
                memcpy(bytes + first_regret, saved, sizeof(saved));
                replace_checksum(bytes, length);

                memcpy(saved, bytes + second_key, sizeof(saved));
                store_u64_le(bytes + second_key, UINT64_C(0));
                replace_checksum(bytes, length);
                expect_format_error(bytes, length, game, &output_state);
                memcpy(bytes + second_key, saved, sizeof(saved));
                replace_checksum(bytes, length);
            }
        }

        corrupt = file_from_bytes(bytes, length - 1, false);
        if (corrupt != NULL) {
            CHECK(cfr_checkpoint_read(corrupt, game, as_state(&output_state),
                                      &output_store, &output_trainer) ==
                  CFR_STATUS_FORMAT_ERROR);
            CHECK(output_store.entries == NULL);
            CHECK(output_trainer.training_iterations == 99);
            CHECK(fclose(corrupt) == 0);
        }

        corrupt = file_from_bytes(bytes, length, true);
        if (corrupt != NULL) {
            CHECK(cfr_checkpoint_read(corrupt, game, as_state(&output_state),
                                      &output_store, &output_trainer) ==
                  CFR_STATUS_FORMAT_ERROR);
            CHECK(output_store.entries == NULL);
            CHECK(output_trainer.training_iterations == 99);
            CHECK(fclose(corrupt) == 0);
        }

        {
            Game incompatible = *game;
            FILE *schema_stream = file_from_bytes(bytes, length, false);

            incompatible.strategy_schema_id = "another-game/v1";
            if (schema_stream != NULL) {
                CHECK(cfr_checkpoint_read(
                          schema_stream, &incompatible, as_state(&output_state),
                          &output_store, &output_trainer) ==
                      CFR_STATUS_INCOMPATIBLE_GAME);
                CHECK(output_store.entries == NULL);
                CHECK(output_trainer.training_iterations == 99);
                CHECK(fclose(schema_stream) == 0);
            }
        }
    }

    free(bytes);
    CHECK(cfr_info_store_destroy(&source_store) == CFR_STATUS_SUCCESS);
    CHECK(fclose(valid) == 0);
}

static void test_text_export_is_sorted_and_policy_only(void) {
    KuhnPokerState state;
    InfoStore store;
    Trainer trainer;
    FILE *stream = tmpfile();
    unsigned char *bytes;
    size_t length = 0;
    const char *key_0;
    const char *key_1;
    const char *key_2;
    const char *key_9;

    CHECK(stream != NULL);
    if (stream == NULL)
        return;
    initialize_kuhn(&state, &store, &trainer, false);
    CHECK(cfr_trainer_run(&trainer, 5) == CFR_STATUS_SUCCESS);
    CHECK(cfr_strategy_write_text(stream, &trainer) == CFR_STATUS_SUCCESS);
    bytes = read_file(stream, &length);
    CHECK(length > 0);
    if (bytes != NULL) {
        const char *text = (const char *)bytes;

        CHECK(strstr(text, "cfr-strategy version=1") != NULL);
        CHECK(strstr(text, "schema=cfr.kuhn-poker/v1") != NULL);
        CHECK(strstr(text, "training_iterations=5") != NULL);
        CHECK(strstr(text, "regret") == NULL);
        CHECK(strstr(text, "strategy_sums") == NULL);
        key_0 = strstr(text, "infoset key=0 ");
        key_1 = strstr(text, "infoset key=1 ");
        key_2 = strstr(text, "infoset key=2 ");
        key_9 = strstr(text, "infoset key=9 ");
        CHECK(key_0 != NULL && key_1 != NULL && key_2 != NULL && key_9 != NULL);
        if (key_0 != NULL && key_1 != NULL && key_2 != NULL && key_9 != NULL)
            CHECK(key_0 < key_1 && key_1 < key_2 && key_2 < key_9);
    }
    free(bytes);
    CHECK(cfr_info_store_destroy(&store) == CFR_STATUS_SUCCESS);
    CHECK(fclose(stream) == 0);
}

static void test_signed_keys_round_trip(void) {
    const Game *game = cfr_kuhn_poker_descriptor();
    KuhnPokerState source_state;
    KuhnPokerState output_state;
    InfoStore source_store;
    InfoStore output_store = {0};
    Trainer source_trainer;
    Trainer output_trainer = {0};
    InfoNode *node = NULL;
    const InfoNode *loaded = NULL;
    FILE *stream = tmpfile();

    CHECK(stream != NULL);
    if (stream == NULL)
        return;
    initialize_kuhn(&source_state, &source_store, &source_trainer, false);
    CHECK(cfr_kuhn_poker_state_init(&output_state) == CFR_STATUS_SUCCESS);
    CHECK(cfr_info_store_get_or_create(&source_store, INT64_MIN, 1, &node) ==
          CFR_STATUS_SUCCESS);
    if (node != NULL) {
        node->regret_sums[0] = -3.5;
        node->strategy_sums[0] = 4.5;
    }
    CHECK(cfr_info_store_get_or_create(&source_store, -1, 1, &node) ==
          CFR_STATUS_SUCCESS);
    if (node != NULL) {
        node->regret_sums[0] = 6.5;
        node->strategy_sums[0] = 7.5;
    }
    CHECK(cfr_checkpoint_write(stream, &source_trainer) == CFR_STATUS_SUCCESS);
    CHECK(fflush(stream) == 0);
    rewind(stream);
    CHECK(cfr_checkpoint_read(stream, game, as_state(&output_state),
                              &output_store, &output_trainer) ==
          CFR_STATUS_SUCCESS);
    CHECK(cfr_info_store_find_const(&output_store, INT64_MIN, &loaded) ==
          CFR_STATUS_SUCCESS);
    if (loaded != NULL) {
        CHECK(loaded->regret_sums[0] == -3.5);
        CHECK(loaded->strategy_sums[0] == 4.5);
    }
    CHECK(cfr_info_store_find_const(&output_store, -1, &loaded) ==
          CFR_STATUS_SUCCESS);
    if (loaded != NULL) {
        CHECK(loaded->regret_sums[0] == 6.5);
        CHECK(loaded->strategy_sums[0] == 7.5);
    }
    CHECK(cfr_info_store_destroy(&source_store) == CFR_STATUS_SUCCESS);
    CHECK(cfr_info_store_destroy(&output_store) == CFR_STATUS_SUCCESS);
    CHECK(fclose(stream) == 0);
}

static void test_validation(void) {
    KuhnPokerState state;
    InfoStore store;
    Trainer trainer;
    Trainer invalid;
    FILE *stream = tmpfile();

    CHECK(stream != NULL);
    if (stream == NULL)
        return;
    initialize_kuhn(&state, &store, &trainer, false);
    invalid = trainer;
    invalid.game = NULL;
    CHECK(cfr_checkpoint_write(stream, &invalid) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(cfr_checkpoint_write(NULL, &trainer) == CFR_STATUS_INVALID_ARGUMENT);
    CHECK(cfr_strategy_write_text(NULL, &trainer) ==
          CFR_STATUS_INVALID_ARGUMENT);
    {
        Game invalid_game = *trainer.game;

        invalid_game.strategy_schema_id = "bad schema";
        invalid.game = &invalid_game;
        CHECK(cfr_checkpoint_write(stream, &invalid) ==
              CFR_STATUS_INVALID_ARGUMENT);
    }
    CHECK(cfr_info_store_destroy(&store) == CFR_STATUS_SUCCESS);
    CHECK(fclose(stream) == 0);
}

#ifdef CFR_TEST_WRAP_ALLOCATOR
static void test_allocation_failures(void) {
    KuhnPokerState source_state;
    KuhnPokerState output_state;
    InfoStore source_store;
    InfoStore output_store = {0};
    Trainer source_trainer;
    Trainer output_trainer = {.training_iterations = 77};
    FILE *stream = tmpfile();
    size_t live_before;

    CHECK(stream != NULL);
    if (stream == NULL)
        return;
    initialize_kuhn(&source_state, &source_store, &source_trainer, false);
    CHECK(cfr_trainer_run(&source_trainer, 2) == CFR_STATUS_SUCCESS);
    live_before = test_allocator_live_allocations();
    test_allocator_fail_after(0);
    CHECK(cfr_checkpoint_write(stream, &source_trainer) ==
          CFR_STATUS_OUT_OF_MEMORY);
    test_allocator_disable_failures();
    CHECK(test_allocator_live_allocations() == live_before);

    rewind(stream);
    CHECK(cfr_checkpoint_write(stream, &source_trainer) == CFR_STATUS_SUCCESS);
    CHECK(fflush(stream) == 0);
    rewind(stream);
    test_allocator_fail_after(0);
    CHECK(cfr_checkpoint_read(stream, cfr_kuhn_poker_descriptor(),
                              as_state(&output_state), &output_store,
                              &output_trainer) == CFR_STATUS_OUT_OF_MEMORY);
    test_allocator_disable_failures();
    CHECK(output_store.entries == NULL);
    CHECK(output_trainer.training_iterations == 77);
    CHECK(test_allocator_live_allocations() == live_before);

    CHECK(cfr_info_store_destroy(&source_store) == CFR_STATUS_SUCCESS);
    CHECK(fclose(stream) == 0);
}
#endif

int test_checkpoint(void) {
    failures = 0;

    test_round_trip_and_exact_continuation(false);
    test_round_trip_and_exact_continuation(true);
    test_rejects_bad_input_transactionally();
    test_text_export_is_sorted_and_policy_only();
    test_signed_keys_round_trip();
    test_validation();
#ifdef CFR_TEST_WRAP_ALLOCATOR
    test_allocation_failures();
    CHECK(test_allocator_live_allocations() == 0);
#endif
    return failures;
}
