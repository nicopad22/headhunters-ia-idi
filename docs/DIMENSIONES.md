# Dimensiones del Proyecto — Headhunters en la era de la IA (IDI1015)

Referencia única de medidas físicas (todas en **milímetros**, salvo indicación explícita) para el diseño del tablero (**Ignacio / Vale**), las tarjetas (**Cata**), las piezas 3D (**Ame**) y la carcasa de la consola electrónica.

> **Nota para corte láser / CNC:** Antes de cortar definitivo el acrílico/MDF de la tapa de la consola, **verificar con pie de metro** los agujeros exactos de montaje de tu OLED 1.3" y el diámetro exacto de tus 5 botones (las tolerancias varían ±0,5 mm entre fabricantes).

---

## 1. Tablero Principal y Espacio Inferior (Alimentación)

| Elemento | Dimensión | Material / Notas |
| :--- | :--- | :--- |
| **Huella externa del tablero** | **`500 × 500 mm`** (`50 × 50 cm`) | Base MDF |
| **Grosor base MDF** | **`5,5 mm`** | Base estructural (con perforación inferior en Recepción para pasar el cable USB-C del ESP-32D) |
| **Patas / elevación en las 4 esquinas** | **`15 mm`** (`1,5 cm`) | Genera cavidad inferior libre de `15 mm` debajo del tablero *(recomendado sumar 1 apoyo central de `15 mm` junto a la caja para evitar flexión del MDF al presionar los 5 botones)* |
| **Altura de murallas de salas** | **`45 mm`** (`4,5 cm`) | Relieve de habitaciones *(altura total tablero con patas: `15 + 5,5 + 45 = 65,5 mm`)* |
| **Capa superior decorativa** | **`2 – 3 mm`** | Acrílico grabado (cuadrícula) + acrílico de color en salas |
| **Power Bank ultra delgado (bajo tablero)** | **`~150 × 74 × 12 mm`** (`10.000 mAh`, salida **USB-A 5V**) | Cabe en los `15 mm` libres con **`3 mm` de margen total** → usar cinta doble faz o Velcro **ultra delgado (`≤ 1,5 mm`)** para dejar `≥ 1,5 mm` de aire sobre la mesa. Ubicar cerca de un **borde exterior** (lejos del RC522, con botón/carga accesibles y opción de desenchufar a PC) |
| **Referencia casilla (PDF original A2)** | `~15 × 15 mm` | Escalar proporcionalmente al tablero `500 × 500 mm` |

---

## 2. Consola Electrónica (Caja empotrada en Recepción)

Plantilla vectorial a escala real **1:1** disponible en [`caja_120x120_1a1.svg`](./caja_120x120_1a1.svg).

### A. Medidas generales de la caja

| Parámetro | Dimensión | Notas |
| :--- | :--- | :--- |
| **Ancho × Largo externo** | **`120 × 120 mm`** (`12 × 12 cm`) | Reserva mínima en la zona de Recepción |
| **Altura total externa máxima** | **`≤ 30 mm`** (`3 cm`) | Límite superior estricto |
| **Grosor de paredes laterales** | **`3,0 mm`** | MDF o acrílico |
| **Cavidad interior útil (X × Y)** | **`114 × 114 mm`** | `120 - 2×3 mm` |
| **Tapa superior (zona OLED + NFC)** | **`2,0 – 3,0 mm`** | **Solo acrílico delgado** (SIN MDF debajo del RC522) |
| **Base inferior** | **`2,0 – 3,0 mm`** *(o `0 mm` si apoya directo sobre el MDF del tablero)* | — |
| **Altura interior libre (Z)** | **`24 – 25 mm`** *(o `27 mm` sin fondo propio)* | Suficiente para placa perforada o ESP-32D plano con Duponts `H-H` |

### B. Dimensiones y ubicación de componentes internos (origen `0,0` = esquina superior izquierda externa de `120 × 120 mm`)

