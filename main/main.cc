/**
  * @file main.cc
  * @brief ESP32-S3 智能语音助手 - 连续对话版
  * 
  * 功能：
  * 1. 唤醒后连续监听，支持多轮对话
  * 2. 10秒无声音自动发送 byebye
  * 3. 回复播放完毕后自动进入下一轮监听
  * 4. 使用 VAD 检测用户语音活动
  * 
  * 触发方式：
  * - 当前：GPIO0 按钮（或 VAD 自动触发）
  * - TODO：替换为 "你好小智" 唤醒词（需解决 hufzip bug）
  */

extern "C"
{
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>  // 【优化】数学函数（sinf, sqrt）
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_afe_config.h"
#include "model_path.h"
#include "bsp_board.h"
#include "esp_log.h"
#include "esp_log_buffer.h"  // 【优化】十六进制日志输出
#include "driver/gpio.h"
#include "nvs_flash.h"
#include "mbedtls/base64.h"  // TTS base64 解码

// 【恢复】本地音频反馈（唤醒成功提示音）
#include "mock_voices/hi.h"     // "你好" 欢迎音频
#include "mock_voices/ok.h"     // "收到" 确认音频  
#include "mock_voices/bye.h"    // "再见" 结束音频
}

#include "audio_manager.h"
#include "wifi_manager.h"
#include "websocket_client.h"

static const char *TAG = "语音助手";

// WebSocket服务器配置 - 本地 Python 代理服务
#define WS_URI "ws://10.147.162.38:8765/ws/esp32"

// 音频参数
#define SAMPLE_RATE 16000

// GPIO按钮（临时用于测试，后续替换为唤醒词）
#define BUTTON_GPIO GPIO_NUM_0

// 超时配置
#define LISTEN_TIMEOUT_MS 15000      // 【修复】监听超时：15秒（给用户更多时间说话）
#define SILENCE_TIMEOUT_MS 3000      // 【修复】静音超时：3秒（用户说完话后3秒无声音才结束，避免录音太短）
#define POST_PLAY_SILENCE_MS 500     // 播放完后等待500ms再开始监听
#define BUTTON_LONG_PRESS_MS 2000    // 按钮长按：2秒结束对话

// 状态机
enum SystemState {
    STATE_SLEEP = 0,            // 睡眠等待唤醒（播放唤醒词后进入此状态）
    STATE_LISTENING = 1,        // 监听用户说话
    STATE_RECORDING = 2,        // 正在录音（检测到声音后）
    STATE_WAITING_RESPONSE = 3, // 等待AI回复
    STATE_PLAYING = 4,          // 播放AI回复
    STATE_CONTINUOUS_LISTEN = 5 // 连续监听模式（播放完后进入）
};

// 全局对象
static WiFiManager* wifi_manager = nullptr;
static WebSocketClient* websocket_client = nullptr;
static AudioManager* audio_manager = nullptr;
// AFE (Audio Front End) 对象 - 整合 WakeNet + VAD + AGC
static esp_afe_sr_iface_t *afe_handle = nullptr;
static esp_afe_sr_data_t *afe_data = nullptr;
static srmodel_list_t *model_list = nullptr;
static int afe_feed_chunksize = 0;
static volatile SystemState current_state = STATE_SLEEP;

// 录音统计
static size_t total_recorded_samples = 0;
static bool vad_speech_detected = false;
static uint32_t silence_start_time = 0;
static uint32_t listen_start_time = 0;

// WebSocket 发送互斥（文本用）
static SemaphoreHandle_t ws_send_mutex = nullptr;

// ── 生产者-消费者发送队列 ──────────────────────────────
// 录音任务(生产者) → 队列 → 发送任务(消费者) 严格 45ms/块
#define AUDIO_CHUNK_SAMPLES 800    // 50ms @ 16kHz（缩小单次发送压力）
#define AUDIO_CHUNK_BYTES   1600   // 800 samples * 2 bytes
#define SEND_QUEUE_SIZE     8      // 最多缓冲 8 块 (400ms)

static QueueHandle_t audio_send_queue = nullptr;
static TaskHandle_t audio_send_task_handle = nullptr;
static int16_t accum_buffer[AUDIO_CHUNK_SAMPLES];   // 累积缓冲区
static size_t accum_samples = 0;
static volatile bool send_task_running = false;

// 【优化】熔断机制 - 放宽到20次，避免误判
static int ws_fail_count = 0;
static uint32_t recording_start_time = 0;
#define CIRCUIT_BREAKER_THRESHOLD 20  // 【放宽】20次失败才熔断

// 【新增】初始发送保护期（前1秒不触发熔断）
#define INITIAL_GRACE_PERIOD_MS 1000
static volatile bool emergency_stop = false;

// AI 响应超时控制
static uint32_t ai_thinking_start_time = 0;
static uint32_t last_binary_audio_time = 0;
static bool waiting_for_audio = false;
#define AI_RESPONSE_TIMEOUT_MS 25000
#define AUDIO_INACTIVITY_TIMEOUT_MS 5000

// 【修复】音频播放标志位（避免在WebSocket回调中直接播放导致看门狗超时）
static volatile bool need_play_welcome = false;     // 需要播放欢迎音
static volatile bool need_play_bye = false;         // 需要播放结束音
static volatile bool need_play_ok = false;          // 需要播放确认音

// TODO: 天气播报功能暂未启用（proxy_server.py 无对应事件），变量已禁用
// static volatile bool is_weather_broadcast = false;
// static int weather_audio_format = 1;

