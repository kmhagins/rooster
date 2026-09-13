// xiao_pins.h
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Analog / Digital Pins
#define XIAO_D0     1
#define XIAO_D1     2
#define XIAO_D2     3
#define XIAO_D3     4
#define XIAO_D4     5
#define XIAO_D5     6
#define XIAO_D6     43
#define XIAO_D7     44
#define XIAO_D8     7
#define XIAO_D9     8
#define XIAO_D10    9

// Onboard User LED (Active Low)
#define XIAO_LED    21

// I2C Pins (Default)
#define XIAO_SDA    5   // Same as D4
#define XIAO_SCL    6   // Same as D5

// SPI Pins (Default)
#define XIAO_MISO   9   // Same as D10
#define XIAO_MOSI   10
#define XIAO_SCK    8   // Same as D9
#define XIAO_SS     7   // Same as D8

#ifdef __cplusplus
}
#endif