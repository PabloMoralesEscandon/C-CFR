#ifndef CFR_TRAVERSAL_H
#define CFR_TRAVERSAL_H

#include "cfr/game.h"
#include "cfr/info_store.h"

/* Número máximo de acciones legales que admite un recorrido. */
#define CFR_TRAVERSAL_MAX_ACTIONS 64

/*
 * Recorre el árbol del juego y actualiza el aprendizaje de target_player.
 *
 * El llamador posee game, state, store y utility_out. La función toma estos
 * parámetros prestados. game debe ser un descriptor válido. state y store
 * deben estar inicializados y se pueden modificar. utility_out debe ser un
 * puntero válido.
 *
 * game->max_legal_actions debe estar entre uno y
 * CFR_TRAVERSAL_MAX_ACTIONS. Cada estado no terminal debe tener entre una y
 * game->max_legal_actions acciones legales. El recorrido no admite actores de
 * azar. Estas condiciones también se aplican a los estados descendientes.
 *
 * La función aplica y deshace acciones sobre state. La función restaura state
 * después de cada acción que se aplicó correctamente. Si una operación para
 * deshacer una acción falla, la función devuelve ese error y no puede
 * garantizar la restauración de state.
 *
 * store conserva la propiedad de sus nodos. La función puede añadir nodos y
 * cambiar sus estadísticas internas. Si ocurre un error, los acumulados que
 * existían antes de la llamada no cambian. Los nodos nuevos con acumulados a
 * cero pueden permanecer en store.
 *
 * Una llamada correcta usa una estrategia fija para cada nodo. Solo actualiza
 * los arrepentimientos y las sumas de estrategia de target_player.
 * utility_out recibe la utilidad desde la perspectiva de target_player. Un
 * error conserva el valor anterior de utility_out.
 */
Status cfr_traverse(const Game *game, GameState *state, InfoStore *store,
                    Player target_player, Utility *utility_out);

#endif
