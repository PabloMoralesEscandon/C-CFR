#include <stdio.h>

int test_public_headers(void);
int test_game_contract(void);

int main(void) {
    int failures = 0;

    failures += test_public_headers();
    failures += test_game_contract();

    if (failures != 0) {
        fprintf(stderr, "Fallaron %d comprobaciones.\n", failures);
        return 1;
    }

    puts("Todas las pruebas terminaron correctamente.");
    return 0;
}
