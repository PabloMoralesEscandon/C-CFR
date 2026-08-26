#include <stdio.h>

#include "test_suite.h"

int main(void) {
    int failures = 0;

    failures += test_public_headers();
    failures += test_game_contract();
    failures += test_info_node();
    failures += test_info_store();
    failures += test_traversal();
    failures += test_chance_trainer();
    failures += test_kuhn_poker();

    if (failures != 0) {
        fprintf(stderr, "Fallaron %d comprobaciones.\n", failures);
        return 1;
    }

    puts("Todas las pruebas terminaron correctamente.");
    return 0;
}