| Componente | Dimensiones físicas (L × A × H) | Zona asignada (X / Y externos) | Calado / Notas de montaje |
| :--- | :--- | :--- | :--- |
| **OLED 1.3" 128×64 (I2C SH1106)** | PCB: `35,4 × 33,5 × 4,5 mm`<br>Área activa: **`29,42 × 14,7 mm`** | Top-Left<br>`X: 10 – 45,4 mm`<br>`Y: 10 – 43,5 mm` | Ventana visible recomendada en tapa: **`30,0 × 15,5 mm`** (centrada sobre el vidrio; o tapa de acrílico transparente con máscara impresa). Montada pegada a la tapa. |
| **Lector NFC RC522 (13.56 MHz)** | PCB: **`40,0 × 60,0 × 4,0 mm`** *(con pines a 90°)* | Top-Right<br>`X: 70 – 110 mm`<br>`Y: 10 – 70 mm` | Pegado al ras por debajo del acrílico (`2–3 mm`). **CRÍTICO: dejar aire libre debajo** (sin ESP32, sin protoboard, sin metal debajo de la bobina). |
| **ESP-32D (N4xx)** | PCB: **`55,0 × 28,0 × 6,0 mm`** *(solo placa)* | Mid-Left (en el piso)<br>`X: 5 – 60 mm`<br>`Y: 50 – 78 mm` | **Sin protoboard debajo.** Cable **USB-C** baja por el orificio perforado en la base MDF hacia el espacio inferior de `15 mm`. |
| **Paso cable USB-C (base MDF)** | Orificio pasacable en el piso *(ya perforado)* | Bajo el extremo USB-C del ESP-32D | Reemplaza la ranura lateral: el cable **USB-A → USB-C** baja al espacio libre de `15 mm` bajo el tablero y conecta al Power Bank de `12 mm`. |
| **5 Botones (`P1`, `P2`, `P3`, `Atrás`, `Cobrar`)** | Paso entre centros: **`21 mm`** | Fila inferior (`Y = 98 mm`)<br>Centros X: **`18, 39, 60, 81, 102 mm`** | Diámetro del calado según botón usado: medir con pie de metro (`~6,5 mm` pulsador chico con tapa, `~12,5 mm` botón 12 mm, o `~16,2 mm` botón panel 16 mm). |
| **Buzzer pasivo** | **`Ø 12,0 × 9,5 mm`** | Entre OLED y RC522<br>Centro `X = 56 mm, Y = 22 mm` | Agujero opcional `Ø 3 mm` en la tapa para mejor volumen. |

### C. Presupuesto vertical (eje Z ≤ 30 mm)

| Método de armado dentro de la caja | Desglose Z | Altura total externa | Estado |
| :--- | :--- | :---: | :---: |
| **Placa perforada (`5×7 cm`) + soldadura plana + pines 90°** | Base `2,5` + ESP32/placa `14` + aire `2` + tapa `2,5` | **`~21 mm`** | **Óptimo** |
| **Sin soldar (ESP-32D pegado al piso + Duponts `H-H`)** | Base `2,0` + ESP32 `6` + Dupont hembra y curva `18` + tapa `2,5` | **`~28,5 mm`** | **Cabe justo** |
| **2× Protoboards 400 pts unidas (`101 × 83 mm`)** | Base `2,5` + protoboard `9,5` + Duponts `20` + tapa `2,5` | **`~34,5 mm`** | **No cabe** *(solo escritorio)* |
| **Protoboard 830 pts (`165 × 55 mm`)** | Largo `165 mm > 120 mm` | — | **No cabe** |

---

## 3. Tarjetas NFC y Piezas Impresas

| Elemento | Cantidad | Dimensiones | Notas |
| :--- | :---: | :--- | :--- |
| **Tarjetas PVC NFC 13.56 MHz** *(CR80 estándar)* | **50** (+ repuestos) | **`85,6 × 54,0 × 0,8 mm`** | `24` postulantes + `8` insights + `18` misterio |
| **Gráfica adhesiva para pegar en tarjetas PVC** | 50 frentes (+ dorsos) | **`84,0 × 52,0 mm`** *(o `85,6 × 54,0 mm` al ras)* | Dejar `0,8 mm` de margen evita que se levanten los bordes al manipular |
| *(Referencia PDF antiguo sin PVC)* | — | `63,0 × 88,0 mm` | Tamaño póker original antes de pasar a tarjetas PVC NFC |
| **Guías de contratación** | 3 | Tamaño carta / A4 o A5 | Una por startup (`Ruedas Verdes`, `Estudio Prisma`, `Billetera Clara`) |
| **Figuras 3D de jugadores + marcadores** | 3 figuras + 15 marcadores | Base `≤ 14 mm` | Deben caber cómodamente dentro de las casillas de pasillo y contratación |
