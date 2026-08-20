#ifndef CFR_INFO_NODE_H
#define CFR_INFO_NODE_H

#include <stddef.h>

#include "cfr/types.h"

/*
 * Conserva el aprendizaje de un conjunto de información.
 *
 * El llamador posee la estructura. El nodo posee los arrays regret_sums y
 * strategy_sums. El llamador debe destruir el nodo antes de abandonar la
 * estructura. La destrucción libera los arrays, pero no libera la estructura.
 *
 * El llamador no debe copiar por asignación un nodo inicializado. Una copia
 * tendría los mismos punteros y no tendría una propiedad independiente.
 *
 * action_count fija el número de índices válidos. Los índices válidos están
 * en el intervalo desde cero hasta action_count menos uno. El adaptador debe
 * mantener una correspondencia estable entre cada índice y su acción legal.
 * El nodo no almacena esa correspondencia.
 *
 * Las operaciones de estrategia y arrepentimiento no reservan memoria. El
 * llamador proporciona los arrays de entrada y de salida. Una operación que
 * devuelve un error conserva el nodo y los arrays de salida.
 *
 * Un argumento numérico no finito produce CFR_STATUS_INVALID_ARGUMENT. Un
 * acumulado no finito o un resultado aritmético no finito produce
 * CFR_STATUS_NUMERIC_ERROR.
 */
typedef struct {
    /* Identifica la decisión observable que representa el nodo. */
    InfoSetKey key;
    /* Indica el número de acciones y de elementos en cada array interno. */
    size_t action_count;
    /* Array propio con un arrepentimiento acumulado por acción. */
    Utility *regret_sums;
    /* Array propio con una suma ponderada de estrategia por acción. */
    double *strategy_sums;
} InfoNode;

/*
 * Inicializa node con key y action_count.
 *
 * node debe estar puesto a cero o debe haber sido destruido. action_count debe
 * ser mayor que cero. La función reserva los dos arrays internos y pone sus
 * elementos a cero.
 *
 * Un argumento inválido produce CFR_STATUS_INVALID_ARGUMENT. Un fallo de una
 * reserva produce CFR_STATUS_OUT_OF_MEMORY. Un error conserva node.
 */
Status cfr_info_node_init(InfoNode *node, InfoSetKey key, size_t action_count);

/*
 * Destruye node y deja todos sus campos a cero.
 *
 * node debe ser distinto de nulo. Un nodo ya destruido produce
 * CFR_STATUS_SUCCESS. La función no libera la estructura que contiene el nodo.
 */
Status cfr_info_node_destroy(InfoNode *node);

/*
 * Calcula la estrategia actual y la escribe en strategy_array.
 *
 * node debe estar inicializado. strategy_array debe ser distinto de nulo.
 * strategy_capacity cuenta elementos y debe ser igual o mayor que
 * node->action_count. Una capacidad menor produce
 * CFR_STATUS_BUFFER_TOO_SMALL.
 *
 * La función usa los arrepentimientos positivos. La función usa una
 * distribución uniforme cuando no existe un arrepentimiento positivo. Un
 * arrepentimiento almacenado no finito produce CFR_STATUS_NUMERIC_ERROR.
 */
Status cfr_info_node_current_strategy(const InfoNode *node,
                                      Probability *strategy_array,
                                      size_t strategy_capacity);

/*
 * Suma regret_change al arrepentimiento de action_index.
 *
 * node debe estar inicializado. action_index debe ser menor que
 * node->action_count. regret_change debe ser finito. Un resultado aritmético
 * no finito produce CFR_STATUS_NUMERIC_ERROR.
 */
Status cfr_info_node_add_regret(InfoNode *node, size_t action_index,
                                Utility regret_change);

/*
 * Acumula strategy_array con el peso weight.
 *
 * strategy_count debe ser igual a node->action_count. Cada probabilidad debe
 * ser finita y debe estar en el intervalo cerrado de cero a uno. La suma debe
 * ser uno dentro de la tolerancia numérica del módulo. weight debe ser finito
 * y debe estar en el intervalo cerrado de cero a uno.
 *
 * Una entrada inválida produce CFR_STATUS_INVALID_ARGUMENT. Un acumulado no
 * finito o negativo produce CFR_STATUS_NUMERIC_ERROR. Un resultado aritmético
 * no finito también produce CFR_STATUS_NUMERIC_ERROR. Un error conserva todos
 * los acumulados.
 */
Status cfr_info_node_accumulate_strategy(InfoNode *node,
                                         const Probability *strategy_array,
                                         size_t strategy_count,
                                         Probability weight);

/*
 * Calcula la estrategia media y la escribe en strategy_array.
 *
 * node debe estar inicializado. strategy_array debe ser distinto de nulo.
 * strategy_capacity cuenta elementos y debe ser igual o mayor que
 * node->action_count. Una capacidad menor produce
 * CFR_STATUS_BUFFER_TOO_SMALL.
 *
 * La función normaliza strategy_sums. La función usa una distribución
 * uniforme cuando todos los acumulados son cero. Un acumulado no finito o
 * negativo produce CFR_STATUS_NUMERIC_ERROR.
 */
Status cfr_info_node_average_strategy(const InfoNode *node,
                                      Probability *strategy_array,
                                      size_t strategy_capacity);

#endif
