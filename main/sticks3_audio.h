#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize ES8311 audio codec and I2S for speaker playback
 *        - Enables PA via M5PM1 GPIO3
 *        - Configures ES8311 over I2C (address 0x18)
 *        - Initializes I2S port 0 for speaker output
 */
esp_err_t sticks3_audio_speaker_init(void);

/**
 * @brief Play a simple tone (blocking)
 * @param freq_hz  Tone frequency in Hz (e.g. 1000 for 1kHz)
 * @param duration_ms  Duration in milliseconds
 * @param volume  Volume level 0-100
 */
esp_err_t sticks3_audio_play_tone(uint16_t freq_hz, uint32_t duration_ms, uint8_t volume);

/**
 * @brief Disable speaker (power down PA and codec)
 */
esp_err_t sticks3_audio_speaker_deinit(void);

#ifdef __cplusplus
}
#endif
