#ifndef CFR_TRAVERSAL_H
#define CFR_TRAVERSAL_H

#include "cfr/game.h"
#include "cfr/info_store.h"

/* Maximum number of legal actions supported by a traversal. */
#define CFR_TRAVERSAL_MAX_ACTIONS 64

/* Contains statistics for a successful traversal. */
typedef struct {
    /*
     * Number of visited states. The counter includes player, chance, and
     * terminal states. Each entry into the recursive helper counts as one
     * visit. The counter saturates at SIZE_MAX.
     */
    size_t visited_nodes;
} TraversalStats;

/*
 * Traverses the game tree and updates learning data for target_player.
 *
 * The caller owns game, state, store, and utility_out. The function borrows
 * these parameters. game must be a valid descriptor. state and store must be
 * initialized and can be modified. utility_out must be a valid pointer.
 *
 * game->max_legal_actions must be between one and CFR_TRAVERSAL_MAX_ACTIONS.
 * Every nonterminal state must have between one and game->max_legal_actions
 * legal actions. These conditions also apply to descendant states.
 *
 * The traversal enumerates every legal action at a chance node. It uses the
 * optional chance_outcomes operation when available. Otherwise, it enumerates
 * legal actions and queries chance_probability once for each action. Every
 * probability must be finite and greater than or equal to zero. A zero
 * probability is valid, and its branch is still traversed.
 *
 * The sum of probabilities must equal one within the module tolerances. The
 * relative tolerance is 1e-8, and the absolute tolerance is 1e-12. The
 * traversal validates the complete distribution before applying the first
 * chance-node action. An invalid distribution produces
 * CFR_STATUS_INVALID_ARGUMENT.
 *
 * A chance node does not create an information node or generate deltas. Chance
 * reach weights regrets but does not weight strategy sums.
 *
 * The function applies and undoes actions on state. It restores state after
 * each successfully applied action. If an undo operation fails, the function
 * returns that error and cannot guarantee that state was restored.
 *
 * Before walking the tree, the traversal calls the public operations' optional
 * validate_state callback once for the root. If game->trusted_operations is
 * non-null, descendant queries and mutations use that table. Public cfr_game_*
 * calls outside the traversal always use game->operations.
 *
 * store retains ownership of its nodes. The function can add nodes and modify
 * their internal statistics. If an error occurs, accumulators that existed
 * before the call do not change. New nodes with zero accumulators can remain in
 * store.
 *
 * A successful call uses a fixed strategy for each node. It updates only the
 * regrets and strategy sums of target_player. utility_out receives utility from
 * the target_player perspective. An error preserves the previous utility_out
 * value.
 */
Status cfr_traverse(const Game *game, GameState *state, InfoStore *store,
                    Player target_player, Utility *utility_out);

/*
 * Runs cfr_traverse and publishes traversal statistics.
 *
 * game, state, store, target_player, and utility_out follow the cfr_traverse
 * contract. The caller owns stats_out. The function borrows and can modify it.
 *
 * stats_out->visited_nodes counts every state passed to the recursive helper.
 * The counter includes player, chance, and terminal states. The function writes
 * utility_out and stats_out only when it returns CFR_STATUS_SUCCESS. An error
 * preserves the previous values of both outputs.
 */
Status cfr_traverse_with_stats(const Game *game, GameState *state,
                               InfoStore *store, Player target_player,
                               Utility *utility_out, TraversalStats *stats_out);

/*
 * Traverses the tree with CFR+ updates for target_player.
 *
 * game, state, store, target_player, and utility_out follow the cfr_traverse
 * contract. iteration identifies the complete CFR+ iteration and must be
 * greater than zero. The traversal weights the average-strategy contribution
 * by iteration and truncates each updated negative regret to zero.
 *
 * The function preserves the cfr_traverse guarantees for state restoration and
 * accumulator atomicity. An iteration value of zero produces
 * CFR_STATUS_INVALID_ARGUMENT.
 */
Status cfr_traverse_plus(const Game *game, GameState *state, InfoStore *store,
                         Player target_player, size_t iteration,
                         Utility *utility_out);

/*
 * Runs cfr_traverse_plus and publishes traversal statistics.
 *
 * All parameters except stats_out follow the cfr_traverse_plus contract.
 * stats_out follows the cfr_traverse_with_stats contract.
 */
Status cfr_traverse_plus_with_stats(
    const Game *game, GameState *state, InfoStore *store, Player target_player,
    size_t iteration, Utility *utility_out, TraversalStats *stats_out);

#endif
