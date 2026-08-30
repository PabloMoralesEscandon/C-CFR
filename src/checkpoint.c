#include <float.h>
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cfr/checkpoint.h"
#include "info_store_internal.h"

#define CHECKPOINT_VERSION UINT32_C(1)
#define STRATEGY_TEXT_VERSION 1
#define SCHEMA_ID_MAX_LENGTH 255

static const unsigned char CHECKPOINT_MAGIC[8] = {'C', 'F', 'R', 'C',
                                                   'K', 'P', 'T', '\0'};

typedef struct {
    uint32_t crc;
} Checksum;

static void checksum_init(Checksum *checksum) {
    checksum->crc = UINT32_MAX;
}

static void checksum_update(Checksum *checksum, const unsigned char *bytes,
                            size_t length) {
    for (size_t index = 0; index < length; index += 1) {
        checksum->crc ^= bytes[index];
        for (unsigned int bit = 0; bit < 8; bit += 1) {
            const uint32_t mask =
                (uint32_t)-(int32_t)(checksum->crc & UINT32_C(1));
            checksum->crc =
                (checksum->crc >> 1) ^ (UINT32_C(0xedb88320) & mask);
        }
    }
}

static uint32_t checksum_value(const Checksum *checksum) {
    return ~checksum->crc;
}

static bool binary64_is_supported(void) {
    return sizeof(double) == sizeof(uint64_t) && FLT_RADIX == 2 &&
           DBL_MANT_DIG == 53 && DBL_MAX_EXP == 1024;
}

static bool schema_id_length(const char *schema_id, size_t *length_out) {
    size_t length = 0;

    if (schema_id == NULL || length_out == NULL)
        return false;
    while (schema_id[length] != '\0') {
        const unsigned char value = (unsigned char)schema_id[length];

        if (length == SCHEMA_ID_MAX_LENGTH || value <= 0x20 || value > 0x7e)
            return false;
        length += 1;
    }
    if (length == 0)
        return false;
    *length_out = length;
    return true;
}

static bool size_fits_u64(size_t value) {
    if (sizeof(size_t) <= sizeof(uint64_t))
        return true;
    return value <= (size_t)UINT64_MAX;
}

static bool u64_fits_size(uint64_t value) {
    if (sizeof(size_t) >= sizeof(uint64_t))
        return true;
    return value <= (uint64_t)SIZE_MAX;
}

static Status write_bytes(FILE *stream, Checksum *checksum,
                          const unsigned char *bytes, size_t length) {
    if (length > 0 && fwrite(bytes, 1, length, stream) != length)
        return CFR_STATUS_IO_ERROR;
    checksum_update(checksum, bytes, length);
    return CFR_STATUS_SUCCESS;
}

static Status write_u32(FILE *stream, Checksum *checksum, uint32_t value) {
    unsigned char bytes[4];

    for (size_t index = 0; index < sizeof(bytes); index += 1)
        bytes[index] = (unsigned char)(value >> (index * 8));
    return write_bytes(stream, checksum, bytes, sizeof(bytes));
}

static Status write_u64(FILE *stream, Checksum *checksum, uint64_t value) {
    unsigned char bytes[8];

    for (size_t index = 0; index < sizeof(bytes); index += 1)
        bytes[index] = (unsigned char)(value >> (index * 8));
    return write_bytes(stream, checksum, bytes, sizeof(bytes));
}

static Status write_checksum(FILE *stream, uint32_t value) {
    unsigned char bytes[4];

    for (size_t index = 0; index < sizeof(bytes); index += 1)
        bytes[index] = (unsigned char)(value >> (index * 8));
    if (fwrite(bytes, 1, sizeof(bytes), stream) != sizeof(bytes))
        return CFR_STATUS_IO_ERROR;
    return CFR_STATUS_SUCCESS;
}

