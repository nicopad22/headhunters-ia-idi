# Planificación, Requerimientos y Especificación Final — Headhunters en la era de la IA (IDI1015)

> **Nota (2026-11-24):** las reglas de juego de este documento quedaron reemplazadas por los PDFs de [`data/Updated game data/`](./data/Updated%20game%20data) (startups NotCow / Fintubal / Ziptedi, rentabilidad inicial 2, cobro $100 × nivel, fin por última ronda al completar 5 cargos, nuevo mazo Misterio). Las 8 tarjetas de ocupación (Insight, $50) se mantienen. `data/game_data.json` y el firmware siguen esas reglas. Las secciones de hardware y BOM siguen vigentes.

Documento maestro actualizado con las decisiones definitivas de hardware, economía digital, directivas de reimpresión y comparación de pantallas económicas para la consola **ESP32 + NFC + 5 botones**.

---

## 1. Especificación definitiva del juego (Consola ESP32 + 5 botones + NFC)

### A. Reglas y arquitectura cerradas
- **Plataforma:** **ESP32 DevKit** (3.3V nativos, 520 KB RAM, 4 MB Flash).
- **Controles (5 botones exactos + NFC + Pantalla + Buzzer):**
  1. `P1` — **Ruedas Verdes** (Pía Riquelme)
  2. `P2` — **Estudio Prisma** (Nico Alvarado)
  3. `P3` — **Billetera Clara** (Elena Huenchu)
  4. `Atrás / Cancelar` (cancela selección/acción, responde **No** en trivias o vuelve al leaderboard)
  5. `Cobrar / Confirmar` (cobra trimestre en Recepción o responde **Sí** en trivias)
- **Economía 100% digital:**
  - Sin billetes físicos ni pagos de consultoría entre jugadores.
  - Saldo inicial: **`$500`**.
- **Costos fijos por modalidad:**
  - **IA sola:** `$100`
  - **Humano:** `$200`
  - **Humano + IA:** `$300` (mayor versatilidad/habilidades)
  - **Comprar Insight (escanear ocupación):** **`$50`**
- **Rentabilidad y victoria:**
  - **Nivel inicial:** **`0`** (5 cargos de `+3` suman un máximo teórico de **`15`**).
  - **Condición de victoria:** Alcanzar **nivel `12`** de rentabilidad (victoria inmediata).
- **Contratación, bloqueo y reemplazo:**
  - La consola **bloquea** contratar cargos que no están en la guía de la startup del jugador.
  - Los postulantes contratados son exclusivos (bloqueados para el resto mientras estén contratados).
  - Si un jugador escanea un postulante de una ocupación ya cubierta, la consola asume **reemplazo**: cobra el costo completo del nuevo, libera al anterior y ajusta la rentabilidad. **Cooldown:** solo se permite si el jugador **contrató otro cargo distinto entre medio** (evita arrepentimiento inmediato).
- **Cobro en Recepción (`Cobrar`):**
  - Monto a cobrar: **`$100 base + $100 por cada nivel de rentabilidad`** (`$100` en nivel 0, `$200` en nivel 1, ..., `$1.300` en nivel 12).
  - **Validación automática:** Solo se habilita si el jugador **compró algo (postulante o insight)** desde su último cobro **o** escaneó la carta de misterio **Pitch exitoso** (`N.º 07`).
