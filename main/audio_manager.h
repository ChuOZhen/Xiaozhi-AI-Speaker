/**
 * @file audio_manager.h
 * @brief 🎧 音频管理器类 - 内存优化版本
 * 
 * 优化策略：
 * 1. 内存复用：录音和播放互斥使用同一块缓冲区
 * 2. 流式处理：支持边录音边发送，无需大缓冲
 * 3. 自动降级：内存不足时自动减小缓冲区大小
 */

#ifndef AUDIO_MANAGER_H
#define AUDIO_MANAGER_H

#include <stdint.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_err.h"

// 音频数据回调函数类型
typedef void (*audio_data_callback_t)(const int16_t* data, size_t samples, void* user_ctx);

class AudioManager {
public:
    /**
     * @brief 创建音频管理器（内存优化版）
     * 
     * @param sample_rate 采样率（默认16000Hz）
     * @param max_recording_sec 最大录音时长（默认3秒，内存不足时会自动减小）
     * @param streaming_buffer_kb 流式播放缓冲区大小（KB，默认32KB）
     */
    AudioManager(uint32_t sample_rate = 16000, 
                 uint32_t max_recording_sec = 3,
                 uint32_t streaming_buffer_kb = 32);
    
    ~AudioManager();

    /**
     * @brief 初始化音频管理器
     * @return ESP_OK=成功，ESP_ERR_NO_MEM=内存不足
     */
    esp_err_t init();

    /**
     * @brief 反初始化，释放资源
     */
    void deinit();

    // 🎙️ ========== 录音相关功能 ==========
    
    /**
     * @brief 开始录音
     * @param streaming_mode 是否启用流式模式（边录边通过回调发送）
     * @param callback 流式数据回调（仅在 streaming_mode=true 时使用）
     * @param user_ctx 回调用户上下文
     */
    void startRecording(bool streaming_mode = false, 
                        audio_data_callback_t callback = nullptr,
                        void* user_ctx = nullptr);

    /**
     * @brief 停止录音
     */
    void stopRecording();

    /**
     * @brief 查询录音状态
     */
    bool isRecording() const { return is_recording; }

    /**
     * @brief 添加音频数据到录音缓冲区
     * @param data 音频数据指针
     * @param samples 样本数量
     * @return true=添加成功，false=缓冲区满了
     */
    bool addRecordingData(const int16_t* data, size_t samples);

    /**
     * @brief 获取录音数据（非流式模式下使用）
     * @param[out] length 录音的样本数
     * @return 指向录音数据的指针
     */
    const int16_t* getRecordingBuffer(size_t& length) const;

    /**
     * @brief 清空录音缓冲区
     */
    void clearRecordingBuffer();
    
    /**
     * @brief 强制重置缓冲区状态（用于录音/播放切换时）
     * 
     * 此函数会强制将缓冲区模式重置为 IDLE，用于解决状态机死锁
     */
    void resetBufferState();

    /**
     * @brief 获取已录音时间
     */
    float getRecordingDuration() const;

    /**
     * @brief 获取当前录音长度（样本数）
     */
    size_t getRecordingLength() const { return recording_length; }

    /**
     * @brief 检查录音缓冲区是否已满
     */
    bool isRecordingBufferFull() const;

    // 🔊 ========== 音频播放相关功能 ==========

    /**
     * @brief 开始流式播放模式
     */
    void startStreamingPlayback();
    
    /**
     * @brief 添加一小段音频到播放队列
     */
    bool addStreamingAudioChunk(const uint8_t* data, size_t size);
    
    /**
     * @brief 结束流式播放
     */
    void finishStreamingPlayback();
    
    /**
     * @brief 检查流式播放是否正在进行
     */
    bool isStreamingActive() const { return is_streaming; }

    /**
     * @brief 播放一段完整的音频（阻塞式）
     * @note 这会暂时占用共享缓冲区
     */
    esp_err_t playAudio(const uint8_t* audio_data, size_t data_len, const char* description);

    /**
     * @brief 查询AI回复是否播放完成
     */
    bool isResponsePlayed() const { return response_played; }

    /**
     * @brief 重置响应播放标志
     */
    void resetResponsePlayedFlag() { response_played = false; }

    // 🔧 ========== 工具函数 ==========

    uint32_t getSampleRate() const { return sample_rate; }
    size_t getRecordingBufferSize() const { return shared_buffer_samples; }

private:
    // 🎶 音频参数
    uint32_t sample_rate;
    uint32_t max_recording_sec;
    uint32_t streaming_buffer_kb;

    // 🔄 共享内存缓冲区（录音和播放互斥使用）
    int16_t* shared_buffer;             // 共享缓冲区
    size_t shared_buffer_samples;       // 缓冲区大小（样本数）
    
    // 当前缓冲区用途
    enum BufferMode {
        BUFFER_IDLE = 0,        // 空闲
        BUFFER_RECORDING = 1,   // 录音中
        BUFFER_PLAYING = 2      // 播放中
    } buffer_mode;

    // 🎙️ 录音相关变量
    size_t recording_length;            // 已录制的样本数
    bool is_recording;                  // 是否正在录音
    bool streaming_mode;                // 是否流式录音模式
    audio_data_callback_t stream_callback;  // 流式数据回调
    void* stream_user_ctx;              // 回调上下文

    // 🔊 响应音频相关变量
    bool response_played;               // 是否已播放完成

    // 🌊 流式播放相关变量
    bool is_streaming;                  // 是否在流式播放中
    uint8_t* streaming_buffer;          // 环形缓冲区
    size_t streaming_buffer_size;       // 缓冲区大小
    size_t streaming_write_pos;         // 写入位置
    size_t streaming_read_pos;          // 读取位置
    static const size_t STREAMING_CHUNK_SIZE = 3200;   // 每次播放3200字节（200ms）

    TaskHandle_t player_task_handle;
    static void player_task(void* pvParameters);
    volatile bool is_finishing;

    // 🏷️ 日志标签
    static const char* TAG;

    // 内部辅助函数
    esp_err_t allocateBuffers();
    void releaseBuffers();
    bool ensureBufferMode(BufferMode required_mode);
};

#endif // AUDIO_MANAGER_H
