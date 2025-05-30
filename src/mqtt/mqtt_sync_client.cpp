#include "mqtt/mqtt_sync_client.h"
#include <iostream>
#include <chrono>
#include <thread>

// 构造函数
MQTTClientWrapper::MQTTClientWrapper(const std::string& brokerUrl,
                                     const std::string& clientId,
                                     const std::string& username,
                                     const std::string& password,
                                     bool cleanSession,
                                     int keepAliveInterval)
        : client(nullptr), connected(false), initialized(false),
          brokerUrl(brokerUrl), clientId(clientId),
          username(username), password(password),
          cleanSession(cleanSession), keepAliveInterval(keepAliveInterval) {

    if (!brokerUrl.empty() && !clientId.empty()) {
        initialize(brokerUrl, clientId, username, password, cleanSession, keepAliveInterval);
    }
}

// 析构函数
MQTTClientWrapper::~MQTTClientWrapper() {
    cleanup();
}

// 初始化MQTT客户端
bool MQTTClientWrapper::initialize(const std::string& brokerUrl, const std::string& clientId,
                                   const std::string& username, const std::string& password,
                                   bool cleanSession, int keepAliveInterval) {
    std::lock_guard<std::mutex> lock(mutex);

    // 如果已经初始化，先清理
    if (initialized) {
        cleanup();
    }

    // 保存连接参数
    this->brokerUrl = brokerUrl;
    this->clientId = clientId;
    this->username = username;
    this->password = password;
    this->cleanSession = cleanSession;
    this->keepAliveInterval = keepAliveInterval;

    // 创建MQTT客户端
    int rc = MQTTClient_create(&client, brokerUrl.c_str(), clientId.c_str(),
                               MQTTCLIENT_PERSISTENCE_NONE, NULL);
    if (rc != MQTTCLIENT_SUCCESS) {
        LOGGER_ERROR("Failed to create MQTT client, return code: " + std::to_string(rc));
        return false;
    }

    // 设置回调函数
    rc = MQTTClient_setCallbacks(client, this,
                                 (MQTTClient_connectionLost*)&MQTTClientWrapper::staticconnectionLostCallback,
                                 (MQTTClient_messageArrived*)&MQTTClientWrapper::messageArrivedCallback,
                                 NULL);
    if (rc != MQTTCLIENT_SUCCESS) {
        LOGGER_ERROR("Failed to set MQTT callbacks, return code: " + std::to_string(rc));
        MQTTClient_destroy(&client);
        return false;
    }

    initialized = true;
    return true;
}

// 连接到MQTT代理
bool MQTTClientWrapper::connect() {
    std::lock_guard<std::mutex> lock(mutex);

    if (!initialized) {
        LOGGER_ERROR("MQTT client not initialized");
        return false;
    }

    if (connected) {
        // 已经连接，无需重新连接
        return true;
    }

    MQTTClient_connectOptions conn_opts = MQTTClient_connectOptions_initializer;
    conn_opts.keepAliveInterval = keepAliveInterval;
    conn_opts.cleansession = cleanSession;

    // 设置用户名和密码（如果提供）
    if (!username.empty()) {
        conn_opts.username = username.c_str();
    }
    if (!password.empty()) {
        conn_opts.password = password.c_str();
    }

    int rc = MQTTClient_connect(client, &conn_opts);
    if (rc != MQTTCLIENT_SUCCESS) {
        LOGGER_ERROR("Failed to connect to MQTT broker, return code: " + std::to_string(rc));
        return false;
    }

    connected = true;
    LOGGER_INFO("Successfully connected to MQTT broker: " + brokerUrl);

    // 重新订阅之前的主题
    for (const auto& pair : topicCallbacks) {
        MQTTClient_subscribe(client, pair.first.c_str(), 1);
    }

    return true;
}

// 断开连接
void MQTTClientWrapper::disconnect() {
    std::lock_guard<std::mutex> lock(mutex);

    if (connected && client != nullptr) {
        MQTTClient_disconnect(client, 1000);
        connected = false;
        LOGGER_INFO("MQTT connection disconnected: " + clientId + " from " + brokerUrl);
    }
}

// 检查是否已连接
bool MQTTClientWrapper::isConnected() {
    std::lock_guard<std::mutex> lock(mutex);
    return connected;
}

