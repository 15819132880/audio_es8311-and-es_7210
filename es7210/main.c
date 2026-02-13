#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_check.h"
#include "nvs_flash.h"

#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"

#include "lwip/sockets.h"
#include "lwip/inet.h"

#include "driver/i2c.h"
#include "driver/i2s_std.h"
#include "driver/i2s_tdm.h"

#include "es7210.h"

// =====================================================
static const char *TAG = "ES7210_FINAL";

// ================= WiFi =================
#define WIFI_SSID "1406"
#define WIFI_PASS "14061406"

#define SERVER_IP   "172.24.7.86"
#define SERVER_PORT 12345

static EventGroupHandle_t wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0

// ================= I2C =================
#define I2C_PORT    I2C_NUM_0
#define I2C_SDA     10
#define I2C_SCL     11

// ================= I2S =================
#define I2S_NUM     0
#define I2S_MCLK    9
#define I2S_BCLK    46
#define I2S_WS      3
#define I2S_DIN     20

// ================= Audio =================
#define SAMPLE_RATE     48000
#define RECORD_SEC      20
#define I2S_READ_BYTES  1024   // ⭐ 必须是 4 的倍数
#define EXAMPLE_I2S_SAMPLE_BITS I2S_DATA_BIT_WIDTH_16BIT
#define EXAMPLE_I2S_TDM_SLOT_MASK I2S_TDM_SLOT0 | I2S_TDM_SLOT1
static es7210_dev_handle_t es7210 = NULL;

// =====================================================
// WiFi
// =====================================================
static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    if (event_base == WIFI_EVENT &&
        event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == IP_EVENT &&
               event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init(void)
{
    wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi connecting...");
    xEventGroupWaitBits(wifi_event_group,
                        WIFI_CONNECTED_BIT,
                        false, true, portMAX_DELAY);
    ESP_LOGI(TAG, "WiFi connected");
}

// =====================================================
// I2C
// =====================================================
static void i2c_init(void)
{
    i2c_config_t cfg = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,
    };
    ESP_ERROR_CHECK(i2c_param_config(I2C_PORT, &cfg));
    ESP_ERROR_CHECK(i2c_driver_install(I2C_PORT, cfg.mode, 0, 0, 0));
}

// =====================================================
// ES7210
// =====================================================
static void es7210_init(void)
{
    es7210_i2c_config_t i2c_cfg = {
        .i2c_port = I2C_PORT,
        .i2c_addr = 0x40,
    };

    ESP_ERROR_CHECK(es7210_new_codec(&i2c_cfg, &es7210));

    es7210_codec_config_t cfg = {
        .i2s_format = ES7210_I2S_FMT_LJ,   // ⭐ ES7210 默认
        .sample_rate_hz = SAMPLE_RATE,
        .mclk_ratio = 256,
        .bit_width = ES7210_I2S_BITS_16B,
        .mic_bias = ES7210_MIC_BIAS_2V87,
        .mic_gain = ES7210_MIC_GAIN_24DB,   // 增加麦克风增益
        .flags.tdm_enable = true,          // 启用TDM模式以支持双麦克风
    };

    ESP_ERROR_CHECK(es7210_config_codec(es7210, &cfg));
    ESP_ERROR_CHECK(es7210_config_volume(es7210, 16));  // 增加ADC音量

    ESP_LOGI(TAG, "ES7210 init OK with TDM enabled");
}

