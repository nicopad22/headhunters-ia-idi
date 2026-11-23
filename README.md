# Headhunters en la era de la IA — Consola ESP-32D (IDI1015)

Repositorio de firmware, datos del juego, diagnósticos de hardware y planificación electrónica para el proyecto final de **IDI1015 (Pensamiento Visual UC)**: *Headhunters en la era de la IA*.

## Documentación principal

- [PLAN_Y_REQUERIMIENTOS.md](./PLAN_Y_REQUERIMIENTOS.md) — Especificación del reglamento digital, directivas de reimpresión y BOM en Santiago.
- [docs/WIRING_Y_CAJA.md](./docs/WIRING_Y_CAJA.md) — Cableado exacto y distribución en caja de `12 × 12 × 3 cm`.
- [docs/DIMENSIONES.md](./docs/DIMENSIONES.md) — Todas las medidas físicas en mm (tablero `50×50 cm`, caja `120×120×30 mm`, componentes y tarjetas PVC `85,6×54 mm`), junto a la plantilla 1:1 [`docs/caja_120x120_1a1.svg`](./docs/caja_120x120_1a1.svg) y [`data/dimensions.json`](./data/dimensions.json).

## Flujo rápido de trabajo

### 1. Probar el flujo completo del juego con 6 tarjetas cualquiera (Dummy Flow Test)
Ejecuta el firmware real (OLED 1.3" SH1106 + RC522 + 5 botones + buzzer) en un recorrido guiado de **11 pasos** con instrucciones en pantalla y Monitor Serie:

```bash
cd firmware
./run_firmware.sh --dummy-test
```

### 2. Registrar las 50 tarjetas NFC desde la consola (Modo Admin)
Sin PC: flashea el firmware de producción y asigna cada tarjeta desde el mismo tablero.

1. **Entrar:** mantén **ATRÁS/CANCELAR 10 s** hasta oír un doble *bip*, suelta y presiona **P1 → P2 → P3** (6 s por botón; un botón equivocado cancela en silencio). También puedes escribir `admin` en el Monitor Serie.
2. **Menú admin:**

| Botón | Acción |
| :-- | :-- |
| `P1` | **Asignar faltantes** — pide una a una solo las tarjetas sin UID |
| `P2` | **Revisar todas** — recorre las 50 (para re-etiquetar o verificar) |
| `P3` | **Probar tarjeta** — acerca cualquier tarjeta y muestra qué es |
| `$ Cobrar` | **Borrar todas** (pide confirmación) |
| `← Atrás` | Salir y volver al juego (la partida en curso se conserva) |

3. **Pantalla de asignación:** muestra el código (`C01–C24` postulantes, `I01–I08` insights, `M01–M18` misterio), el nombre y el UID actual. Acerca la tarjeta física → se guarda en flash y avanza sola.
   - `P1` / `P2`: anterior / siguiente · `P3`: borrar el UID de esta carta (con confirmación) · `←`: volver al menú.
   - Si escaneas una tarjeta que ya pertenece a otra carta, pregunta **"Moverla a Cxx?"** (`$` = sí, `←` = no).

Las asignaciones quedan en la memoria flash del ESP32 (NVS) y **sobreviven reinicios y re-flasheos** (solo `pio run -t erase` las borra). Si la flash está vacía, se usan los UIDs compilados en `game_data.h`.

Respaldo opcional en el repo (cierra el Monitor Serie antes):
```bash
python3.12 scripts/enroll_cards.py --pull     # copia los UIDs del tablero a data/game_data.json + regenera game_data.h
python3.12 scripts/enroll_cards.py --status   # ver progreso (ej. 24/50)
```
*(El asistente antiguo por PC, `python3.12 scripts/enroll_cards.py`, sigue funcionando, pero si el tablero ya tiene asignaciones en flash, éstas tienen prioridad.)*

### 3. Flashear el firmware de producción
```bash
cd firmware
./run_firmware.sh
```

## Estructura del proyecto

```text
juego-idi/
├── PLAN_Y_REQUERIMIENTOS.md    # Especificación final, directivas de impresión y BOM
├── docs/WIRING_Y_CAJA.md       # Pinout ESP-32D y layout de caja 12×12×3 cm
├── data/game_data.json         # Fuente única de verdad (24 postulantes, 8 cargos, 18 misterio)
├── scripts/
│   ├── enroll_cards.py         # Registro por PC / --pull respaldo desde el tablero
│   └── generate_header.py      # Genera firmware/include/game_data.h desde game_data.json
├── firmware/
│   ├── run_firmware.sh         # Compila, flashea y abre Monitor Serie (--dummy-test o producción)
│   ├── platformio.ini          # Configurado para esp32dev (1 MHz SPI, SH1106 OLED)
│   ├── include/{game_data,pin_config}.h
│   ├── include/card_registry.h # Mapa UID→carta guardado en flash (NVS)
│   └── src/{main,card_registry}.cpp # Juego + Modo Admin + UI OLED 1.3" + modo --dummy-test
└── hardware-tests/             # Pruebas aisladas de cada componente (RC522, botones, buzzer, OLED)
```