// 订阅主题
bool MQTTClientWrapper::subscribe(const std::string& topic, int qos, MessageCallback callback) {
    std::lock_guard<std::mutex> lock(mutex);

    if (!initialized) {
        LOGGER_ERROR("MQTT client not initialized");
        return false;
    }

    // 保存回调函数
    topicCallbacks[topic] = callback;

    if (connected) {
        int rc = MQTTClient_subscribe(client, topic.c_str(), qos);
        if (rc != MQTTCLIENT_SUCCESS) {
            LOGGER_ERROR("Failed to subscribe to topic " + topic + ", return code: " + std::to_string(rc));
            return false;
        }
        LOGGER_INFO("Successfully subscribed to topic: " + topic + " (client: " + clientId + ")");
    }

    return true;
}

bool MQTTClientWrapper::subscribe(const std::string& topic, int qos, MessageCallback1 callback) {
    std::lock_guard<std::mutex> lock(mutex);

    if (!initialized) {
        LOGGER_ERROR("MQTT client not initialized");
        return false;
    }

    // 保存回调函数
    topicMessageCallbacks[topic] = callback;

    if (connected) {
        int rc = MQTTClient_subscribe(client, topic.c_str(), qos);
        if (rc != MQTTCLIENT_SUCCESS) {
            LOGGER_ERROR("Failed to subscribe to topic " + topic + ", return code: " + std::to_string(rc));
            return false;
        }
        LOGGER_INFO("Successfully subscribed to topic: " + topic + " (client: " + clientId + ")");
    }

    return true;
}

// 取消订阅主题
bool MQTTClientWrapper::unsubscribe(const std::string& topic) {
    std::lock_guard<std::mutex> lock(mutex);

    if (!initialized || !connected) {
        LOGGER_ERROR("MQTT client not initialized or not connected");
        return false;
    }

    int rc = MQTTClient_unsubscribe(client, topic.c_str());
    if (rc != MQTTCLIENT_SUCCESS) {
        LOGGER_ERROR("Failed to unsubscribe from topic " + topic + ", return code: " + std::to_string(rc));
        return false;
    }

    // 移除回调函数
    topicCallbacks.erase(topic);
    LOGGER_INFO("Successfully unsubscribed from topic: " + topic + " (client: " + clientId + ")");

    return true;
}

// 发布消息
bool MQTTClientWrapper::publish(const std::string& topic, const std::string& payload,
                                int qos, bool retained) {
    std::lock_guard<std::mutex> lock(mutex);

    if (!initialized) {
        LOGGER_ERROR("MQTT client not initialized");
        return false;
    }

    // 如果未连接，尝试重连
    if (!connected) {
        if (!reconnect()) {
            return false;
        }
    }

    MQTTClient_message pubmsg = MQTTClient_message_initializer;
    pubmsg.payload = (void*)payload.c_str();
    pubmsg.payloadlen = payload.length();
    pubmsg.qos = qos;
    pubmsg.retained = retained;

    MQTTClient_deliveryToken token;
    int rc = MQTTClient_publishMessage(client, topic.c_str(), &pubmsg, &token);
    if (rc != MQTTCLIENT_SUCCESS) {
        LOGGER_ERROR("Failed to publish message, return code: " + std::to_string(rc));
        return false;
    }

    // 等待消息发布完成
    rc = MQTTClient_waitForCompletion(client, token, 5000);
    if (rc != MQTTCLIENT_SUCCESS) {
        LOGGER_ERROR("Failed to wait for message delivery completion, return code: " + std::to_string(rc));
        return false;
    }

    return true;
}

// 设置连接断开回调函数
void MQTTClientWrapper::setConnectionLostCallback(std::function<void(const std::string&)> callback) {
    std::lock_guard<std::mutex> lock(mutex);
    connectionLostCallback = callback;
}

// 清理资源
void MQTTClientWrapper::cleanup() {
    std::lock_guard<std::mutex> lock(mutex);

    if (connected) {
        MQTTClient_disconnect(client, 1000);
        connected = false;
    }

    if (initialized) {
        MQTTClient_destroy(&client);
        initialized = false;
    }

    topicCallbacks.clear();
}

// 静态消息到达回调函数
int MQTTClientWrapper::messageArrivedCallback(void* context, char* topicName, int topicLen,
                                              MQTTClient_message* message) {
    MQTTClientWrapper* instance = static_cast<MQTTClientWrapper*>(context);

    if (instance) {
        std::string topic = topicName;
        std::string payload(static_cast<char*>(message->payload), message->payloadlen);

        // 查找对应主题的回调函数
        auto it = instance->topicCallbacks.find(topic);
        if (it != instance->topicCallbacks.end() && it->second) {
            // 调用回调函数
            it->second(topic, payload);
        }
    }

    MQTTClient_freeMessage(&message);
    MQTTClient_free(topicName);
    return 1;
}

