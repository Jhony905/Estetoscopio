#pragma once
#include <Arduino.h>
#include "driver/i2s.h"

namespace AudioI2S {
  struct Config {
    i2s_port_t port = I2S_NUM_0;
    uint32_t sample_rate = 16000;
    int pin_bck;
    int pin_ws;
    int pin_din;
    int pin_dout;
  };

  bool beginRx(const Config &cfg);        // RX (para Micrófono)
  bool beginTx(const Config &cfg);        // TX (para Parlante)
  void end();

  bool read(void *buffer, 
            size_t bytes_a_leer, 
            size_t & bytes_a_leidos, 
            TickType_t timeout_ticks);

  bool write(const void *buffer, 
            size_t bytes_a_escribir, 
            size_t & bytes_escritos, 
            TickType_t timeout_ticks);

  int16_t convert32to16(int32_t sample, float ganancia); // procesamiento local
}
