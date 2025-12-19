#include "AudioI2S.h"

namespace AudioI2S {
  static Config current;
  static bool active = false;

  static bool install(i2s_mode_t mode) {
    i2s_driver_uninstall(current.port);

    i2s_config_t cfg = {
      .mode = (i2s_mode_t)(I2S_MODE_MASTER | mode),
      .sample_rate = current.sample_rate,
      .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
      .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
      .communication_format = (i2s_comm_format_t)(I2S_COMM_FORMAT_STAND_I2S),
      .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
      .dma_buf_count = 6,
      .dma_buf_len = 512,
      .use_apll = false,
      .tx_desc_auto_clear = true,
      .fixed_mclk = 0
    };

    if (i2s_driver_install(current.port, &cfg, 0, nullptr) != ESP_OK)
      return false;

    i2s_pin_config_t pins = {
      .bck_io_num = (mode == I2S_MODE_RX) ? current.pin_bck : 48,
      .ws_io_num = (mode == I2S_MODE_RX) ? current.pin_ws: 38,
      .data_out_num = (mode == I2S_MODE_TX) ? current.pin_dout : -1,
      .data_in_num = (mode == I2S_MODE_RX) ? current.pin_din : -1
    };

    i2s_set_pin(current.port, &pins);
    i2s_zero_dma_buffer(current.port);
    active = true;
    return true;
  }

  bool beginRx(const Config &cfg) { current = cfg; return install(I2S_MODE_RX); }
  bool beginTx(const Config &cfg) { current = cfg; return install(I2S_MODE_TX); }

  void end() {
    if (active) {
      i2s_driver_uninstall(current.port);
      active = false;
    }
  }

  bool read(void *b, size_t l, size_t &o, TickType_t t) {
    return i2s_read(current.port, b, l, &o, t) == ESP_OK;
  }

  bool write(const void *b, size_t l, size_t &o, TickType_t t) {
    return i2s_write(current.port, b, l, &o, t) == ESP_OK;
  }

  int16_t convert32to16(int32_t s, float g) {
    int16_t v = s >> 16;              // Ideal [16], Modificar a 14 implica-> Amplificar la señal ≈ +12 dB; Introducir riesgo de clipping; Distorsión en picos; Sonido más fuerte, pero no fiel. Solo sería válido si: Sabes que el WAV viene con headroom, O buscas compensar una señal muy baja, O estás haciendo pruebas subjetivas de volumen
    float r = v * g;
    if (r > 32767) r = 32767;
    if (r < -32768) r = -32768;
    return (int16_t)r;
  }
}