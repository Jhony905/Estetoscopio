/*
  Realizado en: IDE Arduino (versión 2.3.6)
  Sketch integrado:
  - Display SPD2010 (con Display_SPD2010.*)
  - Touch SPD2010 (Touch_SPD2010.*)
  - LVGL (LVGL_Driver.*)
  - I2S -> WAV recorder (ganancia, anti-clip) Y WAV Play
  - UI: botón GRABAR / REPRODUCIR y etiquetas de estado

  Requiere librerías del fabricante para ESP32-S3-Touch-LCD-1.46B:
  - Display_SPD2010.h
  - Display_SPD2010.cpp
  - I2C_Driver.h
  - I2C_Driver.cpp
  - LVGL_Driver.cpp
  - LVGL_Driver.h
  - SD_Card.cpp
  - SD_Card.h
  - TCA9554PWR.h
  - TCA9554PWR.cpp
  - Touch_SPD2010.h
  - Touch_SPD2010.cpp
  - esp_lcd_spd2010.h
  - esp_lcd_spd2010.c

  Encabezados propios:
  - AudioI2S.cpp
  - AudioI2S.h
*/
// Liberias de Arduino
#include <Arduino.h>
#include "SPIFFS.h"
#include "FS.h"
// Liberias Instaladas
#include <lvgl.h>             // Configuracion v8.3.9
// Liberias de propia-creada
#include "AudioI2S.h"
// Liberias del fabricante
#include "SD_Card.h"
#include "Display_SPD2010.h"
#include "Touch_SPD2010.h"
#include "LVGL_Driver.h"


// ================= CONFIG =================
#define SAMPLE_RATE         16000               // Velocidad de muestra
#define I2S_READ_BYTES      2048                // Bytes de lectura
#define I2S_BITS_PER_SAMPLE 32                  // Tamaño de muestra -> leer 32-bit words (mic 24-bit en contenedor)
#define CHANNELS_OUT        1                   // mono (LEFT)
#define RECORD_SECONDS      10                  // Tiempo de grabación
#define DEFAULT_GAIN        9.0f                // Ganancia de grabación por defecto

// I2S MIC PINS
#define I2S_PIN_BCK         15                  // MIC_SCK
#define I2S_PIN_WS          2                   // MIC_WS
#define I2S_PIN_DIN         39                  // MIC_SD
// I2S SPEAK PINS
#define PCM5101_DOUT        47                  // SPK_I2S_DIN
#define PCM5101_BCK         48                  // SPK_I2S_BCK
#define PCM5101_LRC         38                  // SPK_I2S_LRCK

#define WAV_FILE_SD         "/record.wav"
#define WAV_FILE_SPIFFS     "/record_spiffs.wav"

// ================= GLOBAL =================
static lv_obj_t *btn_rec;                       // Creación de objeto -> Boton de Grabar
static lv_obj_t *btn_play;                      // Creación de objeto -> Boton de Reproducir
static lv_obj_t *label_status;                  // Creación de objeto -> Indicador de estado

static bool recording = false;                  // Variable estado de grabacion
static bool sd_present = false;                 // Variable estado de archivo en SD

AudioI2S::Config i2s_cfg;

// ================= WAV HEADER =================
void build_wav_header(uint8_t h[44], uint32_t sr, uint16_t bps, uint16_t chnls, uint32_t dataBytes) {
  uint32_t chunkSize = 36 + dataBytes;
  uint32_t byteRate = sr * chnls * bps / 8;
  uint16_t blockAlign = chnls * bps / 8;
  memset(h, 0, 44);
  memcpy(h, "RIFF", 4);
  memcpy(h + 8, "WAVEfmt ", 8);
  h[16] = 16; // Subchunk1Size
  h[20] = 1;  // PCM
  h[22] = chnls;
  memcpy(h + 24, &sr, 4);
  memcpy(h + 28, &byteRate, 4);
  h[32] = blockAlign; h[34] = bps;
  memcpy(h + 36, "data", 4);
  memcpy(h + 40, &dataBytes, 4);
}

// ================= RECORD TASK =================
void record_task(void *) {
  File f;
  const char *fn = sd_present ? WAV_FILE_SD : WAV_FILE_SPIFFS;

  if (!sd_present && !SPIFFS.begin(true)) {
    lv_label_set_text(label_status, "SPIFFS ERROR");
    vTaskDelete(nullptr);
  }

  f = sd_present ? SD_MMC.open(fn, FILE_WRITE) : SPIFFS.open(fn, FILE_WRITE);
  if (!f) {
    lv_label_set_text(label_status, "FILE ERROR");
    vTaskDelete(nullptr);
  }

  uint8_t wavHeader[44] = {0};
  f.write(wavHeader, 44);

  AudioI2S::beginRx(i2s_cfg);

  uint8_t buf[I2S_READ_BYTES];
  size_t bytesRead;
  uint32_t pcmBytes = 0;
  uint32_t start = millis();

  while (recording && millis() - start < RECORD_SECONDS * 1000) {
    if (AudioI2S::read(buf, sizeof(buf), bytesRead, 200 / portTICK_PERIOD_MS)) {
      int samples = bytesRead / 4;
      int32_t *p = (int32_t *)buf;
      for (int i = 0; i < samples; i++) {
        int16_t s = AudioI2S::convert32to16(p[i], DEFAULT_GAIN);
        f.write((uint8_t *)&s, 2);
        pcmBytes += 2;
      }
    }
  }

  AudioI2S::end();

  build_wav_header(wavHeader, SAMPLE_RATE, I2S_BITS_PER_SAMPLE, CHANNELS_OUT, pcmBytes);
  f.seek(0);
  f.write(wavHeader, 44);
  f.close();

  recording = false;
  lv_label_set_text(label_status, "Grabacion OK");
  lv_obj_set_style_bg_color(btn_rec, lv_color_hex(0x4b4b4b), 0); // Gris
  vTaskDelete(nullptr);
}

