/*
 * SPDX-FileCopyrightText: 2021-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "audio_pipeline.h"
#include "audio_event_iface.h"
#include "audio_common.h"
#include "fatfs_stream.h"
#include "i2s_stream.h"
#include "raw_stream.h"
#include "board.h"
#include "es8311.h"

static const char *TAG = "ADF_AUDIO_PLAYER";

/* Import music file as buffer */
extern const uint8_t music_pcm_start[] asm("_binary_canon_pcm_start");
extern const uint8_t music_pcm_end[]   asm("_binary_canon_pcm_end");

static audio_pipeline_handle_t pipeline;
static audio_element_handle_t raw_stream_reader, i2s_stream_writer;

static esp_err_t setup_audio_elements(void)
{
    // Initialize audio board
    ESP_RETURN_ON_ERROR(audio_board_init(), TAG, "Audio board initialization failed");
    
    // Create audio pipeline
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    pipeline = audio_pipeline_init(&pipeline_cfg);
    if (pipeline == NULL) {
        ESP_LOGE(TAG, "Failed to initialize audio pipeline");
        return ESP_FAIL;
    }

    // Create RAW stream reader for PCM data
    raw_stream_cfg_t raw_cfg = RAW_STREAM_CFG_DEFAULT();
    raw_cfg.type = AUDIO_STREAM_READER;
    raw_stream_reader = raw_stream_init(&raw_cfg);
    if (raw_stream_reader == NULL) {
        ESP_LOGE(TAG, "Failed to initialize RAW stream");
        return ESP_FAIL;
    }

    // Create I2S stream writer
    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
    i2s_cfg.type = AUDIO_STREAM_WRITER;
    i2s_cfg.i2s_port = BOARD_I2S_PORT;
    i2s_cfg.use_alc = false;
    i2s_cfg.volume = 60;
    
    // Get I2S configuration from board
    i2s_std_config_t board_i2s_cfg;
    ESP_RETURN_ON_ERROR(audio_board_get_i2s_cfg(&board_i2s_cfg), TAG, "Failed to get I2S config");
    memcpy(&i2s_cfg.std_cfg, &board_i2s_cfg, sizeof(i2s_std_config_t));
    
    i2s_stream_writer = i2s_stream_init(&i2s_cfg);
    if (i2s_stream_writer == NULL) {
        ESP_LOGE(TAG, "Failed to initialize I2S stream");
        return ESP_FAIL;
    }

    // Link elements: raw_stream -> i2s_stream
    ESP_RETURN_ON_ERROR(audio_pipeline_register(pipeline, raw_stream_reader, "raw"), TAG, "Register raw stream failed");
    ESP_RETURN_ON_ERROR(audio_pipeline_register(pipeline, i2s_stream_writer, "i2s"), TAG, "Register i2s stream failed");
    
    ESP_RETURN_ON_ERROR(audio_pipeline_link(pipeline, (const char *[]){"raw", "i2s"}, 2), 
                       TAG, "Pipeline link failed");

    return ESP_OK;
}

static void audio_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Starting audio playback task");
    
    // Enable power amplifier
    audio_board_pa_enable(true);
    
    // Setup audio elements
    if (setup_audio_elements() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to setup audio elements");
        vTaskDelete(NULL);
        return;
    }

    // Start pipeline
    ESP_ERROR_CHECK(audio_pipeline_run(pipeline));
    ESP_LOGI(TAG, "Audio pipeline started");

    // Play PCM data continuously
    while (1) {
        // Write PCM data to pipeline
        size_t bytes_written = 0;
        esp_err_t ret = raw_stream_write(raw_stream_reader, (char *)music_pcm_start, 
                                        music_pcm_end - music_pcm_start, &bytes_written);
        
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to write audio data: %s", esp_err_to_name(ret));
        } else {
            ESP_LOGI(TAG, "Wrote %d bytes of audio data", bytes_written);
        }
        
        vTaskDelay(pdMS_TO_TICKS(1000)); // Wait 1 second before next loop
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "ESP-ADF Audio Player Example Started");
    ESP_LOGI(TAG, "Board: ESP32-S3 SZP with ES8311 Codec");
    
    // Create audio playback task
    BaseType_t ret = xTaskCreate(audio_task, "audio_task", 8192, NULL, 5, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create audio task");
    }
}