// 静态连接断开回调函数
void MQTTClientWrapper::staticconnectionLostCallback(void* context, char* cause) {
    MQTTClientWrapper* instance = static_cast<MQTTClientWrapper*>(context);

    if (instance) {
        {
            std::lock_guard<std::mutex> lock(instance->mutex);
            instance->connected = false;
        }

        std::string causeStr = (cause != nullptr) ? cause : "Unknown reason";
        LOGGER_INFO("MQTT connection lost: " + instance->clientId + " from " + instance->brokerUrl + " - " + causeStr);

        // 调用用户设置的回调函数
        if (instance->connectionLostCallback) {
            try {
                instance->connectionLostCallback(causeStr);
            } catch (const std::exception& e) {
                LOGGER_ERROR("Exception in connection lost callback: " + std::string(e.what()));
            } catch (...) {
                LOGGER_ERROR("Unknown exception in connection lost callback");
            }
        }

        // 如果启用了自动重连，尝试重新连接
        if (instance->autoReconnect) {
            instance->reconnect();
        }
    }
}

// 检查客户端健康状态
bool MQTTClientWrapper::checkHealth() {
    std::lock_guard<std::mutex> lock(mutex);

    if (!initialized) {
        return false;
    }

    if (connected) {
        // 尝试发送PING来验证连接状态
        // 注意：MQTTClient库会自动处理PING，但我们可以检查isConnected状态
        bool actuallyConnected = MQTTClient_isConnected(client);
        if (!actuallyConnected) {
            LOGGER_WARNING("MQTT client " + clientId + " reports as connected but is actually disconnected");
            connected = false;
            lastDisconnectTime = std::time(nullptr);
            return false;
        }
        return true;
    } else {
        // 如果断开连接时间超过重连时间，并且启用了自动重连，尝试重新连接
        int64_t now = std::time(nullptr);
        int reconnectDelaySeconds = calculateReconnectDelay() / 1000;

        if (autoReconnect && lastDisconnectTime > 0 &&
            (now - lastDisconnectTime) > reconnectDelaySeconds) {

            LOGGER_INFO("Health check initiating reconnect for MQTT client: " + clientId);
            return reconnect();
        }
        return false;
    }
}

// 获取连接状态持续时间
int64_t MQTTClientWrapper::getStatusDuration() const {
    int64_t now = std::time(nullptr);

    if (connected && connectionStartTime > 0) {
        return now - connectionStartTime;
    } else if (!connected && lastDisconnectTime > 0) {
        return now - lastDisconnectTime;
    }

    return 0;
}

// 强制重连
bool MQTTClientWrapper::forceReconnect() {
    // 断开现有连接
    disconnect();

    // 重置重连计数
    resetReconnectAttempts();

    // 尝试重新连接
    return reconnect();
}

// 设置自动重连策略
void MQTTClientWrapper::setReconnectPolicy(bool enable, int maxAttempts, int initialDelayMs, int maxDelayMs) {
    std::lock_guard<std::mutex> lock(mutex);

    autoReconnect = enable;
    maxReconnectAttempts = maxAttempts;
    initialReconnectDelayMs = initialDelayMs;
    maxReconnectDelayMs = maxDelayMs;

    LOGGER_INFO("Set reconnect policy for MQTT client " + clientId + ": enabled=" +
                 (enable ? "true" : "false") + ", maxAttempts=" + std::to_string(maxAttempts) +
                 ", initialDelay=" + std::to_string(initialDelayMs) + "ms, maxDelay=" +
                 std::to_string(maxDelayMs) + "ms");
}

// 计算重连延迟（指数退避）
int MQTTClientWrapper::calculateReconnectDelay() const {
    int attempt = std::min(reconnectAttempts.load(), 10); // 限制指数增长
    int delay = initialReconnectDelayMs * (1 << attempt);
    return std::min(delay, maxReconnectDelayMs);
}

