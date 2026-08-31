#ifndef CFR_TRAINER_H
#define CFR_TRAINER_H

#include <stddef.h>
#include <stdint.h>

#include "cfr/game.h"
#include "cfr/info_store.h"
#include "cfr/mccfr.h"

/*
 * Contains a snapshot of the trainer's cumulative statistics.
 *
 * All four counters saturate at SIZE_MAX. A saturated counter does not return
 * to zero during a later run.
 */
typedef struct {
    /* Iterations in which all strategic traversals completed. */
    size_t iterations;
    /* Number of traversals that completed successfully. */
    size_t traversals;
    /* Number of states visited by successful traversals. */
    size_t visited_nodes;
    /* Number of failed traversals within cfr_trainer_run. */
    size_t errors;
} TrainerStats;

/* Selects the update rule used by the trainer. */
typedef enum {
    /* Classic CFR with untruncated cumulative regrets and unit weight. */
    CFR_TRAINER_VARIANT_CFR,
    /* CFR+ with truncated regrets and linear averaging. */
    CFR_TRAINER_VARIANT_CFR_PLUS,
    /* External-sampling Monte Carlo CFR. */
    CFR_TRAINER_VARIANT_MCCFR_EXTERNAL
} TrainerVariant;

/*
 * Stores borrowed objects and statistics for a training session.
 *
 * The caller owns Trainer, Game, GameState, and InfoStore. The trainer borrows
 * game, state, and store. The caller must keep all three borrowed objects alive
 * while using the trainer.
 *
 * game is borrowed as const. state and store are borrowed as mutable. The
 * trainer applies and undoes actions on state and adds learning data to store.
 *
 * The trainer does not own the three borrowed objects. cfr_trainer_init does
 * not allocate memory. The trainer also does not retain a copy of the root
 * state.
 */
typedef struct {
    /* Borrowed const game descriptor. */
    const Game *game;
    /* Borrowed mutable root state. */
    GameState *state;
    /* Borrowed mutable store. */
    InfoStore *store;
    /* Variant selected during initialization. */
    TrainerVariant variant;
    /*
     * Number of complete training iterations since initialization. This
     * counter determines the CFR+ weights and is not reset with the public
     * statistics. The counter saturates at SIZE_MAX.
     */
    size_t training_iterations;
    /* Random stream used only by external-sampling MCCFR. */
    MccfrRng mccfr_rng;
    /* Statistics owned by the trainer. */
    TrainerStats stats;
} Trainer;

/*
 * Initializes trainer with three borrowed objects and zeroes the statistics.
 *
 * trainer, game, state, and store must not be null. The caller must provide a
 * valid game, state, and store. The game descriptor must declare one or two
 * strategic players. A null argument or invalid strategic player count
 * produces CFR_STATUS_INVALID_ARGUMENT. An error preserves a non-null trainer.
 */
Status cfr_trainer_init(Trainer *trainer, const Game *game, GameState *state,
                        InfoStore *store);

/*
 * Initializes trainer to run CFR+.
 *
 * Borrowing, ownership, error handling, and trainer preservation are the same
 * as in cfr_trainer_init. The first iteration uses unit weight in the average
 * strategy. Each subsequent complete iteration increases the weight linearly.
 */
Status cfr_trainer_init_plus(Trainer *trainer, const Game *game,
                             GameState *state, InfoStore *store);

/*
 * Initializes trainer to run external-sampling MCCFR.
 *
 * seed selects a deterministic random stream. All seed values are valid.
 * Equal roots, stores, and seeds produce equal samples and learning updates.
 * The traversal samples chance and non-target actions, expands every target
 * action, and keeps sampled opponent decisions consistent within each
 * information set. Ownership and error handling match cfr_trainer_init.
 */
Status cfr_trainer_init_mccfr(Trainer *trainer, const Game *game,
                              GameState *state, InfoStore *store,
                              uint64_t seed);

/*
 * Runs amount iterations with sequential per-player updates.
 *
 * trainer must be initialized. Its three borrowed objects must be valid. An
 * iteration runs one traversal, in order, for each player declared strategic
 * by game->strategic_player_count. The first traversal uses CFR_PLAYER_0. When
 * the count is two, the second uses CFR_PLAYER_1 and observes the learning
 * committed by the first traversal.
 *
 * A trainer initialized with cfr_trainer_init uses classic CFR. A trainer
 * initialized with cfr_trainer_init_plus uses Regret Matching+ and weights the
 * strategies from iteration t with weight t. A trainer initialized with
 * cfr_trainer_init_mccfr uses external-sampling MCCFR and advances its random
 * stream after each successful traversal. Variant state continues across
 * calls.
 *
 * Each traversal commits its own changes. If a later traversal fails, changes
 * from earlier traversals remain in store. iterations increases only after all
 * strategic traversals complete. traversals and visited_nodes increase after
 * each successful traversal. errors increases after a failed traversal.
 * Statistics accumulate across calls. All four counters saturate at SIZE_MAX.
 *
 * An amount of zero produces CFR_STATUS_SUCCESS and does not change the
 * trainer. The function returns the Status of a failed traversal unchanged.
 *
 * After any error, the caller must restore state to the root before calling
 * cfr_trainer_run again. The trainer cannot verify that state represents the
 * root. If an undo operation fails, state can remain at a descendant state.
 */
Status cfr_trainer_run(Trainer *trainer, size_t amount);

/*
 * Copies trainer statistics to stats_out.
 *
 * trainer and stats_out must not be null. The function does not modify trainer.
 * A null argument produces CFR_STATUS_INVALID_ARGUMENT and preserves stats_out.
 */
Status cfr_trainer_get_stats(const Trainer *trainer, TrainerStats *stats_out);

/*
 * Resets all four trainer counters to zero.
 *
 * trainer must not be null. The function preserves game, state, and store. It
 * does not modify the game state, the learning data in the store, variant,
 * training_iterations, or the MCCFR random stream. Therefore, resetting
 * statistics does not reset CFR+ linear weights or MCCFR sampling.
 */
Status cfr_trainer_reset_stats(Trainer *trainer);

#endif
