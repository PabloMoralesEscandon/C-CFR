#ifndef CFR_GAME_H
#define CFR_GAME_H

#include <stdbool.h>
#include <stddef.h>

#include "cfr/types.h"

/*
 * Represents a state through an opaque type.
 *
 * The adapter defines the concrete representation. The caller owns each
 * instance. Operations borrow an instance for the duration of each call.
 */
typedef struct CfrGameState GameState;

typedef struct CfrGameOperations GameOperations;
typedef struct CfrGame Game;

/*
 * Defines the operations that an adapter provides to the engine.
 *
 * The adapter owns this table and its associated context. Both objects must
 * remain alive as long as a Game descriptor that uses them exists.
 *
 * The caller owns the states, arrays, and output variables. Each callback
 * borrows them. A callback must not retain, free, or change ownership of a
 * pointer it receives.
 *
 * A callback must not allocate memory during an operation. The caller provides
 * all storage needed by the operation.
 *
 * An output is valid only when the callback returns CFR_STATUS_SUCCESS. The
 * callback must preserve outputs when it returns an error. The only exception
 * is required_count when the buffer is too small.
 */
struct CfrGameOperations {
    /*
     * Reports whether the state is terminal.
     *
     * This query accepts any valid state. result receives a value only when
     * the operation completes successfully. An invalid state produces
     * CFR_STATUS_INVALID_ARGUMENT.
     */
    Status (*is_terminal)(const void *context, const GameState *state,
                          bool *result);

    /*
     * Gets the terminal utility for one player.
     *
     * The state must be terminal. player must identify a valid player. result
     * receives the utility only when the operation completes successfully. A
     * nonterminal state or invalid player produces CFR_STATUS_INVALID_ARGUMENT.
     */
    Status (*terminal_utility)(const void *context, const GameState *state,
                               Player player, Utility *result);

    /*
     * Gets the actor in a nonterminal state.
     *
     * result receives the actor only when the operation completes
     * successfully. A terminal or invalid state produces
     * CFR_STATUS_INVALID_ARGUMENT.
     */
    Status (*current_actor)(const void *context, const GameState *state,
                            Actor *result);

    /*
     * Enumerates the legal actions of a nonterminal state.
     *
     * capacity is the number of elements available in actions. The actions
     * pointer is required even when capacity is zero.
     *
     * required_count receives the number of elements needed. If capacity is
     * insufficient, the callback returns CFR_STATUS_BUFFER_TOO_SMALL. In that
     * case, required_count receives the required capacity and actions remains
     * unchanged. A terminal or invalid state produces
     * CFR_STATUS_INVALID_ARGUMENT.
     */
    Status (*legal_actions)(const void *context, const GameState *state,
                            Action *actions, size_t capacity,
                            size_t *required_count);

    /*
     * Applies a legal action to a nonterminal state.
     *
     * The callback returns CFR_STATUS_ILLEGAL_ACTION for an illegal action. An
     * invalid state produces CFR_STATUS_INVALID_ARGUMENT. The callback can
     * return CFR_STATUS_BUFFER_TOO_SMALL if it cannot record another action.
     * The callback must not modify the state when it returns an error.
     */
    Status (*apply_action)(const void *context, GameState *state,
                           Action action);

    /*
     * Undoes the last applied action.
     *
     * The state must contain an action that can be undone. The callback must
     * not modify the state when it returns an error. A state without history
     * produces CFR_STATUS_INVALID_ARGUMENT.
     */
    Status (*undo_action)(const void *context, GameState *state);

    /*
     * Gets the probability of a legal chance action.
     *
     * The current actor must be chance. result receives a value between zero
     * and one only when the operation completes successfully. A different
     * actor produces CFR_STATUS_INVALID_ARGUMENT. A non-chance action produces
     * CFR_STATUS_ILLEGAL_ACTION.
     */
    Status (*chance_probability)(const void *context, const GameState *state,
                                 Action action, Probability *result);

    /*
     * Gets the current player's information-set key.
     *
     * The current actor must be a player. States that the player cannot
     * distinguish must produce the same stable key. A different actor or an
     * invalid state produces CFR_STATUS_INVALID_ARGUMENT.
     */
    Status (*information_set_key)(const void *context, const GameState *state,
                                  InfoSetKey *result);
};

/* Describes a game without storing the state of a match. */
struct CfrGame {
    /* Borrowed table. The adapter retains ownership. */
    const GameOperations *operations;
    /*
     * Borrowed context. The adapter retains ownership.
     *
     * The pointer can be null if the adapter needs no configuration. An
     * adapter must document and validate any additional requirements.
     */
    const void *context;
    /* Upper bound on the number of legal actions in any state. */
    size_t max_legal_actions;
};

/*
 * Common rules for the cfr_game_* wrappers:
 *
 * Each wrapper validates game, operations, the callback, state, and required
 * outputs. Failed validation returns CFR_STATUS_INVALID_ARGUMENT. In that
 * case, the wrapper does not call the callback or modify outputs.
 *
 * A wrapper does not validate game rules. The adapter validates the phase,
 * player, and action. The wrapper returns the callback's Status unchanged.
 */

/* Queries whether state is terminal and writes the answer to result. */
Status cfr_game_is_terminal(const Game *game, const GameState *state,
                            bool *result);

/* Queries player terminal utility and writes it to result. */
Status cfr_game_terminal_utility(const Game *game, const GameState *state,
                                 Player player, Utility *result);

/* Queries the current actor and writes it to result. */
Status cfr_game_current_actor(const Game *game, const GameState *state,
                              Actor *result);

/*
 * Enumerates legal actions in the actions array.
 *
 * capacity counts elements. required_count receives the required count. The
 * actions pointer is required even when capacity is zero.
 */
Status cfr_game_legal_actions(const Game *game, const GameState *state,
                              Action *actions, size_t capacity,
                              size_t *required_count);

/* Applies action to the mutable state. */
Status cfr_game_apply_action(const Game *game, GameState *state, Action action);

/* Undoes the last action applied to the mutable state. */
Status cfr_game_undo_action(const Game *game, GameState *state);

/* Queries the probability of action at a chance node. */
Status cfr_game_chance_probability(const Game *game, const GameState *state,
                                   Action action, Probability *result);

/* Queries the stable key of the current information set. */
Status cfr_game_information_set_key(const Game *game, const GameState *state,
                                    InfoSetKey *result);

#endif
