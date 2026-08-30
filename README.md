# CFR en C17

Este proyecto implementa una biblioteca de CFR para juegos extensivos finitos.
La primera versión admite dos jugadores y juegos de suma cero.

El repositorio incluye adaptadores completos de Kuhn Poker y blackjack. Dos
aplicaciones independientes permiten entrenar cada juego; los dos adaptadores
también se pueden usar desde la misma API pública de la biblioteca.

El adaptador de blackjack está declarado en `include/cfr/blackjack.h` y sigue
estas reglas: una baraja de 52 cartas sin reposición, banca plantada en 17
suave, blackjack natural pagado 3:2 y acciones de pedir o plantarse. No incluye
doblaje, separación, seguro ni rendición. `CFR_PLAYER_0` es el jugador y la
banca recibe como `CFR_PLAYER_1` la utilidad opuesta; las extracciones regladas
de la banca se representan como nodos de azar.

## Construcción

Ejecute los comandos desde la raíz del repositorio.

```sh
make all
```

Este objetivo crea los siguientes archivos de entrega:

- `build/release/libcfr.a`
- `build/release/cfr-kuhn`
- `build/release/cfr-blackjack`

La biblioteca no contiene las aplicaciones. Los archivos `app/cfr_cli.c` y
`app/blackjack_cli.c` consumen la API pública como aplicaciones externas.

Para construir únicamente la biblioteca y el ejecutable de blackjack:

```sh
make blackjack
```

Use el siguiente objetivo para crear la configuración de depuración:

```sh
make debug
```

Este objetivo crea la biblioteca, la suite C y las dos aplicaciones de
depuración. Los archivos quedan en `build/debug`.

Use `make clean` para eliminar el directorio `build`.

## Pruebas

Ejecute la suite C y la prueba integral de la aplicación:

```sh
make test
```

La prueba integral de Kuhn comprueba los argumentos, los informes, la
estrategia media, la reproducibilidad y la convergencia. Las pruebas de
blackjack comprueban sus reglas, su integración con `Trainer` sobre un subárbol
acotado y su interfaz sin lanzar involuntariamente un recorrido completo. Se
pueden ejecutar de forma aislada:

```sh
make test-blackjack
```

Use `make test-blackjack-cli` si solo quiere comprobar los argumentos y la
ayuda del ejecutable.

Ejecute la inyección de fallos de reserva:

```sh
make test-alloc
```

Este objetivo inyecta fallos en la suite C de la biblioteca. El objetivo no
inyecta fallos en `cfr-kuhn`.

Los siguientes objetivos ejecutan las pruebas con sanitizadores:

```sh
make test-asan
make test-ubsan
make test-sanitize
```

`test-asan` usa AddressSanitizer. `test-ubsan` usa
UndefinedBehaviorSanitizer. `test-sanitize` combina los dos sanitizadores.

Los objetivos necesitan un compilador que admita las opciones solicitadas. Un
compilador sin ese soporte produce un fallo visible.

## Uso de Kuhn Poker

Muestre la ayuda con uno de estos comandos:

```sh
build/release/cfr-kuhn --help
build/release/cfr-kuhn -h
```

La forma general es esta:

```text
cfr-kuhn --iterations N [--report-every N] [--print-strategy]
```

| Opción | Descripción |
|---|---|
| `--iterations N` | Ejecuta `N` iteraciones. `N` debe ser un entero decimal positivo. |
| `--report-every N` | Publica un informe después de cada bloque de hasta `N` iteraciones. |
| `--print-strategy` | Publica la estrategia media después del último informe. |
| `--help`, `-h` | Publica la ayuda y no inicializa CFR. |

Si omite `--report-every`, la aplicación publica solo el informe final. Use
una frecuencia explícita durante una ejecución larga.

Por ejemplo, use `--report-every 10000` con 100 000 iteraciones. La aplicación
publicará diez informes. Una frecuencia menor aumenta el coste de evaluación.

### Códigos de salida

| Código | Significado |
|---|---|
| `0` | La ayuda o la ejecución terminó correctamente. |
| `1` | Falló una operación, el reloj o una escritura. |
| `2` | Los argumentos son inválidos. |

Estos códigos son los valores que devuelve `main`. El sistema operativo también
puede terminar el proceso mediante una señal. Por ejemplo, una tubería cerrada
puede producir `SIGPIPE`.

## Informes

Cada informe contiene los siguientes campos:

| Campo | Significado |
|---|---|
| `iteraciones` | Número de iteraciones completadas. |
| `valor_medio_jugador_0` | Valor del jugador cero en el perfil de estrategia media. |
| `explotabilidad` | `NashConv` dividido entre dos según la convención del proyecto. |
| `conjuntos_informacion` | Número de conjuntos de información del almacén. |
| `segundos` | Tiempo transcurrido desde el inicio de la ejecución. |

Una explotabilidad menor indica un perfil más difícil de explotar. Una
explotabilidad de cero no permite una mejora unilateral.

