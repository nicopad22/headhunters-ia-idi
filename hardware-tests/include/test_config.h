#pragma once

// ============================================================================
// Toggle individual hardware tests here (1 = ON, 0 = OFF).
// Only NFC (RC522) is enabled by default.
// ============================================================================
#ifndef ENABLE_TEST_NFC
  #define ENABLE_TEST_NFC      1
#endif
#ifndef ENABLE_TEST_BUTTONS
  #define ENABLE_TEST_BUTTONS  0
#endif
#ifndef ENABLE_TEST_BUZZER
  #define ENABLE_TEST_BUZZER   0
#endif
#ifndef ENABLE_TEST_OLED
  #define ENABLE_TEST_OLED     0
#endif

// ============================================================================
// Pin mapping (identical to firmware/include/pin_config.h)
// ============================================================================
constexpr int PIN_SPI_SCK    = 18;
constexpr int PIN_SPI_MISO   = 19;
constexpr int PIN_SPI_MOSI   = 23;

constexpr int PIN_I2C_SDA    = 21;
constexpr int PIN_I2C_SCL    = 22;

constexpr int PIN_RFID_SS    = 5;
constexpr int PIN_RFID_RST   = 4;

constexpr int PIN_BTN_P1     = 32; // Ruedas Verdes
constexpr int PIN_BTN_P2     = 33; // Estudio Prisma
constexpr int PIN_BTN_P3     = 25; // Billetera Clara
constexpr int PIN_BTN_BACK   = 26; // Atras / Cancelar
constexpr int PIN_BTN_COBRAR = 27; // Cobrar / Confirmar

constexpr int PIN_BUZZER     = 14;

constexpr uint8_t OLED_I2C_ADDR = 0x3C;
