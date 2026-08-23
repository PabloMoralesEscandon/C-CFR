#include <stdint.h>

#include "cfr/trainer.h"
#include "cfr/traversal.h"

Status cfr_trainer_init(Trainer *trainer, const Game *game, GameState *state,
                        InfoStore *store) {
    if (trainer == NULL || game == NULL || state == NULL || store == NULL)
        return CFR_STATUS_INVALID_ARGUMENT;
    trainer->game = game;
    trainer->state = state;
    trainer->stats = (TrainerStats){0};
    trainer->store = store;
    return CFR_STATUS_SUCCESS;
}

Status cfr_trainer_run(Trainer *trainer, size_t amount) {
    if (trainer == NULL || trainer->game == NULL || trainer->state == NULL ||
        trainer->store == NULL)
        return CFR_STATUS_INVALID_ARGUMENT;
    Status status;
    for (size_t i = 0; i < amount; i++) {
        TraversalStats traverse_stats = {0};
        Utility utility = 0.0;
        status = cfr_traverse_with_stats(trainer->game, trainer->state,
                                         trainer->store, CFR_PLAYER_0, &utility,
                                         &traverse_stats);
        if (status != CFR_STATUS_SUCCESS) {
            if (!(trainer->stats.errors == SIZE_MAX))
                trainer->stats.errors += 1;
            return status;
        }
        if (!(trainer->stats.traversals == SIZE_MAX))
            trainer->stats.traversals += 1;
        if (trainer->stats.visited_nodes >
            (SIZE_MAX - traverse_stats.visited_nodes))
            trainer->stats.visited_nodes = SIZE_MAX;
        else
            trainer->stats.visited_nodes += traverse_stats.visited_nodes;
        status = cfr_traverse_with_stats(trainer->game, trainer->state,
                                         trainer->store, CFR_PLAYER_1, &utility,
                                         &traverse_stats);
        if (status != CFR_STATUS_SUCCESS) {
            if (!(trainer->stats.errors == SIZE_MAX))
                trainer->stats.errors += 1;
            return status;
        }
        if (!(trainer->stats.traversals == SIZE_MAX))
            trainer->stats.traversals += 1;
        if (trainer->stats.visited_nodes >
            (SIZE_MAX - traverse_stats.visited_nodes))
            trainer->stats.visited_nodes = SIZE_MAX;
        else
            trainer->stats.visited_nodes += traverse_stats.visited_nodes;
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
