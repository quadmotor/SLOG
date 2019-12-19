#pragma once

#include <thread>
#include <zmq.hpp>

#include "common/configuration.h"
#include "connection/channel.h"

namespace slog {

class Broker {
public:
  Broker(std::shared_ptr<Configuration> config, zmq::context_t& context);
  ~Broker();

  void Run();

private:
  std::shared_ptr<Configuration> config_;

  zmq::socket_t router_;
  std::thread thread_;

  std::vector<Channel> channels_;

  std::atomic<bool> running_;
};

} // namespace slog