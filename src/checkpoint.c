#if (defined(__unix__) || defined(__APPLE__)) && \
    !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include <float.h>
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif

#include "cfr/checkpoint.h"
#include "info_store_internal.h"

#define CHECKPOINT_VERSION UINT32_C(1)
#define STRATEGY_TEXT_VERSION 1
#define SCHEMA_ID_MAX_LENGTH ((size_t)255)

static const unsigned char CHECKPOINT_MAGIC[8] = {'C', 'F', 'R', 'C',
                                                   'K', 'P', 'T', '\0'};

typedef struct {
    uint32_t crc;
} Checksum;

/* Generated from the standard reflected CRC-32 polynomial 0xedb88320. */
static const uint32_t CHECKSUM_TABLE[256] = {
    0x00000000U, 0x77073096U, 0xee0e612cU, 0x990951baU, 0x076dc419U,
    0x706af48fU, 0xe963a535U, 0x9e6495a3U, 0x0edb8832U, 0x79dcb8a4U,
    0xe0d5e91eU, 0x97d2d988U, 0x09b64c2bU, 0x7eb17cbdU, 0xe7b82d07U,
    0x90bf1d91U, 0x1db71064U, 0x6ab020f2U, 0xf3b97148U, 0x84be41deU,
    0x1adad47dU, 0x6ddde4ebU, 0xf4d4b551U, 0x83d385c7U, 0x136c9856U,
    0x646ba8c0U, 0xfd62f97aU, 0x8a65c9ecU, 0x14015c4fU, 0x63066cd9U,
    0xfa0f3d63U, 0x8d080df5U, 0x3b6e20c8U, 0x4c69105eU, 0xd56041e4U,
    0xa2677172U, 0x3c03e4d1U, 0x4b04d447U, 0xd20d85fdU, 0xa50ab56bU,
    0x35b5a8faU, 0x42b2986cU, 0xdbbbc9d6U, 0xacbcf940U, 0x32d86ce3U,
    0x45df5c75U, 0xdcd60dcfU, 0xabd13d59U, 0x26d930acU, 0x51de003aU,
    0xc8d75180U, 0xbfd06116U, 0x21b4f4b5U, 0x56b3c423U, 0xcfba9599U,
    0xb8bda50fU, 0x2802b89eU, 0x5f058808U, 0xc60cd9b2U, 0xb10be924U,
    0x2f6f7c87U, 0x58684c11U, 0xc1611dabU, 0xb6662d3dU, 0x76dc4190U,
    0x01db7106U, 0x98d220bcU, 0xefd5102aU, 0x71b18589U, 0x06b6b51fU,
    0x9fbfe4a5U, 0xe8b8d433U, 0x7807c9a2U, 0x0f00f934U, 0x9609a88eU,
    0xe10e9818U, 0x7f6a0dbbU, 0x086d3d2dU, 0x91646c97U, 0xe6635c01U,
    0x6b6b51f4U, 0x1c6c6162U, 0x856530d8U, 0xf262004eU, 0x6c0695edU,
    0x1b01a57bU, 0x8208f4c1U, 0xf50fc457U, 0x65b0d9c6U, 0x12b7e950U,
    0x8bbeb8eaU, 0xfcb9887cU, 0x62dd1ddfU, 0x15da2d49U, 0x8cd37cf3U,
    0xfbd44c65U, 0x4db26158U, 0x3ab551ceU, 0xa3bc0074U, 0xd4bb30e2U,
    0x4adfa541U, 0x3dd895d7U, 0xa4d1c46dU, 0xd3d6f4fbU, 0x4369e96aU,
    0x346ed9fcU, 0xad678846U, 0xda60b8d0U, 0x44042d73U, 0x33031de5U,
    0xaa0a4c5fU, 0xdd0d7cc9U, 0x5005713cU, 0x270241aaU, 0xbe0b1010U,
    0xc90c2086U, 0x5768b525U, 0x206f85b3U, 0xb966d409U, 0xce61e49fU,
    0x5edef90eU, 0x29d9c998U, 0xb0d09822U, 0xc7d7a8b4U, 0x59b33d17U,
    0x2eb40d81U, 0xb7bd5c3bU, 0xc0ba6cadU, 0xedb88320U, 0x9abfb3b6U,
    0x03b6e20cU, 0x74b1d29aU, 0xead54739U, 0x9dd277afU, 0x04db2615U,
    0x73dc1683U, 0xe3630b12U, 0x94643b84U, 0x0d6d6a3eU, 0x7a6a5aa8U,
    0xe40ecf0bU, 0x9309ff9dU, 0x0a00ae27U, 0x7d079eb1U, 0xf00f9344U,
    0x8708a3d2U, 0x1e01f268U, 0x6906c2feU, 0xf762575dU, 0x806567cbU,
    0x196c3671U, 0x6e6b06e7U, 0xfed41b76U, 0x89d32be0U, 0x10da7a5aU,
    0x67dd4accU, 0xf9b9df6fU, 0x8ebeeff9U, 0x17b7be43U, 0x60b08ed5U,
    0xd6d6a3e8U, 0xa1d1937eU, 0x38d8c2c4U, 0x4fdff252U, 0xd1bb67f1U,
    0xa6bc5767U, 0x3fb506ddU, 0x48b2364bU, 0xd80d2bdaU, 0xaf0a1b4cU,
    0x36034af6U, 0x41047a60U, 0xdf60efc3U, 0xa867df55U, 0x316e8eefU,
    0x4669be79U, 0xcb61b38cU, 0xbc66831aU, 0x256fd2a0U, 0x5268e236U,
    0xcc0c7795U, 0xbb0b4703U, 0x220216b9U, 0x5505262fU, 0xc5ba3bbeU,
    0xb2bd0b28U, 0x2bb45a92U, 0x5cb36a04U, 0xc2d7ffa7U, 0xb5d0cf31U,
    0x2cd99e8bU, 0x5bdeae1dU, 0x9b64c2b0U, 0xec63f226U, 0x756aa39cU,
    0x026d930aU, 0x9c0906a9U, 0xeb0e363fU, 0x72076785U, 0x05005713U,
    0x95bf4a82U, 0xe2b87a14U, 0x7bb12baeU, 0x0cb61b38U, 0x92d28e9bU,
    0xe5d5be0dU, 0x7cdcefb7U, 0x0bdbdf21U, 0x86d3d2d4U, 0xf1d4e242U,
    0x68ddb3f8U, 0x1fda836eU, 0x81be16cdU, 0xf6b9265bU, 0x6fb077e1U,
    0x18b74777U, 0x88085ae6U, 0xff0f6a70U, 0x66063bcaU, 0x11010b5cU,
    0x8f659effU, 0xf862ae69U, 0x616bffd3U, 0x166ccf45U, 0xa00ae278U,
    0xd70dd2eeU, 0x4e048354U, 0x3903b3c2U, 0xa7672661U, 0xd06016f7U,
    0x4969474dU, 0x3e6e77dbU, 0xaed16a4aU, 0xd9d65adcU, 0x40df0b66U,
    0x37d83bf0U, 0xa9bcae53U, 0xdebb9ec5U, 0x47b2cf7fU, 0x30b5ffe9U,
    0xbdbdf21cU, 0xcabac28aU, 0x53b39330U, 0x24b4a3a6U, 0xbad03605U,
    0xcdd70693U, 0x54de5729U, 0x23d967bfU, 0xb3667a2eU, 0xc4614ab8U,
    0x5d681b02U, 0x2a6f2b94U, 0xb40bbe37U, 0xc30c8ea1U, 0x5a05df1bU,
    0x2d02ef8dU,
};

