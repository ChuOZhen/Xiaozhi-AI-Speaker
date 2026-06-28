/**
 * @file websocket_client.cc
 * @brief WebSocket客户端实现文件 - 实现与服务器的实时通信
 * 
 * WebSocket是一种全双工通信协议，非常适合实时音频传输。
 */

 #include "websocket_client.h"
 #include "esp_log.h"
 #include <cstring>
 
 static const char *TAG = "WebSocketClient";
 
WebSocketClient::WebSocketClient(const std::string& uri, bool auto_reconnect,
                               int reconnect_interval_ms,
                               bool enable_keepalive,
                               int keepalive_idle,
                               int keepalive_interval)
    : uri_(uri), auto_reconnect_(auto_reconnect),
      reconnect_interval_ms_(reconnect_interval_ms),
      enable_keepalive_(enable_keepalive),
      keepalive_idle_(keepalive_idle),
      keepalive_interval_(keepalive_interval),
      client_(nullptr), connected_(false), reconnect_task_handle_(nullptr) {
}
 
 WebSocketClient::~WebSocketClient() {
     disconnect();
 }
 
 void WebSocketClient::setEventCallback(EventCallback callback) {
     event_callback_ = callback;
 }
 
 void WebSocketClient::websocket_event_handler(void* handler_args, esp_event_base_t base, 
                                              int32_t event_id, void* event_data) {
     // 转换参数类型
     WebSocketClient* ws_client = static_cast<WebSocketClient*>(handler_args);
     esp_websocket_event_data_t* data = (esp_websocket_event_data_t*)event_data;
     
     EventData event;
     event.data = nullptr;
     event.data_len = 0;
     event.op_code = 0;
     
     switch (event_id) {
         case WEBSOCKET_EVENT_CONNECTED:
             ESP_LOGI(TAG, "WebSocket已连接");
             ws_client->connected_ = true;
             event.type = EventType::CONNECTED;
             break;
             
         case WEBSOCKET_EVENT_DISCONNECTED:
             ESP_LOGI(TAG, "WebSocket已断开");
             ws_client->connected_ = false;
             event.type = EventType::DISCONNECTED;
             break;
             
         case WEBSOCKET_EVENT_DATA:
             // 🔍 详细调试：打印所有接收到的数据信息
             ESP_LOGI(TAG, "收到WebSocket数据: 长度=%d, op_code=0x%02x", 
                     data->data_len, data->op_code);
             
             // 【修改】处理 Pong 帧（心跳回应）- 更新最后在线时间
             if (data->op_code == 0x0A) {
                 ESP_LOGD(TAG, "→ Pong帧(心跳回应)，连接正常");
                 // 这里可以更新最后在线时间戳
                 event.type = EventType::PONG;
                 // 仍然触发回调，让上层知道连接活跃
             }
             // 确保数据指针有效（但允许心跳帧长度为0）
             else if (data->data_ptr == nullptr || data->data_len < 0) {
                 ESP_LOGW(TAG, "收到空数据指针，忽略");
                 return;
             }
             
             event.data = (const uint8_t*)data->data_ptr;
             event.data_len = data->data_len;
             event.op_code = data->op_code;
             
             // 根据操作码判断数据类型
             if (data->op_code == 0x01) {        // 文本帧（JSON等）
                 event.type = EventType::DATA_TEXT;
                 ESP_LOGD(TAG, "→ 文本帧: %.*s", 
                         data->data_len > 100 ? 100 : data->data_len, 
                         (char*)data->data_ptr);
             } else if (data->op_code == 0x02) { // 二进制帧（音频等）
                 event.type = EventType::DATA_BINARY;
                 ESP_LOGI(TAG, "→ 二进制帧(音频): %d 字节", data->data_len);
             } else if (data->op_code == 0x08) { // 【新增】Close帧（连接关闭）
                 // 解析关闭原因
                 uint16_t close_code = 1000;
                 const char* close_reason = "Normal";
                 if (data->data_len >= 2) {
                     close_code = ((uint8_t)data->data_ptr[0] << 8) | (uint8_t)data->data_ptr[1];
                     if (data->data_len > 2) {
                         close_reason = data->data_ptr + 2;
                     }
                 }
                 ESP_LOGW(TAG, "→ Close帧: code=%d, reason=%s", close_code, close_reason);
                 ws_client->connected_ = false;
                 event.type = EventType::DISCONNECTED;
             } else if (data->op_code == 0x09) { // Ping帧（心跳检测）
                 event.type = EventType::PING;
             } else if (data->op_code == 0x0A) { // Pong帧（已处理）
                 // 已在上面处理
             } else {
                 ESP_LOGW(TAG, "→ 未知帧类型 op_code=0x%02x", data->op_code);
                 event.type = EventType::DATA_BINARY;
             }
             break;
             
         case WEBSOCKET_EVENT_ERROR:
             ESP_LOGI(TAG, "WebSocket错误");
             ws_client->connected_ = false;
             event.type = EventType::ERROR;
             break;
             
         default:
             return;
     }
     
     // 调用用户设置的事件处理函数
     if (ws_client->event_callback_) {
         ws_client->event_callback_(event);
     }
 }
 
 void WebSocketClient::reconnect_task(void* arg) {
     WebSocketClient* ws_client = static_cast<WebSocketClient*>(arg);

     // 首次等待连接建立完成，防止在连接过程中误触发重连
     vTaskDelay(pdMS_TO_TICKS(ws_client->reconnect_interval_ms_));

     // 重连任务主循环
     while (1) {
         // 检查是否需要重连
         if (!ws_client->connected_ && ws_client->client_ != nullptr && ws_client->auto_reconnect_) {
             ESP_LOGI(TAG, "尝试重新连接WebSocket...");

             // 彻底销毁旧连接，让 lwIP 清理 TCP 状态
             esp_websocket_client_destroy(ws_client->client_);
             ws_client->client_ = nullptr;
             vTaskDelay(pdMS_TO_TICKS(500));

             // 用相同 URI 重建客户端
             esp_websocket_client_config_t ws_cfg = {};
             ws_cfg.uri = ws_client->uri_.c_str();
             ws_cfg.buffer_size = BUFFER_SIZE;
             ws_cfg.task_stack = TASK_STACK_SIZE;
             ws_cfg.reconnect_timeout_ms = RECONNECT_TIMEOUT_MS;
             ws_cfg.network_timeout_ms = 30000;
             ws_cfg.keep_alive_enable = ws_client->enable_keepalive_;
             ws_cfg.keep_alive_idle = ws_client->keepalive_idle_;
             ws_cfg.keep_alive_interval = ws_client->keepalive_interval_;
             ws_cfg.keep_alive_count = 3;

             ws_client->client_ = esp_websocket_client_init(&ws_cfg);
             if (ws_client->client_ == nullptr) {
                 ESP_LOGE(TAG, "重建 WebSocket 客户端失败");
                 vTaskDelay(pdMS_TO_TICKS(ws_client->reconnect_interval_ms_));
                 continue;
             }

             esp_websocket_register_events(ws_client->client_, WEBSOCKET_EVENT_ANY,
                                            websocket_event_handler, ws_client);
             esp_websocket_client_start(ws_client->client_);
         }

         // 休眠一段时间后再检查
         vTaskDelay(pdMS_TO_TICKS(ws_client->reconnect_interval_ms_));
     }
 }
 
 esp_err_t WebSocketClient::connect() {
     if (client_ != nullptr) {
         ESP_LOGW(TAG, "WebSocket客户端已存在");
         return ESP_OK;
     }
     
     ESP_LOGI(TAG, "正在连接WebSocket服务器: %s", uri_.c_str());
     
     // 🔧 配置WebSocket参数
     esp_websocket_client_config_t ws_cfg = {};
     ws_cfg.uri = uri_.c_str();            // 服务器地址
     ws_cfg.buffer_size = BUFFER_SIZE;     // 接收缓冲区16KB
     ws_cfg.task_stack = TASK_STACK_SIZE;  // 任务栈大小8KB
     ws_cfg.reconnect_timeout_ms = RECONNECT_TIMEOUT_MS;  // 重连超时2秒（缩短）
     ws_cfg.network_timeout_ms = 30000;    // 网络超时30秒（给AI处理留时间）
     
     ESP_LOGI(TAG, "WebSocket配置: 接收缓冲=%d, 发送缓冲=%d, 任务栈=%d, 重连=%dms", 
              BUFFER_SIZE, SEND_BUFFER_SIZE, TASK_STACK_SIZE, RECONNECT_TIMEOUT_MS);
     
     // 配置TCP保活参数（防止长时间无数据传输时连接被断开）
     ws_cfg.keep_alive_enable = enable_keepalive_;
     ws_cfg.keep_alive_idle = keepalive_idle_;           // 空闲5秒后开始保活
     ws_cfg.keep_alive_interval = keepalive_interval_;   // 每5秒发送一次保活探测
     ws_cfg.keep_alive_count = 3;                        // 最多重试3次
     
     // 创建 WebSocket客户端实例
     client_ = esp_websocket_client_init(&ws_cfg);
     if (client_ == nullptr) {
         ESP_LOGE(TAG, "WebSocket客户端初始化失败");
         return ESP_FAIL;
     }
     
     // 注册事件处理函数（所有事件都会通知我们）
     esp_websocket_register_events(client_, WEBSOCKET_EVENT_ANY, websocket_event_handler, this);
     
     // 启动WebSocket客户端
     esp_err_t ret = esp_websocket_client_start(client_);
     if (ret != ESP_OK) {
         ESP_LOGE(TAG, "WebSocket客户端启动失败: %s", esp_err_to_name(ret));
         esp_websocket_client_destroy(client_);
         client_ = nullptr;
         return ret;
     }
     
     // 创建自动重连任务
     if (auto_reconnect_ && reconnect_task_handle_ == nullptr) {
         xTaskCreate(reconnect_task, "ws_reconnect", RECONNECT_TASK_STACK_SIZE, 
                    this, 5, &reconnect_task_handle_);
         ESP_LOGI(TAG, "自动重连任务已启动");
     }
     
     return ESP_OK;
 }
 
 void WebSocketClient::disconnect() {
     // 停止自动重连任务
     if (reconnect_task_handle_ != nullptr) {
         vTaskDelete(reconnect_task_handle_);
         reconnect_task_handle_ = nullptr;
         ESP_LOGI(TAG, "自动重连任务已停止");
     }
     
     // 断开并清理WebSocket连接
     if (client_ != nullptr) {
         ESP_LOGI(TAG, "正在断开WebSocket连接...");
         esp_websocket_client_stop(client_);      // 停止连接
         esp_websocket_client_destroy(client_);   // 释放资源
         client_ = nullptr;
         connected_ = false;
         ESP_LOGI(TAG, "WebSocket已完全断开");
     }
 }
 
 int WebSocketClient::sendText(const std::string& text, int timeout_ms) {
     if (client_ == nullptr || !connected_) {
         ESP_LOGW(TAG, "WebSocket未连接，无法发送文本");
         return -1;
     }
     
     // 调用ESP-IDF的WebSocket API发送文本数据
     int len = esp_websocket_client_send_text(client_, text.c_str(), text.length(), 
                                             timeout_ms / portTICK_PERIOD_MS);
     if (len < 0) {
         ESP_LOGE(TAG, "发送文本失败");
     } else {
         ESP_LOGD(TAG, "发送文本成功: %d 字节", len);
     }
     
     return len;
 }
 
 int WebSocketClient::sendBinary(const uint8_t* data, size_t len, int timeout_ms) {
     if (client_ == nullptr || !connected_) {
         ESP_LOGW(TAG, "WebSocket未连接，无法发送二进制数据");
         return -1;
     }

     // 带退避重试：TCP 缓冲区满时 transport_poll_write 返回 0，
     // 短暂等待后重试，而非直接断开连接。
     const int MAX_RETRIES = 5;
     const int RETRY_DELAY_MS = 50;
     int total_sent = 0;

     for (int retry = 0; retry < MAX_RETRIES; retry++) {
         int sent = esp_websocket_client_send_bin(
             client_,
             (const char*)data + total_sent,
             len - total_sent,
             timeout_ms / portTICK_PERIOD_MS);

         if (sent > 0) {
             total_sent += sent;
             if (total_sent >= (int)len) {
                 return total_sent;
             }
             // 部分发送，继续发送剩余数据
             ESP_LOGD(TAG, "部分发送 %d/%zu 字节，继续...", total_sent, len);
             continue;
         }

         // sent <= 0: 缓冲区满或错误
         if (sent == 0) {
             // transport_poll_write(0): TCP 发送缓冲区满，退避重试
             if (retry < MAX_RETRIES - 1) {
                 ESP_LOGW(TAG, "TCP缓冲区满，%dms后重试 (%d/%d)",
                          RETRY_DELAY_MS, retry + 1, MAX_RETRIES);
                 vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS));
                 continue;
             }
             ESP_LOGE(TAG, "发送阻塞，重试%d次后仍失败", MAX_RETRIES);
         } else {
             // sent < 0: 真正的错误
             ESP_LOGE(TAG, "发送二进制数据失败 (ret=%d, errno=%d)", sent, errno);
         }
         return -1;
     }

     return -1;
 }
 
 esp_err_t WebSocketClient::sendPing() {
     if (client_ == nullptr || !connected_) {
         ESP_LOGW(TAG, "WebSocket未连接，无法发送ping");
         return ESP_ERR_INVALID_STATE;
     }
     
     // 提示：ESP-IDF的WebSocket客户端会自动处理ping/pong心跳
     // 如果需要手动发送ping包，可以在这里实现
     // 心跳机制用于保持连接活跃，防止服务器超时断开
     return ESP_OK;
 }