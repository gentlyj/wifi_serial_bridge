#include "sticks3_audio.h"
#include "sticks3_power.h"
#include "esp_log.h"
#include "driver/i2c.h"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/timers.h"
#include <math.h>
#include <string.h>

static const char *TAG = "sticks3_audio";

// ES8311 I2C address
#define ES8311_ADDR 0x18

// I2S pin configuration for StickS3 speaker
#define I2S_MCK_PIN   18
#define I2S_BCK_PIN   17
#define I2S_WS_PIN    15
#define I2S_DATA_PIN  14
#define I2S_PORT      I2S_NUM_0

// Audio parameters
#define SAMPLE_RATE   44100
#define SAMPLE_BITS   I2S_DATA_BIT_WIDTH_16BIT
#define CHANNEL_MODE  I2S_SLOT_MODE_MONO

static i2s_chan_handle_t tx_handle = NULL;

// Tone command type (defined early for timer callback)
typedef struct {
    uint16_t freq_hz;
    uint16_t volume;     // 0-100
    uint32_t duration_ms;
} tone_cmd_t;

static QueueHandle_t tone_queue = NULL;

// PA management with FreeRTOS timer
static TimerHandle_t pa_off_timer = NULL;
static bool pa_is_on = false;
#define PA_OFF_DELAY_MS  100  // delay after tone ends before disabling PA

static void pa_off_timer_cb(TimerHandle_t timer) {
    // Send PA-off command to tone_gen_task (avoids I2C in timer context)
    tone_cmd_t cmd = { .freq_hz = 0, .volume = 0, .duration_ms = 0 };
    xQueueSend(tone_queue, &cmd, 0);
}