El tiempo solo informa sobre la ejecución. El tiempo no cambia el aprendizaje.

## Ejecución corta

Este comando ejecuta cinco iteraciones y publica tres informes:

```sh
build/release/cfr-kuhn --iterations 5 --report-every 2
```

Una ejecución produjo esta salida:

```text
informe iteraciones=2 valor_medio_jugador_0=5.5511151231257827e-17 explotabilidad=0.27083333333333343 conjuntos_informacion=12 segundos=0.000056
informe iteraciones=4 valor_medio_jugador_0=-0.05598958333333337 explotabilidad=0.14062500000000003 conjuntos_informacion=12 segundos=0.000107
informe iteraciones=5 valor_medio_jugador_0=-0.048166666666666691 explotabilidad=0.12138888888888885 conjuntos_informacion=12 segundos=0.000131
```

El campo `segundos` cambia entre ejecuciones.

Añada `--print-strategy` para publicar las doce decisiones. La aplicación
mantiene un orden estable por contexto y por carta.

## Validación larga

Use este comando para repetir la validación larga:

```sh
build/release/cfr-kuhn --iterations 100000 --report-every 100000
```

El valor del jugador cero debe quedar a menos de `0,0001` de `-1/18`. La
explotabilidad debe ser menor o igual que `0,01`.

La medición de esta documentación produjo estos valores:

```text
informe iteraciones=100000 valor_medio_jugador_0=-0.055556357899689546 explotabilidad=1.7310539843606865e-05 conjuntos_informacion=12 segundos=0.556525
```

La distancia respecto de `-1/18` es aproximadamente `8,02e-07`. Los dos
resultados cumplen los límites documentados.

CFR aproxima un equilibrio. La salida no representa una solución exacta.

## Reproducibilidad

El entrenador enumera los resultados de azar. Por ello, el programa no usa una
semilla aleatoria.

Dos ejecuciones con los mismos argumentos producen las mismas métricas. También
producen las mismas estrategias y el mismo orden.

La comparación debe excluir únicamente el campo `segundos`. El tiempo depende
del sistema y puede cambiar entre ejecuciones.

## Referencia de rendimiento

Esta referencia corresponde a una medición concreta del 29 de agosto de 2026.

| Elemento | Valor |
|---|---|
| Compilador | GCC 16.1.1 |
| Sistema | Linux 7.1.3 x86_64 |
| Procesador | AMD Ryzen AI 7 350 con Radeon 860M |
| Configuración | Entrega con `-O2` mediante `make all` |
| Comando | `build/release/cfr-kuhn --iterations 100000 --report-every 100000` |
| Tiempo real | `0,557319` segundos |
| Memoria residente máxima | `1.620` KiB |

Una sonda temporal midió el proceso hijo con `CLOCK_MONOTONIC` y `wait4`. En
Linux, `ru_maxrss` publica la memoria residente máxima en KiB.

Esta medición es una referencia. No es una promesa de rendimiento. El resultado
puede cambiar con el compilador, el equipo y la carga del sistema.

## Uso de blackjack

Muestre la ayuda mediante:

```sh
build/release/cfr-blackjack --help
```

La forma general es:

```text
cfr-blackjack --iterations N [--report-every N] [--evaluate]
```

| Opción | Descripción |
|---|---|
| `--iterations N` | Ejecuta `N` iteraciones completas de entrenamiento. |
| `--report-every N` | Publica estadísticas después de cada bloque de hasta `N` iteraciones. |
| `--evaluate` | Evalúa el perfil medio al terminar y publica su valor y explotabilidad. |
| `--help`, `-h` | Publica la ayuda sin inicializar CFR. |

Empiece siempre con una sola iteración y sin evaluación:

```sh
build/release/cfr-blackjack --iterations 1 --report-every 1
```

Cada iteración ejecuta un recorrido para el único jugador estratégico y
enumera el árbol completo. La banca sigue una política determinista y sus
extracciones son nodos de azar, por lo que no recibe un segundo recorrido. El
árbol de una baraja de blackjack es mucho mayor que el de Kuhn.
La opción `--evaluate` realiza otra enumeración completa y puede necesitar
considerablemente más tiempo y memoria. El ejecutable publica y vacía una
línea `inicio` antes de comenzar el primer recorrido, de modo que una ejecución
larga ofrece confirmación inmediata de su configuración.

Cada informe de entrenamiento contiene:

| Campo | Significado |
|---|---|
| `iteraciones` | Iteraciones completas. |
| `recorridos` | Recorridos CFR completos acumulados. |
| `nodos_visitados` | Estados visitados acumulados. |
| `conjuntos_informacion` | Decisiones distintas almacenadas. |
| `segundos` | Tiempo desde el inicio del proceso. |

Con `--evaluate`, la última línea contiene el valor medio del jugador, el valor
opuesto de la banca, la explotabilidad y el tiempo total.
