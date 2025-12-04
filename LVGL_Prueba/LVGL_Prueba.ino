// Grabar_I2S_a_SD.ino
// Requiere: Arduino IDE con soporte ESP32-S3
// Incluye: driver/i2s.h (ESP-IDF) y SD_MMC para SD en ESP32

#include <SD_MMC.h>
#include "driver/i2s.h"
#include <FS.h>

#define WAV_FILENAME "/record.wav"

// --- Parâmetros de audio ---
const int sampleRate = 16000;              // 16 kHz (ajustable)
const i2s_bits_per_sample_t bits = I2S_BITS_PER_SAMPLE_16BIT;
const int channels = 1;                    // mono

// --- Pines I2S (ajusta según tu placa si es necesario) ---
const gpio_num_t I2S_PIN_BCK = GPIO_NUM_15;   // ejemplo
const gpio_num_t I2S_PIN_WS  = GPIO_NUM_2;   // ejemplo
const gpio_num_t I2S_PIN_DIN = GPIO_NUM_39;  // ejemplo: data in (MIC)
const gpio_num_t I2S_PIN_DOUT = I2S_PIN_NO_CHANGE;

File wavFile;
bool recording = false;

// Escribe cabecera WAV temporal (se actualizará al finalizar)
void writeWavHeader(File &f, uint32_t totalAudioLen, uint32_t sampleRate, uint16_t bitsPerSample, uint16_t channels) {
  uint32_t byteRate = sampleRate * channels * bitsPerSample/8;
  uint32_t blockAlign = channels * bitsPerSample/8;
  // RIFF header
  f.seek(0);
  f.write("RIFF", 4);
  uint32_t chunkSize = 36 + totalAudioLen;
  f.write((const uint8_t*)&chunkSize, 4);
  f.write("WAVE", 4);
  // fmt subchunk
  f.write("fmt ", 4);
  uint32_t subchunk1Size = 16;
  f.write((const uint8_t*)&subchunk1Size, 4);
  uint16_t audioFormat = 1; // PCM
  f.write((const uint8_t*)&audioFormat, 2);
  f.write((const uint8_t*)&channels, 2);
  f.write((const uint8_t*)&sampleRate, 4);
  f.write((const uint8_t*)&byteRate, 4);
  f.write((const uint8_t*)&blockAlign, 2);
  f.write((const uint8_t*)&bitsPerSample, 2);
  // data subchunk
  f.write("data", 4);
  f.write((const uint8_t*)&totalAudioLen, 4);
}

void startI2S() {
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = sampleRate,
    .bits_per_sample = bits,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_I2S_MSB,
    .intr_alloc_flags = 0,
    .dma_buf_count = 4,
    .dma_buf_len = 1024,
    .use_apll = false,
    .tx_desc_auto_clear = false,
    .fixed_mclk = 0
  };

  i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_PIN_BCK,
    .ws_io_num = I2S_PIN_WS,
    .data_out_num = I2S_PIN_DOUT,
    .data_in_num = I2S_PIN_DIN
  };

  i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &pin_config);
  // si tu mic requiere configuración adicional (ej. PDM vs I2S), ajústalo aquí
}

void stopI2S() {
  i2s_driver_uninstall(I2S_NUM_0);
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("Iniciando SD_MMC...");
  if (!SD_MMC.begin()) {
    Serial.println("ERROR: No se montó SD_MMC");
    while (1) delay(1000);
  }
  // Crear archivo y escribir cabecera provisional (tamaño 0)
  wavFile = SD_MMC.open(WAV_FILENAME, FILE_WRITE);
  if(!wavFile) {
    Serial.println("No se pudo crear archivo WAV en SD");
    while (1) delay(1000);
  }
  // reservamos 44 bytes para header
  for (int i=0;i<44;i++) wavFile.write((uint8_t)0);

  Serial.println("Inicia I2S...");
  startI2S();
  recording = true;
}

void loop() {
  if (!recording) { delay(100); return; }

  const int buffSamples = 1024;
  size_t bytesRead;
  uint8_t i2sBuffer[buffSamples * 2]; // 16-bit -> 2 bytes
  // lee datos desde I2S
  esp_err_t r = i2s_read(I2S_NUM_0, (void*)i2sBuffer, sizeof(i2sBuffer), &bytesRead, pdMS_TO_TICKS(1000));
  if (r == ESP_OK && bytesRead > 0) {
    // escribe crudo en el archivo (PCM 16-bit little endian)
    wavFile.write(i2sBuffer, bytesRead);
    wavFile.flush();
    Serial.print("."); // indicador simple
  }

  // EJEMPLO de parada automática tras N segundos (opcional)
  // if ( /* alguna condición */ ) stopRecording();
}

// Llamar manualmente para detener y actualizar cabecera WAV
void stopRecording() {
  if (!recording) return;
  recording = false;
  stopI2S();

  uint32_t fileSize = wavFile.size();
  uint32_t dataLen = fileSize - 44; // después del header provisional
  writeWavHeader(wavFile, dataLen, sampleRate, 16, channels);
  wavFile.close();
  Serial.println("\nGrabación finalizada, header actualizado.");
}
