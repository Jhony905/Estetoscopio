/*
  Realizado en: IDE Arduino (versión 2.3.6)
  Sketch integrado:
  - Display SPD2010 (con Display_SPD2010.*)
  - Touch SPD2010 (Touch_SPD2010.*)
  - LVGL (LVGL_Driver.*)
  - I2S -> WAV recorder (ganancia, anti-clip)
  - UI: botón GRABAR / DETENER y etiquetas de estado

  Requiere librerías para ESP32-S3-Touch-LCD-1.46B
  - Display_SPD2010.h
  - Display_SPD2010.cpp
  - I2C_Driver.h
  - I2C_Driver.cpp
  - LVGL_Driver.cpp
  - LVGL_Driver.h
  - TCA9554PWR.h
  - TCA9554PWR.cpp
  - Touch_SPD2010.h
  - Touch_SPD2010.cpp
  - esp_lcd_spd2010.h
  - esp_lcd_spd2010.c
*/

#include <Arduino.h>
#include "SD_MMC.h"
#include "SPIFFS.h"
#include "driver/i2s.h"

#include "Display_SPD2010.h"
#include "Touch_SPD2010.h"
#include "LVGL_Driver.h"
#include <lvgl.h>                  // Configuracion v8.3.9

// ---------------- CONFIG ----------------
#define SAMPLE_RATE       16000    // 
#define I2S_BITS_IN_OUT   32       // leer 32-bit words (mic 24-bit en contenedor 32)
#define I2S_READ_BYTES    2048     // bytes por i2s_read() (múltiplo de 4)
#define RECORD_SECONDS    10
#define CHANNELS_OUT      1        // mono (LEFT)
#define DEFAULT_GAIN      3.0f     // ganancia por defecto

// Pines Microfono I2S
#define I2S_PIN_BCK   15
#define I2S_PIN_WS     2
#define I2S_PIN_DIN   39
#define I2S_PIN_DOUT  -1

// Pines MicroSD
#define SD_PIN_CLK  14
#define SD_PIN_CMD  17
#define SD_PIN_D0   16

// Nombre archivo WAV
const char *SD_FILENAME = "/record_test.wav";
const char *SPIFFS_FILENAME = "/record_test_spiffs.wav";

// LVGL objects (globales)
static lv_obj_t * btn_record;
static lv_obj_t * label_status;
static lv_obj_t * label_gain;
static bool lv_recording = false;

// I2S/recording state
static float g_mic_gain = DEFAULT_GAIN;
static bool sd_present = false;
static bool i2s_ready = false;

// Forward
void start_recording();
void stop_recording();
void record_task(void *arg);

// ---------------- WAV header util (16-bit PCM) ----------------
void build_wav_header(uint8_t header[44], uint32_t sampleRate, uint16_t bitsPerSample, uint16_t channels, uint32_t dataBytes) {
  uint32_t byteRate = sampleRate * channels * bitsPerSample / 8;
  uint16_t blockAlign = channels * bitsPerSample / 8;
  uint32_t chunkSize = 36 + dataBytes;
  memset(header, 0, 44);
  header[0] = 'R'; header[1] = 'I'; header[2] = 'F'; header[3] = 'F';
  header[4] = (uint8_t)(chunkSize & 0xFF); header[5] = (uint8_t)((chunkSize >> 8) & 0xFF);
  header[6] = (uint8_t)((chunkSize >> 16) & 0xFF); header[7] = (uint8_t)((chunkSize >> 24) & 0xFF);
  header[8] = 'W'; header[9] = 'A'; header[10] = 'V'; header[11] = 'E';
  header[12] = 'f'; header[13] = 'm'; header[14] = 't'; header[15] = ' ';
  header[16] = 16; // Subchunk1Size
  header[20] = 1;  // PCM
  header[22] = (uint8_t)channels;
  header[24] = (uint8_t)(sampleRate & 0xFF); header[25] = (uint8_t)((sampleRate >> 8) & 0xFF);
  header[26] = (uint8_t)((sampleRate >> 16) & 0xFF); header[27] = (uint8_t)((sampleRate >> 24) & 0xFF);
  header[28] = (uint8_t)(byteRate & 0xFF); header[29] = (uint8_t)((byteRate >> 8) & 0xFF);
  header[30] = (uint8_t)((byteRate >> 16) & 0xFF); header[31] = (uint8_t)((byteRate >> 24) & 0xFF);
  header[32] = (uint8_t)(blockAlign & 0xFF); header[33] = (uint8_t)((blockAlign >> 8) & 0xFF);
  header[34] = (uint8_t)(bitsPerSample & 0xFF); header[35] = (uint8_t)((bitsPerSample >> 8) & 0xFF);
  header[36] = 'd'; header[37] = 'a'; header[38] = 't'; header[39] = 'a';
  header[40] = (uint8_t)(dataBytes & 0xFF); header[41] = (uint8_t)((dataBytes >> 8) & 0xFF);
  header[42] = (uint8_t)((dataBytes >> 16) & 0xFF); header[43] = (uint8_t)((dataBytes >> 24) & 0xFF);
}

