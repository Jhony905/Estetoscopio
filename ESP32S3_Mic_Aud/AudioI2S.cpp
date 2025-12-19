#include "AudioI2S.h"

namespace AudioI2S {

  static bool ready = false;
  static Config current;

  bool begin(const Config& cfg) {
    current = cfg;
    i2s_driver_uninstall(cfg.port);

    i2s_config_t i2s_cfg = {
      .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
      .sample_rate = cfg.sample_rate,
      .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
      .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
      .communication_format = (i2s_comm_format_t)(I2S_COMM_FORMAT_STAND_I2S),
      .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
      .dma_buf_count = 4,
      .dma_buf_len = 512,
      .use_apll = false,
      .tx_desc_auto_clear = false,
      .fixed_mclk = 0
    };

    if (i2s_driver_install(cfg.port, &i2s_cfg, 0, nullptr) != ESP_OK)
      return false;

    i2s_pin_config_t pins = {
      .bck_io_num = cfg.pin_bck,
      .ws_io_num  = cfg.pin_ws,
      .data_out_num = cfg.pin_dout,
      .data_in_num  = cfg.pin_din
    };

    if (i2s_set_pin(cfg.port, &pins) != ESP_OK)
      return false;

    if (i2s_set_clk(cfg.port,
                    cfg.sample_rate,
                    I2S_BITS_PER_SAMPLE_32BIT,
                    I2S_CHANNEL_STEREO) != ESP_OK)
      return false;

    i2s_zero_dma_buffer(cfg.port);
    ready = true;
    return true;
  }

  void end() {
    if (ready) {
      i2s_driver_uninstall(current.port);
      ready = false;
    }
  }

  bool isReady() {
    return ready;
  }

  bool read(void* buffer,
            size_t bytes_to_read,
            size_t& bytes_read,
            TickType_t timeout_ticks) {
    if (!ready) return false;
    return i2s_read(current.port,
                    buffer,
                    bytes_to_read,
                    &bytes_read,
                    timeout_ticks) == ESP_OK;
  }

  int16_t convert32to16(int32_t s, float gain) {
    int16_t base = (int16_t)(s >> 16);
    float v = base * gain;
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    return (int16_t)v;
  }
}

/*------------------------------------------------------*/
/*Para Reproducción*/
bool AudioI2S::beginTx(const Config& cfg) {
  current = cfg;
  if (ready) i2s_driver_uninstall(cfg.port);

  i2s_config_t i2s_cfg = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = cfg.sample_rate,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 6,
    .dma_buf_len = 512,
    .use_apll = false,
    .tx_desc_auto_clear = true,
    .fixed_mclk = 0
  };

  if (i2s_driver_install(cfg.port, &i2s_cfg, 0, nullptr) != ESP_OK)
    return false;

  i2s_pin_config_t pins = {
    .bck_io_num = cfg.pin_bck,
    .ws_io_num  = cfg.pin_ws,
    .data_out_num = cfg.pin_dout,
    .data_in_num  = -1
  };

  if (i2s_set_pin(cfg.port, &pins) != ESP_OK)
    return false;

  i2s_zero_dma_buffer(cfg.port);
  ready = true;
  return true;
}

bool AudioI2S::write(const void* buffer,
                     size_t bytes_to_write,
                     size_t& bytes_written,
                     TickType_t timeout_ticks) {
  if (!ready) return false;
  return i2s_write(current.port,
                   buffer,
                   bytes_to_write,
                   &bytes_written,
                   timeout_ticks) == ESP_OK;
}
 