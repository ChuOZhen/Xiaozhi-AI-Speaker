/**
 * @file audio_manager.cc
 * @brief 🎧 音频管理器实现 - 内存优化版本
 * 
 * 核心优化：
 * 1. 录音和播放共享同一块内存（互斥使用）
 * 2. 支持边录音边发送（streaming），无需大缓冲
 * 3. 内存不足时自动降级（减小缓冲区）
 */

extern "C" {
#include <string.h>
#include "esp_log.h"
#include "bsp_board.h"
}

#include "audio_manager.h"

const char* AudioManager::TAG = "AudioManager";

// 最小缓冲区配置（降级时使用）
static const uint32_t MIN_RECORDING_SEC = 1;      // 最少1秒录音
static const uint32_t MIN_STREAMING_KB = 16;      // 最少16KB流式缓冲

AudioManager::AudioManager(uint32_t sample_rate, 
                           uint32_t max_recording_sec,
                           uint32_t streaming_buffer_kb)
    : sample_rate(sample_rate)
    , max_recording_sec(max_recording_sec)
    , streaming_buffer_kb(streaming_buffer_kb)
    , shared_buffer(nullptr)
    , shared_buffer_samples(0)
    , buffer_mode(BUFFER_IDLE)
    , recording_length(0)
    , is_recording(false)
    , streaming_mode(false)
    , stream_callback(nullptr)
    , stream_user_ctx(nullptr)
    , response_played(false)
    , is_streaming(false)
    , streaming_buffer(nullptr)
    , streaming_buffer_size(streaming_buffer_kb * 1024)
    , streaming_write_pos(0)
    , streaming_read_pos(0)
    , player_task_handle(nullptr)
    , is_finishing(false)
{
}

AudioManager::~AudioManager() {
    deinit();
}