// ---------------- I2S init para formato de audio ----------------
bool init_i2s_standard_rx() {
  Serial.println("Configurando I2S (Modo Estandar RX) ... ");
  i2s_driver_uninstall(I2S_NUM_0);
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SAMPLE_RATE,
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

  esp_err_t err = i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
  if (err != ESP_OK) {
    Serial.printf("i2s_driver_install err: %d\n", err);
    return false;
  }
  i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_PIN_BCK,
    .ws_io_num  = I2S_PIN_WS,
    .data_out_num = I2S_PIN_DOUT,
    .data_in_num  = I2S_PIN_DIN
  };
  if (i2s_set_pin(I2S_NUM_0, &pin_config) != ESP_OK) return false;
  if (i2s_set_clk(I2S_NUM_0, SAMPLE_RATE, I2S_BITS_PER_SAMPLE_32BIT, I2S_CHANNEL_STEREO) != ESP_OK) return false;
  i2s_zero_dma_buffer(I2S_NUM_0);
  Serial.println("I2S configurado correctamente");
  return true;
}

// ---------------- LVGL UI helpers ----------------
static void btn_event_cb(lv_event_t * e) {
  lv_obj_t * btn = lv_event_get_target(e);
  if (!lv_recording) {
    lv_recording = true;
    lv_label_set_text(label_status, "Preparando grabacion...");
    start_recording();
    lv_label_set_text(label_status, "Grabando...");
    lv_obj_set_style_bg_color(btn_record, lv_color_hex(0xa30000), 0); // rojo
  } else {
    // detener
    lv_recording = false;
    stop_recording();
    lv_label_set_text(label_status, "Grabacion detenida");
    lv_obj_set_style_bg_color(btn_record, lv_color_hex(0x4b4b4b), 0); // Gris
  }
}

