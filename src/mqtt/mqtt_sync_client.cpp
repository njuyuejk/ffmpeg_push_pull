#include "mqtt/mqtt_sync_client.h"
#include <iostream>
#include <chrono>
#include <thread>

// 获取单例实例
MQTTClientWrapper& MQTTClientWrapper::getInstance() {
    static MQTTClientWrapper instance;
    return instance;
}

// 构造函数
MQTTClientWrapper::MQTTClientWrapper()
        : client(nullptr), connected(false), initialized(false),
          cleanSession(true), keepAliveInterval(60) {
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
        // 已经连接，无需重复连接
        return true;
    }

    MQTTClient_connectOptions conn_opts = MQTTClient_connectOptions_initializer;
    conn_opts.keepAliveInterval = keepAliveInterval;
    conn_opts.cleansession = cleanSession;

    // 设置用户名和密码（如果有）
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
        Logger::info("MQTT connection disconnected");
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
        Logger::info("Successfully subscribed to topic: " + topic);
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
    Logger::info("Successfully unsubscribed from topic: " + topic);

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
        instance->connected = false;

        std::string causeStr = (cause != nullptr) ? cause : "Unknown reason";
        Logger::info("MQTT connection lost: " + causeStr);

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
        Logger::info("Attempting to reconnect to MQTT broker (" + std::to_string((i+1)) + "/" + std::to_string(MAX_RETRY) + ")...");

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
                Logger::info("Successfully reconnected to MQTT broker");

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

    Logger::error("Failed to reconnect to MQTT broker after maximum retry attempts");
    return false;
}