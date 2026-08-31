/*
 * Exports the complete Kuhn Poker or Leduc Poker decision tree as JSON.
 *
 * The tool is not part of the library or its applications. It reads a trained
 * average strategy, walks the whole game tree through the public cfr_game_*
 * operations, and writes one JSON document that tools/poker_tree_view.html
 * renders as an interactive graph.
 *
 * Every node carries its reach probability under the average-strategy profile
 * and the expected utility of player zero below it. Every edge carries the
 * probability the acting entity gives to that action: a chance probability at
 * a deal node, and an average-strategy probability at a player node.
 */

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfr/checkpoint.h"
#include "cfr/evaluation.h"
#include "cfr/game.h"
#include "cfr/info_store.h"
#include "cfr/kuhn_poker.h"
#include "cfr/leduc_poker.h"
#include "cfr/trainer.h"

/* The Leduc private deal has the largest action set of the supported games. */
#define TREE_MAX_ACTIONS CFR_LEDUC_POKER_MAX_POSSIBLE_ACTIONS
/* Holds one composed label, such as an information-set description. */
#define TREE_LABEL_CAPACITY 160
/* Holds one line of a text strategy export. */
#define TREE_LINE_CAPACITY 1024
/* Guards against a runaway walk if an adapter ever stops terminating. */
#define TREE_MAX_DEPTH 64

typedef enum {
    TREE_EXIT_SUCCESS = 0,
    TREE_EXIT_RUNTIME_ERROR = 1,
    TREE_EXIT_USAGE_ERROR = 2
} TreeExitCode;

typedef enum { TREE_PARSE_READY, TREE_PARSE_HELP, TREE_PARSE_ERROR } TreeParse;

typedef enum { TREE_GAME_KUHN, TREE_GAME_LEDUC } TreeGameId;

typedef struct {
    TreeGameId game_id;
    const char *load_path;
    const char *strategy_path;
    const char *output_path;
} TreeOptions;

/* Holds one information set read from a text strategy export. */
typedef struct {
    InfoSetKey key;
    size_t action_count;
    Probability probabilities[TREE_MAX_ACTIONS];
} TextStrategyRow;

/*
 * Supplies average strategies from whichever artifact the caller provided.
 *
 * At most one of store and rows is populated. With neither, every player node
 * falls back to a uniform strategy, which still shows the tree shape.
 */
typedef struct {
    const InfoStore *store;
    const TextStrategyRow *rows;
    size_t row_count;
    const char *variant;
    size_t training_iterations;
    const char *origin;
} StrategySource;

/* Adapts one concrete game to the game-independent walk below. */
typedef struct {
    const char *id;
    const Game *game;
    GameState *state;
    const GameState *const_state;
    /* Writes the members of the "detail" object, without the braces. */
    Status (*write_detail)(FILE *stream, const void *storage);
    /* Composes the short label shown on the node itself. */
    Status (*node_label)(const void *storage, bool terminal, Actor actor,
                         char *buffer, size_t capacity);
    /* Composes the label of the information set the current actor observes. */
    Status (*infoset_label)(const void *storage, char *buffer, size_t capacity);
    const char *(*action_name)(Action action);
    const void *storage;
} TreeGame;

/* Carries the state shared by every step of the recursive walk. */
typedef struct {
    FILE *stream;
    const TreeGame *tree_game;
    const StrategySource *strategy;
    size_t node_count;
    size_t player_node_count;
    size_t terminal_count;
    size_t deepest_level;
    bool missing_information_set;
} TreeWriter;