void create_ui(void) {
  lv_obj_t * scr = lv_scr_act();

  // estado
  label_status = lv_label_create(scr);
  lv_label_set_text(label_status, "Listo");
  lv_obj_align(label_status, LV_ALIGN_TOP_MID, 0, 20);  // lv_obj_align(label_status, LV_ALIGN_TOP_MID, x, y);

  // Ganancia
  label_gain = lv_label_create(scr);
  lv_label_set_text_fmt(label_gain, "Ganancia: %.2fx", g_mic_gain);
  lv_obj_align(label_gain, LV_ALIGN_TOP_MID, 0, 8);  // lv_obj_align(label_gain, LV_ALIGN_TOP_MID, x, y);

  // Boton grabar
  btn_record = lv_btn_create(scr);
  lv_obj_set_size(btn_record, 170, 80);
  lv_obj_align(btn_record, LV_ALIGN_CENTER, 0, 0);  // lv_obj_align(btn_record, LV_ALIGN_CENTER, x, y);
  lv_obj_add_event_cb(btn_record, btn_event_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_set_style_bg_color(btn_record, lv_color_hex(0x004100), 0); // verde

  lv_obj_t * label = lv_label_create(btn_record);
  lv_label_set_text(label, "GRABAR");
  lv_obj_center(label);
}

// ---------------- Recording control ----------------
TaskHandle_t recordTaskHandle = nullptr;

void start_recording() {
  if (recordTaskHandle == nullptr) {
    xTaskCreatePinnedToCore(record_task, "recTask", 32*1024, NULL, 5, &recordTaskHandle, 1);
  }
}

void stop_recording() {
  if (recordTaskHandle) {
    vTaskDelete(recordTaskHandle);
    recordTaskHandle = nullptr;
  }
}

// ---------------- record_task ----------------
void record_task(void *arg) {
  // Decide target file: SD if present, else SPIFFS
  bool use_sd = sd_present;
  File f;
  const char *filename = use_sd ? SD_FILENAME : SPIFFS_FILENAME;

  if (!use_sd) {
    if (!SPIFFS.begin(true)) {
      Serial.println("SPIFFS begin failed");
      lv_label_set_text(label_status, "ERROR: SPIFFS fallo");
      vTaskDelete(NULL);
      return;
    }
  }

  Serial.printf("Creando archivo %s ...\n", filename);
  if (use_sd) {
    f = SD_MMC.open(filename, FILE_WRITE);
  } else {
    f = SPIFFS.open(filename, FILE_WRITE);
  }
  if (!f) {
    Serial.println("ERROR: no pudo abrir archivo de salida");
    lv_label_set_text(label_status, "ERROR: abrir archivo");
    vTaskDelete(NULL);
    return;
  }

  // reservar header (44 bytes)
  uint8_t zero44[44] = {0};
  f.write(zero44, 44);

  uint32_t bytes_pcm = 0;
  uint8_t readBuf[I2S_READ_BYTES];
  uint32_t start = millis();

  while (lv_recording && (millis() - start) < (uint32_t)RECORD_SECONDS * 1000) {
    size_t bytesRead = 0;
    esp_err_t r = i2s_read(I2S_NUM_0, readBuf, I2S_READ_BYTES, &bytesRead, 200 / portTICK_PERIOD_MS);
    if (r != ESP_OK) {
      Serial.printf("i2s_read err: %d\n", r);
      break;
    }
    if (bytesRead == 0) continue;

    int samples = bytesRead / 4; // 32-bit words
    int32_t *words = (int32_t *)readBuf;

    for (int i = 0; i < samples; ++i) {
      int32_t w = words[i];
      int16_t sample16 = (int16_t)(w >> 16);

      // aplicar ganancia
      float amplified = (float)sample16 * g_mic_gain;
      if (amplified > 32767.0f) amplified = 32767.0f;
      if (amplified < -32768.0f) amplified = -32768.0f;
      int16_t out16 = (int16_t)amplified;
      f.write((uint8_t *)&out16, sizeof(out16));
      bytes_pcm += sizeof(out16);
    }
  }

  // escribir cabecera WAV correcta (16-bit PCM)
  uint8_t header[44];
  build_wav_header(header, SAMPLE_RATE, I2S_BITS_IN_OUT, CHANNELS_OUT, bytes_pcm);
  f.seek(0);
  f.write(header, 44);
  f.close();

  Serial.printf("Archivo guardado: %s  (bytes PCM=%u)\n", filename, bytes_pcm);
  lv_label_set_text(label_status, "Grabacion lista");
  // terminar task
  lv_recording = false;
  recordTaskHandle = nullptr;
  vTaskDelete(NULL);
}

// ---------------- setup / loop ----------------
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== INICIANDO SISTEMA: DISPLAY + LVGL + I2S + SD ===");

  // Inicializa display (usa tus módulos)
  Serial.println("Inicializando display SPD2010...");
  // Asegúrate de que Display_SPD2010.* tiene las funciones: SPD2010_Init, LCD_Init, Backlight_Init, etc.
  LCD_Init();
  Backlight_Init();
  Set_Backlight(40);
  if (!SPD2010_Init()) {
    Serial.println("Warning: SPD2010_Init() devolvió false (continuando)");
  } else {
    Serial.println("Pantalla SPD2010 inicializada.");
  }

  I2C_Init();
  TCA9554PWR_Init(0x00); 
  // Inicializa LVGL
  Serial.println("Inicializando LVGL...");
  Lvgl_Init();      // registra display/touch con LVGL
  create_ui();      // crea botones/labels

  // Intentar iniciar SD_MMC (pero no bloquear si falla)
  Serial.println("Inicializando SD_MMC...");
  SD_MMC.setPins(SD_PIN_CLK, SD_PIN_CMD, SD_PIN_D0, -1, -1, -1);
  sd_present = false;
  if (SD_MMC.begin("/sdcard", true)) {
    sd_present = true;
    uint64_t sz = SD_MMC.cardSize() / (1024 * 1024);
    Serial.printf("SD_MMC inicializada OK, %llu MB\n", sz);
    lv_label_set_text(label_status, "SD detectada");
  } else {
    Serial.println("Warning: SD_MMC.begin() falló (continuando sin SD)");
    lv_label_set_text(label_status, "No SD (usando SPIFFS)");
  }

  // Inicializar I2S
  Serial.println("Inicializando I2S...");
  if (init_i2s_standard_rx()) {
    Serial.println("I2S listo");
    i2s_ready = true;
  } else {
    Serial.println("ERROR: init I2S failed");
    lv_label_set_text(label_status, "ERROR: I2S");
    i2s_ready = false;
  }

  // Indicar ganancia actual
  lv_label_set_text_fmt(label_gain, "Ganancia: %.2fx", g_mic_gain);

  Serial.println("Configuracion Inicial finalizada.");
}

void loop() {
  // Manejador LVGL
  Lvgl_Loop();
  delay(5);
}