// 尝试重新连接
bool MQTTClientWrapper::reconnect() {
    // 在非锁定状态下增加重连计数，减少锁的竞争
    int attempts = ++reconnectAttempts;

    // 如果设置了最大重试次数并且已经达到，则不再尝试
    if (maxReconnectAttempts > 0 && attempts > maxReconnectAttempts) {
        LOGGER_ERROR("MQTT client " + clientId + " exceeded maximum reconnect attempts (" +
                      std::to_string(maxReconnectAttempts) + ")");
        return false;
    }

    int reconnectDelay = calculateReconnectDelay();
    int64_t now = std::time(nullptr);

    // 如果上次重连时间太近，按照退避策略延迟重连
    if (lastReconnectTime > 0 && (now - lastReconnectTime) * 1000 < reconnectDelay) {
        return false;
    }

    LOGGER_INFO("Attempting to reconnect MQTT client " + clientId + " to " + brokerUrl +
                 " (attempt " + std::to_string(attempts) + ", delay: " +
                 std::to_string(reconnectDelay) + "ms)");

    lastReconnectTime = now;

    {
        std::lock_guard<std::mutex> lock(mutex);

        MQTTClient_connectOptions conn_opts = MQTTClient_connectOptions_initializer;
        conn_opts.keepAliveInterval = keepAliveInterval;
        conn_opts.cleansession = cleanSession;

        if (!username.empty()) {
            conn_opts.username = username.c_str();
        }
        if (!password.empty()) {
            conn_opts.password = password.c_str();
        }

        int rc = MQTTClient_connect(client, &conn_opts);
        if (rc == MQTTCLIENT_SUCCESS) {
            connected = true;
            connectionStartTime = now;
            lastDisconnectTime = 0;
            LOGGER_INFO("Successfully reconnected MQTT client: " + clientId + " to " + brokerUrl);

            // 重新订阅所有主题
            for (const auto& pair : topicCallbacks) {
                MQTTClient_subscribe(client, pair.first.c_str(), 1);
                LOGGER_DEBUG("Resubscribed to topic: " + pair.first);
            }

            // 重置重连尝试计数
            reconnectAttempts = 0;
            return true;
        } else {
            LOGGER_WARNING("Failed to reconnect MQTT client " + clientId + ", error code: " +
                            std::to_string(rc));
        }
    }

    return false;
}
// MQTTClientManager实现

// 获取单例实例
MQTTClientManager& MQTTClientManager::getInstance() {
    static MQTTClientManager instance;
    return instance;
}

// 创建新的MQTT客户端
bool MQTTClientManager::createClient(const std::string& clientName,
                                     const std::string& brokerUrl,
                                     const std::string& clientId,
                                     const std::string& username,
                                     const std::string& password,
                                     bool cleanSession,
                                     int keepAliveInterval) {
    std::lock_guard<std::mutex> lock(mutex);

    // 检查客户端是否已存在
    if (clients.find(clientName) != clients.end()) {
        LOGGER_WARNING("MQTT client already exists with name: " + clientName);
        return false;
    }

    // 创建新的客户端
    auto client = std::make_shared<MQTTClientWrapper>(
            brokerUrl, clientId, username, password, cleanSession, keepAliveInterval
    );

    // 初始化并连接客户端
    if (!client->isConnected() && !client->connect()) {
        LOGGER_WARNING("Failed to connect new MQTT client: " + clientName);
        // 即使连接失败，我们仍然将客户端添加到映射中
        // 它可以稍后连接
    }

    // 添加到映射中
    clients[clientName] = client;
    LOGGER_INFO("Created MQTT client: " + clientName + " for broker: " + brokerUrl);

    return true;
}

// 获取MQTT客户端
std::shared_ptr<MQTTClientWrapper> MQTTClientManager::getClient(const std::string& clientName) {
    std::lock_guard<std::mutex> lock(mutex);

    auto it = clients.find(clientName);
    if (it != clients.end()) {
        return it->second;
    }

    return nullptr;
}

// 移除MQTT客户端
bool MQTTClientManager::removeClient(const std::string& clientName) {
    std::lock_guard<std::mutex> lock(mutex);

    auto it = clients.find(clientName);
    if (it != clients.end()) {
        // 断开连接并清理客户端
        it->second->disconnect();
        it->second->cleanup();

        // 从映射中移除
        clients.erase(it);
        LOGGER_INFO("Removed MQTT client: " + clientName);
        return true;
    }

    return false;
}

// 获取所有客户端名称
std::vector<std::string> MQTTClientManager::listClients() {
    std::lock_guard<std::mutex> lock(mutex);

    std::vector<std::string> clientNames;
    for (const auto& pair : clients) {
        clientNames.push_back(pair.first);
    }

    return clientNames;
}

// 清理所有客户端
void MQTTClientManager::cleanup() {
    std::lock_guard<std::mutex> lock(mutex);

    for (auto& pair : clients) {
        pair.second->disconnect();
        pair.second->cleanup();
    }

    clients.clear();
    LOGGER_INFO("Cleaned up all MQTT clients");
}

