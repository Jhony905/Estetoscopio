/*
  Realizado en: IDE Arduino (versión 2.3.6)
  Sketch integrado:
  - Display SPD2010 (con Display_SPD2010.*)
  - Touch SPD2010 (Touch_SPD2010.*)
  - LVGL (LVGL_Driver.*)
  - I2S -> WAV recorder (ganancia, anti-clip)
  - UI: botón GRABAR / DETENER y etiquetas de estado

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
// Liberias Instaladas
#include <lvgl.h>             // Configuracion v8.3.9
// Liberias de propia-creada
#include "AudioI2S.h"
// Liberias del fabricante
#include "SD_Card.h"
#include "Display_SPD2010.h"
#include "Touch_SPD2010.h"
#include "LVGL_Driver.h"


// ---------------- CONFIG ----------------
#define SAMPLE_RATE         16000
#define I2S_READ_BYTES      2048
#define I2S_BITS_PER_SAMPLE 32            // leer 32-bit words (mic 24-bit en contenedor 
#define CHANNELS_OUT        1             // mono (LEFT)
#define RECORD_SECONDS      10            // Tiempo de grabación
#define DEFAULT_GAIN        9.0f          // Ganancia de grabación por defecto

const char *SD_FILENAME     = "/record.wav";
const char *SPIFFS_FILENAME = "/record_spiffs.wav";

// LVGL
static lv_obj_t *btn_record;
static lv_obj_t *label_status;
static lv_obj_t *label_gain;

static bool recording = false;
static float mic_gain = DEFAULT_GAIN;
static bool sd_present = false;

TaskHandle_t recordTaskHandle = nullptr;

// ---------------- WAV HEADER ----------------
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

// ---------------- RECORD TASK ----------------
void record_task(void*) {
  File f;
  bool use_sd = sd_present;
  const char* fn = use_sd ? SD_FILENAME : SPIFFS_FILENAME;

  if (!use_sd && !SPIFFS.begin(true)) {
    lv_label_set_text(label_status, "ERROR SPIFFS");
    vTaskDelete(nullptr);
  }

  f = use_sd ? SD_MMC.open(fn, FILE_WRITE) : SPIFFS.open(fn, FILE_WRITE);
  if (!f) {
    lv_label_set_text(label_status, "ERROR ARCHIVO");
    vTaskDelete(nullptr);
  }

  uint8_t header[44] = {0};
  f.write(header, 44);

  uint8_t buf[I2S_READ_BYTES];
  size_t bytesRead;
  uint32_t pcmBytes = 0;
  uint32_t start = millis();

  while (recording && millis() - start < RECORD_SECONDS * 1000) {
    if (AudioI2S::read(buf, sizeof(buf), bytesRead, 200 / portTICK_PERIOD_MS)) {
      int samples = bytesRead / 4;
      int32_t* w = (int32_t*)buf;
      for (int i = 0; i < samples; i++) {
        int16_t s = AudioI2S::convert32to16(w[i], mic_gain);
        f.write((uint8_t*)&s, 2);
        pcmBytes += 2;
      }
    }
  }

  build_wav_header(header, SAMPLE_RATE, I2S_BITS_PER_SAMPLE, CHANNELS_OUT, pcmBytes);
  f.seek(0);
  f.write(header, 44);
  f.close();

  recording = false;
  recordTaskHandle = nullptr;
  lv_label_set_text(label_status, "Grabacion lista");
  vTaskDelete(nullptr);
}

// ---------------- UI ----------------
/*Para Grabación*/
static void btn_cb(lv_event_t*) {
  if (!recording) {
    recording = true;
    lv_label_set_text(label_status, "Grabando...");
    xTaskCreatePinnedToCore(record_task, "rec", 32 * 1024, nullptr, 5, &recordTaskHandle, 1);
  } else {
    recording = false;
    lv_label_set_text(label_status, "Detenido");
  }
}

/*Para Reproducción*/
void play_wav(const char* filename) {
  File f = sd_present ? SD_MMC.open(filename) : SPIFFS.open(filename);
  if (!f) {
    lv_label_set_text(label_status, "ERROR PLAY");
    return;
  }

  f.seek(44); // saltar header WAV

  uint8_t buf[2048];
  size_t br, bw;

  lv_label_set_text(label_status, "Reproduciendo...");

  while ((br = f.read(buf, sizeof(buf))) > 0) {
    AudioI2S::write(buf, br, bw, portMAX_DELAY);
  }

  f.close();
  lv_label_set_text(label_status, "Reproduccion lista");
}

/*Pantalla incial*/
void create_ui() {
  lv_obj_t *scr = lv_scr_act();
  label_status = lv_label_create(scr);
  lv_label_set_text(label_status, "Listo");
  lv_obj_align(label_status, LV_ALIGN_TOP_MID, 0, 20);

  label_gain = lv_label_create(scr);
  lv_label_set_text_fmt(label_gain, "Ganancia %.1fx", mic_gain);
  lv_obj_align(label_gain, LV_ALIGN_TOP_MID, 0, 5);

  btn_record = lv_btn_create(scr);
  lv_obj_set_size(btn_record, 160, 80);
  lv_obj_center(btn_record);
  lv_obj_add_event_cb(btn_record, btn_cb, LV_EVENT_CLICKED, nullptr);

  lv_obj_t *l = lv_label_create(btn_record);
  lv_label_set_text(l, "GRABAR");
  lv_obj_center(l);
}

// ---------------- SETUP / LOOP ----------------
void setup() {
  Serial.begin(115200);

  LCD_Init();
  Backlight_Init();
  Set_Backlight(40);

  I2C_Init();
  TCA9554PWR_Init(0x00);

  Lvgl_Init();
  create_ui();

  SD_Init();
  sd_present = SD_MMC.begin("/sdcard", true);

  AudioI2S::Config cfg;
  cfg.sample_rate = SAMPLE_RATE;
  cfg.pin_bck = I2S_PIN_BCK;
  cfg.pin_ws  = I2S_PIN_WS;
  cfg.pin_din = I2S_PIN_DIN;

  AudioI2S::begin(cfg);
}

void loop() {
  Lvgl_Loop();
  delay(5);
}
