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
    default: return "UNKNOWN";
  }
}

MqttPublisher::MqttPublisher(const MqttConfig& cfg) : cfg_(cfg) {}
MqttPublisher::~MqttPublisher() { disconnect(); }

bool MqttPublisher::connect() {
  if (!cfg_.enabled) return true;
#ifdef WVM_USE_MOSQUITTO
  mosquitto_lib_init();
  mosq_ = mosquitto_new(cfg_.client_id.c_str(), true, nullptr);
  if (!mosq_) return false;
  int rc = mosquitto_connect(mosq_, cfg_.host.c_str(), cfg_.port, 30);
  connected_ = (rc == MOSQ_ERR_SUCCESS);
  return connected_;
#else
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
  if (!cfg_.enabled) return true;
  if (!connected_) return false;

  std::string payload = to_json(e);

#ifdef WVM_USE_MOSQUITTO
  int rc = mosquitto_publish(mosq_, nullptr, cfg_.topic.c_str(),
                            (int)payload.size(), payload.data(), 0, false);
  if (rc != MOSQ_ERR_SUCCESS) {
    Logger::instance().log(LogLevel::WARN, "MQTT publish failed rc=%d", rc);
    return false;
  }
  mosquitto_loop(mosq_, 0, 1);
  return true;
#else
  Logger::instance().log(LogLevel::INFO, "MQTT(STUB) topic=%s payload=%s",
                         cfg_.topic.c_str(), payload.c_str());
  return true;
#endif
}

} // namespace wvm