static Status read_bytes(FILE *stream, Checksum *checksum,
                         unsigned char *bytes, size_t length) {
    if (length > 0 && fread(bytes, 1, length, stream) != length) {
        return ferror(stream) ? CFR_STATUS_IO_ERROR : CFR_STATUS_FORMAT_ERROR;
    }
    checksum_update(checksum, bytes, length);
    return CFR_STATUS_SUCCESS;
}

static Status read_u32(FILE *stream, Checksum *checksum, uint32_t *value_out) {
    unsigned char bytes[4];
    Status status = read_bytes(stream, checksum, bytes, sizeof(bytes));
    uint32_t value = 0;

    if (status != CFR_STATUS_SUCCESS)
        return status;
    for (size_t index = 0; index < sizeof(bytes); index += 1)
        value |= (uint32_t)bytes[index] << (index * 8);
    *value_out = value;
    return CFR_STATUS_SUCCESS;
}

static Status read_u64(FILE *stream, Checksum *checksum, uint64_t *value_out) {
    unsigned char bytes[8];
    Status status = read_bytes(stream, checksum, bytes, sizeof(bytes));
    uint64_t value = 0;

    if (status != CFR_STATUS_SUCCESS)
        return status;
    for (size_t index = 0; index < sizeof(bytes); index += 1)
        value |= (uint64_t)bytes[index] << (index * 8);
    *value_out = value;
    return CFR_STATUS_SUCCESS;
}

static Status read_checksum(FILE *stream, uint32_t *value_out) {
    unsigned char bytes[4];
    uint32_t value = 0;

    if (fread(bytes, 1, sizeof(bytes), stream) != sizeof(bytes))
        return ferror(stream) ? CFR_STATUS_IO_ERROR : CFR_STATUS_FORMAT_ERROR;
    for (size_t index = 0; index < sizeof(bytes); index += 1)
        value |= (uint32_t)bytes[index] << (index * 8);
    *value_out = value;
    return CFR_STATUS_SUCCESS;
}

static int compare_nodes(const void *left_pointer, const void *right_pointer) {
    const InfoNode *left = *(const InfoNode *const *)left_pointer;
    const InfoNode *right = *(const InfoNode *const *)right_pointer;

    if (left->key < right->key)
        return -1;
    if (left->key > right->key)
        return 1;
    return 0;
}

static Status validate_node(const InfoNode *node, size_t max_legal_actions,
                            TrainerVariant variant) {
    if (node == NULL || node->regret_sums == NULL ||
        node->strategy_sums == NULL || node->action_count == 0 ||
        node->action_count > max_legal_actions ||
        node->action_count > SIZE_MAX / sizeof(Probability) ||
        !size_fits_u64(node->action_count)) {
        return CFR_STATUS_INVALID_ARGUMENT;
    }
    for (size_t index = 0; index < node->action_count; index += 1) {
        if (!isfinite(node->regret_sums[index]) ||
            !isfinite(node->strategy_sums[index]) ||
            node->strategy_sums[index] < 0.0) {
            return CFR_STATUS_NUMERIC_ERROR;
        }
        if (variant == CFR_TRAINER_VARIANT_CFR_PLUS &&
            node->regret_sums[index] < 0.0) {
            return CFR_STATUS_NUMERIC_ERROR;
        }
    }
    return CFR_STATUS_SUCCESS;
}