// ================= PLAY WAV =================
void play_wav_file(const char *fn) {
  File f = sd_present ? SD_MMC.open(fn) : SPIFFS.open(fn);
  if (!f) {
    lv_label_set_text(label_status, "PLAY ERROR");
    return;
  }

  f.seek(44); // skip WAV header

  AudioI2S::beginTx(i2s_cfg);

  int32_t mono_buf[256];          // 256 muestras MONO
  int32_t stereo_buf[512];        // 256 * 2 (L/R)
  size_t br, bw;

  while ((br = f.read((uint8_t*)mono_buf, sizeof(mono_buf))) > 0) {   // Leer los bytes y reinterpretarlos
    int samples = br / sizeof(int32_t);

    for (int i = 0; i < samples; i++) {
      stereo_buf[2*i]     = mono_buf[i]; // LEFT
      stereo_buf[2*i + 1] = mono_buf[i]; // RIGHT
    }

    AudioI2S::write(stereo_buf,
                    samples * 2 * sizeof(int32_t),
                    bw,
                    portMAX_DELAY);
  }

  AudioI2S::end();
  f.close();
  lv_label_set_text(label_status, "Play terminado");
  lv_obj_set_style_bg_color(btn_play, lv_color_hex(0x4b4b4b), 0); // Gris
}

// ================= UI =================
static void rec_cb(lv_event_t *) {
  if (!recording) {
    recording = true;
    lv_label_set_text(label_status, "Grabando...");
    lv_obj_set_style_bg_color(btn_rec, lv_color_hex(0xa30000), 0); // rojo
    xTaskCreatePinnedToCore(record_task, "rec", 32 * 1024, nullptr, 5, nullptr, 1);
  }
}

static void play_cb(lv_event_t *) {
  lv_label_set_text(label_status, "Reproduciendo...");
  lv_obj_set_style_bg_color(btn_play, lv_color_hex(0x004100), 0); // verde
  play_wav_file(sd_present ? WAV_FILE_SD : WAV_FILE_SPIFFS);
}

void create_ui() {
  lv_obj_t *scr = lv_scr_act();

  label_status = lv_label_create(scr);
  lv_label_set_text(label_status, "Listo");
  lv_obj_align(label_status, LV_ALIGN_TOP_MID, 0, 10);

  btn_rec = lv_btn_create(scr);
  lv_obj_set_size(btn_rec, 140, 60);
  lv_obj_align(btn_rec, LV_ALIGN_CENTER, -80, 40);
  lv_obj_add_event_cb(btn_rec, rec_cb, LV_EVENT_CLICKED, nullptr);
  lv_obj_set_style_bg_color(btn_rec, lv_color_hex(0x4b4b4b), 0); // Gris
  lv_label_set_text(lv_label_create(btn_rec), "GRABAR");

  btn_play = lv_btn_create(scr);
  lv_obj_set_size(btn_play, 140, 60);
  lv_obj_align(btn_play, LV_ALIGN_CENTER, 80, 40);
  lv_obj_add_event_cb(btn_play, play_cb, LV_EVENT_CLICKED, nullptr);
  lv_obj_set_style_bg_color(btn_play, lv_color_hex(0x4b4b4b), 0); // Gris
  lv_label_set_text(lv_label_create(btn_play), "PLAY");
}

// ================= SETUP / LOOP =================
void setup() {                                                                                                                                                                                  
  Serial.begin(115200);                         // Inicializacion de comunicacion monitor serial

  LCD_Init();                                   // Inicializacion de LCD
  Backlight_Init();                             // inicializacion de luz de fondo
  Set_Backlight(40);

  I2C_Init();                                   // Inicializacion de comunicacion I2C
  TCA9554PWR_Init(0x00);                        // Inicializacion de Touch                       

  Lvgl_Init();                                  // Inicializacion de Libreria de graficos
  create_ui();                                  // Inicializacion de graficos del proyecto

  SD_Init();                                    // Inicializacion de MicroSD
  sd_present = SD_MMC.begin("/sdcard", true);

  i2s_cfg.sample_rate = SAMPLE_RATE;            // Inicializacion de comunicacion I2S
  i2s_cfg.pin_bck  = I2S_PIN_BCK;
  i2s_cfg.pin_ws   = I2S_PIN_WS;
  i2s_cfg.pin_din  = I2S_PIN_DIN;
  i2s_cfg.pin_dout = 47;
}

void loop() {
  Lvgl_Loop();
  delay(5);
}