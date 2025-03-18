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
        Logger::error("Failed to create MQTT client, return code: " + std::to_string(rc));
        return false;
    }

    // 设置回调函数
    rc = MQTTClient_setCallbacks(client, this,
                                 (MQTTClient_connectionLost*)&MQTTClientWrapper::staticconnectionLostCallback,
                                 (MQTTClient_messageArrived*)&MQTTClientWrapper::messageArrivedCallback,
                                 NULL);
    if (rc != MQTTCLIENT_SUCCESS) {
        Logger::error("Failed to set MQTT callbacks, return code: " + std::to_string(rc));
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
        Logger::error("MQTT client not initialized");
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
        Logger::error("Failed to connect to MQTT broker, return code: " + std::to_string(rc));
        return false;
    }

    connected = true;
    Logger::info("Successfully connected to MQTT broker: " + brokerUrl);

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
        Logger::info("MQTT connection disconnected: " + clientId + " from " + brokerUrl);
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
        Logger::error("MQTT client not initialized");
        return false;
    }

    // 保存回调函数
    topicCallbacks[topic] = callback;

    if (connected) {
        int rc = MQTTClient_subscribe(client, topic.c_str(), qos);
        if (rc != MQTTCLIENT_SUCCESS) {
            Logger::error("Failed to subscribe to topic " + topic + ", return code: " + std::to_string(rc));
            return false;
        }
        Logger::info("Successfully subscribed to topic: " + topic + " (client: " + clientId + ")");
    }

    return true;
}

// 取消订阅主题
bool MQTTClientWrapper::unsubscribe(const std::string& topic) {
    std::lock_guard<std::mutex> lock(mutex);

    if (!initialized || !connected) {
        Logger::error("MQTT client not initialized or not connected");
        return false;
    }

    int rc = MQTTClient_unsubscribe(client, topic.c_str());
    if (rc != MQTTCLIENT_SUCCESS) {
        Logger::error("Failed to unsubscribe from topic " + topic + ", return code: " + std::to_string(rc));
        return false;
    }

    // 移除回调函数
    topicCallbacks.erase(topic);
    Logger::info("Successfully unsubscribed from topic: " + topic + " (client: " + clientId + ")");

    return true;
}

// 发布消息
bool MQTTClientWrapper::publish(const std::string& topic, const std::string& payload,
                                int qos, bool retained) {
    std::lock_guard<std::mutex> lock(mutex);

    if (!initialized) {
        Logger::error("MQTT client not initialized");
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
        Logger::error("Failed to publish message, return code: " + std::to_string(rc));
        return false;
    }

    // 等待消息发布完成
    rc = MQTTClient_waitForCompletion(client, token, 5000);
    if (rc != MQTTCLIENT_SUCCESS) {
        Logger::error("Failed to wait for message delivery completion, return code: " + std::to_string(rc));
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
        Logger::info("MQTT connection lost: " + instance->clientId + " from " + instance->brokerUrl + " - " + causeStr);

        // 调用用户设置的回调函数
        if (instance->connectionLostCallback) {
            instance->connectionLostCallback(causeStr);
        }

        // 尝试重新连接
        instance->reconnect();
    }
}

// 尝试重新连接
bool MQTTClientWrapper::reconnect() {
    // 在非锁定状态下尝试重连，避免死锁
    static const int MAX_RETRY = 5;
    static const int RETRY_INTERVAL_MS = 3000;

    for (int i = 0; i < MAX_RETRY; i++) {
        Logger::info("Attempting to reconnect to MQTT broker (" + std::to_string(i+1) + "/" +
                     std::to_string(MAX_RETRY) + "): " + clientId + " to " + brokerUrl);

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
                Logger::info("Successfully reconnected to MQTT broker: " + brokerUrl);

                // 重新订阅所有主题
                for (const auto& pair : topicCallbacks) {
                    MQTTClient_subscribe(client, pair.first.c_str(), 1);
                }

                return true;
            }
        }

        // 等待一段时间再重试
        std::this_thread::sleep_for(std::chrono::milliseconds(RETRY_INTERVAL_MS));
    }

    Logger::error("Failed to reconnect to MQTT broker after maximum retry attempts: " + clientId + " to " + brokerUrl);
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
        Logger::warning("MQTT client already exists with name: " + clientName);
        return false;
    }

    // 创建新的客户端
    auto client = std::make_shared<MQTTClientWrapper>(
            brokerUrl, clientId, username, password, cleanSession, keepAliveInterval
    );

    // 初始化并连接客户端
    if (!client->isConnected() && !client->connect()) {
        Logger::warning("Failed to connect new MQTT client: " + clientName);
        // 即使连接失败，我们仍然将客户端添加到映射中
        // 它可以稍后连接
    }

    // 添加到映射中
    clients[clientName] = client;
    Logger::info("Created MQTT client: " + clientName + " for broker: " + brokerUrl);

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
        Logger::info("Removed MQTT client: " + clientName);
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
    Logger::info("Cleaned up all MQTT clients");
}

// 析构函数
MQTTClientManager::~MQTTClientManager() {
    cleanup();
}