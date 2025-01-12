#pragma once

#include <glog/logging.h>

#include <unordered_set>
#include <variant>
#include <vector>

#include "common/configuration.h"
#include "common/txn_holder.h"
#include "connection/broker.h"
#include "connection/sender.h"
#include "connection/zmq_utils.h"
#include "module/base/module.h"
#include "proto/internal.pb.h"
#include "storage/mem_only_storage.h"

using std::pair;
using std::shared_ptr;
using std::string;
using std::unique_ptr;
namespace slog {

const auto kTestModuleTimeout = 5ms;

using ConfigVec = std::vector<ConfigurationPtr>;

ConfigVec MakeTestConfigurations(string&& prefix, int num_replicas, int num_partitions,
                                 internal::Configuration common_config = {});

Transaction* MakeTestTransaction(const ConfigurationPtr& config, TxnId id, const std::vector<KeyEntry>& keys,
                                 const std::variant<string, int>& proc = "", MachineId coordinator = 0);

TxnHolder MakeTestTxnHolder(const ConfigurationPtr& config, TxnId id, const std::vector<KeyEntry>& keys,
                            const std::variant<string, int>& proc = "");

using ModuleRunnerPtr = unique_ptr<ModuleRunner>;

/**
 * This is a fake SLOG system where we can only add a subset
 * of modules to test them in isolation.
 */
class TestSlog {
 public:
  TestSlog(const ConfigurationPtr& config);
  void Data(Key&& key, Record&& record);
  void AddServerAndClient();
  void AddForwarder();
  void AddSequencer();
  void AddInterleaver();
  void AddScheduler();
  void AddLocalPaxos();
  void AddGlobalPaxos();
  void AddMultiHomeOrderer();

  void AddOutputChannel(Channel channel);
  zmq::pollitem_t GetPollItemForChannel(Channel channel);

  EnvelopePtr ReceiveFromOutputChannel(Channel channel) {
    CHECK(channels_.count(channel) > 0) << "Channel " << channel << " does not exist";
    return RecvEnvelope(channels_[channel]);
  }

  unique_ptr<Sender> NewSender();

  void StartInNewThreads();
  void SendTxn(Transaction* txn);
  Transaction RecvTxnResult();

  const ConfigurationPtr& config() const { return config_; }

 private:
  ConfigurationPtr config_;
  shared_ptr<MemOnlyStorage<Key, Record, Metadata>> storage_;
  shared_ptr<Broker> broker_;
  ModuleRunnerPtr server_;
  ModuleRunnerPtr forwarder_;
  ModuleRunnerPtr sequencer_;
  ModuleRunnerPtr interleaver_;
  ModuleRunnerPtr scheduler_;
  ModuleRunnerPtr local_paxos_;
  ModuleRunnerPtr global_paxos_;
  ModuleRunnerPtr multi_home_orderer_;

  std::unordered_map<Channel, zmq::socket_t> channels_;

  zmq::context_t client_context_;
  zmq::socket_t client_socket_;
};

}  // namespace slog