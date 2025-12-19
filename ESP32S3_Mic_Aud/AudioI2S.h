#pragma once
#include <Arduino.h>
#include "driver/i2s.h"

// I2S MIC PINS
#define I2S_PIN_BCK   15
#define I2S_PIN_WS     2
#define I2S_PIN_DIN   39

namespace AudioI2S {

  struct Config {
    i2s_port_t port = I2S_NUM_0;
    uint32_t sample_rate = 16000;
    int pin_bck;
    int pin_ws;
    int pin_din;
    int pin_dout = -1;
  };

  /*Para Grabación - Microfono*/
  bool begin(const Config& cfg);          // RX (micrófono)
  void end();
  bool isReady();

  bool read(void* buffer,
            size_t bytes_to_read,
            size_t& bytes_read,
            TickType_t timeout_ticks);

  int16_t convert32to16(int32_t sample32, float gain);    // procesamiento local

  /*-----------------------------------------------------------------------------*/
  /*Para Reproducción*/
  //bool beginRx(const Config& cfg);
  bool beginTx(const Config& cfg);

  bool write(const void* buffer,
             size_t bytes_to_write,
             size_t& bytes_written,
             TickType_t timeout_ticks);
}