/* POSIX stdio locks are recursive; acquire once across each complete stream
 * operation instead of once for every small fread/fwrite/fprintf call. */
static void stream_lock(FILE *stream) {
#if defined(_POSIX_VERSION)
    flockfile(stream);
#else
    (void)stream;
#endif
}

static void stream_unlock(FILE *stream) {
#if defined(_POSIX_VERSION)
    funlockfile(stream);
#else
    (void)stream;
#endif
}

static void checksum_init(Checksum *checksum) {
    checksum->crc = UINT32_MAX;
}

static void checksum_update(Checksum *checksum, const unsigned char *bytes,
                            size_t length) {
    for (size_t index = 0; index < length; index += 1) {
        const uint8_t table_index =
            (uint8_t)(checksum->crc ^ bytes[index]);
        checksum->crc =
            (checksum->crc >> 8) ^ CHECKSUM_TABLE[table_index];
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
         trainer->variant != CFR_TRAINER_VARIANT_CFR_PLUS &&
         trainer->variant != CFR_TRAINER_VARIANT_MCCFR_EXTERNAL) ||
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

    stream_lock(stream);
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
    if (trainer->variant == CFR_TRAINER_VARIANT_MCCFR_EXTERNAL) {
        WRITE_OR_CLEAN(
            write_u64(stream, &checksum, trainer->mccfr_rng.state));
    }
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
    stream_unlock(stream);
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
                          MccfrRng *mccfr_rng_out,
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
        variant != (uint32_t)CFR_TRAINER_VARIANT_CFR_PLUS &&
        variant != (uint32_t)CFR_TRAINER_VARIANT_MCCFR_EXTERNAL) {
        return CFR_STATUS_FORMAT_ERROR;
    }
    *variant_out = (TrainerVariant)variant;
    mccfr_rng_out->state = 0;
    if (variant == (uint32_t)CFR_TRAINER_VARIANT_MCCFR_EXTERNAL) {
        READ_OR_RETURN(read_u64(stream, checksum, &mccfr_rng_out->state));
    }
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
    MccfrRng mccfr_rng = {0};
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
    stream_lock(stream);
    checksum_init(&checksum);
    status = read_header(stream, game, &checksum, &variant, &mccfr_rng,
                         &training_iterations, &stats, &node_count);
    if (status != CFR_STATUS_SUCCESS)
        goto cleanup;
    status = cfr_info_store_init(&temporary_store);
    if (status != CFR_STATUS_SUCCESS)
        goto cleanup;
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
                             .mccfr_rng = mccfr_rng,
                             .stats = stats};
    status = CFR_STATUS_SUCCESS;

cleanup:
    if (store_initialized)
        (void)cfr_info_store_destroy(&temporary_store);
    stream_unlock(stream);
    return status;
}

static const char *variant_name(TrainerVariant variant) {
    if (variant == CFR_TRAINER_VARIANT_CFR)
        return "cfr";
    if (variant == CFR_TRAINER_VARIANT_CFR_PLUS)
        return "cfr-plus";
    return "mccfr-external";
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
    stream_lock(stream);
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
    stream_unlock(stream);
    free(strategy);
    free(nodes);
    return status;
}