static void pa_ensure_on(void) {
    if (!pa_is_on) {
        sticks3_power_speaker_pa_enable();
        pa_is_on = true;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static void pa_schedule_off(void) {
    if (pa_off_timer) {
        xTimerReset(pa_off_timer, 0);
    }
}

static void pa_cancel_off(void) {
    if (pa_off_timer) {
        xTimerStop(pa_off_timer, 0);
    }
}

// Tone generator — blocks when idle, no CPU waste

#define FADE_SAMPLES  (SAMPLE_RATE * 20 / 1000)  // 20ms fade
#define TAIL_SAMPLES  (SAMPLE_RATE * 100 / 1000)  // 100ms trailing silence

static void tone_gen_task(void *arg) {
    tone_cmd_t cmd = {};
    const uint32_t buf_samples = 256;
    int16_t *buf = (int16_t *)malloc(buf_samples * sizeof(int16_t));
    float phase = 0.0f;

    while (1) {
        // Block until a command arrives — zero CPU when idle
        if (xQueueReceive(tone_queue, &cmd, portMAX_DELAY) != pdTRUE) continue;
        // PA-off signal from timer
        if (cmd.freq_hz == 0 && cmd.duration_ms == 0) {
            if (pa_is_on) {
                sticks3_power_speaker_pa_disable();
                pa_is_on = false;
            }
            continue;
        }
        if (cmd.freq_hz == 0 || cmd.duration_ms == 0) continue;

        pa_cancel_off();
        pa_ensure_on();

        const uint32_t num_samples = SAMPLE_RATE * cmd.duration_ms / 1000;
        const float amplitude = cmd.volume * 32767.0f / 100.0f;
        float freq = cmd.freq_hz;
        float phase_inc = 2.0f * M_PI * freq / SAMPLE_RATE;

        uint32_t samples_written = 0;
        bool need_crossfade = false;

        // --- Tone playback with fade in/out and crossfade ---
        while (samples_written < num_samples) {
            tone_cmd_t new_cmd;
            if (xQueueReceive(tone_queue, &new_cmd, 0) == pdTRUE && new_cmd.freq_hz > 0) {
                need_crossfade = true;
                cmd = new_cmd;
            }

            uint32_t chunk = (num_samples - samples_written > buf_samples) ?
                             buf_samples : (num_samples - samples_written);

            for (uint32_t i = 0; i < chunk; i++) {
                float sine = sinf(phase);
                phase += phase_inc;
                if (phase >= 2.0f * M_PI) phase -= 2.0f * M_PI;

                float env = 1.0f;
                uint32_t pos = samples_written + i;

                if (need_crossfade && pos < FADE_SAMPLES) {
                    env = 1.0f - (float)pos / FADE_SAMPLES;
                } else if (pos < FADE_SAMPLES) {
                    env = (float)pos / FADE_SAMPLES;
                } else if (pos >= num_samples - FADE_SAMPLES) {
                    env = (float)(num_samples - 1 - pos) / FADE_SAMPLES;
                }
                buf[i] = (int16_t)(sine * amplitude * env);
            }

            size_t bw = 0;
            i2s_channel_write(tx_handle, buf, chunk * sizeof(int16_t), &bw, pdMS_TO_TICKS(1000));
            samples_written += chunk;

            if (need_crossfade && samples_written >= FADE_SAMPLES) {
                need_crossfade = false;
                freq = cmd.freq_hz;
                phase_inc = 2.0f * M_PI * freq / SAMPLE_RATE;
                samples_written = 0;
            }
        }

        // --- Trailing silence: enough for DMA to flush before PA-off timer fires ---
        memset(buf, 0, buf_samples * sizeof(int16_t));
        for (uint32_t written = 0; written < TAIL_SAMPLES; written += buf_samples) {
            uint32_t chunk = TAIL_SAMPLES - written;
            if (chunk > buf_samples) chunk = buf_samples;
            size_t bw = 0;
            i2s_channel_write(tx_handle, buf, chunk * sizeof(int16_t), &bw, pdMS_TO_TICKS(100));
        }

        pa_schedule_off();
    }
    free(buf);
}

// ES8311 register write via legacy I2C driver
static esp_err_t es8311_write_reg(uint8_t reg, uint8_t val) {
    i2c_port_t port = sticks3_power_get_i2c_port();

    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (ES8311_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_write_byte(cmd, val, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(port, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    return ret;
}

// Configure ES8311 for I2S playback
// Register values from m5unified _speaker_enabled_cb_sticks3 (M5Unified.cpp:499-508)
static esp_err_t es8311_configure(void) {
    esp_err_t ret;

    ret = es8311_write_reg(0x00, 0x80);  // CSM POWER ON
    ret = es8311_write_reg(0x01, 0xB5);  // MCLK=BCLK
    ret = es8311_write_reg(0x02, 0x18);  // MULT_PRE=3
    ret = es8311_write_reg(0x0D, 0x01);  // Power up analog circuitry
    ret = es8311_write_reg(0x12, 0x00);  // Power up DAC
    ret = es8311_write_reg(0x13, 0x10);  // Enable output to HP drive
    ret = es8311_write_reg(0x32, 0xBF);  // DAC volume (0 dB)
    ret = es8311_write_reg(0x37, 0x08);  // Bypass DAC equalizer

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ES8311 configure failed");
        return ret;
    }

    return ESP_OK;
}

// Initialize I2S for speaker output
static esp_err_t i2s_speaker_init(void) {
    // Create I2S channel
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;

    esp_err_t ret = i2s_new_channel(&chan_cfg, &tx_handle, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S channel create failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Configure I2S standard mode
    i2s_std_config_t std_cfg = {};
    std_cfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE);
    std_cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(SAMPLE_BITS, CHANNEL_MODE);
    std_cfg.gpio_cfg.mclk = (gpio_num_t)I2S_MCK_PIN;
    std_cfg.gpio_cfg.bclk = (gpio_num_t)I2S_BCK_PIN;
    std_cfg.gpio_cfg.ws   = (gpio_num_t)I2S_WS_PIN;
    std_cfg.gpio_cfg.dout = (gpio_num_t)I2S_DATA_PIN;
    std_cfg.gpio_cfg.din  = I2S_GPIO_UNUSED;
    std_cfg.gpio_cfg.invert_flags.mclk_inv = false;
    std_cfg.gpio_cfg.invert_flags.bclk_inv = false;
    std_cfg.gpio_cfg.invert_flags.ws_inv   = false;

    ret = i2s_channel_init_std_mode(tx_handle, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S std init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = i2s_channel_enable(tx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S enable failed: %s", esp_err_to_name(ret));
        return ret;
    }

    return ESP_OK;
}

esp_err_t sticks3_audio_speaker_init(void) {
    // 1. Configure ES8311 codec first (PA is OFF, no noise output)
    esp_err_t ret = es8311_configure();
    if (ret != ESP_OK) return ret;

    // 2. Initialize I2S speaker
    ret = i2s_speaker_init();
    if (ret != ESP_OK) return ret;

    // 3. Write silence to flush I2S pipeline before enabling PA
    int16_t silence[256] = {};
    size_t bytes_written = 0;
    for (int i = 0; i < 4; i++) {
        i2s_channel_write(tx_handle, silence, sizeof(silence), &bytes_written, pdMS_TO_TICKS(100));
    }

    // 4. Create PA-off timer and continuous audio stream task
    pa_off_timer = xTimerCreate("pa_off", pdMS_TO_TICKS(PA_OFF_DELAY_MS),
                                pdFALSE, NULL, pa_off_timer_cb);
    tone_queue = xQueueCreate(8, sizeof(tone_cmd_t));
    xTaskCreate(tone_gen_task, "tone_gen", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "Speaker initialized (I2S port %d, %dHz)", I2S_PORT, SAMPLE_RATE);
    return ESP_OK;
}

esp_err_t sticks3_audio_play_tone(uint16_t freq_hz, uint32_t duration_ms, uint8_t volume) {
    if (!tone_queue) return ESP_ERR_INVALID_STATE;
    tone_cmd_t cmd = { .freq_hz = freq_hz, .volume = volume, .duration_ms = duration_ms };
    xQueueSend(tone_queue, &cmd, 0);
    return ESP_OK;
}

esp_err_t sticks3_audio_speaker_deinit(void) {
    // Stop timer
    if (pa_off_timer) {
        xTimerDelete(pa_off_timer, portMAX_DELAY);
        pa_off_timer = NULL;
    }

    // Disable I2S
    if (tx_handle) {
        i2s_channel_disable(tx_handle);
        i2s_del_channel(tx_handle);
        tx_handle = NULL;
    }

    // Disable PA
    sticks3_power_speaker_pa_disable();
    pa_is_on = false;
    return ESP_OK;
}
