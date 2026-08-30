#include <stdint.h>

#include "cfr/trainer.h"
#include "cfr/traversal.h"

static bool strategic_player_count_is_valid(size_t count) {
    return count == 1 || count == 2;
}

static Status run_player_traversal(Trainer *trainer, Player player) {
    TraversalStats traverse_stats = {0};
    Utility utility = 0.0;
    Status status = cfr_traverse_with_stats(
        trainer->game, trainer->state, trainer->store, player, &utility,
        &traverse_stats);

    if (status != CFR_STATUS_SUCCESS) {
        if (trainer->stats.errors != SIZE_MAX)
            trainer->stats.errors += 1;
        return status;
    }
    if (trainer->stats.traversals != SIZE_MAX)
        trainer->stats.traversals += 1;
    if (trainer->stats.visited_nodes >
        (SIZE_MAX - traverse_stats.visited_nodes)) {
        trainer->stats.visited_nodes = SIZE_MAX;
    } else {
        trainer->stats.visited_nodes += traverse_stats.visited_nodes;
    }
    return CFR_STATUS_SUCCESS;
}

Status cfr_trainer_init(Trainer *trainer, const Game *game, GameState *state,
                        InfoStore *store) {
    if (trainer == NULL || game == NULL || state == NULL || store == NULL ||
        !strategic_player_count_is_valid(game->strategic_player_count))
        return CFR_STATUS_INVALID_ARGUMENT;
    trainer->game = game;
    trainer->state = state;
    trainer->stats = (TrainerStats){0};
    trainer->store = store;
    return CFR_STATUS_SUCCESS;
}

Status cfr_trainer_run(Trainer *trainer, size_t amount) {
    if (trainer == NULL || trainer->game == NULL || trainer->state == NULL ||
        trainer->store == NULL || !strategic_player_count_is_valid(
                                      trainer->game->strategic_player_count))
        return CFR_STATUS_INVALID_ARGUMENT;
    for (size_t i = 0; i < amount; i++) {
        for (size_t player_index = 0;
             player_index < trainer->game->strategic_player_count;
             player_index++) {
            const Status status = run_player_traversal(
                trainer, (Player)player_index);

            if (status != CFR_STATUS_SUCCESS)
                return status;
        }
        if (!(trainer->stats.iterations == SIZE_MAX))
            trainer->stats.iterations += 1;
    }
    return CFR_STATUS_SUCCESS;
}

Status cfr_trainer_get_stats(const Trainer *trainer, TrainerStats *stats_out) {
    if (trainer == NULL || stats_out == NULL)
        return CFR_STATUS_INVALID_ARGUMENT;
    stats_out->errors = trainer->stats.errors;
    stats_out->iterations = trainer->stats.iterations;
    stats_out->traversals = trainer->stats.traversals;
    stats_out->visited_nodes = trainer->stats.visited_nodes;
    return CFR_STATUS_SUCCESS;
}

Status cfr_trainer_reset_stats(Trainer *trainer) {
    if (trainer == NULL)
        return CFR_STATUS_INVALID_ARGUMENT;
    trainer->stats = (TrainerStats){0};
    return CFR_STATUS_SUCCESS;
}
