#ifndef CFR_EVALUATION_H
#define CFR_EVALUATION_H

#include <stddef.h>

#include "cfr/game.h"
#include "cfr/info_store.h"
#include "cfr/types.h"

/*
 * Contiene las métricas de una evaluación puntual.
 *
 * La estructura contiene valores y no posee memoria dinámica. Cada valor usa
 * la utilidad del jugador correspondiente.
 */
typedef struct {
    /* Valor del jugador cero en el perfil de estrategia media. */
    Utility profile_value_player_0;
    /* Valor del jugador uno en el perfil de estrategia media. */
    Utility profile_value_player_1;
    /* Mejor valor del jugador cero contra la estrategia media rival. */
    Utility best_response_value_player_0;
    /* Mejor valor del jugador uno contra la estrategia media rival. */
    Utility best_response_value_player_1;
    /* Diferencia entre la mejor respuesta y el perfil del jugador cero. */
    Utility improvement_player_0;
    /* Diferencia entre la mejor respuesta y el perfil del jugador uno. */
    Utility improvement_player_1;
    /* Suma de las mejoras de los dos jugadores. */
    Utility nash_conv;
    /* NashConv dividido entre dos según la convención de este proyecto. */
    Utility exploitability;
} EvaluationMetrics;

/*
 * Copia la estrategia media del conjunto que identifica key.
 *
 * La función normaliza strategy_sums. La función no usa la estrategia actual.
 * store debe estar inicializado. strategy_out y required_count son
 * obligatorios, incluso si capacity vale cero. capacity cuenta elementos.
 *
 * Un éxito publica la estrategia y la cantidad de acciones. Una capacidad
 * insuficiente conserva strategy_out. En ese caso, la función publica la
 * cantidad necesaria y devuelve CFR_STATUS_BUFFER_TOO_SMALL.
 *
 * Una clave ausente devuelve CFR_STATUS_NOT_FOUND. La función no crea un nodo.
 * Un argumento inválido devuelve CFR_STATUS_INVALID_ARGUMENT. Un acumulado
 * negativo o no finito devuelve CFR_STATUS_NUMERIC_ERROR. Estos errores
 * conservan las dos salidas.
 *
 * La función no modifica el almacén ni sus estadísticas.
 */
Status cfr_evaluation_average_strategy(const InfoStore *store, InfoSetKey key,
                                       Probability *strategy_out,
                                       size_t capacity, size_t *required_count);

/*
 * Evalúa el perfil de estrategia media desde la perspectiva de player.
 *
 * game, state, store y utility_out son obligatorios. player debe identificar
 * un jugador válido. game->max_legal_actions debe ser mayor que cero.
 *
 * La función enumera todas las ramas, incluidas las ramas de probabilidad cero.
 * La función no usa muestreo. La función no usa la estrategia actual. La
 * memoria temporal crece con el árbol completo.
 *
 * Un éxito publica utility_out y restaura state. La función no modifica store.
 * Un error conserva utility_out.
 *
 * Una clave ausente produce
 * CFR_STATUS_NOT_FOUND. Un fallo de reserva produce
 * CFR_STATUS_OUT_OF_MEMORY. Un argumento o modelo incoherente produce
 * CFR_STATUS_INVALID_ARGUMENT. Un cálculo no finito produce
 * CFR_STATUS_NUMERIC_ERROR.
 *
 * La función propaga los errores de las operaciones del juego. La función
 * restaura state antes de propagar un error de una rama. Si undo_action falla,
 * ese error tiene prioridad y state puede quedar sin restaurar.
 */
Status cfr_evaluation_profile_value(const Game *game, GameState *state,
                                    const InfoStore *store, Player player,
                                    Utility *utility_out);

/*
 * Calcula la mejor respuesta de player contra la estrategia media del rival.
 *
 * game, state, store y utility_out son obligatorios. player debe identificar
 * un jugador válido. game->max_legal_actions debe ser mayor que cero.
 *
 * La mejor respuesta usa una acción determinista por conjunto de información.
 * Todas las apariciones del conjunto usan la misma acción. La función enumera
 * el árbol completo y no usa muestreo. La memoria temporal crece con el árbol.
 *
 * Un éxito publica utility_out y restaura state. La función no modifica store.
 * Un error conserva utility_out.
 *
 * Una clave ausente produce
 * CFR_STATUS_NOT_FOUND. Un fallo de reserva produce
 * CFR_STATUS_OUT_OF_MEMORY. Un argumento o modelo incoherente produce
 * CFR_STATUS_INVALID_ARGUMENT. Un cálculo no finito produce
 * CFR_STATUS_NUMERIC_ERROR.
 *
 * La función propaga los errores de las operaciones del juego. La función
 * restaura state antes de propagar un error de una rama. Si undo_action falla,
 * ese error tiene prioridad y state puede quedar sin restaurar.
 */
Status cfr_evaluation_best_response_value(const Game *game, GameState *state,
                                          const InfoStore *store, Player player,
                                          Utility *utility_out);

/*
 * Calcula todas las métricas en una sola evaluación del árbol.
 *
 * game, state, store y eval_out son obligatorios.
 * game->max_legal_actions debe ser mayor que cero. La función usa la estrategia
 * media y no usa la estrategia actual.
 *
 * La función construye una sola instantánea del árbol completo. La función
 * enumera las ramas de probabilidad cero y no usa muestreo. La memoria temporal
 * crece con el árbol completo.
 *
 * NashConv es la suma de las dos mejoras. La explotabilidad es NashConv
 * dividido entre dos según la convención de este proyecto.
 *
 * Un éxito publica eval_out y restaura state. La función no modifica store. Un
 * error conserva eval_out.
 *
 * Una clave ausente produce CFR_STATUS_NOT_FOUND. Un fallo de reserva produce
 * CFR_STATUS_OUT_OF_MEMORY. Un argumento o modelo incoherente produce
 * CFR_STATUS_INVALID_ARGUMENT. Un cálculo no finito produce
 * CFR_STATUS_NUMERIC_ERROR.
 *
 * La función propaga los errores de las operaciones del juego. La función
 * restaura state antes de propagar un error de una rama. Si undo_action falla,
 * ese error tiene prioridad y state puede quedar sin restaurar.
 */
Status cfr_evaluation_metrics(const Game *game, GameState *state,
                              const InfoStore *store,
                              EvaluationMetrics *eval_out);

#endif