- **Cartas de Misterio (18 cartas con NFC):**
  - **N.º 01 Ronda de inversión:** `+$300` automático.
  - **N.º 02 Clientes piden trato humano:** `+$100` automático por cada `Humano` contratado.
  - **N.º 03 Nueva herramienta de IA:** `+$100` automático por cada `Humano + IA` contratado.
  - **N.º 04 Error viral:** `-$100` automático por cada `IA sola` contratado (tope mínimo `$0`).
  - **N.º 05 Auditoría de calidad (ajustada):** Si tienes al menos un `IA sola` de rentabilidad Baja (`+1`), multa automática de `-$100`.
  - **N.º 06 Caída del servidor:** Vuelves a Recepción sin habilitar cobro (`cobro_habilitado = false`).
  - **N.º 07 Pitch exitoso:** Vuelves a Recepción con cobro habilitado (`cobro_habilitado = true`).
  - **N.º 08 Feria laboral / N.º 10 Headhunting directo / N.º 14 Ascensor exprés:** La consola muestra el aviso y el jugador ejecuta la jugada en el tablero.
  - **N.º 09 Práctica profesional:** Descuento guardado de `-$100` para la próxima contratación.
  - **N.º 11 Capacitación interna:** Habilita mejorar 1 `Humano` propio a `Humano + IA` del mismo cargo por `$100` (y saltando el cooldown).
  - **N.º 12 Reestructuración (reemplaza a *Fuga de talento*):** Segunda copia de Reestructuración para evitar intercambios manuales de 2 jugadores en consola.
  - **N.º 13 Reestructuración:** Escaneas uno de tus postulantes (o `Cancelar`), lo liberas, resta su rentabilidad y reembolsa la mitad de su costo.
  - **N.º 15–18 Dato OIT (4 trivias):** Respondes en consola con **`Cobrar` = Sí (`+$100`)** o **`Cancelar` = No (`$0`)**.

### B. Directivas exactas para rehacer las impresiones (cambios respecto a `Headhunters_Ilustrado_Juego_Completo.pdf`)

1. **Páginas 1–4 (Portada, Armado y Reglamento):**
   - **Armado (pág. 2):** Quitar *"Billetes $50, $100 y $500"* de la tabla de piezas y quitar *"y $500"* de la entrega inicial física (aclarar que saldo `$500` y rentabilidad `0` parten en la consola). Agregar **8 Tarjetas de Insight ($50)**.
   - **Cómo se juega (pág. 3):** Quitar el ejemplo numérico de los letreros (`0,39` y `0,62`), porque ahora el puntaje OIT es secreto hasta pagar el insight. Agregar la acción **Comprar Insight ($50)** escaneando la tarjeta de ocupación.
   - **Contratar y cobrar (pág. 4):**
     - Actualizar precios a **`IA sola $100 · Humano $200 · Humano + IA $300`**.
     - Añadir la regla de reemplazo con cooldown (*"solo después de haber contratado otro cargo entre medio"*).
     - **Eliminar** completamente la sección **"Consultoría"**.
     - Actualizar **Recepción**: *"Cobras $100 base + $100 por cada nivel de rentabilidad (solo si contrataste, compraste insight o sacaste Pitch exitoso desde tu último cobro)"*.
     - Actualizar **Si no te alcanza el dinero**: aclarar que no puedes contratar y que las multas automáticas dejan tu saldo mínimo en `$0` (sin devolver tarjetas salvo por carta *Reestructuración*).
     - Actualizar **Fin del juego**: regla única → **gana de inmediato el primero en llegar a nivel `12` de rentabilidad**.
2. **Página 6 / `Tablero_A2` (Tablero físico 50×50 cm MDF/Acrílico):**
   - **Borrar el óvalo con el puntaje OIT (`0,32`..`0,62`)** en los letreros de las **8 salas** (para que comprar insight de `$50` tenga sentido).
   - **Actualizar los precios** en las 3 casillas de cada sala (8 salas = 24 casillas):
     - `Humano` (mostaza): **`$200`** (antes `$300`)
     - `Humano + IA` (terracota): **`$300`** (antes `$200`)
     - `IA sola` (petróleo): **`$100`** (igual)
   - **Recepción central:** Adaptar la huella al tamaño real de la caja de consola (pantalla + **5 botones** + zona *"Acerca tu tarjeta"* con **acrílico de 2–3 mm sin MDF debajo de la antena RC522**).
3. **Páginas 7–12 (24 Tarjetas de Postulante):**
   - **Frente:** Cambiar `$300 → $200` en las 8 tarjetas `Humano` (mostaza), y `$200 → $300` en las 8 tarjetas `Humano + IA` (terracota). Mantener `$100` en `IA sola`.
   - **¡Ocultar `OIT 0,XX` y la barra de puntos del pie de las 24 tarjetas!** (Para no regalar el dato del insight).
   - **Dorso:** Si se quiere ocultar la rentabilidad (`+1/+2/+3`) antes de contratar para que la consola dé el veredicto al escanear, dejar el dorso limpio con diseño genérico de carta (o mostrar la explicación OIT solo como tarjeta de consulta post-partida).
   - **NFC:** Pegar 1 sticker NFC 13.56 MHz en cada una de las 24 tarjetas.
