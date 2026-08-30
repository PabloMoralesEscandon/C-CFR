#ifndef CFR_TRAINER_H
#define CFR_TRAINER_H

#include <stddef.h>

#include "cfr/game.h"
#include "cfr/info_store.h"

/*
 * Contiene una copia de las estadísticas acumuladas del entrenador.
 *
 * Los cuatro contadores se saturan en SIZE_MAX. Un contador saturado no vuelve
 * a cero durante una ejecución posterior.
 */
typedef struct {
    /* Iteraciones que completaron todos los recorridos estratégicos. */
    size_t iterations;
    /* Número de recorridos que terminaron correctamente. */
    size_t traversals;
    /* Número de estados visitados por los recorridos correctos. */
    size_t visited_nodes;
    /* Número de recorridos fallidos dentro de cfr_trainer_run. */
    size_t errors;
} TrainerStats;

/*
 * Conserva los préstamos y las estadísticas de un entrenamiento.
 *
 * El llamador posee Trainer, Game, GameState e InfoStore. El entrenador toma
 * game, state y store prestados. El llamador debe mantener los tres préstamos
 * vivos mientras use el entrenador.
 *
 * game es un préstamo constante. state y store son préstamos modificables. El
 * entrenador aplica y deshace acciones sobre state. El entrenador añade
 * aprendizaje a store.
 *
 * El entrenador no posee los tres préstamos. cfr_trainer_init no reserva
 * memoria. El entrenador tampoco conserva una copia del estado raíz.
 */
typedef struct {
    /* Descriptor constante y prestado del juego. */
    const Game *game;
    /* Estado raíz modificable y prestado. */
    GameState *state;
    /* Almacén modificable y prestado. */
    InfoStore *store;
    /* Estadísticas que pertenecen al entrenador. */
    TrainerStats stats;
} Trainer;

/*
 * Inicializa trainer con tres préstamos y pone las estadísticas a cero.
 *
 * trainer, game, state y store deben ser distintos de nulo. El llamador debe
 * proporcionar un juego, un estado y un almacén válidos. El descriptor debe
 * declarar uno o dos jugadores estratégicos. Un argumento nulo o una cantidad
 * estratégica inválida produce CFR_STATUS_INVALID_ARGUMENT. Un error conserva
 * un trainer no nulo.
 */
Status cfr_trainer_init(Trainer *trainer, const Game *game, GameState *state,
                        InfoStore *store);

/*
 * Ejecuta amount iteraciones con actualización alterna.
 *
 * trainer debe estar inicializado. Los tres préstamos de trainer deben ser
 * válidos. Una iteración ejecuta, en orden, un recorrido para cada jugador
 * declarado estratégico por game->strategic_player_count. El primer recorrido
 * usa CFR_PLAYER_0 y, cuando la cantidad es dos, el segundo usa CFR_PLAYER_1 y
 * observa el aprendizaje que confirmó el primero.
 *
 * Cada recorrido confirma sus propios cambios. Si un recorrido posterior
 * falla, los cambios de los anteriores permanecen en store. iterations aumenta
 * solo después de todos los recorridos estratégicos de la iteración.
 * traversals y visited_nodes aumentan después de cada recorrido correcto.
 * errors aumenta después de un recorrido fallido. Las estadísticas se
 * acumulan entre llamadas. Los cuatro contadores se saturan en SIZE_MAX.
 *
 * Un valor amount igual a cero produce CFR_STATUS_SUCCESS y no cambia el
 * entrenador. La función devuelve sin cambios el Status de un recorrido
 * fallido.
 *
 * Después de cualquier error, el llamador debe restaurar state a la raíz antes
 * de volver a llamar a cfr_trainer_run. El entrenador no puede comprobar si
 * state representa la raíz. Si una operación para deshacer una acción falla,
 * state puede permanecer en un estado descendiente.
 */
Status cfr_trainer_run(Trainer *trainer, size_t amount);

/*
 * Copia las estadísticas de trainer en stats_out.
 *
 * trainer y stats_out deben ser distintos de nulo. La función no modifica
 * trainer. Un argumento nulo produce CFR_STATUS_INVALID_ARGUMENT y conserva
 * stats_out.
 */
Status cfr_trainer_get_stats(const Trainer *trainer, TrainerStats *stats_out);

/*
 * Pone a cero los cuatro contadores de trainer.
 *
 * trainer debe ser distinto de nulo. La función conserva game, state y store.
 * La función no modifica el estado ni el aprendizaje del almacén.
 */
Status cfr_trainer_reset_stats(Trainer *trainer);

#endif