/**
 * @brief 【优化】计算音频缓冲区的RMS值和DC偏移
 * @return RMS值，用于检测静音或异常数据
 */
static float calculate_audio_rms(const int16_t* data, size_t samples) {
    if (samples == 0) return 0.0f;
    
    double sum = 0.0;
    double dc_offset = 0.0;
    
    // 计算DC偏移（平均值）
    for (size_t i = 0; i < samples; i++) {
        dc_offset += data[i];
    }
    dc_offset /= samples;
    
    // 计算RMS（去除DC偏移后）
    for (size_t i = 0; i < samples; i++) {
        double diff = data[i] - dc_offset;
        sum += diff * diff;
    }
    
    return sqrt(sum / samples);
}

/**
 * @brief 安全的 WebSocket 文本发送
 */
static void ws_send_text_safe(const char* text) {
    if (websocket_client && websocket_client->isConnected()) {
        xSemaphoreTake(ws_send_mutex, portMAX_DELAY);
        websocket_client->sendText(text);
        xSemaphoreGive(ws_send_mutex);
    }
}

/**
 * @brief 专用音频发送任务（消费者）
 *
 * 从队列取块，严格按 100ms/块 节奏发送。
 * 只要节奏不破，TCP 缓冲区永远不会满。
 * 收到 NULL 指针时退出。
 */
static void audio_send_task(void *arg) {
    (void)arg;
    uint8_t *chunk = nullptr;
    TickType_t last_send_tick = 0;
    int chunk_count = 0;

    ESP_LOGI(TAG, "📤 音频发送任务启动");

    while (1) {
        // 从队列取块（最多等 500ms）
        if (xQueueReceive(audio_send_queue, &chunk, pdMS_TO_TICKS(500)) != pdTRUE) {
            // 超时：检查是否该退出了
            if (!send_task_running) break;
            continue;
        }

        // NULL = 停止信号
        if (chunk == nullptr) break;

        // 严格节流：距上次发送不足 45ms 就等待
        TickType_t now_tick = xTaskGetTickCount();
        if (last_send_tick != 0) {
            int32_t elapsed = (now_tick - last_send_tick) * portTICK_PERIOD_MS;
            if (elapsed < 45) {
                vTaskDelay(pdMS_TO_TICKS(45 - elapsed));
            }
        }

        // 发送：此时 TCP 缓冲区一定有空闲（严格 100ms 一块 = 32KB/s）
        if (websocket_client && websocket_client->isConnected()) {
            xSemaphoreTake(ws_send_mutex, pdMS_TO_TICKS(200));
            int ret = websocket_client->sendBinary(chunk, AUDIO_CHUNK_BYTES);
            xSemaphoreGive(ws_send_mutex);

            if (ret > 0) {
                chunk_count++;
                ws_fail_count = 0;
                ESP_LOGD(TAG, "📤 块#%d 发送 %d 字节", chunk_count, ret);
            } else {
                ESP_LOGW(TAG, "⚠️ 块#%d 发送失败 (ret=%d)", chunk_count, ret);
                ws_fail_count++;
                if (ws_fail_count >= CIRCUIT_BREAKER_THRESHOLD) {
                    ESP_LOGE(TAG, "🔴 连续发送失败，熔断");
                    emergency_stop = true;
                    free(chunk);
                    break;
                }
            }
        } else {
            ESP_LOGW(TAG, "⚠️ WebSocket 已断开，丢弃队列中的块");
        }

        free(chunk);
        last_send_tick = xTaskGetTickCount();
    }

    // 清理队列中剩余块
    while (xQueueReceive(audio_send_queue, &chunk, 0) == pdTRUE) {
        if (chunk) free(chunk);
    }

    ESP_LOGI(TAG, "📤 音频发送任务结束 (共发送 %d 块)", chunk_count);
    send_task_running = false;
    vTaskDelete(nullptr);
}

/**
 * @brief 将累积缓冲区内容推入发送队列
 */
static void flush_accum_to_queue() {
    if (accum_samples == 0) return;

    uint8_t *chunk = (uint8_t*)malloc(accum_samples * sizeof(int16_t));
    if (!chunk) {
        ESP_LOGW(TAG, "⚠️ malloc 失败，丢弃 %zu 样本", accum_samples);
        accum_samples = 0;
        return;
    }
    memcpy(chunk, accum_buffer, accum_samples * sizeof(int16_t));

    if (xQueueSend(audio_send_queue, &chunk, 0) != pdTRUE) {
        ESP_LOGW(TAG, "⚠️ 发送队列满(%d)，丢弃 %zu 样本", SEND_QUEUE_SIZE, accum_samples);
        free(chunk);
    }
    accum_samples = 0;
}

/**
 * @brief 推送 AFE 数据：累积到块大小后入队
 */
static void push_afe_data(const int16_t* data, size_t samples) {
    total_recorded_samples += samples;

    if (emergency_stop) return;

    // RMS 静音过滤
    float rms = calculate_audio_rms(data, samples);
    if (rms < 50.0f) return;

    for (size_t i = 0; i < samples; i++) {
        accum_buffer[accum_samples++] = data[i];
        if (accum_samples >= AUDIO_CHUNK_SAMPLES) {
            flush_accum_to_queue();
        }
    }
}

/**
 * @brief 排空队列：停止录音后把剩余数据发完
 */
static void drain_send_queue() {
    // 先发送累积缓冲区中的剩余数据
    if (accum_samples > 0) {
        flush_accum_to_queue();
    }

    // 等发送任务把队列中所有块发完（最多等 3 秒）
    int wait_ms = 0;
    while (send_task_running && uxQueueMessagesWaiting(audio_send_queue) > 0 && wait_ms < 3000) {
        vTaskDelay(pdMS_TO_TICKS(50));
        wait_ms += 50;
    }
}