esp_err_t AudioManager::allocateBuffers() {
    ESP_LOGI(TAG, "分配音频缓冲区...");
    
    // 计算共享缓冲区大小（用于录音或播放）
    shared_buffer_samples = sample_rate * max_recording_sec;
    size_t shared_buffer_bytes = shared_buffer_samples * sizeof(int16_t);
    
    // 尝试分配共享缓冲区
    shared_buffer = (int16_t*)malloc(shared_buffer_bytes);
    
    if (shared_buffer == nullptr) {
        // 降级：减小录音缓冲区
        ESP_LOGW(TAG, "内存不足，尝试减小录音缓冲区...");
        max_recording_sec = MIN_RECORDING_SEC;
        shared_buffer_samples = sample_rate * max_recording_sec;
        shared_buffer_bytes = shared_buffer_samples * sizeof(int16_t);
        
        shared_buffer = (int16_t*)malloc(shared_buffer_bytes);
        if (shared_buffer == nullptr) {
            ESP_LOGE(TAG, "无法分配共享缓冲区（%zu字节）", shared_buffer_bytes);
            return ESP_ERR_NO_MEM;
        }
    }
    
    ESP_LOGI(TAG, "✓ 共享缓冲区: %zu 样本 (%.1f秒, %zu字节)", 
             shared_buffer_samples, 
             (float)shared_buffer_samples / sample_rate,
             shared_buffer_bytes);
    
    // 分配流式播放缓冲区（优先 PSRAM）
    streaming_buffer_size = streaming_buffer_kb * 1024;
    streaming_buffer = (uint8_t*)heap_caps_malloc(
        streaming_buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (streaming_buffer == nullptr) {
        streaming_buffer = (uint8_t*)malloc(streaming_buffer_size);
        ESP_LOGW(TAG, "PSRAM 不足，回退内部 RAM");
    }

    if (streaming_buffer == nullptr) {
        // 降级：减小流式缓冲区
        ESP_LOGW(TAG, "内存不足，尝试减小流式缓冲区...");
        streaming_buffer_kb = MIN_STREAMING_KB;
        streaming_buffer_size = streaming_buffer_kb * 1024;
        streaming_buffer = (uint8_t*)heap_caps_malloc(
            streaming_buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (streaming_buffer == nullptr) {
            streaming_buffer = (uint8_t*)malloc(streaming_buffer_size);
        }

        if (streaming_buffer == nullptr) {
            ESP_LOGE(TAG, "无法分配流式缓冲区（%zu字节）", streaming_buffer_size);
            free(shared_buffer);
            shared_buffer = nullptr;
            return ESP_ERR_NO_MEM;
        }
    }
    
    ESP_LOGI(TAG, "✓ 流式缓冲区: %zu KB", streaming_buffer_kb);
    
    return ESP_OK;
}

void AudioManager::releaseBuffers() {
    if (shared_buffer != nullptr) {
        free(shared_buffer);
        shared_buffer = nullptr;
    }
    if (streaming_buffer != nullptr) {
        free(streaming_buffer);
        streaming_buffer = nullptr;
    }
}

esp_err_t AudioManager::init() {
    ESP_LOGI(TAG, "初始化音频管理器（内存优化版）...");
    
    esp_err_t ret = allocateBuffers();
    if (ret != ESP_OK) {
        return ret;
    }
    
    buffer_mode = BUFFER_IDLE;
    
    // 创建播放任务
    xTaskCreatePinnedToCore(player_task, "audio_player", 8192, this, 5, &player_task_handle, 1);
    
    ESP_LOGI(TAG, "音频管理器初始化完成");
    return ESP_OK;
}

void AudioManager::deinit() {
    if (player_task_handle != nullptr) {
        vTaskDelete(player_task_handle);
        player_task_handle = nullptr;
    }
    releaseBuffers();
}

bool AudioManager::ensureBufferMode(BufferMode required_mode) {
    if (buffer_mode == required_mode) {
        return true;
    }
    
    if (buffer_mode != BUFFER_IDLE) {
        ESP_LOGW(TAG, "缓冲区忙，当前模式: %d，请求模式: %d", buffer_mode, required_mode);
        return false;
    }
    
    buffer_mode = required_mode;
    return true;
}

// 🎙️ ========== 录音功能实现 ==========

void AudioManager::startRecording(bool streaming_mode, 
                                   audio_data_callback_t callback,
                                   void* user_ctx) {
    if (!ensureBufferMode(BUFFER_RECORDING)) {
        ESP_LOGE(TAG, "无法开始录音：缓冲区忙");
        return;
    }
    
    this->streaming_mode = streaming_mode;
    this->stream_callback = callback;
    this->stream_user_ctx = user_ctx;
    
    is_recording = true;
    recording_length = 0;
    
    if (streaming_mode) {
        ESP_LOGI(TAG, "开始录音（流式模式）...");
    } else {
        ESP_LOGI(TAG, "开始录音（缓冲模式，最大%.1f秒）...", 
                 (float)shared_buffer_samples / sample_rate);
    }
}

void AudioManager::stopRecording() {
    if (!is_recording) {
        return;
    }
    
    is_recording = false;
    ESP_LOGI(TAG, "停止录音，时长: %.2f秒 (%zu样本)", 
             getRecordingDuration(), recording_length);
    
    // 流式模式下，数据已实时发送，直接释放缓冲区
    if (streaming_mode) {
        buffer_mode = BUFFER_IDLE;
        streaming_mode = false;
        stream_callback = nullptr;
        ESP_LOGD(TAG, "流式录音结束，缓冲区已释放");
    }
    // 非流式模式：保持 BUFFER_RECORDING 模式，直到 clearRecordingBuffer() 被调用
}

bool AudioManager::addRecordingData(const int16_t* data, size_t samples) {
    if (!is_recording) {
        return false;
    }
    
    // 流式模式：直接通过回调发送
    if (streaming_mode && stream_callback != nullptr) {
        stream_callback(data, samples, stream_user_ctx);
        recording_length += samples;
        return true;
    }
    
    // 缓冲模式：存入共享缓冲区
    if (recording_length + samples > shared_buffer_samples) {
        ESP_LOGW(TAG, "录音缓冲区已满");
        return false;
    }
    
    memcpy(&shared_buffer[recording_length], data, samples * sizeof(int16_t));
    recording_length += samples;
    return true;
}

const int16_t* AudioManager::getRecordingBuffer(size_t& length) const {
    length = recording_length;
    return shared_buffer;
}

void AudioManager::clearRecordingBuffer() {
    recording_length = 0;
    if (buffer_mode == BUFFER_RECORDING) {
        buffer_mode = BUFFER_IDLE;
    }
}

void AudioManager::resetBufferState() {
    ESP_LOGI(TAG, "强制重置缓冲区状态（当前模式: %d -> IDLE）", buffer_mode);
    
    // 重置所有录音相关状态
    is_recording = false;
    recording_length = 0;
    streaming_mode = false;
    stream_callback = nullptr;
    
    // 重置缓冲区模式为 IDLE
    buffer_mode = BUFFER_IDLE;
    
    ESP_LOGD(TAG, "缓冲区状态已重置，准备接收AI回复");
}

float AudioManager::getRecordingDuration() const {
    return (float)recording_length / sample_rate;
}

bool AudioManager::isRecordingBufferFull() const {
    return recording_length >= shared_buffer_samples;
}

// 🔊 ========== 音频播放功能实现 ==========

esp_err_t AudioManager::playAudio(const uint8_t* audio_data, size_t data_len, const char* description) {
    if (!ensureBufferMode(BUFFER_PLAYING)) {
        ESP_LOGW(TAG, "无法播放：缓冲区忙");
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "播放%s...", description);
    esp_err_t ret = bsp_play_audio(audio_data, data_len);
    
    buffer_mode = BUFFER_IDLE;  // 释放缓冲区
    
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "✓ %s播放完成", description);
    } else {
        ESP_LOGE(TAG, "%s播放失败: %s", description, esp_err_to_name(ret));
    }
    return ret;
}

