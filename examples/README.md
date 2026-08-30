# Ejemplos

El repositorio incluye adaptadores completos de Kuhn Poker y blackjack. La
aplicación de `app/cfr_cli.c` conecta Kuhn Poker con el entrenador y el
evaluador. `app/blackjack_cli.c` hace lo mismo con blackjack y evita la
evaluación exhaustiva salvo que se solicite mediante `--evaluate`.

La aplicación consume la API pública mediante las funciones `cfr_game_*`. La
aplicación también conserva la propiedad del estado del juego.

La aplicación no forma parte de la biblioteca pública. Consulte el
[`README.md`](../README.md) principal para construir y usar el ejecutable.