/**
 * @brief 【恢复】播放本地音频（PCM格式）
 * 
 * 用于播放唤醒成功提示音、确认音等
 * 
 * @param audio_data PCM音频数据（16kHz, 16bit, 单声道）
 * @param data_len 数据长度（字节）
 * @param description 音频描述
 */
static void play_local_audio(const uint8_t *audio_data, size_t data_len, const char *description) {
    // 【关键修复】检查 audio_manager 是否已初始化
    if (audio_manager == nullptr) {
        ESP_LOGW(TAG, "⚠️ 音频管理器未初始化，跳过播放");
        return;
    }
    
    ESP_LOGI(TAG, "🔊 播放%s...", description);
    
    // 重置缓冲区并播放
    audio_manager->resetBufferState();
    esp_err_t ret = bsp_play_audio(audio_data, data_len);
    
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "✅ %s播放完成", description);
    } else {
        ESP_LOGE(TAG, "❌ %s播放失败: %s", description, esp_err_to_name(ret));
    }
    
    // 播放完成后重置缓冲区
    audio_manager->resetBufferState();
}

/**
 * @brief 进入睡眠状态（等待唤醒词）
 */
static void enter_sleep_state() {
    ESP_LOGI(TAG, "😴 进入睡眠状态，等待唤醒...");
    current_state = STATE_SLEEP;
    // 【修复】检查 audio_manager 是否已初始化
    if (audio_manager != nullptr) {
        audio_manager->resetBufferState();
    }
    // TODO: 启动唤醒词检测
}

/**
 * @brief 发送录音数据并结束本轮对话
 */
static void finish_recording(bool send_byebye = false) {
    audio_manager->stopRecording();

    float duration = (float)total_recorded_samples / SAMPLE_RATE;
    ESP_LOGI(TAG, "⏹️ 录音结束，时长: %.2f秒", duration);

    // 排空发送队列（先发送剩余累积数据，等任务清空队列）
    drain_send_queue();

    // 发 NULL 停止发送任务
    send_task_running = false;
    uint8_t *stop_signal = nullptr;
    xQueueSend(audio_send_queue, &stop_signal, pdMS_TO_TICKS(100));

    // 等待发送任务退出
    int wait = 0;
    while (audio_send_task_handle && wait < 30) {
        eTaskState state = eTaskGetState(audio_send_task_handle);
        if (state == eDeleted || state == eInvalid) break;
        vTaskDelay(pdMS_TO_TICKS(100));
        wait++;
    }
    audio_send_task_handle = nullptr;

    // 等 TCP 缓冲区排空最后一块音频（避免 recording_ended JSON 写时溢出）
    vTaskDelay(pdMS_TO_TICKS(200));

    // 先发事件
    if (send_byebye || duration < 0.3f) {
        ESP_LOGI(TAG, "👋 用户无回应，发送 byebye");
        ws_send_text_safe("{\"event\":\"byebye\"}");
        need_play_bye = true;
    } else {
        ws_send_text_safe("{\"event\":\"recording_ended\"}");
    }

    if (send_byebye || duration < 0.3f) {
        enter_sleep_state();
    } else {
        current_state = STATE_WAITING_RESPONSE;
        ESP_LOGI(TAG, "⏳ 等待AI回复...");
    }
}

/**
 * @brief 开始监听用户说话（连续对话模式）
 * @param from_wakeup 是否从唤醒状态进入（true=播放"你好"，false=不播放）
 */
static void start_listening(bool from_wakeup = false) {
    ESP_LOGI(TAG, "👂 开始监听用户说话（10秒超时）...");
    
    // 【修复】只有从唤醒状态进入时才播放"你好"
    if (from_wakeup) {
        need_play_welcome = true;
    }
    
    // 【修复】重置所有状态
    total_recorded_samples = 0;
    vad_speech_detected = false;
    silence_start_time = 0;
    listen_start_time = esp_timer_get_time() / 1000;
    accum_samples = 0;  // 清空累积缓冲区
    ws_fail_count = 0;  // 重置熔断计数
    emergency_stop = false;  // 清除熔断标志

    // 确保缓冲区空闲
    audio_manager->resetBufferState();
    
    current_state = STATE_LISTENING;
}

/**
 * @brief 检测到声音，开始录音
 */
static void start_recording_on_voice() {
    ESP_LOGI(TAG, "🔴 检测到声音，开始录音...");

    // 先通知服务器开始录音
    ws_send_text_safe("{\"event\":\"recording_started\"}");

    // 重置状态
    ws_fail_count = 0;
    emergency_stop = false;
    recording_start_time = esp_timer_get_time() / 1000;
    accum_samples = 0;

    // 创建发送队列
    if (audio_send_queue == nullptr) {
        audio_send_queue = xQueueCreate(SEND_QUEUE_SIZE, sizeof(uint8_t*));
    } else {
        // 【修复】清空队列中可能残留的 NULL 停止信号
        uint8_t *stale;
        while (xQueueReceive(audio_send_queue, &stale, 0) == pdTRUE) {
            if (stale) free(stale);
        }
    }

    // 启动专用发送任务（优先级 3，栈 4KB）
    send_task_running = true;
    xTaskCreate(audio_send_task, "audio_tx", 4096, nullptr, 3, &audio_send_task_handle);

    // 启动流式录音（音频数据由主循环 AFE 路径推送，此回调为空）
    audio_manager->startRecording(true,
        [](const int16_t*, size_t, void*) {}, nullptr);

    current_state = STATE_RECORDING;
}

