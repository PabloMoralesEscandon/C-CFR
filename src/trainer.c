#include <stdint.h>

#include "cfr/trainer.h"
#include "cfr/traversal.h"
#include "mccfr_internal.h"
#include "mccfr_sequential_internal.h"
#include "traversal_internal.h"

static bool strategic_player_count_is_valid(size_t count) {
    return count == 1 || count == 2;
}

static Status trainer_init(Trainer *trainer, const Game *game, GameState *state,
                           InfoStore *store, TrainerVariant variant,
                           uint64_t seed) {
    if (trainer == NULL || game == NULL || state == NULL || store == NULL ||
        !strategic_player_count_is_valid(game->strategic_player_count))
        return CFR_STATUS_INVALID_ARGUMENT;
    trainer->game = game;
    trainer->state = state;
    trainer->store = store;
    trainer->variant = variant;
    trainer->training_iterations = 0;
    const Status status = cfr_mccfr_rng_seed(&trainer->mccfr_rng, seed);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    trainer->stats = (TrainerStats){0};
    return CFR_STATUS_SUCCESS;
}

Status cfr_trainer_init(Trainer *trainer, const Game *game, GameState *state,
                        InfoStore *store) {
    return trainer_init(trainer, game, state, store, CFR_TRAINER_VARIANT_CFR,
                        0);
}

Status cfr_trainer_init_plus(Trainer *trainer, const Game *game,
                             GameState *state, InfoStore *store) {
    return trainer_init(trainer, game, state, store,
                        CFR_TRAINER_VARIANT_CFR_PLUS, 0);
}

Status cfr_trainer_init_mccfr(Trainer *trainer, const Game *game,
                              GameState *state, InfoStore *store,
                              uint64_t seed) {
    return trainer_init(trainer, game, state, store,
                        CFR_TRAINER_VARIANT_MCCFR_EXTERNAL, seed);
}

static Status trainer_traverse(Trainer *trainer, Player target_player,
                               size_t iteration, Utility *utility,
                               TraversalStats *stats,
                               MccfrWorkspace *mccfr_workspace,
                               MccfrSequentialWorkspace *sequential_workspace,
                               bool concurrent_mccfr,
                               CfrFullTraversalWorkspace *full_workspace) {
    if (trainer->variant == CFR_TRAINER_VARIANT_CFR ||
        trainer->variant == CFR_TRAINER_VARIANT_CFR_PLUS) {
        return cfr_full_traverse_in_workspace(
            trainer->game, trainer->state, trainer->store, target_player,
            iteration, trainer->variant == CFR_TRAINER_VARIANT_CFR_PLUS,
            utility, stats, full_workspace);
    }
    if (trainer->variant == CFR_TRAINER_VARIANT_MCCFR_EXTERNAL) {
        if (!concurrent_mccfr) {
            return cfr_mccfr_sequential_external_traverse_in_workspace(
                trainer->game, trainer->state, trainer->store,
                target_player, &trainer->mccfr_rng, utility, stats,
                sequential_workspace);
        }
        return cfr_mccfr_external_traverse_in_workspace(
            trainer->game, trainer->state, trainer->store, target_player,
            &trainer->mccfr_rng, utility, stats, mccfr_workspace);
    }
    return CFR_STATUS_INVALID_ARGUMENT;
}

