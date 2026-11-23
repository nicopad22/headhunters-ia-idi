# Cableado y Diseño de Caja (12 × 12 × 3 cm) — Consola Headhunters

Especificación eléctrica y mecánica para **ESP-32D (N4xx)** + **RC522 NFC (SPI)** + **OLED 1.3" 128×64 (I2C SH1106)** + **5 botones** + **buzzer pasivo** dentro de una caja de **12 × 12 cm** y **≤ 3 cm de alto**.

---

## 1. Tabla de conexiones (100% 3.3V nativo — sin conversores ni resistencias externas)

> Coincide exactamente con `firmware/include/pin_config.h`.

| Componente | Pin del módulo | Pin del ESP-32D | Notas importantes |
| :--- | :--- | :--- | :--- |
| **RC522 NFC (SPI)** | `3.3V` | `3V3` | **NUNCA conectar a 5V/VIN** |
| | `GND` | `GND` | Tierra común |
| | `SDA` (SS) | `GPIO 5` | Chip Select SPI |
| | `SCK` | `GPIO 18` | VSPI Clock |
| | `MOSI` | `GPIO 23` | VSPI MOSI |
| | `MISO` | `GPIO 19` | VSPI MISO |
| | `RST` | `GPIO 4` | Libre de pines I2C (`21`/`22`) |
| | `IRQ` | *(sin conectar)* | — |
| **OLED 1.3" 128×64 (I2C)** | `VCC` | `3V3` | **¡Verificar orden `VCC`/`GND` serigrafiado en tu placa!** |
| | `GND` | `GND` | Tierra común |
| | `SDA` | `GPIO 21` | I2C SDA (dirección típica `0x3C`, controlador **SH1106**) |
| | `SCL` | `GPIO 22` | I2C SCL |
| **Botón P1 (Ruedas Verdes)** | Pata A / Pata B | `GPIO 32` / `GND` | Usa `INPUT_PULLUP` interno |
| **Botón P2 (Estudio Prisma)** | Pata A / Pata B | `GPIO 33` / `GND` | Usa `INPUT_PULLUP` interno |
| **Botón P3 (Billetera Clara)** | Pata A / Pata B | `GPIO 25` / `GND` | Usa `INPUT_PULLUP` interno |
| **Botón Atrás / Cancelar** | Pata A / Pata B | `GPIO 26` / `GND` | Usa `INPUT_PULLUP` interno |
| **Botón Cobrar / Confirmar** | Pata A / Pata B | `GPIO 27` / `GND` | Usa `INPUT_PULLUP` interno |
| **Buzzer Pasivo** | `+` / `-` | `GPIO 14` / `GND` | Opcional: resistencia `100 Ω` en serie si es buzzer suelto |
| **Alimentación** | Cable USB-A → USB-C | Puerto USB-C ESP-32D | Power Bank ultra delgado `10.000 mAh` (`12 mm` alto, salida USB-A 5V) montado bajo el tablero (`15 mm` libres) |

*(Tip para pulsadores de 4 patas: conecta dos patas en diagonal opuesta para evitar cortocircuitar el mismo riel interno).*

---

## 2. Distribución en planta (120 × 120 mm externos / ~114 × 114 mm internos)

Para mantener el alto bajo **30 mm** y **no perder alcance NFC**, ningún componente debe apilarse encima de otro, y **no debe haber metal (ni ESP32 ni protoboard) debajo de la antena del RC522**:

```text
+------------ 120 mm (114 mm interior) ------------+
|  +--------------+  (Buz)  +-------------------+  |
|  |  OLED 1.3"   |   12mm  |    RC522 NFC      |  |
|  |  36 x 34 mm  |         |    40 x 60 mm     |  |
|  | (en la tapa) |         |  (bajo acrílico   |  |
|  +--------------+         |   2-3 mm, AIRE    |  |
|                           |   LIBRE debajo)   |  |
|  [USB-C ↓]============+   +-------------------+  |
|  |piso ESP-32D (piso) |                          |
|       55 x 28 mm      |                          |
|  +--------------------+                          |
|                                                  |
|   (P1)     (P2)     (P3)     (<-)     ($)        |
|  <----- 5 botones (~21 mm entre centros) ----->  |
+--------------------------------------------------+
```

- **Puerto USB y alimentación bajo el tablero:** El cable **USB-A → USB-C** baja desde el ESP-32D por el orificio perforado en la base MDF hacia la cavidad inferior de **`15 mm`** (tablero levantado `15 mm` en las 4 esquinas) donde se fija el Power Bank ultra delgado (`12 mm`) con Velcro/cinta delgada (`≤ 1,5 mm`), ubicado cerca de un borde exterior (lejos del RC522, con acceso fácil al botón/carga y opción de desenchufar a PC).
- **RC522:** pegado por debajo de la tapa de **acrílico de 2–3 mm** usando **pines a 90°** (si usas pines rectos + Dupont hacia abajo ocupa ~22 mm, pero como hay aire libre debajo igual cabe en ~25 mm internos).

---

## 3. Análisis de altura (¿cabe en ≤ 30 mm?) y problema de las 2 protoboards

### ¿Por qué NO caben 2 protoboards unidas dentro de la caja de 12 × 12 × 3 cm?
1. **Protoboards de 830 puntos (`165 × 55 mm`):** miden **16,5 cm de largo** → físicamente no entran en una caja de 12 cm.
2. **Dos protoboards de 400 puntos unidas (`~101 × 83 mm`):**
   - Cubren casi todo el piso interior (`114 × 114 mm`), metiendo pistas metálicas justo debajo de la antena del **RC522** (mata el alcance NFC).
   - **Altura real con cables Dupont:** `2,5 mm` base + `9,5 mm` protoboard + `~20 mm` conector Dupont macho + curva del cable + `2,5 mm` tapa = **`~34,5 mm`** → **EXCEDE el límite de 30 mm**.

### Cómo SÍ cabe en ≤ 30 mm (3 opciones reales)

| Opción de montaje | Altura total externa | ¿Cabe en ≤ 30 mm? | Recomendación |
| :--- | :---: | :---: | :--- |
| **A. 2× protoboard 400 pts + cables Dupont** | **~34,5 – 38 mm** | **NO** | Usar **solo en tu escritorio** para prototipar y probar el código |
| **B. Sin soldar: ESP-32D plano en el piso + cables Dupont Hembra-Hembra (`H-H`)** | **~28 – 29 mm** | **SÍ (justo)** | Pegar ESP-32D con cinta doble faz al piso (sin protoboard debajo). Las carcasas hembra calzan *sobre* los pines (`6 mm` ESP32 + `14 mm` Dupont + `4 mm` curva + `4,5 mm` base/tapa ≈ `28,5 mm`). Unir `GND` y `3V3` comunes con empalme + huincha o una sola mini protoboard de 170 pts en una esquina libre |
| **C. Placa perforada (`5×7 cm`) / cables soldados planos + pines a 90°** | **~20 – 22 mm** | **SÍ (sobrado)** | **La mejor para entrega final:** ultra delgada, cero cables sueltos al mover el tablero y ~8–10 mm de holgura bajo los 3 cm |

### Nota clave de software para tu OLED 1.3" 128×64
Las pantallas **OLED 1.3" 128×64 I2C** (como la de Afel) casi siempre traen controlador **SH1106** (no SSD1306; si usas librería SSD1306 verás una franja de ruido vertical al costado). En `firmware/platformio.ini` ya dejamos activadas `Adafruit GFX Library` y `Adafruit SH110X` (`Adafruit_SH1106G`).