// =====================================================
// I2S (Modified to TDM Mode to match your working config)
// =====================================================
static i2s_chan_handle_t i2s_init(void)
{
    i2s_chan_handle_t rx;

    // 1. 配置通道 (使用默认配置，自动选择端口)
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &rx));

    // 2. 配置 TDM 参数
    i2s_tdm_config_t tdm_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = {
            .data_bit_width = EXAMPLE_I2S_SAMPLE_BITS,
            .slot_bit_width = EXAMPLE_I2S_SAMPLE_BITS,
            .slot_mode = I2S_SLOT_MODE_STEREO,
            .slot_mask = EXAMPLE_I2S_TDM_SLOT_MASK,      // 正确设置slot掩码
            .ws_width = 16,                             // 与数据宽度匹配
            .bit_shift = false,                         // ES7210使用左对齐
        },
        .gpio_cfg = {
            .mclk = I2S_MCLK,
            .bclk = I2S_BCLK,
            .ws   = I2S_WS,
            .dout = -1, // ESP32 作为接收端，DOUT 不用，设为 -1 或特定 GPIO
            .din  = I2S_DIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false
            }
        },
    };

    // 3. 初始化通道为 TDM 模式（一次性完成配置）
    ESP_ERROR_CHECK(i2s_channel_init_tdm_mode(rx, &tdm_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(rx));

    ESP_LOGI(TAG, "I2S TDM mode initialized with dual mic support");
    return rx;
}
// =====================================================
// main
// =====================================================
void app_main(void)
{
    wifi_init();
    i2c_init();
    es7210_init();

    i2s_chan_handle_t rx = i2s_init();

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in dest = {
        .sin_family = AF_INET,
        .sin_port = htons(SERVER_PORT),
    };
    inet_pton(AF_INET, SERVER_IP, &dest.sin_addr);

    static int16_t i2s_buf[I2S_READ_BYTES / 2];
    static int16_t mono_buf[I2S_READ_BYTES / 4];

    uint32_t sent = 0;
    uint32_t target = SAMPLE_RATE * 2 * RECORD_SEC;

    ESP_LOGI(TAG, "Start recording...");

    // 添加一个短暂延迟以确保系统稳定
    vTaskDelay(pdMS_TO_TICKS(100));

    while (sent < target) {
        size_t rlen = 0;

        ESP_ERROR_CHECK(
            i2s_channel_read(rx,
                             i2s_buf,
                             I2S_READ_BYTES,
                             &rlen,
                             portMAX_DELAY));

        // 添加调试信息来检测是否有音频数据
        if (rlen > 0) {
            int frames = rlen / 4;   // ⭐ stereo frame

            // 计算音频数据的统计信息
            int16_t min_val = 32767, max_val = -32768;
            int32_t sum_abs = 0;  // 用于计算平均绝对值
            for (int i = 0; i < frames * 2; i++) {  // 乘以2是因为立体声有两个通道
                if (i2s_buf[i] < min_val) min_val = i2s_buf[i];
                if (i2s_buf[i] > max_val) max_val = i2s_buf[i];
                
                // 累加绝对值以计算平均幅度
                sum_abs += (i2s_buf[i] >= 0) ? i2s_buf[i] : -i2s_buf[i];
            }
            
            int avg_amplitude = sum_abs / (frames * 2);

            // 每隔一定时间打印一次音频数据统计
            if ((sent % (SAMPLE_RATE * 2)) == 0) {  // 每秒打印一次
                ESP_LOGI(TAG, "Audio stats: frames=%d, min=%d, max=%d, range=%d, avg_amp=%d", 
                         frames, min_val, max_val, max_val - min_val, avg_amplitude);
            }

            // 从两个通道中提取音频数据（假设双麦克风TDM模式）
            for (int i = 0; i < frames; i++) {
                // 使用左右声道的平均值，或者可以选择其中一个声道
                mono_buf[i] = (i2s_buf[i * 2] + i2s_buf[i * 2 + 1]) / 2;  // 左右声道平均
            }

            int send_bytes = frames * sizeof(int16_t);

            int result = sendto(sock,
                   mono_buf,
                   send_bytes,
                   0,
                   (struct sockaddr *)&dest,
                   sizeof(dest));
                   
            if (result < 0) {
                ESP_LOGE(TAG, "Failed to send UDP packet: errno %d", errno);
            }

            sent += send_bytes;
        }
    }

    sendto(sock, "END_OF_AUDIO", 12, 0,
           (struct sockaddr *)&dest, sizeof(dest));

    close(sock);
    ESP_LOGI(TAG, "DONE");
}