/**
 * @brief WebSocket事件处理函数
 */
static void on_websocket_event(const WebSocketClient::EventData& event) {
    switch (event.type) {
    case WebSocketClient::EventType::CONNECTED:
        ESP_LOGI(TAG, "WebSocket已连接");
        
        // 【优化】检查是否需要恢复之前的对话
        if (current_state == STATE_WAITING_RESPONSE) {
            ESP_LOGW(TAG, "🔄 重连成功，重新请求AI回复...");
            ws_send_text_safe("{\"event\":\"reconnect_resume\",\"last_state\":\"waiting_response\"}");
        } else if (current_state == STATE_RECORDING) {
            // 有未发送的录音数据
            ESP_LOGW(TAG, "🔄 重连成功，发送未完成的录音...");
            drain_send_queue();
            ws_send_text_safe("{\"event\":\"recording_ended\"}");
            current_state = STATE_WAITING_RESPONSE;
        } else {
            // 正常连接，发送就绪信号
            ws_send_text_safe("{\"event\":\"client_ready\",\"audio_format\":\"pcm\",\"sample_rate\":16000,\"channels\":1,\"bits\":16}");
        }
        break;
        
    case WebSocketClient::EventType::DISCONNECTED:
        ESP_LOGE(TAG, "WebSocket已断开");
        // 【修复】服务器断开时，停止所有操作并清空状态
        // 【关键修复】检查 audio_manager 是否已初始化
        if (audio_manager != nullptr) {
            if (audio_manager->isRecording()) {
                audio_manager->stopRecording();
            }
            if (audio_manager->isStreamingActive()) {
                audio_manager->finishStreamingPlayback();
            }
        }
        // 【修复】清空缓冲区，准备重新开始
        accum_samples = 0;
        emergency_stop = false;
        ws_fail_count = 0;
        // 【修复】服务器错误时，回到睡眠状态等待重连
        if (current_state != STATE_SLEEP) {
            ESP_LOGW(TAG, "⚠️ 服务器断开，重置状态等待重连");
            current_state = STATE_SLEEP;
        }
        break;
        
    case WebSocketClient::EventType::DATA_BINARY:
    {
        // 🔍 调试：打印所有收到的二进制数据信息
        ESP_LOGI(TAG, "📦 收到二进制数据: %zu 字节, 当前状态: %d", event.data_len, current_state);
        
        // 更新最后收到音频的时间
        if (event.data_len > 0) {
            last_binary_audio_time = esp_timer_get_time() / 1000;
            waiting_for_audio = false;  // 收到音频，不再等待
            
            // 打印前4字节（用于调试格式）
            ESP_LOGD(TAG, "  数据头: %02X %02X %02X %02X", 
                     event.data[0], event.data[1], event.data[2], event.data[3]);
        }
        
        // 检查是否为MP3格式
        if (event.data_len > 2 && event.data[0] == 0xFF && 
            (event.data[1] == 0xFB || event.data[1] == 0xF3 || event.data[1] == 0xFA)) {
            ESP_LOGE(TAG, "❌ 收到MP3格式数据！请服务器发送PCM原始数据");
            ws_send_text_safe("{\"event\":\"error\",\"message\":\"Unsupported format: MP3\"}");
            break;
        }
        
        // 允许接收音频的状态：等待回复、播放中、连续监听过渡期
        bool should_process = (audio_manager != nullptr && event.data_len > 0) &&
                              (current_state == STATE_WAITING_RESPONSE ||
                               current_state == STATE_PLAYING ||
                               current_state == STATE_CONTINUOUS_LISTEN);

        if (should_process) {
            // 确保已启动流式播放（如果没启动则自动启动）
            if (!audio_manager->isStreamingActive()) {
                ESP_LOGI(TAG, "▶️ 自动启动流式播放...");
                audio_manager->startStreamingPlayback();
                // 若当前不在播放状态，切回播放状态以正确接收后续数据
                if (current_state != STATE_PLAYING && current_state != STATE_WAITING_RESPONSE) {
                    current_state = STATE_PLAYING;
                }
            }

            // 添加音频数据到播放队列
            bool added = audio_manager->addStreamingAudioChunk(event.data, event.data_len);
            if (!added) {
                ESP_LOGW(TAG, "⚠️ 音频缓冲区满，丢包 %zu 字节", event.data_len);
            } else {
                ESP_LOGD(TAG, "✓ 音频数据已加入播放队列: %zu 字节", event.data_len);
            }
        } else if (audio_manager == nullptr) {
            ESP_LOGW(TAG, "⚠️ 收到音频但管理器未初始化");
        } else {
            ESP_LOGW(TAG, "⚠️ 忽略音频数据（当前状态: %d 不是播放状态）", current_state);
        }
        break;
    }

    case WebSocketClient::EventType::DATA_TEXT:
        if (event.data && event.data_len > 0) {
            char *json_str = (char *)malloc(event.data_len + 1);
            if (json_str) {
                memcpy(json_str, event.data, event.data_len);
                json_str[event.data_len] = '\0';
                ESP_LOGI(TAG, "📨 收到: %s", json_str);
                
                // 收到 ai_thinking，开始计时等待音频
                if (strstr(json_str, "ai_thinking")) {
                    ESP_LOGI(TAG, "🤖 AI开始思考，启动响应计时器...");
                    ai_thinking_start_time = esp_timer_get_time() / 1000;
                    waiting_for_audio = true;
                    last_binary_audio_time = 0;  // 重置音频接收时间
                }
                // 回复结束信号
                else if (strstr(json_str, "response_finished") || strstr(json_str, "audio_end")) {
                    ESP_LOGI(TAG, "✅ AI回复结束");
                    waiting_for_audio = false;
                    // 【修复】检查 audio_manager 是否已初始化
                    if (audio_manager != nullptr && audio_manager->isStreamingActive()) {
                        audio_manager->finishStreamingPlayback();
                    }
                }
                // AI 主动结束对话（比如说了再见）
                else if (strstr(json_str, "conversation_ended")) {
                    ESP_LOGI(TAG, "👋 AI结束对话");
                    waiting_for_audio = false;

                    // 【修复】设置标志位，让主循环播放音频（避免在回调中阻塞）
                    need_play_bye = true;

                    enter_sleep_state();
                }
                // TTS 开始 - 准备播放语音回复
                else if (strstr(json_str, "tts_start")) {
                    ESP_LOGI(TAG, "🔊 TTS 语音合成开始...");
                    waiting_for_audio = false;

                    // 从 JSON 提取 AI 回复文本（用于日志）
                    const char* text_key = strstr(json_str, "\"text\":\"");
                    if (text_key) {
                        const char* text_start = text_key + 8;
                        const char* text_end = strchr(text_start, '"');
                        if (text_end) {
                            int text_len = text_end - text_start;
                            if (text_len > 100) text_len = 100;
                            ESP_LOGI(TAG, "💬 AI: %.*s", text_len, text_start);
                        }
                    }

                    current_state = STATE_PLAYING;
                    if (audio_manager != nullptr) {
                        audio_manager->resetResponsePlayedFlag();
                        audio_manager->resetBufferState();
                        audio_manager->startStreamingPlayback();
                    }
                }
                // TTS 播放结束
                else if (strstr(json_str, "tts_end")) {
                    ESP_LOGI(TAG, "✅ TTS 播放结束");
                    if (audio_manager != nullptr && audio_manager->isStreamingActive()) {
                        audio_manager->finishStreamingPlayback();
                    }
                    // 主循环 STATE_PLAYING 处理器会检测播放完成并转换状态
                }
                // TTS 失败
                else if (strstr(json_str, "tts_failed")) {
                    ESP_LOGE(TAG, "❌ TTS 语音合成失败");
                    waiting_for_audio = false;
                    if (audio_manager != nullptr && audio_manager->isStreamingActive()) {
                        audio_manager->finishStreamingPlayback();
                    }
                    current_state = STATE_SLEEP;
                }
                // 文件已保存（服务器确认收到音频）
                else if (strstr(json_str, "file_saved")) {
                    const char* fn_key = strstr(json_str, "\"filename\":\"");
                    if (fn_key) {
                        const char* fn_start = fn_key + 12;
                        const char* fn_end = strchr(fn_start, '"');
                        if (fn_end) {
                            ESP_LOGI(TAG, "📁 音频已保存: %.*s", (int)(fn_end - fn_start), fn_start);
                        }
                    }
                }
                // 服务端错误/超时
                else if (strstr(json_str, "error") || strstr(json_str, "TIMEOUT")) {
                    ESP_LOGE(TAG, "❌ 服务端报告错误: %s", json_str);
                    waiting_for_audio = false;
                    // 等待一段时间后重新开始监听
                    vTaskDelay(pdMS_TO_TICKS(1000));
                    if (current_state != STATE_SLEEP) {
                        start_listening(false);  // 【修复】连续对话，不播放"你好"
                    }
                }
                
                free(json_str);
            }
        }
        break;

    case WebSocketClient::EventType::ERROR:
        ESP_LOGE(TAG, "WebSocket错误");
        break;
        
    default:
        break;
    }
}

