#pragma once

// ESP32 DevKit pin mapping (3.3V native).
// Total inputs: 5 buttons (3 players + Back/Cancel + Cobrar).

// Shared SPI Bus (RC522 + optional SPI display like Nokia 5110 / TFT)
constexpr int PIN_SPI_SCK   = 18;
constexpr int PIN_SPI_MISO  = 19;
constexpr int PIN_SPI_MOSI  = 23;

// Default Hardware I2C Bus (for OLED 1.3" I2C or LCD 16x2 / 20x4 I2C via level shifter)
constexpr int PIN_I2C_SDA   = 21;
constexpr int PIN_I2C_SCL   = 22;

// RC522 NFC/RFID Reader (kept off GPIO 21/22 so I2C remains free)
constexpr int PIN_RFID_SS   = 5;
constexpr int PIN_RFID_RST  = 4;

// 5 Console Buttons (active LOW using INPUT_PULLUP)
constexpr int PIN_BTN_P1     = 32; // Ruedas Verdes
constexpr int PIN_BTN_P2     = 33; // Estudio Prisma
constexpr int PIN_BTN_P3     = 25; // Billetera Clara
constexpr int PIN_BTN_BACK   = 26; // Atras / Cancelar / No
constexpr int PIN_BTN_COBRAR = 27; // Cobrar / Confirmar / Si

// Audio feedback (passive buzzer)
constexpr int PIN_BUZZER     = 14;

// Optional SPI display control pins (e.g. Nokia 5110 PCD8544 or SPI TFT)
// constexpr int PIN_DISP_CS  = 15;
// constexpr int PIN_DISP_DC  = 2;
// constexpr int PIN_DISP_RST = 16;