static Status collect_nodes(const Trainer *trainer, const InfoNode ***nodes_out,
                            size_t *count_out) {
    const InfoStore *store;
    const InfoNode **nodes = NULL;
    size_t count = 0;

    if (trainer == NULL || trainer->game == NULL || trainer->state == NULL ||
        trainer->store == NULL || trainer->store->entries == NULL ||
        trainer->store->capacity == 0 || trainer->game->max_legal_actions == 0 ||
        nodes_out == NULL || count_out == NULL ||
        (trainer->variant != CFR_TRAINER_VARIANT_CFR &&
         trainer->variant != CFR_TRAINER_VARIANT_CFR_PLUS) ||
        !size_fits_u64(trainer->training_iterations) ||
        !size_fits_u64(trainer->stats.iterations) ||
        !size_fits_u64(trainer->stats.traversals) ||
        !size_fits_u64(trainer->stats.visited_nodes) ||
        !size_fits_u64(trainer->stats.errors)) {
        return CFR_STATUS_INVALID_ARGUMENT;
    }
    store = trainer->store;
    if (!size_fits_u64(store->size) ||
        (store->size > 0 &&
         store->size > SIZE_MAX / sizeof(const InfoNode *))) {
        return CFR_STATUS_INVALID_ARGUMENT;
    }
    if (store->size > 0) {
        nodes = malloc(store->size * sizeof(*nodes));
        if (nodes == NULL)
            return CFR_STATUS_OUT_OF_MEMORY;
    }
    for (size_t index = 0; index < store->capacity; index += 1) {
        const InfoNode *node = store->entries[index].node;
        Status status;

        if (node == NULL)
            continue;
        if (count == store->size) {
            free(nodes);
            return CFR_STATUS_INVALID_ARGUMENT;
        }
        status = validate_node(node, trainer->game->max_legal_actions,
                               trainer->variant);
        if (status != CFR_STATUS_SUCCESS) {
            free(nodes);
            return status;
        }
        nodes[count] = node;
        count += 1;
    }
    if (count != store->size) {
        free(nodes);
        return CFR_STATUS_INVALID_ARGUMENT;
    }
    if (count > 1)
        qsort(nodes, count, sizeof(*nodes), compare_nodes);
    for (size_t index = 1; index < count; index += 1) {
        if (nodes[index - 1]->key == nodes[index]->key) {
            free(nodes);
            return CFR_STATUS_INVALID_ARGUMENT;
        }
    }
    *nodes_out = nodes;
    *count_out = count;
    return CFR_STATUS_SUCCESS;
}

