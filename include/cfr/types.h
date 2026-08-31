#ifndef CFR_TYPES_H
#define CFR_TYPES_H

#include <stdint.h>

#ifdef __cplusplus
#define CFR_EXTERN_C_BEGIN extern "C" {
#define CFR_EXTERN_C_END }
#define CFR_ENUM_INT(name) enum name : int
#else
#define CFR_EXTERN_C_BEGIN
#define CFR_EXTERN_C_END
#define CFR_ENUM_INT(name) enum name
#endif

CFR_EXTERN_C_BEGIN

/* Identifies one of the two players in the zero-sum game. */
typedef CFR_ENUM_INT(CfrPlayer) { CFR_PLAYER_0, CFR_PLAYER_1 } Player;

/* Identifies the entity that selects the next action. */
typedef CFR_ENUM_INT(CfrActorKind) {
    CFR_ACTOR_PLAYER,
    CFR_ACTOR_CHANCE
} ActorKind;

/* Describes the current actor. */
typedef struct {
    ActorKind kind;
    /* Interpret this field only when kind is CFR_ACTOR_PLAYER. */
    Player player;
} Actor;

/* Identifies an action according to the adapter's rules. */
typedef int Action;

/* Provides a stable identifier for an information set. */
typedef int64_t InfoSetKey;

/* Indicates the result of a contract operation. */
typedef CFR_ENUM_INT(CfrStatus) {
    /* The operation completed successfully. Outputs are valid. */
    CFR_STATUS_SUCCESS,
    /* An argument is null, invalid, or inconsistent with the state. */
    CFR_STATUS_INVALID_ARGUMENT,
    /* The action is not legal in the current state. */
    CFR_STATUS_ILLEGAL_ACTION,
    /* The caller-provided buffer does not have enough capacity. */
    CFR_STATUS_BUFFER_TOO_SMALL,
    /* An accumulator or arithmetic result is not finite. */
    CFR_STATUS_NUMERIC_ERROR,
    /* The module could not complete an internal allocation. */
    CFR_STATUS_OUT_OF_MEMORY,
    /* A valid lookup did not find the requested key. */
    CFR_STATUS_NOT_FOUND,
    /* A stream read or write operation failed. */
    CFR_STATUS_IO_ERROR,
    /* Serialized input is malformed, corrupt, or unsupported. */
    CFR_STATUS_FORMAT_ERROR,
    /* Serialized learning data belongs to another game schema. */
    CFR_STATUS_INCOMPATIBLE_GAME
} Status;

/* Represents the utility of a terminal state for one player. */
typedef double Utility;

/* Represents a probability in the closed interval from zero to one. */
typedef double Probability;

CFR_EXTERN_C_END

#endif