static Status run_player_traversal(Trainer *trainer, Player player,
                                   size_t iteration,
                                   MccfrWorkspace *mccfr_workspace,
                                   MccfrSequentialWorkspace *sequential_workspace,
                                   bool concurrent_mccfr,
                                   CfrFullTraversalWorkspace *full_workspace) {
    TraversalStats traverse_stats = {0};
    Utility utility = 0.0;
    const Status status = trainer_traverse(
        trainer, player, iteration, &utility, &traverse_stats,
        mccfr_workspace, sequential_workspace, concurrent_mccfr,
        full_workspace);

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

static Status trainer_run(Trainer *trainer, size_t amount,
                          bool concurrent_mccfr) {
    if (trainer == NULL || trainer->game == NULL || trainer->state == NULL ||
        trainer->store == NULL || !strategic_player_count_is_valid(
                                      trainer->game->strategic_player_count) ||
        (trainer->variant != CFR_TRAINER_VARIANT_CFR &&
         trainer->variant != CFR_TRAINER_VARIANT_CFR_PLUS &&
         trainer->variant != CFR_TRAINER_VARIANT_MCCFR_EXTERNAL))
        return CFR_STATUS_INVALID_ARGUMENT;
    if (concurrent_mccfr &&
        trainer->variant != CFR_TRAINER_VARIANT_MCCFR_EXTERNAL) {
        return CFR_STATUS_INVALID_ARGUMENT;
    }
    if (amount == 0)
        return CFR_STATUS_SUCCESS;

    const Status validation_status =
        cfr_game_validate_state(trainer->game, trainer->state);
    if (validation_status != CFR_STATUS_SUCCESS) {
        if (trainer->stats.errors != SIZE_MAX)
            trainer->stats.errors += 1;
        return validation_status;
    }

    union {
        MccfrWorkspace concurrent;
        MccfrSequentialWorkspace sequential;
    } mccfr_workspace = {0};
    MccfrWorkspace *mccfr_workspace_pointer = NULL;
    MccfrSequentialWorkspace *sequential_workspace_pointer = NULL;
    CfrFullTraversalWorkspace *full_workspace = NULL;
    if (trainer->variant == CFR_TRAINER_VARIANT_MCCFR_EXTERNAL) {
        const Status status =
            concurrent_mccfr
                ? cfr_mccfr_workspace_init(
                      &mccfr_workspace.concurrent,
                      trainer->game->max_legal_actions)
                : cfr_mccfr_sequential_workspace_init(
                      &mccfr_workspace.sequential,
                      trainer->game->max_legal_actions);

        if (status != CFR_STATUS_SUCCESS) {
            if (trainer->stats.errors != SIZE_MAX)
                trainer->stats.errors += 1;
            return status;
        }
        if (concurrent_mccfr)
            mccfr_workspace_pointer = &mccfr_workspace.concurrent;
        else
            sequential_workspace_pointer = &mccfr_workspace.sequential;
    } else {
        const Status status = cfr_full_traversal_workspace_create(
            trainer->game->max_legal_actions, &full_workspace);

        if (status != CFR_STATUS_SUCCESS) {
            if (trainer->stats.errors != SIZE_MAX)
                trainer->stats.errors += 1;
            return status;
        }
    }

    Status result = CFR_STATUS_SUCCESS;
    for (size_t i = 0; i < amount; i++) {
        size_t iteration = trainer->training_iterations;
        if (iteration != SIZE_MAX)
            iteration += 1;
        for (size_t player_index = 0;
             player_index < trainer->game->strategic_player_count;
             player_index++) {
            const Status status = run_player_traversal(
                trainer, (Player)player_index, iteration,
                mccfr_workspace_pointer, sequential_workspace_pointer,
                concurrent_mccfr, full_workspace);

            if (status != CFR_STATUS_SUCCESS) {
                result = status;
                goto cleanup;
            }
        }
        if (!(trainer->stats.iterations == SIZE_MAX))
            trainer->stats.iterations += 1;
        if (trainer->training_iterations != SIZE_MAX)
            trainer->training_iterations += 1;
    }

cleanup:
    cfr_full_traversal_workspace_destroy(full_workspace);
    if (trainer->variant == CFR_TRAINER_VARIANT_MCCFR_EXTERNAL) {
        if (concurrent_mccfr)
            cfr_mccfr_workspace_destroy(&mccfr_workspace.concurrent);
        else
            cfr_mccfr_sequential_workspace_destroy(
                &mccfr_workspace.sequential);
    }
    return result;
}

Status cfr_trainer_run(Trainer *trainer, size_t amount) {
    return trainer_run(trainer, amount, false);
}

Status cfr_trainer_run_concurrent(Trainer *trainer, size_t amount) {
    return trainer_run(trainer, amount, true);
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