// --- 程序主入口 ---
extern "C" void app_main(void) {
    // NVS 初始化
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 创建 WebSocket 发送互斥锁
    ws_send_mutex = xSemaphoreCreateMutex();

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "🎙️ ESP32-S3 智能语音助手（连续对话版）");
    ESP_LOGI(TAG, "音频配置: 16kHz, 16-bit, 单声道, PCM");
    ESP_LOGI(TAG, "触发方式: GPIO0按钮（后续替换为唤醒词）");
    ESP_LOGI(TAG, "========================================");

    // WiFi 连接
    ESP_LOGI(TAG, "📶 连接WiFi...");
    wifi_manager = new WiFiManager(CONFIG_MY_WIFI_SSID, CONFIG_MY_WIFI_PASSWORD);
    if (wifi_manager->connect() != ESP_OK) {
        ESP_LOGE(TAG, "WiFi连接失败");
        return;
    }

    // WebSocket 连接
    ESP_LOGI(TAG, "🌐 连接WebSocket服务器...");
    websocket_client = new WebSocketClient(WS_URI, true, 5000);
    websocket_client->setEventCallback(on_websocket_event);
    if (websocket_client->connect() != ESP_OK) {
        ESP_LOGE(TAG, "WebSocket连接失败");
        return;
    }

    // 初始化麦克风
    ESP_LOGI(TAG, "🎤 初始化麦克风...");
    ret = bsp_board_init(SAMPLE_RATE, 1, 16);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "麦克风初始化失败: %s", esp_err_to_name(ret));
        return;
    }

    // 初始化播放
    ESP_LOGI(TAG, "🔊 初始化音频播放...");
    ret = bsp_audio_init(SAMPLE_RATE, 1, 16);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "音频播放初始化失败: %s", esp_err_to_name(ret));
        return;
    }

    // 初始化 AFE (Audio Front End) - 整合 WakeNet + VAD + AGC + NS
    ESP_LOGI(TAG, "🧠 初始化 ESP-SR AFE Pipeline...");

    // 从 SPIFFS 分区加载语音模型
    model_list = esp_srmodel_init("model");
    if (!model_list) {
        ESP_LOGE(TAG, "❌ 模型加载失败！请检查 model 分区");
        return;
    }
    ESP_LOGI(TAG, "✅ 加载了 %d 个模型", model_list->num);
    for (int i = 0; i < model_list->num; i++) {
        ESP_LOGI(TAG, "  模型[%d]: %s", i, model_list->model_name[i]);
    }

    // 创建 AFE 配置（单麦克风 "M" 模式，无回声参考通道）
    afe_config_t *afe_cfg = afe_config_init("M", model_list, AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
    if (!afe_cfg) {
        ESP_LOGE(TAG, "❌ AFE 配置创建失败");
        esp_srmodel_deinit(model_list);
        return;
    }

    // 配置 WakeNet 唤醒词
    afe_cfg->wakenet_init = true;
    char *wn_model = esp_srmodel_filter(model_list, ESP_WN_PREFIX, "nihaoxiaozhi");
    if (!wn_model) {
        ESP_LOGE(TAG, "❌ 未找到 '你好小智' 唤醒词模型！");
        afe_config_free(afe_cfg);
        esp_srmodel_deinit(model_list);
        return;
    }
    afe_cfg->wakenet_model_name = wn_model;
    afe_cfg->wakenet_mode = DET_MODE_90;
    ESP_LOGI(TAG, "✅ 唤醒词模型: %s", wn_model);

    // 配置 VAD
    afe_cfg->vad_init = true;
    afe_cfg->vad_mode = VAD_MODE_3;

    // 单麦识别场景：关闭 NS（降噪会降低识别精度，库自身也建议关闭）
    afe_cfg->ns_init = false;

    // 配置 AGC 自动增益
    afe_cfg->agc_init = true;
    afe_cfg->agc_mode = AFE_AGC_MODE_WEBRTC;

    // 单麦：关闭 AEC 和 SE
    afe_cfg->aec_init = false;
    afe_cfg->se_init = false;

    // 内存分配模式：优先内部 RAM，不足时用 PSRAM
    afe_cfg->memory_alloc_mode = AFE_MEMORY_ALLOC_INTERNAL_PSRAM_BALANCE;
    afe_cfg->afe_ringbuf_size = 50;
    afe_cfg->afe_linear_gain = 1.0f;

    // AFE 任务配置
    afe_cfg->afe_perferred_core = 1;
    afe_cfg->afe_perferred_priority = 5;

    // 检查并修正配置冲突
    afe_cfg = afe_config_check(afe_cfg);

    // 创建 AFE 实例
    afe_handle = esp_afe_handle_from_config(afe_cfg);
    if (!afe_handle) {
        ESP_LOGE(TAG, "❌ 获取 AFE 接口失败");
        afe_config_free(afe_cfg);
        esp_srmodel_deinit(model_list);
        return;
    }

    afe_data = afe_handle->create_from_config(afe_cfg);
    if (!afe_data) {
        ESP_LOGE(TAG, "❌ 创建 AFE 实例失败");
        afe_config_free(afe_cfg);
        esp_srmodel_deinit(model_list);
        return;
    }

    afe_feed_chunksize = afe_handle->get_feed_chunksize(afe_data);
    int fetch_chunksize = afe_handle->get_fetch_chunksize(afe_data);
    ESP_LOGI(TAG, "✅ AFE 初始化成功 (feed=%d, fetch=%d samples)",
             afe_feed_chunksize, fetch_chunksize);

    // 打印 AFE pipeline 信息
    afe_handle->print_pipeline(afe_data);

    // 释放配置（不再需要）
    afe_config_free(afe_cfg);

    // 初始化音频管理器
    ESP_LOGI(TAG, "🎛️ 初始化音频管理器...");
    audio_manager = new AudioManager(SAMPLE_RATE, 10, 64);  // 10秒录音缓冲，64KB流式缓冲(PSRAM)
    ret = audio_manager->init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "音频管理器初始化失败: %s", esp_err_to_name(ret));
        return;
    }

    // 配置 GPIO0 按钮
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << BUTTON_GPIO);
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io_conf);

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "✅ 系统就绪！说 '你好小智' 唤醒");
    ESP_LOGI(TAG, "流程：唤醒 → 监听10秒 → AI回复 → 继续监听");
    ESP_LOGI(TAG, "      （GPIO0按钮长按结束对话，短按备用唤醒）");
    ESP_LOGI(TAG, "========================================");

    // 分配音频采样缓冲区（使用 AFE 的 feed_chunksize）
    int16_t* audio_buffer = (int16_t*)malloc(afe_feed_chunksize * sizeof(int16_t));
    if (!audio_buffer) {
        ESP_LOGE(TAG, "音频缓冲区分配失败");
        return;
    }

    bool was_pressed = false;
    uint32_t play_finish_time = 0;
    uint32_t playing_start_time = 0;  // STATE_PLAYING 进入时间，用于超时检测
    current_state = STATE_SLEEP;

    // --- 主循环 ---
    while (1) {
        // ========== 1. 音频读取 + AFE feed ==========
        afe_fetch_result_t *afe_result = nullptr;

        // 只在需要时读取麦克风（播放时避免回环）
        bool should_capture = (current_state != STATE_PLAYING);

        if (should_capture) {
            ret = bsp_get_feed_data(false, audio_buffer, afe_feed_chunksize * sizeof(int16_t));
            if (ret == ESP_OK) {
                // Feed raw audio to AFE pipeline
                afe_handle->feed(afe_data, audio_buffer);
                // Fetch processed result (may return NULL if frame not ready)
                afe_result = afe_handle->fetch(afe_data);
                if (afe_result) {
                    // AFE frame ready, process in state machine below
                }
            }
        }

        // ========== 2. 按钮处理（保留长按结束对话 + 短按备用唤醒） ==========
        bool is_pressed = (gpio_get_level(BUTTON_GPIO) == 0);

        static uint32_t button_press_start_time = 0;
        static bool long_press_triggered = false;

        if (is_pressed && !was_pressed) {
            button_press_start_time = esp_timer_get_time() / 1000;
            long_press_triggered = false;
            ESP_LOGD(TAG, "🔘 按钮按下，开始计时...");
        }

        // 长按检测（2秒）- 在非睡眠状态下结束对话
        if (is_pressed && current_state != STATE_SLEEP) {
            uint32_t press_duration = (esp_timer_get_time() / 1000) - button_press_start_time;
            if (press_duration >= BUTTON_LONG_PRESS_MS && !long_press_triggered) {
                ESP_LOGI(TAG, "🔘 按钮长按2秒，结束对话");
                long_press_triggered = true;

                if (current_state == STATE_RECORDING) {
                    finish_recording(true);
                } else if (current_state == STATE_WAITING_RESPONSE ||
                           current_state == STATE_PLAYING ||
                           current_state == STATE_CONTINUOUS_LISTEN) {
                    ws_send_text_safe("{\"event\":\"byebye\"}");
                    need_play_bye = true;
                    enter_sleep_state();
                } else {
                    finish_recording(true);
                }
            }
        }

        // ========== 3. 音频播放标志位（带 AFE 保护） ==========
        if (need_play_welcome) {
            need_play_welcome = false;
            afe_handle->disable_wakenet(afe_data);
            afe_handle->disable_vad(afe_data);
            play_local_audio((const uint8_t *)hi, hi_len, "欢迎音频(你好)");
            afe_handle->enable_vad(afe_data);
            afe_handle->enable_wakenet(afe_data);
            afe_handle->reset_buffer(afe_data);
        }
        if (need_play_bye) {
            need_play_bye = false;
            afe_handle->disable_wakenet(afe_data);
            afe_handle->disable_vad(afe_data);
            play_local_audio((const uint8_t *)bye, bye_len, "结束音频(拜拜)");
            afe_handle->enable_vad(afe_data);
            afe_handle->enable_wakenet(afe_data);
            afe_handle->reset_buffer(afe_data);
        }
        if (need_play_ok) {
            need_play_ok = false;
            afe_handle->disable_wakenet(afe_data);
            afe_handle->disable_vad(afe_data);
            play_local_audio((const uint8_t *)ok, ok_len, "确认音频(收到)");
            afe_handle->enable_vad(afe_data);
            afe_handle->enable_wakenet(afe_data);
            afe_handle->reset_buffer(afe_data);
        }

        // ========== 4. 状态机 ==========

        // === STATE_SLEEP：等待唤醒词 ===
        if (current_state == STATE_SLEEP) {
            if (afe_result && afe_result->wakeup_state == WAKENET_DETECTED) {
                ESP_LOGI(TAG, "🔔 唤醒词检测到！你好小智");
                ws_send_text_safe("{\"event\":\"wakeup\",\"keyword\":\"nihaoxiaozhi\"}");
                start_listening(true);
            }
            // 保留按钮短按作为备用唤醒
            else if (is_pressed && !was_pressed) {
                ESP_LOGI(TAG, "🔔 按钮备用唤醒");
                ws_send_text_safe("{\"event\":\"wakeup\",\"keyword\":\"button\"}");
                start_listening(true);
            }
        }

        // === STATE_LISTENING：监听用户说话 ===
        else if (current_state == STATE_LISTENING) {
            if (afe_result && afe_result->data && afe_result->data_size > 0) {
                int16_t *proc_data = afe_result->data;
                int proc_samples = afe_result->data_size / sizeof(int16_t);

                // AFE VAD 状态 + 能量验证
                float rms = calculate_audio_rms(proc_data, proc_samples);

                if (afe_result->vad_state == VAD_SPEECH && rms > 500.0f) {
                    ESP_LOGI(TAG, "🗣️ AFE-VAD+能量检测通过，开始录音 (RMS=%.1f)", rms);
                    start_recording_on_voice();
                } else if (afe_result->vad_state == VAD_SPEECH) {
                    ESP_LOGD(TAG, "🤫 AFE-VAD触发但能量过低，忽略 (RMS=%.1f)", rms);
                }

                // 检查监听超时（15秒无声音）
                uint32_t elapsed = (esp_timer_get_time() / 1000) - listen_start_time;
                if (elapsed > LISTEN_TIMEOUT_MS) {
                    ESP_LOGI(TAG, "⏰ 监听超时");
                    finish_recording(true);
                }
            }
        }

        // === STATE_RECORDING：正在录音 ===
        else if (current_state == STATE_RECORDING) {
            if (!websocket_client || !websocket_client->isConnected()) {
                ESP_LOGW(TAG, "⚠️ 录音时服务器断开，停止录音");
                audio_manager->stopRecording();
                send_task_running = false;
                accum_samples = 0;
                current_state = STATE_SLEEP;
                continue;
            }

            if (afe_result && afe_result->data && afe_result->data_size > 0) {
                int16_t *proc_data = afe_result->data;
                int proc_samples = afe_result->data_size / sizeof(int16_t);

                // 累积 AFE 数据，满 1600 samples (100ms) 时推入发送队列
                push_afe_data(proc_data, proc_samples);

                // AFE VAD 静音检测
                if (afe_result->vad_state == VAD_SPEECH) {
                    silence_start_time = 0;
                    vad_speech_detected = true;
                } else {
                    if (silence_start_time == 0) {
                        silence_start_time = esp_timer_get_time() / 1000;
                    } else {
                        uint32_t silence_duration = (esp_timer_get_time() / 1000) - silence_start_time;
                        if (silence_duration > SILENCE_TIMEOUT_MS) {
                            ESP_LOGI(TAG, "🤫 检测到3秒静音，用户说完");
                            finish_recording(false);
                        }
                    }
                }

                // 检查总录音时长（最多15秒）
                float duration = (float)total_recorded_samples / SAMPLE_RATE;
                if (duration >= 15.0f) {
                    ESP_LOGI(TAG, "⏰ 录音达到15秒上限");
                    finish_recording(false);
                }
            }
        }
        
        // === 状态：等待回复 ===
        else if (current_state == STATE_WAITING_RESPONSE) {
            // 【修复】检查服务器连接状态，断开时停止等待
            if (!websocket_client || !websocket_client->isConnected()) {
                ESP_LOGW(TAG, "⚠️ 等待回复时服务器断开，回到睡眠状态");
                waiting_for_audio = false;
                ai_thinking_start_time = 0;
                current_state = STATE_SLEEP;
                continue;
            }
            
            // 检查AI响应超时
            if (waiting_for_audio && ai_thinking_start_time > 0) {
                uint32_t elapsed = (esp_timer_get_time() / 1000) - ai_thinking_start_time;
                
                // 25秒无音频数据，主动询问服务端状态
                if (elapsed > AI_RESPONSE_TIMEOUT_MS) {
                    ESP_LOGW(TAG, "⏰ AI响应超时 (%u秒)，询问服务端状态...", elapsed / 1000);
                    ws_send_text_safe("{\"event\":\"query_status\"}");
                    
                    // 再等待5秒，如果仍无响应则重新开始监听
                    if (elapsed > AI_RESPONSE_TIMEOUT_MS + 5000) {
                        ESP_LOGE(TAG, "❌ 服务端无响应，重新开始监听");
                        waiting_for_audio = false;
                        ai_thinking_start_time = 0;
                        start_listening(false);  // 连续对话，不播放"你好"
                    }
                }
                // 每5秒打印一次等待日志
                else if (elapsed % 5000 < 100) {
                    ESP_LOGI(TAG, "⏳ 等待AI音频... (%u秒)", elapsed / 1000);
                    ws_send_text_safe("{\"event\":\"query_status\",\"task_id\":\"unknown\"}");
                }
            }
            
            // 检查音频流中断（收到过音频但超过5秒没新数据）
            if (last_binary_audio_time > 0 && audio_manager->isStreamingActive()) {
                uint32_t audio_idle_time = (esp_timer_get_time() / 1000) - last_binary_audio_time;
                if (audio_idle_time > AUDIO_INACTIVITY_TIMEOUT_MS) {
                    ESP_LOGW(TAG, "⚠️ 音频流中断 %u秒，结束播放", audio_idle_time / 1000);
                    audio_manager->finishStreamingPlayback();
                }
            }
        }
        
        // === 状态：播放AI回复 ===
        else if (current_state == STATE_PLAYING) {
            // 用 response_played 标志判断播放完成（player_task 在 I2S 排空后置位）
            // 不能用 !isStreamingActive() —— 流式刚开始时 is_streaming 可能尚未置位
            if (audio_manager->isResponsePlayed()) {
                ESP_LOGI(TAG, "✅ AI回复播放完成");
                audio_manager->resetResponsePlayedFlag();
                playing_start_time = 0;
                play_finish_time = esp_timer_get_time() / 1000;
                current_state = STATE_CONTINUOUS_LISTEN;
            }
            // 超时保护：若流式播放从未启动，5秒后回退
            else if (!audio_manager->isStreamingActive()) {
                if (playing_start_time == 0) {
                    playing_start_time = esp_timer_get_time() / 1000;
                } else if ((esp_timer_get_time() / 1000) - playing_start_time > 5000) {
                    ESP_LOGW(TAG, "⚠️ 播放状态超时（5秒无音频流），回到睡眠");
                    playing_start_time = 0;
                    current_state = STATE_SLEEP;
                }
            } else {
                playing_start_time = 0;  // 流式已启动，重置超时
            }
        }
        
        // === 状态：连续监听准备 ===
        else if (current_state == STATE_CONTINUOUS_LISTEN) {
            // 等待500ms后开始下一轮监听
            uint32_t elapsed = (esp_timer_get_time() / 1000) - play_finish_time;
            if (elapsed > POST_PLAY_SILENCE_MS) {
                start_listening(false);  // 【修复】连续对话，不播放"你好"
            }
        }
        
        was_pressed = is_pressed;
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // 清理
    free(audio_buffer);
    if (afe_data && afe_handle) afe_handle->destroy(afe_data);
    if (model_list) esp_srmodel_deinit(model_list);
    if (ws_send_mutex) vSemaphoreDelete(ws_send_mutex);
    delete websocket_client;
    delete wifi_manager;
    delete audio_manager;
    vTaskDelete(NULL);
}
