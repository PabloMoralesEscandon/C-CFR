#include "cfr/game.h"

Status cfr_game_is_terminal(const Game *game, const GameState *state,
                            bool *result) {
    if (game == NULL || game->operations == NULL ||
        game->operations->is_terminal == NULL || state == NULL ||
        result == NULL) {
        return CFR_STATUS_INVALID_ARGUMENT;
    }

    return game->operations->is_terminal(game->context, state, result);
}

Status cfr_game_terminal_utility(const Game *game, const GameState *state,
                                 Player player, Utility *result) {
    if (game == NULL || game->operations == NULL ||
        game->operations->terminal_utility == NULL || state == NULL ||
        result == NULL) {
        return CFR_STATUS_INVALID_ARGUMENT;
    }

    return game->operations->terminal_utility(game->context, state, player,
                                              result);
}

Status cfr_game_current_actor(const Game *game, const GameState *state,
                              Actor *result) {
    if (game == NULL || game->operations == NULL ||
        game->operations->current_actor == NULL || state == NULL ||
        result == NULL) {
        return CFR_STATUS_INVALID_ARGUMENT;
    }

    return game->operations->current_actor(game->context, state, result);
}

Status cfr_game_legal_actions(const Game *game, const GameState *state,
                              Action *actions, size_t capacity,
                              size_t *required_count) {
    if (game == NULL || game->operations == NULL ||
        game->operations->legal_actions == NULL || state == NULL ||
        actions == NULL || required_count == NULL) {
        return CFR_STATUS_INVALID_ARGUMENT;
    }

    return game->operations->legal_actions(game->context, state, actions,
                                           capacity, required_count);
}

Status cfr_game_apply_action(const Game *game, GameState *state,
                             Action action) {
    if (game == NULL || game->operations == NULL ||
        game->operations->apply_action == NULL || state == NULL) {
        return CFR_STATUS_INVALID_ARGUMENT;
    }

    return game->operations->apply_action(game->context, state, action);
}

Status cfr_game_undo_action(const Game *game, GameState *state) {
    if (game == NULL || game->operations == NULL ||
        game->operations->undo_action == NULL || state == NULL) {
        return CFR_STATUS_INVALID_ARGUMENT;
    }

    return game->operations->undo_action(game->context, state);
}

Status cfr_game_chance_probability(const Game *game, const GameState *state,
                                   Action action, Probability *result) {
    if (game == NULL || game->operations == NULL ||
        game->operations->chance_probability == NULL || state == NULL ||
        result == NULL) {
        return CFR_STATUS_INVALID_ARGUMENT;
    }

    return game->operations->chance_probability(game->context, state, action,
                                                result);
}

Status cfr_game_information_set_key(const Game *game, const GameState *state,
                                    InfoSetKey *result) {
    if (game == NULL || game->operations == NULL ||
        game->operations->information_set_key == NULL || state == NULL ||
        result == NULL) {
        return CFR_STATUS_INVALID_ARGUMENT;
    }

    return game->operations->information_set_key(game->context, state, result);
}
