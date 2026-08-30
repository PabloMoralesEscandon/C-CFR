#ifndef CFR_TRAVERSAL_H
#define CFR_TRAVERSAL_H

#include "cfr/game.h"
#include "cfr/info_store.h"

/* Número máximo de acciones legales que admite un recorrido. */
#define CFR_TRAVERSAL_MAX_ACTIONS 64

/* Contiene las estadísticas de un recorrido correcto. */
typedef struct {
    /*
     * Número de estados visitados. El contador incluye estados de jugador,
     * estados de azar y estados terminales. Cada entrada en la ayuda recursiva
     * cuenta como una visita. El contador se satura en SIZE_MAX.
     */
    size_t visited_nodes;
} TraversalStats;

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
 * game->max_legal_actions acciones legales. Estas condiciones también se
 * aplican a los estados descendientes.
 *
 * El recorrido enumera todas las acciones legales de un nodo de azar. El
 * recorrido consulta chance_probability una vez para cada acción. Cada
 * probabilidad debe ser finita y mayor o igual que cero. Una probabilidad cero
 * es válida y su rama también se recorre.
 *
 * La suma de las probabilidades debe ser uno dentro de las tolerancias del
 * módulo. La tolerancia relativa es 1e-8. La tolerancia absoluta es 1e-12. El
 * recorrido valida la distribución completa antes de aplicar la primera acción
 * del nodo de azar. Una distribución inválida produce
 * CFR_STATUS_INVALID_ARGUMENT.
 *
 * Un nodo de azar no crea un nodo de información y no genera deltas. El alcance
 * de azar pondera los arrepentimientos. El alcance de azar no pondera las sumas
 * de estrategia.
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

/*
 * Ejecuta cfr_traverse y publica las estadísticas del recorrido.
 *
 * game, state, store, target_player y utility_out tienen el contrato de
 * cfr_traverse. El llamador posee stats_out. La función toma stats_out prestado
 * y lo puede modificar.
 *
 * stats_out->visited_nodes cuenta cada estado que entra en la ayuda recursiva.
 * El contador incluye estados de jugador, estados de azar y estados terminales.
 * La función escribe utility_out y stats_out solo cuando devuelve
 * CFR_STATUS_SUCCESS. Un error conserva los valores anteriores de las dos
 * salidas.
 */
Status cfr_traverse_with_stats(const Game *game, GameState *state,
                               InfoStore *store, Player target_player,
                               Utility *utility_out, TraversalStats *stats_out);

/*
 * Recorre el árbol con las actualizaciones de CFR+ para target_player.
 *
 * game, state, store, target_player y utility_out tienen el contrato de
 * cfr_traverse. iteration identifica la iteración completa de CFR+ y debe ser
 * mayor que cero. El recorrido pondera por iteration la contribución a la
 * estrategia media y trunca a cero cada arrepentimiento actualizado que
 * resultaría negativo.
 *
 * La función conserva las garantías de restauración del estado y de
 * atomicidad de los acumulados de cfr_traverse. Un valor iteration igual a
 * cero produce CFR_STATUS_INVALID_ARGUMENT.
 */
Status cfr_traverse_plus(const Game *game, GameState *state, InfoStore *store,
                         Player target_player, size_t iteration,
                         Utility *utility_out);

/*
 * Ejecuta cfr_traverse_plus y publica las estadísticas del recorrido.
 *
 * Todos los parámetros salvo stats_out tienen el contrato de
 * cfr_traverse_plus. stats_out tiene el contrato de cfr_traverse_with_stats.
 */
Status cfr_traverse_plus_with_stats(
    const Game *game, GameState *state, InfoStore *store, Player target_player,
    size_t iteration, Utility *utility_out, TraversalStats *stats_out);

#endif
