# Headhunters en la era de la IA — Consola ESP-32D (IDI1015)

Repositorio de firmware, datos del juego, diagnósticos de hardware y planificación electrónica para el proyecto final de **IDI1015 (Pensamiento Visual UC)**: *Headhunters en la era de la IA*.

## Documentación principal

- [`data/Updated game data/`](./data/Updated%20game%20data) — **Fuente vigente de reglas** (Cómo se juega, Fichas de jugador, Tarjetas de postulante, Cartas Misterio, Plano del tablero v4). Las 8 tarjetas de ocupación (Insight) se mantienen de la versión anterior. `data/game_data.json` está sincronizado con estos PDFs.

- [PLAN_Y_REQUERIMIENTOS.md](./PLAN_Y_REQUERIMIENTOS.md) — Especificación del reglamento digital, directivas de reimpresión y BOM en Santiago.
- [docs/WIRING_Y_CAJA.md](./docs/WIRING_Y_CAJA.md) — Cableado exacto y distribución en caja de `12 × 12 × 3 cm`.
- [docs/DIMENSIONES.md](./docs/DIMENSIONES.md) — Todas las medidas físicas en mm (tablero `50×50 cm`, caja `120×120×30 mm`, componentes y tarjetas PVC `85,6×54 mm`), junto a la plantilla 1:1 [`docs/caja_120x120_1a1.svg`](./docs/caja_120x120_1a1.svg) y [`data/dimensions.json`](./data/dimensions.json).

## Cómo corre una partida en la consola

1. **Pantalla de inicio** → `[$]` empieza la partida.
2. La consola **sortea quién empieza** y muestra el orden (P1 NotCow → P2 Fintubal → P3 Ziptedi, desde el que empieza).
3. **Cada turno** la pantalla dice de quién es el turno: lanza los dados, mueve y haz **una sola acción**: contratar (escanear postulante), sacar una carta Misterio (escanearla) o cobrar en Recepción (`[$]`). Las tarjetas de ocupación (Insight, $50) no gastan la acción.
4. `[<-]` **termina el turno** y la consola anuncia al siguiente jugador. `P1`/`P2`/`P3` durante un turno muestran los puntajes.
5. Cuando alguien completa sus 5 cargos, cada otro jugador juega un último turno y la consola muestra el ranking final (`[<-]` = nueva partida).

Excepciones de las cartas Misterio: *Pitch exitoso* cobra el trimestre al tiro, *Headhunting directo* permite contratar en ese mismo turno y *Capacitación interna* hace el cambio Humano → Humano + IA en el momento.

## Flujo rápido de trabajo

### 1. Probar el flujo completo del juego con 6 tarjetas cualquiera (Dummy Flow Test)
Ejecuta el firmware real (OLED 1.3" SH1106 + RC522 + 5 botones + buzzer) en un recorrido guiado de **11 pasos** con instrucciones en pantalla y Monitor Serie:

```bash
cd firmware
./run_firmware.sh --dummy-test
```

### 2a. Registrar las 50 tarjetas con el teléfono (sin el circuito)
App web en [`webapp/`](./webapp) (Chrome en Android + Web NFC, desplegable en Vercel como sitio estático).

1. Abre la app en **Chrome para Android** con NFC activado → **Activar lector** (pide permiso la primera vez).
2. Pestaña **Probar**: acerca una tarjeta para confirmar que el teléfono la lee.
3. Pestaña **Etiquetar**: muestra cada carta (`C01–C24` postulantes, `I01–I08` ocupaciones, `M01–M18` Misterio); acerca la tarjeta física, se guarda y avanza sola. Avisa si la tarjeta ya pertenece a otra carta. Lecturas repetidas de la misma tarjeta en < 1 s se ignoran. Si una tarjeta no se puede leer, usa **Ingresar a mano** (UID desde la app *NFC Tools*).
4. Pestaña **Exportar** → descarga `headhunters_nfc_uids.json` (el progreso queda guardado en el teléfono aunque cierres la página).
5. En el computador:
```bash
python3.12 scripts/import_uids.py ~/Descargas/headhunters_nfc_uids.json   # actualiza data/game_data.json + game_data.h
cd firmware && ./run_firmware.sh                                           # al primer arranque reemplaza lo guardado en flash
```
El UID que usa el juego son los **primeros 4 bytes** del número de serie (mismo orden que el RC522), así que no hay que invertir bytes.
La lista de cartas de la app (`webapp/cards.json`) se regenera con `scripts/generate_header.py` cada vez que cambia `data/game_data.json`.

Despliegue: `cd webapp && vercel --prod` (sitio estático, sin build). Para probar en local: `python3 -m http.server -d webapp` (Web NFC requiere HTTPS, así que en el teléfono usa la URL de Vercel).

### 2b. Registrar las 50 tarjetas NFC desde la consola (Modo Admin)
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

3. **Pantalla de asignación:** muestra el código (`C01–C24` postulantes, `I01–I08` ocupaciones, `M01–M18` misterio), el nombre y el UID actual. Acerca la tarjeta física → se guarda en flash y avanza sola.
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
├── data/game_data.json         # Fuente única de verdad (24 postulantes, 8 cargos, 3 startups, 18 misterio)
├── scripts/
│   ├── enroll_cards.py         # Registro por PC / --pull respaldo desde el tablero
│   ├── import_uids.py          # Importa el JSON exportado por webapp/ a game_data.json
│   └── generate_header.py      # Genera firmware/include/game_data.h (+ webapp/cards.json) desde game_data.json
├── webapp/                     # App web (Web NFC) para etiquetar tarjetas con el teléfono → JSON
├── firmware/
│   ├── run_firmware.sh         # Compila, flashea y abre Monitor Serie (--dummy-test o producción)
│   ├── platformio.ini          # Configurado para esp32dev (1 MHz SPI, SH1106 OLED)
│   ├── include/{game_data,pin_config}.h
│   ├── include/card_registry.h # Mapa UID→carta guardado en flash (NVS)
│   └── src/{main,card_registry}.cpp # Juego + Modo Admin + UI OLED 1.3" + modo --dummy-test
└── hardware-tests/             # Pruebas aisladas de cada componente (RC522, botones, buzzer, OLED)
```