// 🌊 ========== 流式播放功能实现 ==========

void AudioManager::startStreamingPlayback() {
    if (!ensureBufferMode(BUFFER_PLAYING)) {
        ESP_LOGW(TAG, "无法开始流式播放：缓冲区忙");
        return;
    }
    
    // 【关键修复】重新启用I2S和功放（如果之前被关闭了）
    bsp_audio_resume();
    
    ESP_LOGI(TAG, "开始流式音频播放");
    is_streaming = true;
    streaming_write_pos = 0;
    streaming_read_pos = 0;
    response_played = false;
    is_finishing = false;
    
    if (streaming_buffer) {
        memset(streaming_buffer, 0, streaming_buffer_size);
    }
}

bool AudioManager::addStreamingAudioChunk(const uint8_t* data, size_t size) {
    if (!is_streaming || !streaming_buffer || !data) {
        return false;
    }
    
    // 计算环形缓冲区的剩余空间
    size_t available_space;
    if (streaming_write_pos >= streaming_read_pos) {
        available_space = streaming_buffer_size - (streaming_write_pos - streaming_read_pos) - 1;
    } else {
        available_space = streaming_read_pos - streaming_write_pos - 1;
    }
    
    if (size > available_space) {
        ESP_LOGW(TAG, "流式缓冲区满: 需要%zu, 可用%zu", size, available_space);
        return false;
    }
    
    // 写入环形缓冲区
    size_t bytes_to_end = streaming_buffer_size - streaming_write_pos;
    if (size <= bytes_to_end) {
        memcpy(streaming_buffer + streaming_write_pos, data, size);
        streaming_write_pos += size;
    } else {
        memcpy(streaming_buffer + streaming_write_pos, data, bytes_to_end);
        memcpy(streaming_buffer, data + bytes_to_end, size - bytes_to_end);
        streaming_write_pos = size - bytes_to_end;
    }
    
    if (streaming_write_pos >= streaming_buffer_size) {
        streaming_write_pos = 0;
    }
    
    return true;
}

