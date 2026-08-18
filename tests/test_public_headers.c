#include "cfr/types.h"
#include "cfr/game.h"

int test_public_headers(void) {
    Player player = CFR_PLAYER_0;
    Actor actor = {.kind = CFR_ACTOR_PLAYER, .player = player};
    Action action = 0;
    InfoSetKey key = 0;
    Status status = CFR_STATUS_SUCCESS;
    Utility utility = 0.0;
    Probability probability = 1.0;

    return (actor.player == CFR_PLAYER_0 && action == key &&
            status == CFR_STATUS_SUCCESS && utility == 0.0 &&
            probability == 1.0)
               ? 0
               : 1;
}
