#ifndef CFR_MCCFR_H
#define CFR_MCCFR_H

#include <stdint.h>

#include "cfr/game.h"
#include "cfr/info_store.h"
#include "cfr/traversal.h"

CFR_EXTERN_C_BEGIN

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
 * The traversal updates regrets for target_player and average strategies for
 * the sampled player. Sampling supplies the external reach that weights a
 * counterfactual regret, so regret deltas carry no importance weight. A
 * sampled player's information set is reached with exactly the probability
 * that the player and chance reach it, so its strategy delta is the unweighted
 * current strategy. Each information set therefore accumulates the full CFR
 * strategy sums scaled by one constant, and cfr_info_node_average_strategy
 * normalizes that constant away. With two strategic players, a complete
 * iteration must traverse once for each player so that both average strategies
 * advance. cfr_trainer_run does so.
 *
 * A game with one strategic player has no sampled player to carry the average.
 * The traversal then accumulates the own-reach-weighted strategy at
 * target_player's own information sets, which remains proportional to the full
 * CFR strategy sums because only chance separates the traversal from the
 * information set and chance does not depend on the strategy.
 *
 * game->strategic_player_count must be one or two.
 *
 * Regret and strategy deltas are committed only after a successful traversal.
 * New zero-valued nodes can remain after an error, matching cfr_traverse.
 *
 * rng advances only when the complete traversal and delta commit succeed.
 * state restoration and all other game contracts match cfr_traverse.
 *
 * Multiple threads can traverse concurrently into the same initialized store.
 * Each thread must supply its own mutable state, random stream, outputs, and
 * any mutable game context. The game operations must be safe to call
 * concurrently. Store initialization and destruction must not overlap a
 * traversal. Each traversal retains the first strategy snapshot it reads for
 * every information set, including its sampled opponent action, so later
 * visits cannot mix that decision with a concurrent update. Each successful
 * traversal commits its deltas atomically and can observe an earlier concurrent
 * commit at one information set than at another.
 */
Status cfr_mccfr_external_traverse(const Game *game, GameState *state,
                                   InfoStore *store, Player target_player,
                                   MccfrRng *rng, Utility *utility_out);

/* Runs cfr_mccfr_external_traverse and publishes visited-state statistics. */
Status cfr_mccfr_external_traverse_with_stats(
    const Game *game, GameState *state, InfoStore *store, Player target_player,
    MccfrRng *rng, Utility *utility_out, TraversalStats *stats_out);

CFR_EXTERN_C_END

#endif
