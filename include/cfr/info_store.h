#ifndef CFR_INFO_STORE_H
#define CFR_INFO_STORE_H

#include <stddef.h>

#include "cfr/info_node.h"
#include "cfr/types.h"

typedef struct CfrInfoStoreEntry InfoStoreEntry;

/*
 * Relaciona claves de conjuntos de información con nodos de aprendizaje.
 *
 * El llamador posee la estructura InfoStore. El almacén posee el array de
 * celdas, cada InfoNode y los arrays internos de cada nodo. El llamador no debe
 * modificar los campos de InfoStore.
 *
 * El llamador no debe copiar por asignación un almacén inicializado. Una copia
 * tendría los mismos punteros y no tendría una propiedad independiente.
 *
 * El almacén no permite borrar un nodo individual. cfr_info_store_destroy
 * libera todos los recursos que posee el almacén.
 */
typedef struct {
    /* Array propio de celdas privadas. El llamador no usa este puntero. */
    InfoStoreEntry *entries;
    /* Número de nodos que contiene el almacén. */
    size_t size;
    /* Número de celdas reservadas en el array. */
    size_t capacity;
    /* Celdas con otra clave que encontraron find y get_or_create. */
    size_t collision_count;
    /* Número de crecimientos que terminaron correctamente. */
    size_t growth_count;
} InfoStore;

/* Contiene una copia de las estadísticas del almacén. */
typedef struct {
    /* Número de nodos que contiene el almacén. */
    size_t size;
    /* Número de celdas reservadas en el array. */
    size_t capacity;
    /*
     * Número acumulado de celdas con otra clave que encontraron find y
     * get_or_create. El contador se satura en SIZE_MAX y no vuelve a cero.
     */
    size_t collision_count;
    /* Número de crecimientos que terminaron correctamente. */
    size_t growth_count;
} InfoStoreStats;

/*
 * Inicializa info_store y reserva la tabla inicial.
 *
 * info_store debe estar puesto a cero o debe haber sido destruido. La función
 * devuelve CFR_STATUS_OUT_OF_MEMORY cuando no puede reservar la tabla. Un
 * error conserva info_store.
 */
Status cfr_info_store_init(InfoStore *info_store);

/*
 * Destruye info_store y deja todos sus campos a cero.
 *
 * La función libera las celdas, los nodos y los arrays internos de los nodos.
 * Un almacén puesto a cero o ya destruido produce CFR_STATUS_SUCCESS. Un
 * puntero nulo produce CFR_STATUS_INVALID_ARGUMENT.
 *
 * La destrucción invalida todos los punteros prestados por el almacén. La
 * función no libera la estructura InfoStore que posee el llamador.
 */
Status cfr_info_store_destroy(InfoStore *info_store);

/*
 * Busca key y escribe el nodo asociado en node_out.
 *
 * info_store debe estar inicializado. Si la clave existe, node_out recibe un
 * puntero prestado. El llamador no debe destruir ni liberar el nodo. El puntero
 * conserva su dirección durante los crecimientos del almacén. El puntero deja
 * de ser válido cuando el llamador destruye el almacén.
 *
 * Si la clave no existe, la función escribe un puntero nulo y devuelve
 * CFR_STATUS_NOT_FOUND. Un argumento inválido produce
 * CFR_STATUS_INVALID_ARGUMENT y conserva node_out.
 *
 * El sondeo puede aumentar collision_count aunque la función no encuentre la
 * clave.
 */
Status cfr_info_store_find(InfoStore *info_store, InfoSetKey key,
                           InfoNode **node_out);

/*
 * Obtiene el nodo de key o crea un nodo con action_count.
 *
 * info_store debe estar inicializado. action_count debe ser mayor que cero. Si
 * la clave existe, action_count debe coincidir con el número de acciones del
 * nodo. Una cantidad distinta produce CFR_STATUS_INVALID_ARGUMENT y conserva
 * el nodo.
 *
 * Si la clave no existe, la función crea un nodo. La función aumenta la
 * capacidad antes de que la inserción supere tres cuartos de las celdas. Cada
 * crecimiento duplica la capacidad y conserva las direcciones de los nodos.
 *
 * node_out recibe un puntero prestado solo cuando la función termina
 * correctamente. El llamador no debe destruir ni liberar el nodo. El puntero
 * conserva su dirección durante los crecimientos. El puntero deja de ser
 * válido cuando el llamador destruye el almacén.
 *
 * Un error conserva node_out y no publica un nodo parcial. Un error conserva
 * los nodos existentes. El sondeo puede aumentar collision_count antes del
 * error.
 */
Status cfr_info_store_get_or_create(InfoStore *info_store, InfoSetKey key,
                                    size_t action_count, InfoNode **node_out);

/*
 * Copia las estadísticas actuales en stats_out.
 *
 * La copia no contiene punteros y no cambia después de otra operación. Un
 * argumento inválido produce CFR_STATUS_INVALID_ARGUMENT y conserva
 * stats_out.
 */
Status cfr_info_store_get_stats(const InfoStore *info_store,
                                InfoStoreStats *stats_out);

#endif