static bool print_usage(FILE *stream, const char *program_name) {
    return fprintf(stream,
                   "Usage: %s --game kuhn|leduc [--load FILE] "
                   "[--strategy FILE] [--output FILE]\n"
                   "       %s --help\n"
                   "\n"
                   "Options:\n"
                   "  --game NAME      Game to walk: kuhn or leduc.\n"
                   "  --load FILE      Read the average strategy from a "
                   "binary checkpoint.\n"
                   "  --strategy FILE  Read the average strategy from an "
                   "--export-strategy text file.\n"
                   "  --output FILE    Write the JSON tree to FILE instead of "
                   "standard output.\n"
                   "  --help, -h       Display this help.\n"
                   "\n"
                   "Without --load or --strategy the walk uses a uniform "
                   "strategy and shows the tree shape only.\n",
                   program_name, program_name) >= 0;
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

static void report_status(const char *action, Status status) {
    (void)fprintf(stderr, "error: could not %s (%s)\n", action,
                  status_name(status));
}

/* Appends text to buffer and reports whether everything still fits. */
static bool append_text(char *buffer, size_t capacity, size_t *length,
                        const char *text) {
    size_t text_length;

    if (buffer == NULL || length == NULL || text == NULL)
        return false;
    text_length = strlen(text);
    if (text_length + 1 > capacity - *length)
        return false;
    memcpy(buffer + *length, text, text_length + 1);
    *length += text_length;
    return true;
}

/* ---------------------------------------------------------------- Kuhn --- */

static const char *kuhn_card_symbol(KuhnPokerCard card) {
    switch (card) {
    case CFR_KUHN_POKER_CARD_JACK:
        return "J";
    case CFR_KUHN_POKER_CARD_QUEEN:
        return "Q";
    case CFR_KUHN_POKER_CARD_KING:
        return "K";
    case CFR_KUHN_POKER_CARD_NOT_DEALT:
        return "-";
    default:
        return NULL;
    }
}

static const char *kuhn_action_name(Action action) {
    switch ((KuhnPokerAction)action) {
    case CFR_KUHN_POKER_ACTION_JQ:
        return "JQ";
    case CFR_KUHN_POKER_ACTION_JK:
        return "JK";
    case CFR_KUHN_POKER_ACTION_QJ:
        return "QJ";
    case CFR_KUHN_POKER_ACTION_QK:
        return "QK";
    case CFR_KUHN_POKER_ACTION_KJ:
        return "KJ";
    case CFR_KUHN_POKER_ACTION_KQ:
        return "KQ";
    case CFR_KUHN_POKER_ACTION_BET:
        return "bet";
    case CFR_KUHN_POKER_ACTION_FOLD:
        return "fold";
    case CFR_KUHN_POKER_ACTION_CALL:
        return "call";
    case CFR_KUHN_POKER_ACTION_CHECK:
        return "check";
    case CFR_KUHN_POKER_ACTION_NONE:
    default:
        return NULL;
    }
}

static const char *kuhn_phase_name(KuhnPokerPhase phase) {
    switch (phase) {
    case CFR_KUHN_POKER_PHASE_CHANCE:
        return "deal";
    case CFR_KUHN_POKER_PHASE_PLAYER_0_OPEN:
        return "player_0_open";
    case CFR_KUHN_POKER_PHASE_PLAYER_1_AFTER_CHECK:
        return "player_1_after_check";
    case CFR_KUHN_POKER_PHASE_PLAYER_1_FACING_OPEN_BET:
        return "player_1_facing_bet";
    case CFR_KUHN_POKER_PHASE_PLAYER_0_FACING_CHECK_BET:
        return "player_0_facing_bet";
    case CFR_KUHN_POKER_PHASE_TERMINAL:
        return "terminal";
    default:
        return NULL;
    }
}

/* Reconstructs the pot from the antes and the visible betting actions. */
static int kuhn_pot(const KuhnPokerState *state) {
    int pot = 2;
    size_t index;

    for (index = 0; index < state->public_action_count; index++) {
        if (state->public_actions[index] == CFR_KUHN_POKER_ACTION_BET ||
            state->public_actions[index] == CFR_KUHN_POKER_ACTION_CALL) {
            pot += 1;
        }
    }
    return pot;
}

/* Writes the betting actions of a Kuhn state as a JSON array. */
static Status kuhn_write_history(FILE *stream, const KuhnPokerState *state) {
    size_t index;

    if (fputc('[', stream) == EOF)
        return CFR_STATUS_IO_ERROR;
    for (index = 0; index < state->public_action_count; index++) {
        const char *name = kuhn_action_name(state->public_actions[index]);

        if (name == NULL)
            return CFR_STATUS_INVALID_ARGUMENT;
        if (fprintf(stream, "%s\"%s\"", index > 0 ? "," : "", name) < 0)
            return CFR_STATUS_IO_ERROR;
    }
    return fputc(']', stream) == EOF ? CFR_STATUS_IO_ERROR
                                     : CFR_STATUS_SUCCESS;
}

static Status kuhn_write_detail(FILE *stream, const void *storage) {
    const KuhnPokerState *state = storage;
    const char *phase = kuhn_phase_name(state->phase);
    const char *first = kuhn_card_symbol(state->cards[0]);
    const char *second = kuhn_card_symbol(state->cards[1]);
    Status status;

    if (phase == NULL || first == NULL || second == NULL)
        return CFR_STATUS_INVALID_ARGUMENT;
    if (fprintf(stream, "\"phase\":\"%s\",\"private\":[\"%s\",\"%s\"],", phase,
                first, second) < 0) {
        return CFR_STATUS_IO_ERROR;
    }
    if (fprintf(stream, "\"pot\":%d,\"history\":", kuhn_pot(state)) < 0)
        return CFR_STATUS_IO_ERROR;
    status = kuhn_write_history(stream, state);
    return status;
}

/* Appends the betting actions of a Kuhn state, or a dash when there are none. */
static bool kuhn_append_history(const KuhnPokerState *state, char *buffer,
                                size_t capacity, size_t *length) {
    size_t index;

    if (state->public_action_count == 0)
        return append_text(buffer, capacity, length, "-");
    for (index = 0; index < state->public_action_count; index++) {
        const char *name = kuhn_action_name(state->public_actions[index]);

        if (name == NULL)
            return false;
        if (index > 0 && !append_text(buffer, capacity, length, "-"))
            return false;
        if (!append_text(buffer, capacity, length, name))
            return false;
    }
    return true;
}

static Status kuhn_node_label(const void *storage, bool terminal, Actor actor,
                              char *buffer, size_t capacity) {
    const KuhnPokerState *state = storage;
    size_t length = 0;

    buffer[0] = '\0';
    if (terminal) {
        const bool folded =
            state->public_action_count > 0 &&
            state->public_actions[state->public_action_count - 1] ==
                CFR_KUHN_POKER_ACTION_FOLD;

        return append_text(buffer, capacity, &length,
                           folded ? "fold" : "showdown")
                   ? CFR_STATUS_SUCCESS
                   : CFR_STATUS_BUFFER_TOO_SMALL;
    }
    if (actor.kind == CFR_ACTOR_CHANCE) {
        return append_text(buffer, capacity, &length, "deal")
                   ? CFR_STATUS_SUCCESS
                   : CFR_STATUS_BUFFER_TOO_SMALL;
    }
    if (!append_text(buffer, capacity, &length,
                     actor.player == CFR_PLAYER_0 ? "P0 " : "P1 ")) {
        return CFR_STATUS_BUFFER_TOO_SMALL;
    }
    {
        const char *symbol = kuhn_card_symbol(state->cards[actor.player]);

        if (symbol == NULL)
            return CFR_STATUS_INVALID_ARGUMENT;
        if (!append_text(buffer, capacity, &length, symbol))
            return CFR_STATUS_BUFFER_TOO_SMALL;
    }
    return CFR_STATUS_SUCCESS;
}

static Status kuhn_infoset_label(const void *storage, char *buffer,
                                 size_t capacity) {
    const KuhnPokerState *state = storage;
    size_t length = 0;
    const char *symbol;
    Player player;

    buffer[0] = '\0';
    switch (state->phase) {
    case CFR_KUHN_POKER_PHASE_PLAYER_0_OPEN:
    case CFR_KUHN_POKER_PHASE_PLAYER_0_FACING_CHECK_BET:
        player = CFR_PLAYER_0;
        break;
    case CFR_KUHN_POKER_PHASE_PLAYER_1_AFTER_CHECK:
    case CFR_KUHN_POKER_PHASE_PLAYER_1_FACING_OPEN_BET:
        player = CFR_PLAYER_1;
        break;
    default:
        return CFR_STATUS_INVALID_ARGUMENT;
    }
    symbol = kuhn_card_symbol(state->cards[player]);
    if (symbol == NULL)
        return CFR_STATUS_INVALID_ARGUMENT;
    if (!append_text(buffer, capacity, &length,
                     player == CFR_PLAYER_0 ? "P0 " : "P1 ") ||
        !append_text(buffer, capacity, &length, symbol) ||
        !append_text(buffer, capacity, &length, " \xc2\xb7 ") ||
        !kuhn_append_history(state, buffer, capacity, &length)) {
        return CFR_STATUS_BUFFER_TOO_SMALL;
    }
    return CFR_STATUS_SUCCESS;
}

/* --------------------------------------------------------------- Leduc --- */

static const char *leduc_card_symbol(LeducPokerCard card) {
    switch (card) {
    case CFR_LEDUC_POKER_CARD_JACK:
        return "J";
    case CFR_LEDUC_POKER_CARD_QUEEN:
        return "Q";
    case CFR_LEDUC_POKER_CARD_KING:
        return "K";
    case CFR_LEDUC_POKER_CARD_NOT_DEALT:
        return "-";
    default:
        return NULL;
    }
}

static const char *leduc_action_name(Action action) {
    switch ((LeducPokerAction)action) {
    case CFR_LEDUC_POKER_ACTION_DEAL_JJ:
        return "JJ";
    case CFR_LEDUC_POKER_ACTION_DEAL_JQ:
        return "JQ";
    case CFR_LEDUC_POKER_ACTION_DEAL_JK:
        return "JK";
    case CFR_LEDUC_POKER_ACTION_DEAL_QJ:
        return "QJ";
    case CFR_LEDUC_POKER_ACTION_DEAL_QQ:
        return "QQ";
    case CFR_LEDUC_POKER_ACTION_DEAL_QK:
        return "QK";
    case CFR_LEDUC_POKER_ACTION_DEAL_KJ:
        return "KJ";
    case CFR_LEDUC_POKER_ACTION_DEAL_KQ:
        return "KQ";
    case CFR_LEDUC_POKER_ACTION_DEAL_KK:
        return "KK";
    case CFR_LEDUC_POKER_ACTION_REVEAL_J:
        return "J";
    case CFR_LEDUC_POKER_ACTION_REVEAL_Q:
        return "Q";
    case CFR_LEDUC_POKER_ACTION_REVEAL_K:
        return "K";
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
    case CFR_LEDUC_POKER_ACTION_NONE:
    default:
        return NULL;
    }
}

static const char *leduc_phase_name(LeducPokerPhase phase) {
    switch (phase) {
    case CFR_LEDUC_POKER_PHASE_PRIVATE_DEAL:
        return "private_deal";
    case CFR_LEDUC_POKER_PHASE_FIRST_BETTING:
        return "first_betting";
    case CFR_LEDUC_POKER_PHASE_PUBLIC_DEAL:
        return "public_deal";
    case CFR_LEDUC_POKER_PHASE_SECOND_BETTING:
        return "second_betting";
    case CFR_LEDUC_POKER_PHASE_TERMINAL:
        return "terminal";
    default:
        return NULL;
    }
}

static Status leduc_write_history(FILE *stream, const LeducPokerState *state) {
    size_t index;

    if (fputc('[', stream) == EOF)
        return CFR_STATUS_IO_ERROR;
    for (index = 0; index < state->public_action_count; index++) {
        const char *name = leduc_action_name(state->public_actions[index]);

        if (name == NULL)
            return CFR_STATUS_INVALID_ARGUMENT;
        if (fprintf(stream, "%s\"%s\"", index > 0 ? "," : "", name) < 0)
            return CFR_STATUS_IO_ERROR;
    }
    return fputc(']', stream) == EOF ? CFR_STATUS_IO_ERROR
                                     : CFR_STATUS_SUCCESS;
}

static Status leduc_write_detail(FILE *stream, const void *storage) {
    const LeducPokerState *state = storage;
    const char *phase = leduc_phase_name(state->phase);
    const char *first = leduc_card_symbol(state->private_cards[0]);
    const char *second = leduc_card_symbol(state->private_cards[1]);
    const char *board = leduc_card_symbol(state->public_card);

    if (phase == NULL || first == NULL || second == NULL || board == NULL)
        return CFR_STATUS_INVALID_ARGUMENT;
    if (fprintf(stream,
                "\"phase\":\"%s\",\"private\":[\"%s\",\"%s\"],"
                "\"board\":\"%s\",\"contributions\":[%d,%d],\"pot\":%d,"
                "\"folded\":%s,\"roundStart\":%zu,\"history\":",
                phase, first, second, board, state->contributions[0],
                state->contributions[1],
                state->contributions[0] + state->contributions[1],
                state->folded ? "true" : "false",
                state->round_start_index) < 0) {
        return CFR_STATUS_IO_ERROR;
    }
    return leduc_write_history(stream, state);
}

/*
 * Appends the visible betting actions, separating the two rounds with a bar.
 *
 * Actions before round_start_index belong to the first betting round.
 */
static bool leduc_append_history(const LeducPokerState *state, char *buffer,
                                 size_t capacity, size_t *length) {
    size_t index;

    if (state->public_action_count == 0)
        return append_text(buffer, capacity, length, "-");
    for (index = 0; index < state->public_action_count; index++) {
        const char *name = leduc_action_name(state->public_actions[index]);

        if (name == NULL)
            return false;
        if (index > 0 &&
            !append_text(buffer, capacity, length,
                         index == state->round_start_index ? " | " : "-")) {
            return false;
        }
        if (!append_text(buffer, capacity, length, name))
            return false;
    }
    return true;
}

static Status leduc_node_label(const void *storage, bool terminal, Actor actor,
                               char *buffer, size_t capacity) {
    const LeducPokerState *state = storage;
    size_t length = 0;
    const char *symbol;

    buffer[0] = '\0';
    if (terminal) {
        return append_text(buffer, capacity, &length,
                           state->folded ? "fold" : "showdown")
                   ? CFR_STATUS_SUCCESS
                   : CFR_STATUS_BUFFER_TOO_SMALL;
    }
    if (actor.kind == CFR_ACTOR_CHANCE) {
        const bool private_deal =
            state->phase == CFR_LEDUC_POKER_PHASE_PRIVATE_DEAL;

        return append_text(buffer, capacity, &length,
                           private_deal ? "deal" : "board")
                   ? CFR_STATUS_SUCCESS
                   : CFR_STATUS_BUFFER_TOO_SMALL;
    }
    symbol = leduc_card_symbol(state->private_cards[actor.player]);
    if (symbol == NULL)
        return CFR_STATUS_INVALID_ARGUMENT;
    if (!append_text(buffer, capacity, &length,
                     actor.player == CFR_PLAYER_0 ? "P0 " : "P1 ") ||
        !append_text(buffer, capacity, &length, symbol)) {
        return CFR_STATUS_BUFFER_TOO_SMALL;
    }
    if (state->public_card != CFR_LEDUC_POKER_CARD_NOT_DEALT) {
        const char *board = leduc_card_symbol(state->public_card);

        if (board == NULL)
            return CFR_STATUS_INVALID_ARGUMENT;
        if (!append_text(buffer, capacity, &length, "/") ||
            !append_text(buffer, capacity, &length, board)) {
            return CFR_STATUS_BUFFER_TOO_SMALL;
        }
    }
    return CFR_STATUS_SUCCESS;
}

static Status leduc_infoset_label(const void *storage, char *buffer,
                                  size_t capacity) {
    const LeducPokerState *state = storage;
    size_t length = 0;
    const char *symbol;
    const char *board;

    buffer[0] = '\0';
    if (state->phase != CFR_LEDUC_POKER_PHASE_FIRST_BETTING &&
        state->phase != CFR_LEDUC_POKER_PHASE_SECOND_BETTING) {
        return CFR_STATUS_INVALID_ARGUMENT;
    }
    symbol = leduc_card_symbol(state->private_cards[state->current_player]);
    board = leduc_card_symbol(state->public_card);
    if (symbol == NULL || board == NULL)
        return CFR_STATUS_INVALID_ARGUMENT;
    if (!append_text(buffer, capacity, &length,
                     state->current_player == CFR_PLAYER_0 ? "P0 " : "P1 ") ||
        !append_text(buffer, capacity, &length, symbol) ||
        !append_text(buffer, capacity, &length, "/") ||
        !append_text(buffer, capacity, &length, board) ||
        !append_text(buffer, capacity, &length, " \xc2\xb7 ") ||
        !leduc_append_history(state, buffer, capacity, &length)) {
        return CFR_STATUS_BUFFER_TOO_SMALL;
    }
    return CFR_STATUS_SUCCESS;
}

/* ------------------------------------------------------------ strategy --- */

/*
 * Parses one "infoset key=K actions=N action_0=P ..." line.
 *
 * The function accepts only the exact field order written by
 * cfr_strategy_write_text and rejects anything else as a format error.
 */
static Status parse_strategy_line(const char *line, TextStrategyRow *row_out) {
    long long key = 0;
    unsigned long long action_count = 0;
    int consumed = 0;
    size_t index;

    if (sscanf(line, "infoset key=%lld actions=%llu%n", &key, &action_count,
               &consumed) != 2) {
        return CFR_STATUS_FORMAT_ERROR;
    }
    if (action_count == 0 || action_count > TREE_MAX_ACTIONS)
        return CFR_STATUS_FORMAT_ERROR;
    row_out->key = (InfoSetKey)key;
    row_out->action_count = (size_t)action_count;
    line += consumed;
    for (index = 0; index < row_out->action_count; index++) {
        unsigned long long field = 0;
        double probability = 0.0;

        if (sscanf(line, " action_%llu=%lf%n", &field, &probability,
                   &consumed) != 2) {
            return CFR_STATUS_FORMAT_ERROR;
        }
        if (field != (unsigned long long)index || !isfinite(probability))
            return CFR_STATUS_FORMAT_ERROR;
        row_out->probabilities[index] = probability;
        line += consumed;
    }
    return CFR_STATUS_SUCCESS;
}

/* Reads a text strategy export into a freshly allocated row array. */
/* variant_out must have room for TREE_LABEL_CAPACITY bytes. */
static Status load_text_strategy(const char *path, const char *schema_id,
                                 TextStrategyRow **rows_out,
                                 size_t *row_count_out, char *variant_out,
                                 size_t *iterations_out) {
    char line[TREE_LINE_CAPACITY];
    char schema[TREE_LABEL_CAPACITY];
    FILE *stream;
    TextStrategyRow *rows = NULL;
    size_t capacity = 0;
    size_t count = 0;
    unsigned long long iterations = 0;
    unsigned long long declared = 0;
    int version = 0;
    Status status = CFR_STATUS_SUCCESS;

    stream = fopen(path, "rb");
    if (stream == NULL) {
        (void)fprintf(stderr, "error: could not open '%s': %s\n", path,
                      strerror(errno));
        return CFR_STATUS_IO_ERROR;
    }
    if (fgets(line, sizeof(line), stream) == NULL) {
        (void)fclose(stream);
        return CFR_STATUS_FORMAT_ERROR;
    }
    if (sscanf(line,
               "cfr-strategy version=%d schema=%159s variant=%159s "
               "training_iterations=%llu information_sets=%llu",
               &version, schema, variant_out, &iterations, &declared) != 5) {
        (void)fclose(stream);
        return CFR_STATUS_FORMAT_ERROR;
    }
    if (strcmp(schema, schema_id) != 0) {
        (void)fprintf(stderr,
                      "error: strategy schema '%s' does not match the "
                      "selected game schema '%s'\n",
                      schema, schema_id);
        (void)fclose(stream);
        return CFR_STATUS_INCOMPATIBLE_GAME;
    }
    while (fgets(line, sizeof(line), stream) != NULL) {
        if (count == capacity) {
            const size_t next = capacity == 0 ? 64 : capacity * 2;
            TextStrategyRow *grown;

            if (next > SIZE_MAX / sizeof(*rows)) {
                status = CFR_STATUS_OUT_OF_MEMORY;
                break;
            }
            grown = realloc(rows, next * sizeof(*rows));
            if (grown == NULL) {
                status = CFR_STATUS_OUT_OF_MEMORY;
                break;
            }
            rows = grown;
            capacity = next;
        }
        status = parse_strategy_line(line, &rows[count]);
        if (status != CFR_STATUS_SUCCESS)
            break;
        count += 1;
    }
    if (status == CFR_STATUS_SUCCESS && ferror(stream) != 0)
        status = CFR_STATUS_IO_ERROR;
    if (status == CFR_STATUS_SUCCESS && count != (size_t)declared)
        status = CFR_STATUS_FORMAT_ERROR;
    (void)fclose(stream);
    if (status != CFR_STATUS_SUCCESS) {
        free(rows);
        return status;
    }
    *rows_out = rows;
    *row_count_out = count;
    *iterations_out = (size_t)iterations;
    return CFR_STATUS_SUCCESS;
}

/*
 * Copies the average strategy for key, or a uniform strategy when it is absent.
 *
 * found_out reports whether the strategy came from the loaded artifact. An
 * absent key is not an error: the walk still shows the branch and the viewer
 * marks it.
 */
static Status strategy_for_key(const StrategySource *source, InfoSetKey key,
                               size_t action_count, Probability *strategy,
                               bool *found_out) {
    size_t index;

    *found_out = false;
    if (source->store != NULL) {
        size_t required = 0;
        const Status status = cfr_evaluation_average_strategy(
            source->store, key, strategy, action_count, &required);

        if (status == CFR_STATUS_SUCCESS) {
            if (required != action_count)
                return CFR_STATUS_INVALID_ARGUMENT;
            *found_out = true;
            return CFR_STATUS_SUCCESS;
        }
        if (status != CFR_STATUS_NOT_FOUND)
            return status;
    }
    for (index = 0; index < source->row_count; index++) {
        if (source->rows[index].key != key)
            continue;
        if (source->rows[index].action_count != action_count)
            return CFR_STATUS_INVALID_ARGUMENT;
        memcpy(strategy, source->rows[index].probabilities,
               action_count * sizeof(*strategy));
        *found_out = true;
        return CFR_STATUS_SUCCESS;
    }
    for (index = 0; index < action_count; index++)
        strategy[index] = 1.0 / (Probability)action_count;
    return CFR_STATUS_SUCCESS;
}

/* ------------------------------------------------------------ the walk --- */

static Status write_json_string(FILE *stream, const char *text) {
    if (fputc('"', stream) == EOF)
        return CFR_STATUS_IO_ERROR;
    for (; *text != '\0'; text++) {
        const unsigned char character = (unsigned char)*text;
        int written;

        if (character == '"' || character == '\\')
            written = fprintf(stream, "\\%c", character);
        else if (character < 0x20)
            written = fprintf(stream, "\\u%04x", character);
        else
            written = fputc((int)character, stream) == EOF ? -1 : 1;
        if (written < 0)
            return CFR_STATUS_IO_ERROR;
    }
    return fputc('"', stream) == EOF ? CFR_STATUS_IO_ERROR
                                     : CFR_STATUS_SUCCESS;
}

static Status write_number(FILE *stream, double value) {
    if (!isfinite(value))
        return CFR_STATUS_NUMERIC_ERROR;
    return fprintf(stream, "%.12g", value) < 0 ? CFR_STATUS_IO_ERROR
                                               : CFR_STATUS_SUCCESS;
}

/*
 * Writes the subtree rooted at the current state and returns its value.
 *
 * value_out receives the expected utility of player zero below the node under
 * the average-strategy profile. Chance nodes average over their outcomes and
 * player nodes average over their strategy, so the value of the root is the
 * value of the profile itself.
 *
 * reach is the probability of arriving at the node when both players follow
 * the average strategy and chance follows the rules. The walk writes it before
 * the children and writes value_out after them, which keeps the whole document
 * streamable without holding the tree in memory.
 */
static Status write_subtree(TreeWriter *writer, size_t depth, Probability reach,
                            Utility *value_out) {
    const TreeGame *tree_game = writer->tree_game;
    const Game *game = tree_game->game;
    FILE *stream = writer->stream;
    Action actions[TREE_MAX_ACTIONS];
    Probability weights[TREE_MAX_ACTIONS];
    char label[TREE_LABEL_CAPACITY];
    size_t action_count = 0;
    Utility value = 0.0;
    bool terminal = false;
    Actor actor = {0};
    size_t index;
    Status status;

    if (depth > TREE_MAX_DEPTH)
        return CFR_STATUS_INVALID_ARGUMENT;
    writer->node_count += 1;
    if (depth > writer->deepest_level)
        writer->deepest_level = depth;

    status = cfr_game_is_terminal(game, tree_game->const_state, &terminal);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    if (!terminal) {
        status = cfr_game_current_actor(game, tree_game->const_state, &actor);
        if (status != CFR_STATUS_SUCCESS)
            return status;
    }
    status = tree_game->node_label(tree_game->storage, terminal, actor, label,
                                   sizeof(label));
    if (status != CFR_STATUS_SUCCESS)
        return status;

    if (fputc('{', stream) == EOF)
        return CFR_STATUS_IO_ERROR;
    if (fputs("\"kind\":\"", stream) == EOF)
        return CFR_STATUS_IO_ERROR;
    if (fputs(terminal ? "terminal"
                       : (actor.kind == CFR_ACTOR_CHANCE ? "chance" : "player"),
              stream) == EOF) {
        return CFR_STATUS_IO_ERROR;
    }
    if (fputs("\",\"label\":", stream) == EOF)
        return CFR_STATUS_IO_ERROR;
    status = write_json_string(stream, label);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    if (fputs(",\"reach\":", stream) == EOF)
        return CFR_STATUS_IO_ERROR;
    status = write_number(stream, reach);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    if (fputs(",\"detail\":{", stream) == EOF)
        return CFR_STATUS_IO_ERROR;
    status = tree_game->write_detail(stream, tree_game->storage);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    if (fputc('}', stream) == EOF)
        return CFR_STATUS_IO_ERROR;

    if (terminal) {
        writer->terminal_count += 1;
        status = cfr_game_terminal_utility(game, tree_game->const_state,
                                           CFR_PLAYER_0, &value);
        if (status != CFR_STATUS_SUCCESS)
            return status;
        if (fputs(",\"ev\":", stream) == EOF)
            return CFR_STATUS_IO_ERROR;
        status = write_number(stream, value);
        if (status != CFR_STATUS_SUCCESS)
            return status;
        if (fputc('}', stream) == EOF)
            return CFR_STATUS_IO_ERROR;
        *value_out = value;
        return CFR_STATUS_SUCCESS;
    }

    status = cfr_game_legal_actions(game, tree_game->const_state, actions,
                                    TREE_MAX_ACTIONS, &action_count);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    if (action_count == 0)
        return CFR_STATUS_INVALID_ARGUMENT;

    if (actor.kind == CFR_ACTOR_CHANCE) {
        for (index = 0; index < action_count; index++) {
            status = cfr_game_chance_probability(game, tree_game->const_state,
                                                 actions[index],
                                                 &weights[index]);
            if (status != CFR_STATUS_SUCCESS)
                return status;
        }
    } else {
        InfoSetKey key;
        bool found = false;

        writer->player_node_count += 1;
        status = cfr_game_information_set_key(game, tree_game->const_state,
                                              &key);
        if (status != CFR_STATUS_SUCCESS)
            return status;
        status = strategy_for_key(writer->strategy, key, action_count, weights,
                                  &found);
        if (status != CFR_STATUS_SUCCESS)
            return status;
        if (!found)
            writer->missing_information_set = true;
        if (fprintf(stream, ",\"player\":%d,\"key\":%" PRId64 ",\"known\":%s",
                    (int)actor.player, key, found ? "true" : "false") < 0) {
            return CFR_STATUS_IO_ERROR;
        }
        if (fputs(",\"infoset\":", stream) == EOF)
            return CFR_STATUS_IO_ERROR;
        status = tree_game->infoset_label(tree_game->storage, label,
                                          sizeof(label));
        if (status != CFR_STATUS_SUCCESS)
            return status;
        status = write_json_string(stream, label);
        if (status != CFR_STATUS_SUCCESS)
            return status;
    }

    if (fputs(",\"children\":[", stream) == EOF)
        return CFR_STATUS_IO_ERROR;
    for (index = 0; index < action_count; index++) {
        const char *name = tree_game->action_name(actions[index]);
        Utility child_value = 0.0;
        Status undo_status;

        if (name == NULL)
            return CFR_STATUS_INVALID_ARGUMENT;
        if (index > 0 && fputc(',', stream) == EOF)
            return CFR_STATUS_IO_ERROR;
        if (fputs("{\"action\":", stream) == EOF)
            return CFR_STATUS_IO_ERROR;
        status = write_json_string(stream, name);
        if (status != CFR_STATUS_SUCCESS)
            return status;
        if (fputs(",\"p\":", stream) == EOF)
            return CFR_STATUS_IO_ERROR;
        status = write_number(stream, weights[index]);
        if (status != CFR_STATUS_SUCCESS)
            return status;
        if (fputs(",\"node\":", stream) == EOF)
            return CFR_STATUS_IO_ERROR;

        status = cfr_game_apply_action(game, tree_game->state, actions[index]);
        if (status != CFR_STATUS_SUCCESS)
            return status;
        status = write_subtree(writer, depth + 1, reach * weights[index],
                               &child_value);
        undo_status = cfr_game_undo_action(game, tree_game->state);
        if (status != CFR_STATUS_SUCCESS)
            return status;
        if (undo_status != CFR_STATUS_SUCCESS)
            return undo_status;
        value += weights[index] * child_value;
        if (fputc('}', stream) == EOF)
            return CFR_STATUS_IO_ERROR;
    }
    if (fputs("],\"ev\":", stream) == EOF)
        return CFR_STATUS_IO_ERROR;
    status = write_number(stream, value);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    if (fputc('}', stream) == EOF)
        return CFR_STATUS_IO_ERROR;
    *value_out = value;
    return CFR_STATUS_SUCCESS;
}

static Status write_document(FILE *stream, const TreeGame *tree_game,
                             const StrategySource *strategy) {
    TreeWriter writer = {0};
    Utility root_value = 0.0;
    Status status;

    writer.stream = stream;
    writer.tree_game = tree_game;
    writer.strategy = strategy;

    if (fputs("{\"game\":\"", stream) == EOF)
        return CFR_STATUS_IO_ERROR;
    if (fputs(tree_game->id, stream) == EOF)
        return CFR_STATUS_IO_ERROR;
    if (fprintf(stream,
                "\",\"schema\":\"%s\",\"variant\":\"%s\","
                "\"trainingIterations\":%zu,\"source\":\"%s\",\"root\":",
                tree_game->game->strategy_schema_id, strategy->variant,
                strategy->training_iterations, strategy->origin) < 0) {
        return CFR_STATUS_IO_ERROR;
    }
    status = write_subtree(&writer, 0, 1.0, &root_value);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    if (fprintf(stream,
                ",\"stats\":{\"nodes\":%zu,\"playerNodes\":%zu,"
                "\"terminals\":%zu,\"depth\":%zu,\"complete\":%s}}\n",
                writer.node_count, writer.player_node_count,
                writer.terminal_count, writer.deepest_level,
                writer.missing_information_set ? "false" : "true") < 0) {
        return CFR_STATUS_IO_ERROR;
    }
    if (writer.missing_information_set) {
        (void)fprintf(stderr,
                      "warning: the strategy did not cover every information "
                      "set; uncovered nodes use a uniform strategy\n");
    }
    return CFR_STATUS_SUCCESS;
}

/* ---------------------------------------------------------------- main --- */

static TreeParse parse_options(int argc, char **argv, TreeOptions *options_out) {
    TreeOptions options = {0};
    bool game_seen = false;
    int index;

    options.game_id = TREE_GAME_KUHN;
    for (index = 1; index < argc; index++) {
        const char *argument = argv[index];

        if (strcmp(argument, "--help") == 0 || strcmp(argument, "-h") == 0)
            return TREE_PARSE_HELP;
        if (strcmp(argument, "--game") == 0) {
            if (index + 1 >= argc) {
                (void)fprintf(stderr, "error: --game requires a value\n");
                return TREE_PARSE_ERROR;
            }
            index += 1;
            if (strcmp(argv[index], "kuhn") == 0) {
                options.game_id = TREE_GAME_KUHN;
            } else if (strcmp(argv[index], "leduc") == 0) {
                options.game_id = TREE_GAME_LEDUC;
            } else {
                (void)fprintf(stderr,
                              "error: unknown game '%s'; use kuhn or leduc\n",
                              argv[index]);
                return TREE_PARSE_ERROR;
            }
            game_seen = true;
            continue;
        }
        if (strcmp(argument, "--load") == 0 ||
            strcmp(argument, "--strategy") == 0 ||
            strcmp(argument, "--output") == 0) {
            const char **destination;

            if (index + 1 >= argc) {
                (void)fprintf(stderr, "error: %s requires a value\n", argument);
                return TREE_PARSE_ERROR;
            }
            if (strcmp(argument, "--load") == 0)
                destination = &options.load_path;
            else if (strcmp(argument, "--strategy") == 0)
                destination = &options.strategy_path;
            else
                destination = &options.output_path;
            if (*destination != NULL) {
                (void)fprintf(stderr, "error: %s was given twice\n", argument);
                return TREE_PARSE_ERROR;
            }
            index += 1;
            *destination = argv[index];
            continue;
        }
        (void)fprintf(stderr, "error: unknown option '%s'\n", argument);
        return TREE_PARSE_ERROR;
    }
    if (!game_seen) {
        (void)fprintf(stderr, "error: --game is required\n");
        return TREE_PARSE_ERROR;
    }
    if (options.load_path != NULL && options.strategy_path != NULL) {
        (void)fprintf(stderr,
                      "error: --load and --strategy are mutually exclusive\n");
        return TREE_PARSE_ERROR;
    }
    *options_out = options;
    return TREE_PARSE_READY;
}

static const char *variant_name(TrainerVariant variant) {
    return variant == CFR_TRAINER_VARIANT_CFR_PLUS ? "cfr-plus" : "cfr";
}

int main(int argc, char **argv) {
    TreeOptions options = {0};
    TreeGame tree_game = {0};
    StrategySource strategy = {0};
    KuhnPokerState kuhn_state = {0};
    LeducPokerState leduc_state = {0};
    TextStrategyRow *rows = NULL;
    InfoStore store = {0};
    Trainer trainer = {0};
    char variant_buffer[TREE_LABEL_CAPACITY] = "unknown";
    FILE *output = stdout;
    Status status;
    int exit_code = TREE_EXIT_SUCCESS;

    switch (parse_options(argc, argv, &options)) {
    case TREE_PARSE_HELP:
        return print_usage(stdout, argv[0]) ? TREE_EXIT_SUCCESS
                                            : TREE_EXIT_RUNTIME_ERROR;
    case TREE_PARSE_ERROR:
        (void)print_usage(stderr, argv[0]);
        return TREE_EXIT_USAGE_ERROR;
    case TREE_PARSE_READY:
    default:
        break;
    }

    if (options.game_id == TREE_GAME_KUHN) {
        status = cfr_kuhn_poker_state_init(&kuhn_state);
        if (status != CFR_STATUS_SUCCESS) {
            report_status("initialize the Kuhn Poker root", status);
            return TREE_EXIT_RUNTIME_ERROR;
        }
        tree_game.id = "kuhn";
        tree_game.game = cfr_kuhn_poker_descriptor();
        tree_game.state = cfr_kuhn_poker_state_as_game_state(&kuhn_state);
        tree_game.const_state =
            cfr_kuhn_poker_state_as_game_state_const(&kuhn_state);
        tree_game.write_detail = kuhn_write_detail;
        tree_game.node_label = kuhn_node_label;
        tree_game.infoset_label = kuhn_infoset_label;
        tree_game.action_name = kuhn_action_name;
        tree_game.storage = &kuhn_state;
    } else {
        status = cfr_leduc_poker_state_init(&leduc_state);
        if (status != CFR_STATUS_SUCCESS) {
            report_status("initialize the Leduc Poker root", status);
            return TREE_EXIT_RUNTIME_ERROR;
        }
        tree_game.id = "leduc";
        tree_game.game = cfr_leduc_poker_descriptor();
        tree_game.state = cfr_leduc_poker_state_as_game_state(&leduc_state);
        tree_game.const_state =
            cfr_leduc_poker_state_as_game_state_const(&leduc_state);
        tree_game.write_detail = leduc_write_detail;
        tree_game.node_label = leduc_node_label;
        tree_game.infoset_label = leduc_infoset_label;
        tree_game.action_name = leduc_action_name;
        tree_game.storage = &leduc_state;
    }

    strategy.variant = "unknown";
    strategy.origin = "uniform";
    if (options.load_path != NULL) {
        FILE *checkpoint = fopen(options.load_path, "rb");

        if (checkpoint == NULL) {
            (void)fprintf(stderr, "error: could not open '%s': %s\n",
                          options.load_path, strerror(errno));
            return TREE_EXIT_RUNTIME_ERROR;
        }
        status = cfr_checkpoint_read(checkpoint, tree_game.game,
                                     tree_game.state, &store, &trainer);
        (void)fclose(checkpoint);
        if (status != CFR_STATUS_SUCCESS) {
            report_status("read the checkpoint", status);
            return TREE_EXIT_RUNTIME_ERROR;
        }
        strategy.store = &store;
        strategy.variant = variant_name(trainer.variant);
        strategy.training_iterations = trainer.training_iterations;
        strategy.origin = "checkpoint";
    } else if (options.strategy_path != NULL) {
        status = load_text_strategy(
            options.strategy_path, tree_game.game->strategy_schema_id, &rows,
            &strategy.row_count, variant_buffer,
            &strategy.training_iterations);
        if (status != CFR_STATUS_SUCCESS) {
            report_status("read the strategy export", status);
            return TREE_EXIT_RUNTIME_ERROR;
        }
        strategy.rows = rows;
        strategy.variant = variant_buffer;
        strategy.origin = "export";
    }

    if (options.output_path != NULL) {
        output = fopen(options.output_path, "wb");
        if (output == NULL) {
            (void)fprintf(stderr, "error: could not create '%s': %s\n",
                          options.output_path, strerror(errno));
            exit_code = TREE_EXIT_RUNTIME_ERROR;
            goto cleanup;
        }
    }
    status = write_document(output, &tree_game, &strategy);
    if (status != CFR_STATUS_SUCCESS) {
        report_status("write the decision tree", status);
        exit_code = TREE_EXIT_RUNTIME_ERROR;
    }
    if (output != stdout) {
        if (fclose(output) != 0 && exit_code == TREE_EXIT_SUCCESS) {
            (void)fprintf(stderr, "error: could not close '%s': %s\n",
                          options.output_path, strerror(errno));
            exit_code = TREE_EXIT_RUNTIME_ERROR;
        }
    } else if (fflush(output) != 0 && exit_code == TREE_EXIT_SUCCESS) {
        (void)fprintf(stderr, "error: could not flush standard output\n");
        exit_code = TREE_EXIT_RUNTIME_ERROR;
    }

cleanup:
    free(rows);
    (void)cfr_info_store_destroy(&store);
    return exit_code;
}
