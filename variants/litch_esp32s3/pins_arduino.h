#pragma once

#include <stdint.h>

#define USB_VID 0x303A
#define USB_PID 0x4001
#define USB_MANUFACTURER "Foxie Products"
#define USB_PRODUCT "Litch"
#define USB_SERIAL "__MAC__"

#define BUTTON 0

static const uint8_t TX = 43;
static const uint8_t RX = 44;

static const uint8_t SDA = 3;
static const uint8_t SCL = 4;

static const uint8_t SS = 42;
static const uint8_t MOSI = 35;
static const uint8_t MISO = 37;
static const uint8_t SCK = 36;
