#ifndef CFR_CHECKPOINT_H
#define CFR_CHECKPOINT_H

#include <stdio.h>

#include "cfr/game.h"
#include "cfr/info_store.h"
#include "cfr/trainer.h"

CFR_EXTERN_C_BEGIN

/*
 * Writes a complete, resumable trainer checkpoint to stream.
 *
 * The checkpoint contains the trainer variant, training iteration counter,
 * statistics, cumulative regrets, cumulative strategy sums, and the random
 * stream state required to continue MCCFR exactly. Records are ordered by
 * information-set key and encoded in a portable, versioned binary format. The
 * game descriptor must provide a valid strategy_schema_id.
 *
 * The caller owns stream, must open it in binary mode, and remains responsible
 * for closing it. A successful call does not flush stream. A stream failure
 * returns CFR_STATUS_IO_ERROR. Invalid trainer data returns
 * CFR_STATUS_INVALID_ARGUMENT or CFR_STATUS_NUMERIC_ERROR.
 */
Status cfr_checkpoint_write(FILE *stream, const Trainer *trainer);

/*
 * Reads a complete checkpoint and binds a restored trainer to game and state.
 *
 * store_out must be zero-initialized or previously destroyed. state must be a
 * root state suitable for subsequent training or evaluation. The checkpoint's
 * schema identifier must equal game->strategy_schema_id.
 *
 * Success transfers ownership of the restored nodes to store_out and publishes
 * trainer_out. An error preserves both outputs and does not modify state. The
 * caller owns stream, must open it in binary mode, and remains responsible for
 * closing it.
 */
Status cfr_checkpoint_read(FILE *stream, const Game *game, GameState *state,
                           InfoStore *store_out, Trainer *trainer_out);

/*
 * Writes a deterministic, human-readable snapshot of the average strategy.
 *
 * The export contains normalized probabilities ordered by information-set key
 * and action index. It intentionally omits regrets and cannot be imported or
 * used to resume training. The caller owns and closes stream. Success does not
 * flush it.
 */
Status cfr_strategy_write_text(FILE *stream, const Trainer *trainer);

CFR_EXTERN_C_END

#endif