4. **Páginas 13–16 (18 Cartas de Misterio):**
   - **N.º 05 Auditoría de calidad:** Cambiar a *"Si tienes un «IA sola» con rentabilidad Baja (+1), pagas automáticamente $100 de multa en la consola."*
   - **N.º 12:** Reemplazar *Fuga de talento* por una segunda carta de **Reestructuración** (*"Devuelve un postulante tuyo a su sala y recupera la mitad de lo que pagaste"*).
   - **N.º 15–18 Dato OIT:** Agregar nota corta abajo: *"Confirma en consola: Cobrar = Sí / Cancelar = No"*.
   - **NFC:** Pegar 1 sticker NFC en cada una de las 18 cartas.
5. **Páginas 17–19 (3 Guías de Contratación):**
   - **Borrar `OIT 0,XX` y la barra de puntos** arriba a la derecha de cada uno de los 5 cargos en las 3 guías.
   - **Actualizar el pie de página** en las 3 guías: **`IA sola $100 · Humano $200 · Humano + IA $300`**.
6. **Nuevo set a diseñar e imprimir: 8 Tarjetas de Insight ($50):**
   - Una tarjeta pequeña por cada sala/cargo (ej. `Taller mecánico — Ingeniero/a mecánico/a · Insight $50`), con su respectivo sticker NFC detrás para escanearla en la consola.
7. **Páginas 21–23 (Respaldo, Billetes y Panel Consola):**
   - **Pág. 21 (Rentabilidad):** Si se imprime como apoyo visual opcional, renumerar escala de **`0` a `15`** (inicio en **`0`** `$100`, meta destacada en **`12`** `$1.300`).
   - **Pág. 22 (Billetes):** **NO IMPRIMIR** (se elimina).
   - **Pág. 23 (Panel de consola y Tarjeta de modos):**
     - Actualizar **Tarjeta de modos (atril)** a **`IA sola $100 · Humano $200 · Humano + IA $300`**.
     - Actualizar **Panel de la consola** para incluir los **5 botones** (`Ruedas Verdes`, `Estudio Prisma`, `Billetera Clara`, `Cancelar`, `Cobrar`) y calzar con las medidas de la pantalla elegida.

---

## 2. Comparación de pantallas económicas estilo robótico y presupuesto total (Santiago, Chile)

### A. Comparación de pantallas low-res / estilo robótico (precios verificados al 2026-11-16)

