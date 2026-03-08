#include "wvm/mqtt.hpp"
#include "wvm/logger.hpp"
#include <sstream>

#ifdef WVM_USE_MOSQUITTO
#include <mosquitto.h>
#endif

namespace wvm {

static const char* etype(EventType t) {
  switch (t) {
    case EventType::BLACK: return "BLACK";
    case EventType::FREEZE: return "FREEZE";
    case EventType::LAG: return "LAG";
    case EventType::STUTTER: return "STUTTER";
    case EventType::MISMATCH: return "MISMATCH";
    case EventType::RECOVERED: return "RECOVERED";
    case EventType::HEALTH: return "HEALTH";
    case EventType::CAMERA_OFFLINE: return "CAMERA_OFFLINE";
    default: return "UNKNOWN";
  }
}

MqttPublisher::MqttPublisher(const MqttConfig& cfg) : cfg_(cfg) {}
MqttPublisher::~MqttPublisher() { disconnect(); }

bool MqttPublisher::connect() {
  fprintf(stderr, "[MQTT_CONN] connect() called\n");
  fprintf(stderr, "[MQTT_CONN] Config: enabled=%d host=%s port=%d topic=%s client_id=%s\n",
    (int)cfg_.enabled, cfg_.host.c_str(), cfg_.port, cfg_.topic.c_str(), cfg_.client_id.c_str());
  
  if (!cfg_.enabled) {
    fprintf(stderr, "[MQTT_CONN] MQTT disabled in config\n");
    return true;
  }
  
#ifdef WVM_USE_MOSQUITTO
  fprintf(stderr, "[MQTT_CONN] WVM_USE_MOSQUITTO is defined\n");
  mosquitto_lib_init();
  fprintf(stderr, "[MQTT_CONN] mosquitto_lib_init() done\n");
  
  mosq_ = mosquitto_new(cfg_.client_id.c_str(), true, nullptr);
  if (!mosq_) {
    fprintf(stderr, "[MQTT_CONN] ERROR: mosquitto_new() failed\n");
    return false;
  }
  fprintf(stderr, "[MQTT_CONN] mosquitto_new() SUCCESS, connecting to %s:%d\n",
    cfg_.host.c_str(), cfg_.port);
  
  int rc = mosquitto_connect(mosq_, cfg_.host.c_str(), cfg_.port, 30);
  fprintf(stderr, "[MQTT_CONN] mosquitto_connect() returned rc=%d\n", rc);
  
  connected_ = (rc == MOSQ_ERR_SUCCESS);
  if (connected_) {
    fprintf(stderr, "[MQTT_CONN] ✅ Connected successfully\n");
  } else {
    fprintf(stderr, "[MQTT_CONN] ❌ Connection failed\n");
  }
  return connected_;
#else
  fprintf(stderr, "[MQTT_CONN] WARNING: WVM_USE_MOSQUITTO not defined, using stub mode\n");
  connected_ = true;
  return true;
#endif
}

void MqttPublisher::disconnect() {
#ifdef WVM_USE_MOSQUITTO
  if (mosq_) {
    mosquitto_disconnect(mosq_);
    mosquitto_destroy(mosq_);
    mosq_ = nullptr;
    mosquitto_lib_cleanup();
  }
#endif
  connected_ = false;
}

std::string MqttPublisher::to_json(const Event& e) const {
  std::ostringstream oss;
  oss << "{"
      << "\"ts_ms\":" << e.ts_ms << ","
      << "\"tv_id\":\"" << e.tv_id << "\","
      << "\"type\":\"" << etype(e.type) << "\","
      << "\"details\":" << (e.details.empty() ? "\"\"" : e.details)
      << "}";
  return oss.str();
}

bool MqttPublisher::publish(const Event& e) {
  fprintf(stderr, "[MQTT_PUB] publish() called: tv=%s type=%d(%s) ts=%llu\n",
    e.tv_id.c_str(), (int)e.type, etype(e.type), (unsigned long long)e.ts_ms);
  
  if (!cfg_.enabled) {
    fprintf(stderr, "[MQTT_PUB] MQTT disabled in config, skipping\n");
    return true;
  }
  
  if (!connected_) {
    fprintf(stderr, "[MQTT_PUB] ERROR: Not connected to MQTT broker!\n");
    return false;
  }

  std::string payload = to_json(e);
  fprintf(stderr, "[MQTT_PUB] JSON payload: %s\n", payload.c_str());

#ifdef WVM_USE_MOSQUITTO
  fprintf(stderr, "[MQTT_PUB] Publishing to topic: %s\n", cfg_.topic.c_str());
  Logger::instance().log(LogLevel::INFO, "MQTT_PUBLISH %s", payload.c_str());
  int rc = mosquitto_publish(mosq_, nullptr, cfg_.topic.c_str(),
                            (int)payload.size(), payload.data(), 0, false);
  if (rc != MOSQ_ERR_SUCCESS) {
    fprintf(stderr, "[MQTT_PUB] ERROR: mosquitto_publish failed rc=%d\n", rc);
    Logger::instance().log(LogLevel::WARN, "MQTT publish failed rc=%d", rc);
    return false;
  }
  fprintf(stderr, "[MQTT_PUB] mosquitto_publish SUCCESS\n");
  mosquitto_loop(mosq_, 0, 1);
  fprintf(stderr, "[MQTT_PUB] mosquitto_loop completed\n");
  return true;
#else
  fprintf(stderr, "[MQTT_PUB] STUB mode (WVM_USE_MOSQUITTO not defined)\n");
  Logger::instance().log(LogLevel::INFO, "MQTT(STUB) topic=%s payload=%s",
                         cfg_.topic.c_str(), payload.c_str());
  return true;
#endif
}

} // namespace wvm