static uint64_t double_bits(double value) {
    uint64_t bits;

    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static double bits_double(uint64_t bits) {
    double value;

    memcpy(&value, &bits, sizeof(value));
    return value;
}

static InfoSetKey decode_key(uint64_t encoded) {
    if (encoded <= (uint64_t)INT64_MAX)
        return (InfoSetKey)encoded;
    return (InfoSetKey)(-((int64_t)(UINT64_MAX - encoded)) - INT64_C(1));
}

Status cfr_checkpoint_write(FILE *stream, const Trainer *trainer) {
    const InfoNode **nodes = NULL;
    const char *schema_id;
    size_t schema_length;
    size_t node_count = 0;
    Checksum checksum;
    Status status;

    if (stream == NULL || trainer == NULL || trainer->game == NULL)
        return CFR_STATUS_INVALID_ARGUMENT;
    if (!binary64_is_supported())
        return CFR_STATUS_FORMAT_ERROR;
    schema_id = trainer->game->strategy_schema_id;
    if (!schema_id_length(schema_id, &schema_length))
        return CFR_STATUS_INVALID_ARGUMENT;
    status = collect_nodes(trainer, &nodes, &node_count);
    if (status != CFR_STATUS_SUCCESS)
        return status;

    checksum_init(&checksum);
#define WRITE_OR_CLEAN(expression)                                             \
    do {                                                                       \
        status = (expression);                                                 \
        if (status != CFR_STATUS_SUCCESS)                                      \
            goto cleanup;                                                      \
    } while (0)

    WRITE_OR_CLEAN(write_bytes(stream, &checksum, CHECKPOINT_MAGIC,
                               sizeof(CHECKPOINT_MAGIC)));
    WRITE_OR_CLEAN(write_u32(stream, &checksum, CHECKPOINT_VERSION));
    WRITE_OR_CLEAN(write_u32(stream, &checksum, (uint32_t)schema_length));
    WRITE_OR_CLEAN(write_bytes(stream, &checksum,
                               (const unsigned char *)schema_id, schema_length));
    WRITE_OR_CLEAN(write_u32(stream, &checksum, (uint32_t)trainer->variant));
    WRITE_OR_CLEAN(write_u64(stream, &checksum,
                             (uint64_t)trainer->training_iterations));
    WRITE_OR_CLEAN(
        write_u64(stream, &checksum, (uint64_t)trainer->stats.iterations));
    WRITE_OR_CLEAN(
        write_u64(stream, &checksum, (uint64_t)trainer->stats.traversals));
    WRITE_OR_CLEAN(
        write_u64(stream, &checksum, (uint64_t)trainer->stats.visited_nodes));
    WRITE_OR_CLEAN(
        write_u64(stream, &checksum, (uint64_t)trainer->stats.errors));
    WRITE_OR_CLEAN(write_u64(stream, &checksum, (uint64_t)node_count));

    for (size_t node_index = 0; node_index < node_count; node_index += 1) {
        const InfoNode *node = nodes[node_index];

        WRITE_OR_CLEAN(write_u64(stream, &checksum, (uint64_t)node->key));
        WRITE_OR_CLEAN(
            write_u64(stream, &checksum, (uint64_t)node->action_count));
        for (size_t action = 0; action < node->action_count; action += 1) {
            WRITE_OR_CLEAN(write_u64(
                stream, &checksum, double_bits(node->regret_sums[action])));
        }
        for (size_t action = 0; action < node->action_count; action += 1) {
            WRITE_OR_CLEAN(write_u64(
                stream, &checksum, double_bits(node->strategy_sums[action])));
        }
    }
    status = write_checksum(stream, checksum_value(&checksum));

cleanup:
    free(nodes);
#undef WRITE_OR_CLEAN
    return status;
}

static Status read_size(FILE *stream, Checksum *checksum, size_t *value_out) {
    uint64_t encoded;
    Status status = read_u64(stream, checksum, &encoded);

    if (status != CFR_STATUS_SUCCESS)
        return status;
    if (!u64_fits_size(encoded))
        return CFR_STATUS_FORMAT_ERROR;
    *value_out = (size_t)encoded;
    return CFR_STATUS_SUCCESS;
}

static Status read_header(FILE *stream, const Game *game, Checksum *checksum,
                          TrainerVariant *variant_out,
                          size_t *training_iterations_out,
                          TrainerStats *stats_out, size_t *node_count_out) {
    unsigned char magic[sizeof(CHECKPOINT_MAGIC)];
    unsigned char schema[SCHEMA_ID_MAX_LENGTH];
    uint32_t version;
    uint32_t schema_length;
    uint32_t variant;
    size_t expected_schema_length;
    Status status;

#define READ_OR_RETURN(expression)                                             \
    do {                                                                       \
        status = (expression);                                                 \
        if (status != CFR_STATUS_SUCCESS)                                      \
            return status;                                                     \
    } while (0)

    READ_OR_RETURN(read_bytes(stream, checksum, magic, sizeof(magic)));
    if (memcmp(magic, CHECKPOINT_MAGIC, sizeof(magic)) != 0)
        return CFR_STATUS_FORMAT_ERROR;
    READ_OR_RETURN(read_u32(stream, checksum, &version));
    if (version != CHECKPOINT_VERSION)
        return CFR_STATUS_FORMAT_ERROR;
    READ_OR_RETURN(read_u32(stream, checksum, &schema_length));
    if (schema_length == 0 || schema_length > SCHEMA_ID_MAX_LENGTH)
        return CFR_STATUS_FORMAT_ERROR;
    READ_OR_RETURN(read_bytes(stream, checksum, schema, schema_length));
    if (!schema_id_length(game->strategy_schema_id, &expected_schema_length))
        return CFR_STATUS_INVALID_ARGUMENT;
    if (schema_length != expected_schema_length ||
        memcmp(schema, game->strategy_schema_id, schema_length) != 0) {
        return CFR_STATUS_INCOMPATIBLE_GAME;
    }
    READ_OR_RETURN(read_u32(stream, checksum, &variant));
    if (variant != (uint32_t)CFR_TRAINER_VARIANT_CFR &&
        variant != (uint32_t)CFR_TRAINER_VARIANT_CFR_PLUS) {
        return CFR_STATUS_FORMAT_ERROR;
    }
    *variant_out = (TrainerVariant)variant;
    READ_OR_RETURN(read_size(stream, checksum, training_iterations_out));
    READ_OR_RETURN(read_size(stream, checksum, &stats_out->iterations));
    READ_OR_RETURN(read_size(stream, checksum, &stats_out->traversals));
    READ_OR_RETURN(read_size(stream, checksum, &stats_out->visited_nodes));
    READ_OR_RETURN(read_size(stream, checksum, &stats_out->errors));
    READ_OR_RETURN(read_size(stream, checksum, node_count_out));
#undef READ_OR_RETURN
    return CFR_STATUS_SUCCESS;
}

static Status read_node(FILE *stream, Checksum *checksum, const Game *game,
                        TrainerVariant variant, InfoStore *store) {
    uint64_t encoded_key;
    size_t action_count;
    InfoNode *node = NULL;
    const InfoNode *existing = NULL;
    Status status;

    status = read_u64(stream, checksum, &encoded_key);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    status = read_size(stream, checksum, &action_count);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    if (action_count == 0 || action_count > game->max_legal_actions)
        return CFR_STATUS_FORMAT_ERROR;
    const InfoSetKey key = decode_key(encoded_key);

    status = cfr_info_store_find_const(store, key, &existing);
    if (status == CFR_STATUS_SUCCESS)
        return CFR_STATUS_FORMAT_ERROR;
    if (status != CFR_STATUS_NOT_FOUND)
        return CFR_STATUS_FORMAT_ERROR;
    status =
        cfr_info_store_get_or_create(store, key, action_count, &node);
    if (status != CFR_STATUS_SUCCESS)
        return status;

    for (size_t action = 0; action < action_count; action += 1) {
        uint64_t bits;
        double value;

        status = read_u64(stream, checksum, &bits);
        if (status != CFR_STATUS_SUCCESS)
            return status;
        value = bits_double(bits);
        if (!isfinite(value) ||
            (variant == CFR_TRAINER_VARIANT_CFR_PLUS && value < 0.0)) {
            return CFR_STATUS_FORMAT_ERROR;
        }
        node->regret_sums[action] = value;
    }
    for (size_t action = 0; action < action_count; action += 1) {
        uint64_t bits;
        double value;

        status = read_u64(stream, checksum, &bits);
        if (status != CFR_STATUS_SUCCESS)
            return status;
        value = bits_double(bits);
        if (!isfinite(value) || value < 0.0)
            return CFR_STATUS_FORMAT_ERROR;
        node->strategy_sums[action] = value;
    }
    return CFR_STATUS_SUCCESS;
}

Status cfr_checkpoint_read(FILE *stream, const Game *game, GameState *state,
                           InfoStore *store_out, Trainer *trainer_out) {
    InfoStore temporary_store = {0};
    TrainerStats stats = {0};
    TrainerVariant variant = CFR_TRAINER_VARIANT_CFR;
    size_t training_iterations = 0;
    size_t node_count = 0;
    uint32_t stored_checksum;
    Checksum checksum;
    bool store_initialized = false;
    Status status;

    if (stream == NULL || game == NULL || state == NULL || store_out == NULL ||
        trainer_out == NULL || store_out->entries != NULL ||
        store_out->size != 0 || store_out->capacity != 0 ||
        store_out->collision_count != 0 || store_out->growth_count != 0 ||
        game->max_legal_actions == 0)
        return CFR_STATUS_INVALID_ARGUMENT;
    if (!binary64_is_supported())
        return CFR_STATUS_FORMAT_ERROR;
    checksum_init(&checksum);
    status = read_header(stream, game, &checksum, &variant,
                         &training_iterations, &stats, &node_count);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    status = cfr_info_store_init(&temporary_store);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    store_initialized = true;
    for (size_t index = 0; index < node_count; index += 1) {
        status = read_node(stream, &checksum, game, variant, &temporary_store);
        if (status != CFR_STATUS_SUCCESS)
            goto cleanup;
    }
    status = read_checksum(stream, &stored_checksum);
    if (status != CFR_STATUS_SUCCESS)
        goto cleanup;
    if (stored_checksum != checksum_value(&checksum)) {
        status = CFR_STATUS_FORMAT_ERROR;
        goto cleanup;
    }
    {
        const int trailing = fgetc(stream);

        if (trailing != EOF) {
            status = CFR_STATUS_FORMAT_ERROR;
            goto cleanup;
        }
        if (ferror(stream)) {
            status = CFR_STATUS_IO_ERROR;
            goto cleanup;
        }
    }
    *store_out = temporary_store;
    store_initialized = false;
    *trainer_out = (Trainer){.game = game,
                             .state = state,
                             .store = store_out,
                             .variant = variant,
                             .training_iterations = training_iterations,
                             .stats = stats};
    return CFR_STATUS_SUCCESS;

cleanup:
    if (store_initialized)
        (void)cfr_info_store_destroy(&temporary_store);
    return status;
}

static const char *variant_name(TrainerVariant variant) {
    return variant == CFR_TRAINER_VARIANT_CFR ? "cfr" : "cfr-plus";
}

Status cfr_strategy_write_text(FILE *stream, const Trainer *trainer) {
    const InfoNode **nodes = NULL;
    const char *schema_id;
    size_t schema_length;
    size_t node_count = 0;
    Probability *strategy = NULL;
    size_t strategy_capacity = 0;
    Status status;

    if (stream == NULL || trainer == NULL || trainer->game == NULL)
        return CFR_STATUS_INVALID_ARGUMENT;
    schema_id = trainer->game->strategy_schema_id;
    if (!schema_id_length(schema_id, &schema_length))
        return CFR_STATUS_INVALID_ARGUMENT;
    (void)schema_length;
    status = collect_nodes(trainer, &nodes, &node_count);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    for (size_t index = 0; index < node_count; index += 1) {
        if (nodes[index]->action_count > strategy_capacity)
            strategy_capacity = nodes[index]->action_count;
    }
    if (strategy_capacity > 0) {
        strategy = malloc(strategy_capacity * sizeof(*strategy));
        if (strategy == NULL) {
            free(nodes);
            return CFR_STATUS_OUT_OF_MEMORY;
        }
    }
    if (fprintf(stream,
                "cfr-strategy version=%d schema=%s variant=%s "
                "training_iterations=%zu information_sets=%zu\n",
                STRATEGY_TEXT_VERSION, schema_id,
                variant_name(trainer->variant), trainer->training_iterations,
                node_count) < 0) {
        status = CFR_STATUS_IO_ERROR;
        goto cleanup;
    }
    for (size_t node_index = 0; node_index < node_count; node_index += 1) {
        const InfoNode *node = nodes[node_index];

        status = cfr_info_node_average_strategy(node, strategy,
                                                strategy_capacity);
        if (status != CFR_STATUS_SUCCESS)
            goto cleanup;
        if (fprintf(stream, "infoset key=%" PRId64 " actions=%zu",
                    node->key, node->action_count) < 0) {
            status = CFR_STATUS_IO_ERROR;
            goto cleanup;
        }
        for (size_t action = 0; action < node->action_count; action += 1) {
            if (fprintf(stream, " action_%zu=%.17g", action,
                        strategy[action]) < 0) {
                status = CFR_STATUS_IO_ERROR;
                goto cleanup;
            }
        }
        if (fputc('\n', stream) == EOF) {
            status = CFR_STATUS_IO_ERROR;
            goto cleanup;
        }
    }
    status = CFR_STATUS_SUCCESS;

cleanup:
    free(strategy);
    free(nodes);
    return status;
}
