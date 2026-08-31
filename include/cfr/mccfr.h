#ifndef CFR_MCCFR_H
#define CFR_MCCFR_H

#include <stdint.h>

#include "cfr/game.h"
#include "cfr/info_store.h"
#include "cfr/traversal.h"

/*
 * State for the deterministic random stream used by MCCFR.
 *
 * The structure is public so callers can save, copy, and restore a stream.
 * Its representation is part of the library API, but callers must use
 * cfr_mccfr_rng_seed rather than assigning a transformed seed themselves.
 */
typedef struct {
    uint64_t state;
} MccfrRng;

/* Seeds an MCCFR random stream. Every uint64_t value, including zero, is valid. */
Status cfr_mccfr_rng_seed(MccfrRng *rng, uint64_t seed);

/*
 * Runs one external-sampling MCCFR traversal for target_player.
 *
 * Chance and the other player's decisions are sampled. Every action of the
 * target player is traversed. Opponent decisions are sampled from the strategy
 * stored for their information-set key; a sample is cached by information set
 * for the duration of the traversal. Consequently, two states that the
 * opponent cannot distinguish cannot receive different sampled decisions as a
 * consequence of hidden state.
 *
 * Average-strategy deltas for target_player use inverse external-reach
 * weighting, making them unbiased estimates of the full CFR strategy sums.
 * Regret and strategy deltas are committed only after a successful traversal.
 * New zero-valued nodes can remain after an error, matching cfr_traverse.
 *
 * rng advances only when the complete traversal and delta commit succeed.
 * state restoration and all other game contracts match cfr_traverse.
 */
Status cfr_mccfr_external_traverse(const Game *game, GameState *state,
                                   InfoStore *store, Player target_player,
                                   MccfrRng *rng, Utility *utility_out);

/* Runs cfr_mccfr_external_traverse and publishes visited-state statistics. */
Status cfr_mccfr_external_traverse_with_stats(
    const Game *game, GameState *state, InfoStore *store, Player target_player,
    MccfrRng *rng, Utility *utility_out, TraversalStats *stats_out);

#endif
