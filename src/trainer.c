#include <stdint.h>

#include "cfr/trainer.h"
#include "cfr/traversal.h"

static bool strategic_player_count_is_valid(size_t count) {
    return count == 1 || count == 2;
}

static Status trainer_init(Trainer *trainer, const Game *game, GameState *state,
                           InfoStore *store, TrainerVariant variant) {
    if (trainer == NULL || game == NULL || state == NULL || store == NULL ||
        !strategic_player_count_is_valid(game->strategic_player_count))
        return CFR_STATUS_INVALID_ARGUMENT;
    trainer->game = game;
    trainer->state = state;
    trainer->store = store;
    trainer->variant = variant;
    trainer->training_iterations = 0;
    trainer->stats = (TrainerStats){0};
    return CFR_STATUS_SUCCESS;
}

Status cfr_trainer_init(Trainer *trainer, const Game *game, GameState *state,
                        InfoStore *store) {
    return trainer_init(trainer, game, state, store, CFR_TRAINER_VARIANT_CFR);
}

Status cfr_trainer_init_plus(Trainer *trainer, const Game *game,
                             GameState *state, InfoStore *store) {
    return trainer_init(trainer, game, state, store,
                        CFR_TRAINER_VARIANT_CFR_PLUS);
}

static Status trainer_traverse(Trainer *trainer, Player target_player,
                               size_t iteration, Utility *utility,
                               TraversalStats *stats) {
    if (trainer->variant == CFR_TRAINER_VARIANT_CFR) {
        return cfr_traverse_with_stats(trainer->game, trainer->state,
                                       trainer->store, target_player, utility,
                                       stats);
    }
    if (trainer->variant == CFR_TRAINER_VARIANT_CFR_PLUS) {
        return cfr_traverse_plus_with_stats(
            trainer->game, trainer->state, trainer->store, target_player,
            iteration, utility, stats);
    }
    return CFR_STATUS_INVALID_ARGUMENT;
}

static Status run_player_traversal(Trainer *trainer, Player player,
                                   size_t iteration) {
    TraversalStats traverse_stats = {0};
    Utility utility = 0.0;
    const Status status = trainer_traverse(trainer, player, iteration, &utility,
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

Status cfr_trainer_run(Trainer *trainer, size_t amount) {
    if (trainer == NULL || trainer->game == NULL || trainer->state == NULL ||
        trainer->store == NULL || !strategic_player_count_is_valid(
                                      trainer->game->strategic_player_count) ||
        (trainer->variant != CFR_TRAINER_VARIANT_CFR &&
         trainer->variant != CFR_TRAINER_VARIANT_CFR_PLUS))
        return CFR_STATUS_INVALID_ARGUMENT;
    for (size_t i = 0; i < amount; i++) {
        size_t iteration = trainer->training_iterations;
        if (iteration != SIZE_MAX)
            iteration += 1;
        for (size_t player_index = 0;
             player_index < trainer->game->strategic_player_count;
             player_index++) {
            const Status status = run_player_traversal(
                trainer, (Player)player_index, iteration);

            if (status != CFR_STATUS_SUCCESS)
                return status;
        }
        if (!(trainer->stats.iterations == SIZE_MAX))
            trainer->stats.iterations += 1;
        if (trainer->training_iterations != SIZE_MAX)
            trainer->training_iterations += 1;
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
