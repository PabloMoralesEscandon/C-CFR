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
 * Node accumulators are copied under their locks, so concurrent updates cannot
 * tear an individual record. The checkpoint is not a globally coordinated
 * snapshot: callers that need an exactly resumable multi-worker training state
 * must pause all workers before this call. Trainer counters and random-stream
 * state are not synchronized by this function.
 *
 * The caller owns stream, must open it in binary mode, and remains responsible
 * for closing it. A successful call does not flush stream. A stream failure
 * returns CFR_STATUS_IO_ERROR. Invalid trainer data returns
 * CFR_STATUS_INVALID_ARGUMENT or CFR_STATUS_NUMERIC_ERROR.
 */
Status cfr_checkpoint_write(FILE *stream, const Trainer *trainer);

/*
 * Writes the same checkpoint payload as cfr_checkpoint_write in a Zstandard
 * frame at compression level 1. The raw payload retains its version and CRC,
 * and decompression reproduces cfr_checkpoint_write output byte for byte.
 * The same worker-quiescence requirement applies when the checkpoint must be
 * exactly resumable.
 *
 * The caller owns stream, must open it in binary mode, and remains responsible
 * for closing it. A successful call completes the Zstandard frame but does not
 * flush stream.
 */
Status cfr_checkpoint_write_zstd(FILE *stream, const Trainer *trainer);

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
 * Reads one Zstandard-framed checkpoint written by
 * cfr_checkpoint_write_zstd. Concatenated frames and trailing data are
 * rejected. The ownership and transactional guarantees are the same as for
 * cfr_checkpoint_read.
 */
Status cfr_checkpoint_read_zstd(FILE *stream, const Game *game,
                                GameState *state, InfoStore *store_out,
                                Trainer *trainer_out);

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
