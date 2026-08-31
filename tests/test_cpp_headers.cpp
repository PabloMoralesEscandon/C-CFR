#include <type_traits>

#include "cfr/blackjack.h"
#include "cfr/checkpoint.h"
#include "cfr/evaluation.h"
#include "cfr/game.h"
#include "cfr/info_node.h"
#include "cfr/info_store.h"
#include "cfr/kuhn_poker.h"
#include "cfr/leduc_poker.h"
#include "cfr/mccfr.h"
#include "cfr/trainer.h"
#include "cfr/traversal.h"
#include "cfr/types.h"

static_assert(std::is_standard_layout_v<Game>);
static_assert(std::is_standard_layout_v<InfoNode>);
static_assert(std::is_standard_layout_v<InfoStore>);
static_assert(std::is_standard_layout_v<Trainer>);
static_assert(std::is_trivially_copyable_v<EvaluationMetrics>);