> **Nota técnica importante sobre 5V vs 3.3V con ESP32:**
> Las pantallas **LCD 16x2** y **LCD 20x4** clásicas funcionan a **5V** (y su módulo I2C trae pull-ups a 5V). Para no dañar los pines de 3.3V del ESP32, requieren un **Conversor de Nivel Lógico I2C** (**$2.000 CLP** en [Afel](https://afel.cl/products/conversor-de-nivel-logico-i2c-5v-a-3-3v)), lo cual debe sumarse a su costo real. Las pantallas **Nokia 5110** y **OLED** operan directo a **3.3V** sin conversor.

| Pantalla | Estilo visual | Qué cabe en pantalla a la vez | Voltaje / Pines | Precio pantalla (CLP) | Costo real con adaptador | Veredicto |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **LCD 16x2 I2C** | Clásico LCD azul/verde retro de 2 líneas × 16 chars | **Muy apretado:** Solo cabe texto corto o 2 líneas (`RV:06 EP:09` / `BC:03 $500`). Hay que rotar o abreviar mucho para 3 barras de progreso | 5V I2C (2 pines + **conversor I2C**) | **$3.990** ([Afel](https://afel.cl/products/pantalla-lcd-1602-azul-interfaz-i2c-soldado) / [Hubot](https://hubot.cl/producto/modulo-display-lcd-1602-con-interfaz-i2c-sku-282-1d1/))<br>$5.490 ([Mechatronic](https://www.mechatronicstore.cl/pantalla-lcd1602/)) | **~$5.990** *(+$2.000 conversor)* | Barata, pero 16×2 es incómoda para mostrar 3 barras de progreso a la vez |
| **LCD 20x4 I2C** | LCD robótico grande (98×60 mm), 4 líneas × 20 chars | **Excelente para texto + barras:** Líneas 1–3 muestran las 3 startups con barras de bloques (`████░░ 08/12`) y línea 4 muestra saldo/estado. Súper legible de lejos | 5V I2C (2 pines + **conversor I2C**) | **$7.500** ([Afel 2004 I2C](https://afel.cl/products/pantalla-2004-azul-con-interfaz-i2c))<br>$11.990 ([Mechatronic](https://www.mechatronicstore.cl/pantalla-lcd2004-con-i2c/)) | **~$9.500** *(+$2.000 conversor)* | **Mejor en tamaño/legibilidad** si quieren estética LCD retro grande |
| **LCD Nokia 5110 (84×48)** | Pantalla monocroma retro pixelada de celular/robot | **Muy bueno en gráficos:** 84×48 píxeles libres para dibujar 3 barras reales hacia `12` + texto + mascota robot pixel-art | **3.3V nativo** (SPI, 5 pines, **sin conversor**) | **$5.000** ([Afel](https://afel.cl/products/pantalla-display-lcd-nokia-5110)) | **$5.000** | **Más barata que 16x2+conversor**, look bien retro/robótico y 100% 3.3V |
| **OLED 1.3" 128×64 I2C** *(o 0.91" 128×32)* | Píxeles nítidos blancos/azules sobre negro | **Muy nítido:** 128×64 píxeles alcanzan perfecto para 3 barras + montos + robotito. *(Ojo: es chica física, ~3 cm de ancho)* | **3.3V nativo** (2 pines I2C, **sin conversor**) | **$5.000** ([Afel 1.3" blanca](https://afel.cl/products/pantalla-lcd-oled-1-3-128x64-caracteres-blancos) / [azul](https://afel.cl/products/pantalla-lcd-oled-1-3-128x64-caracteres-azules))<br>$3.990 ([Afel 0.91"](https://afel.cl/products/pantalla-lcd-oled-0-91)) | **$5.000** *(o $3.990 en 0.91")* | **Empata como la opción más barata real ($5.000)**, cableado mínimo (2 pines) y máxima nitidez |

### B. Lista de compra optimizada y Gasto Total Estimado (CLP)

Estrategia recomendada para gastar lo mínimo en envíos:
1. **MechatronicStore** (solo por el pack de 100 stickers NFC a $19.990 + el RC522 más barato a $1.890 — tienen retiro/envío en Santiago).
2. **Afel** (retiro presencial en **Obispo Donoso 6, Of. 302, Providencia** — Metro Salvador, sin envío) para pantalla, cables, protoboard, botones, buzzer y (si toca comprarlo) ESP32.

| Ítem                                      | Producto y enlace verificado                                                                                                                                                                                                                                  | Precio (CLP) |
| :---------------------------------------- | :------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ | -----------: |
| **50 Tags NFC + repuestos**               | [100× Etiquetas adhesivas RFID/NFC 13.56 MHz (MechatronicStore)](https://www.mechatronicstore.cl/etiqueta-con-chip-rfid-cuid-nfc-de-1356-mhz-regrabable-100-unidades/)                                                                                        |      $19.990 |
| **Lector NFC/RFID**                       | [Módulo RFID RC522 (MechatronicStore)](https://www.mechatronicstore.cl/modulo-rfid-rc522/)                                                                                                                                                                    |       $1.890 |
| **Protoboard**                            | [Mini Protoboard 400 puntos (Afel)](https://afel.cl/products/mini-protoboard-400-puntos)                                                                                                                                                                      |       $1.500 |
| **Cables de conexión**                    | [Pack 60 Cables Dupont M-M/M-H/H-H (Afel)](https://afel.cl/products/pack-60-cables-de-conexion)                                                                                                                                                               |       $2.500 |
| **5 Botones** *(opción económica)*        | [Kit 5 Botones Táctiles 6×6×5 (Afel)](https://afel.cl/products/kit-boton-tactil-6x6x5-interruptor-5-unidades) *(o $1.700 en [Hubot](https://hubot.cl/producto/5-botones-12mm-tactil-c-tapa-colores-sku-423/) si prefieren botones grandes con tapa de color)* |         $500 |
| **Buzzer pasivo**                         | [Buzzer Pasivo Solo (Afel)](https://afel.cl/products/buzzer-pasivo-solo)                                                                                                                                                                                      |         $300 |
| **Subtotal base (sin pantalla ni ESP32)** |                                                                                                                                                                                                                                                               |  **$26.680** |

#### Totales según pantalla elegida (con y sin ESP32):

| Configuración | Pantalla (+ conversor si aplica) | Total **SIN** comprar ESP32 *(te lo prestan)* | Total **COMPRANDO** ESP32 ([Afel $8.000](https://afel.cl/products/esp32-devkit-v1-30-pines-microusb)) |
| :--- | ---: | ---: | ---: |
| **Opción 1 (Mínimo costo / 3.3V):** OLED 0.91" I2C | $3.990 | **$30.670** | **$38.670** |
| **Opción 2 (Recomendada económica / 3.3V):** **OLED 1.3" I2C** o **Nokia 5110** | $5.000 | **$31.680** | **$39.680** |
| **Opción 3 (LCD retro 2 líneas):** LCD 16x2 I2C + Conversor I2C | $5.990 | **$32.670** | **$40.670** |
| **Opción 4 (Mejor legibilidad LCD retro):** **LCD 20x4 I2C** + Conversor I2C | $9.500 | **$36.180** | **$44.180** |

*(Nota: si suman botones de 12 mm con tapa de colores o 16 mm de montaje en panel en vez de los micro-pulsadores de $500, agreguen ~$1.200–$2.500 CLP al total. Y si compran todo en Afel evitando el despacho de MechatronicStore, ojo que 10 packs × 5 stickers NFC en Afel saldrían $30.000 vs $19.990 por 100 unidades en MechatronicStore).*

---

## 3. Qué necesitas aprender antes de meterte de lleno al proyecto

1. **Configurar PlatformIO (en VS Code) o Arduino IDE para ESP32:**
   - Instalar el paquete de placas `esp32`, driver USB-Serial (`CP2102` o `CH340`) y probar la lógica primero por el **Monitor Serie** (`115200 baud`) usando [main.cpp](file:///home/nicoquinta/code/juego-idi/firmware/src/main.cpp).
2. **Entradas digitales (`INPUT_PULLUP`) y Debounce:**
   - Los 5 botones van entre su pin GPIO y `GND` (`LOW` = presionado). El código base ya incluye debounce de 35 ms con `millis()`.
3. **Buses SPI e I2C en ESP32 (y cuidado con 5V si eliges LCD 16x2/20x4):**
   - El RC522 usa **SPI** (`SCK=18`, `MISO=19`, `MOSI=23`, `SS=5`, `RST=4`).
   - Las pantallas OLED/LCD I2C usan `SDA=21` y `SCL=22` (librerías `U8g2` / `Adafruit_SH110X` o `LiquidCrystal_I2C`). Si usas LCD de 5V, recuerda pasar `SDA`/`SCL` por el conversor de nivel lógico (`LV` a 3.3V del ESP32, `HV` a 5V/`VIN`).
   - Si eliges **Nokia 5110**, comparte el bus SPI (`SCK`/`MOSI`) con un pin `CS` propio (`PIN_DISP_CS = 15`) y usa `Adafruit_PCD8544`.
4. **Mapeo de UIDs NFC de las 50 tarjetas:**
   - Lee el UID único de fábrica de cada sticker con el Monitor Serie, pégalo en `data/game_data.json` y corre `python3.12 scripts/generate_header.py` para actualizar `firmware/include/game_data.h`.
5. **Prueba física sobre acrílico:**
   - Verifica el alcance del RC522 a través de los 2–3 mm de acrílico antes de pegar la electrónica dentro de la Recepción.