// 检查所有客户端的健康状态并尝试恢复
int MQTTClientManager::checkAndRecoverClients() {
    std::lock_guard<std::mutex> lock(mutex);
    int healthyCount = 0;

    LOGGER_DEBUG("Checking health of " + std::to_string(clients.size()) + " MQTT clients");

    for (auto& pair : clients) {
        try {
            const std::string& clientName = pair.first;
            auto& client = pair.second;

            bool healthy = client->checkHealth();

            if (healthy) {
                healthyCount++;
            } else {
                int64_t disconnectedTime = client->getStatusDuration();
                LOGGER_WARNING("MQTT client " + clientName + " is unhealthy, disconnected for " +
                                std::to_string(disconnectedTime) + " seconds");

                // 对长时间断开连接的客户端进行强制重连
                if (disconnectedTime > 300) { // 5分钟
                    LOGGER_INFO("Forcing reconnect for long-disconnected client: " + clientName);
                    if (client->forceReconnect()) {
                        healthyCount++;
                    }
                }
            }
        } catch (const std::exception& e) {
            LOGGER_ERROR("Error checking MQTT client health: " + std::string(e.what()));
        }
    }

    return healthyCount;
}

// 设置MQTT连接健康检查的间隔时间
void MQTTClientManager::setHealthCheckInterval(int intervalSeconds) {
    if (intervalSeconds < 1) {
        LOGGER_WARNING("Invalid health check interval, must be at least 1 second");
        intervalSeconds = 1;
    }

    std::lock_guard<std::mutex> lock(mutex);
    healthCheckIntervalSeconds = intervalSeconds;
    LOGGER_INFO("Set MQTT health check interval to " + std::to_string(intervalSeconds) + " seconds");
}

// 启动周期性健康检查
void MQTTClientManager::startPeriodicHealthCheck() {
    // 先停止现有的检查
    stopPeriodicHealthCheck();

    std::lock_guard<std::mutex> lock(mutex);

    // 启动新的健康检查线程
    healthCheckRunning = true;
    healthCheckThread = std::thread(&MQTTClientManager::healthCheckLoop, this);

    LOGGER_INFO("Started periodic MQTT health check (interval: " +
                 std::to_string(healthCheckIntervalSeconds) + " seconds)");
}

// 停止周期性健康检查
void MQTTClientManager::stopPeriodicHealthCheck() {
    bool wasRunning = false;

    {
        std::lock_guard<std::mutex> lock(mutex);
        wasRunning = healthCheckRunning;
        healthCheckRunning = false;
    }

    if (wasRunning && healthCheckThread.joinable()) {
        healthCheckThread.join();
        LOGGER_INFO("Stopped periodic MQTT health check");
    }
}

// 设置全局重连策略
void MQTTClientManager::setGlobalReconnectPolicy(bool enable, int maxAttempts,
                                                 int initialDelayMs, int maxDelayMs) {
    std::lock_guard<std::mutex> lock(mutex);

    for (auto& pair : clients) {
        try {
            pair.second->setReconnectPolicy(enable, maxAttempts, initialDelayMs, maxDelayMs);
        } catch (const std::exception& e) {
            LOGGER_ERROR("Error setting reconnect policy for MQTT client " + pair.first +
                          ": " + e.what());
        }
    }

    LOGGER_INFO("Set global MQTT reconnect policy: enabled=" + std::string((enable ? "true" : "false")) +
                 ", maxAttempts=" + std::to_string(maxAttempts) + ", initialDelay=" +
                 std::to_string(initialDelayMs) + "ms, maxDelay=" + std::to_string(maxDelayMs) + "ms");
}

// 健康检查线程函数
void MQTTClientManager::healthCheckLoop() {
    LOGGER_INFO("MQTT health check loop started");

    while (healthCheckRunning) {
        try {
            int healthyCount = checkAndRecoverClients();
            int totalCount = 0;

            {
                std::lock_guard<std::mutex> lock(mutex);
                totalCount = clients.size();
            }

            LOGGER_DEBUG("MQTT health check: " + std::to_string(healthyCount) + "/" +
                          std::to_string(totalCount) + " clients healthy");
        } catch (const std::exception& e) {
            LOGGER_ERROR("Error in MQTT health check loop: " + std::string(e.what()));
        }

        // 使用sleep_for的好处是可以更快地响应停止请求
        for (int i = 0; i < healthCheckIntervalSeconds && healthCheckRunning; i++) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

    LOGGER_INFO("MQTT health check loop stopped");
}


// 析构函数
MQTTClientManager::~MQTTClientManager() {
    cleanup();
}