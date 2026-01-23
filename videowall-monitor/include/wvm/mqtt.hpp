#pragma once
#include "wvm/config.hpp"
#include "wvm/types.hpp"
#include <string>

namespace wvm {

class MqttPublisher {
public:
  explicit MqttPublisher(const MqttConfig& cfg);
  ~MqttPublisher();

  bool connect();
  void disconnect();
  bool publish(const Event& e);

private:
  MqttConfig cfg_;
  bool connected_ = false;

#ifdef WVM_USE_MOSQUITTO
  struct mosquitto* mosq_ = nullptr;
#endif

  std::string to_json(const Event& e) const;
};

} // namespace wvm
