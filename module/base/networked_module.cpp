#include "module/base/networked_module.h"

#include <glog/logging.h>

#include <sstream>

#include "common/constants.h"
#include "connection/broker.h"
#include "connection/sender.h"

using std::make_unique;
using std::move;
using std::unique_ptr;
using std::vector;

namespace slog {

NetworkedModule::NetworkedModule(const std::string& name, const std::shared_ptr<Broker>& broker, Channel channel,
                                 std::chrono::milliseconds poll_timeout, int recv_batch)
    : Module(name),
      context_(broker->context()),
      channel_(channel),
      pull_socket_(*context_, ZMQ_PULL),
      sender_(broker),
      poller_(poll_timeout),
      recv_batch_(recv_batch) {
  broker->AddChannel(channel);
  pull_socket_.bind(MakeInProcChannelAddress(channel));
  // Remove limit on the zmq message queues
  pull_socket_.set(zmq::sockopt::rcvhwm, 0);

  auto& config = broker->config();
  std::ostringstream os;
  os << "module = " << name << ", rep = " << config->local_replica() << ", part = " << config->local_partition()
     << ", machine_id = " << config->local_machine_id();
  debug_info_ = os.str();
}

NetworkedModule::~NetworkedModule() { LOG(INFO) << name() << " stopped. Work done: " << work_; }

zmq::socket_t& NetworkedModule::GetCustomSocket(size_t i) { return custom_sockets_[i]; }

const std::shared_ptr<zmq::context_t> NetworkedModule::context() const { return context_; }

void NetworkedModule::SetUp() {
  VLOG(1) << "Thread info: " << debug_info_;

  poller_.PushSocket(pull_socket_);
  custom_sockets_ = InitializeCustomSockets();
  for (auto& socket : custom_sockets_) {
    poller_.PushSocket(socket);
  }

  Initialize();
}

bool NetworkedModule::Loop() {
  if (poller_.Wait() <= 0) {
    return false;
  }

  for (int i = 0; i < recv_batch_; i++) {
    // Message from pull socket
    if (auto env = RecvEnvelope(pull_socket_, true /* dont_wait */); env != nullptr) {
#ifdef ENABLE_WORK_MEASURING
      auto start = std::chrono::steady_clock::now();
#endif

      if (env->has_request()) {
        HandleInternalRequest(move(env));
      } else if (env->has_response()) {
        HandleInternalResponse(move(env));
      }

#ifdef ENABLE_WORK_MEASURING
      work_ += (std::chrono::steady_clock::now() - start).count();
#endif
    }

    for (size_t i = 0; i < custom_sockets_.size(); i++) {
#ifdef ENABLE_WORK_MEASURING
      auto start = std::chrono::steady_clock::now();
      if (HandleCustomSocket(custom_sockets_[i], i)) {
        work_ += (std::chrono::steady_clock::now() - start).count();
      }
#else
      HandleCustomSocket(custom_sockets_[i], i);
#endif
    }
  }

  return false;
}

void NetworkedModule::Send(const internal::Envelope& env, MachineId to_machine_id, Channel to_channel) {
  sender_.Send(env, to_machine_id, to_channel);
}

void NetworkedModule::Send(EnvelopePtr&& env, Channel to_channel) { sender_.Send(move(env), to_channel); }

void NetworkedModule::Send(const internal::Envelope& env, const std::vector<MachineId>& to_machine_ids,
                           Channel to_channel) {
  sender_.Send(env, to_machine_ids, to_channel);
}

void NetworkedModule::Send(EnvelopePtr&& env, const std::vector<MachineId>& to_machine_ids, Channel to_channel) {
  sender_.Send(move(env), to_machine_ids, to_channel);
}

void NetworkedModule::NewTimedCallback(microseconds timeout, std::function<void()>&& cb) {
  poller_.AddTimedCallback(timeout, std::move(cb));
}

}  // namespace slog