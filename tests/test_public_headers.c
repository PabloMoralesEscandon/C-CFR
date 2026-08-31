#include "cfr/blackjack.h"
#include "cfr/checkpoint.h"
#include "cfr/evaluation.h"
#include "cfr/game.h"
#include "cfr/info_node.h"
#include "cfr/info_store.h"
#include "cfr/kuhn_poker.h"
#include "cfr/leduc_poker.h"
#include "cfr/trainer.h"
#include "cfr/traversal.h"
#include "cfr/types.h"
#include "test_suite.h"

int test_public_headers(void) {
    Player player = CFR_PLAYER_0;
    Actor actor = {.kind = CFR_ACTOR_PLAYER, .player = player};
    Action action = 0;
    InfoSetKey key = 0;
    Status status = CFR_STATUS_SUCCESS;
    Utility utility = 0.0;
    Probability probability = 1.0;
    InfoNode node = {0};
    InfoStore store = {0};
    KuhnPokerState kuhn_poker_state = {0};
    LeducPokerState leduc_poker_state = {0};
    BlackjackState blackjack_state = {0};
    EvaluationMetrics evaluation_metrics = {0};
    TrainerVariant trainer_variant = CFR_TRAINER_VARIANT_CFR_PLUS;
    Status (*checkpoint_writer)(FILE *, const Trainer *) =
        cfr_checkpoint_write;

    return (actor.player == CFR_PLAYER_0 && action == key &&
            status == CFR_STATUS_SUCCESS && utility == 0.0 &&
            probability == 1.0 && node.action_count == 0 && store.size == 0 &&
            kuhn_poker_state.public_action_count == 0 &&
            leduc_poker_state.public_action_count == 0 &&
            blackjack_state.player_hand.card_count == 0 &&
            evaluation_metrics.exploitability == 0.0 &&
            trainer_variant == CFR_TRAINER_VARIANT_CFR_PLUS &&
            checkpoint_writer != NULL)
               ? 0
               : 1;
}
#include "cfr/checkpoint.h"