void AudioManager::finishStreamingPlayback() {
    if (!is_streaming) {
        return;
    }
    
    ESP_LOGI(TAG, "标记流式播放结束");
    is_finishing = true;
}

void AudioManager::player_task(void* pvParameters) {
    AudioManager* manager = (AudioManager*)pvParameters;
    uint8_t* chunk = (uint8_t*)malloc(STREAMING_CHUNK_SIZE);
    if (chunk == nullptr) {
        ESP_LOGE(TAG, "播放任务缓冲区分配失败");
        vTaskDelete(NULL);
        return;
    }
    
    while (1) {
        if (!manager->is_streaming) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        
        // 计算可读数据量
        size_t available_data;
        if (manager->streaming_write_pos >= manager->streaming_read_pos) {
            available_data = manager->streaming_write_pos - manager->streaming_read_pos;
        } else {
            available_data = manager->streaming_buffer_size - manager->streaming_read_pos + manager->streaming_write_pos;
        }
        
        if (available_data >= STREAMING_CHUNK_SIZE) {
            // 读取并播放一个块
            size_t bytes_to_end = manager->streaming_buffer_size - manager->streaming_read_pos;
            if (STREAMING_CHUNK_SIZE <= bytes_to_end) {
                memcpy(chunk, manager->streaming_buffer + manager->streaming_read_pos, STREAMING_CHUNK_SIZE);
                manager->streaming_read_pos += STREAMING_CHUNK_SIZE;
            } else {
                memcpy(chunk, manager->streaming_buffer + manager->streaming_read_pos, bytes_to_end);
                memcpy(chunk + bytes_to_end, manager->streaming_buffer, STREAMING_CHUNK_SIZE - bytes_to_end);
                manager->streaming_read_pos = STREAMING_CHUNK_SIZE - bytes_to_end;
            }
            
            if (manager->streaming_read_pos >= manager->streaming_buffer_size) {
                manager->streaming_read_pos = 0;
            }
            
            // 播放
            esp_err_t ret = bsp_play_audio_stream(chunk, STREAMING_CHUNK_SIZE);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "流式播放失败: %s", esp_err_to_name(ret));
            }
            
        } else if (manager->is_finishing && available_data > 0) {
            // 播放剩余数据
            if (manager->streaming_write_pos >= manager->streaming_read_pos) {
                memcpy(chunk, manager->streaming_buffer + manager->streaming_read_pos, available_data);
            } else {
                size_t bytes_to_end = manager->streaming_buffer_size - manager->streaming_read_pos;
                memcpy(chunk, manager->streaming_buffer + manager->streaming_read_pos, bytes_to_end);
                memcpy(chunk + bytes_to_end, manager->streaming_buffer, available_data - bytes_to_end);
            }
            
            bsp_play_audio_stream(chunk, available_data);
            
            // 清理状态
            manager->streaming_read_pos = 0;
            manager->streaming_write_pos = 0;
            manager->is_finishing = false;
            manager->is_streaming = false;
            manager->buffer_mode = BUFFER_IDLE;
            manager->response_played = true;
            bsp_audio_stop();
            ESP_LOGI(TAG, "流式播放完成");
            
        } else if (manager->is_finishing && available_data == 0) {
            // 无剩余数据
            manager->is_finishing = false;
            manager->is_streaming = false;
            manager->buffer_mode = BUFFER_IDLE;
            manager->response_played = true;
            bsp_audio_stop();
            ESP_LOGI(TAG, "流式播放完成 (无剩余数据)");
            
        } else {
            // 等待更多数据
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    
    free(chunk);
}
