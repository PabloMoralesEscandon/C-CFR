# Ejemplos

El repositorio incluye un adaptador completo de Kuhn Poker. La aplicación de
`app/cfr_cli.c` conecta el adaptador con el entrenador y el evaluador.

La aplicación consume la API pública mediante las funciones `cfr_game_*`. La
aplicación también conserva la propiedad del estado del juego.

La aplicación no forma parte de la biblioteca pública. Consulte el
[`README.md`](../README.md) principal para construir y usar el ejecutable.

El ejemplo usa CFR clásico de forma predeterminada. Pase `--cfr-plus` para usar
Regret Matching+ y el promedio lineal de estrategias.
