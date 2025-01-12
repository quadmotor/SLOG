#include "test/test_utils.h"

#include <glog/logging.h>

#include <random>

#include "common/proto_utils.h"
#include "connection/zmq_utils.h"
#include "module/consensus.h"
#include "module/forwarder.h"
#include "module/interleaver.h"
#include "module/multi_home_orderer.h"
#include "module/scheduler.h"
#include "module/sequencer.h"
#include "module/server.h"
#include "module/ticker.h"
#include "proto/api.pb.h"

using std::make_shared;
using std::to_string;

namespace slog {

ConfigVec MakeTestConfigurations(string&& prefix, int num_replicas, int num_partitions,
                                 internal::Configuration common_config) {
  std::random_device rd;
  std::mt19937 re(rd());
  std::uniform_int_distribution<> dis(10000, 30000);
  int num_machines = num_replicas * num_partitions;
  string addr = "/tmp/test_" + prefix;

  common_config.set_protocol("ipc");
  common_config.add_broker_ports(0);
  common_config.add_broker_ports(1);
  common_config.set_num_partitions(num_partitions);
  common_config.mutable_hash_partitioning()->set_partition_key_num_bytes(1);
  common_config.set_sequencer_batch_duration(1);
  common_config.set_forwarder_batch_duration(1);
  for (int r = 0; r < num_replicas; r++) {
    auto replica = common_config.add_replicas();
    for (int p = 0; p < num_partitions; p++) {
      replica->add_addresses(addr + to_string(r * num_partitions + p));
    }
  }

  ConfigVec configs;
  configs.reserve(num_machines);

  for (int rep = 0; rep < num_replicas; rep++) {
    for (int part = 0; part < num_partitions; part++) {
      // Generate different server ports because tests
      // run on the same machine
      common_config.set_server_port(dis(re));
      int i = rep * num_partitions + part;
      string local_addr = addr + to_string(i);
      configs.push_back(std::make_shared<Configuration>(common_config, local_addr));
    }
  }

  return configs;
}

Transaction* MakeTestTransaction(const ConfigurationPtr& config, TxnId id, const std::vector<KeyEntry>& keys,
                                 const std::variant<string, int>& proc, MachineId coordinator) {
  auto txn = MakeTransaction(keys, proc, coordinator);
  txn->mutable_internal()->set_id(id);

  PopulateInvolvedPartitions(config, *txn);

  return txn;
}

TxnHolder MakeTestTxnHolder(const ConfigurationPtr& config, TxnId id, const std::vector<KeyEntry>& keys,
                            const std::variant<string, int>& proc) {
  auto txn = MakeTestTransaction(config, id, keys, proc);

  vector<Transaction*> lo_txns;
  for (int i = 0; i < txn->internal().involved_replicas_size(); ++i) {
    auto lo = GenerateLockOnlyTxn(txn, txn->internal().involved_replicas(i));
    auto partitioned_lo = GeneratePartitionedTxn(config, lo, config->local_partition(), true);
    if (partitioned_lo != nullptr) {
      lo_txns.push_back(partitioned_lo);
    }
  }
  delete txn;

  CHECK(!lo_txns.empty());

  TxnHolder holder(config, lo_txns[0]);
  for (size_t i = 1; i < lo_txns.size(); ++i) {
    holder.AddLockOnlyTxn(lo_txns[i]);
  }
  return holder;
}

TestSlog::TestSlog(const ConfigurationPtr& config)
    : config_(config),
      storage_(new MemOnlyStorage<Key, Record, Metadata>()),
      broker_(Broker::New(config, kTestModuleTimeout)),
      client_context_(1) {
  client_context_.set(zmq::ctxopt::blocky, false);
  client_socket_ = zmq::socket_t(client_context_, ZMQ_DEALER);
}

void TestSlog::Data(Key&& key, Record&& record) {
  CHECK(config_->key_is_in_local_partition(key))
      << "Key \"" << key << "\" belongs to partition " << config_->partition_of_key(key);
  storage_->Write(key, record);
}

void TestSlog::AddServerAndClient() { server_ = MakeRunnerFor<Server>(config_, broker_, kTestModuleTimeout); }

void TestSlog::AddForwarder() { forwarder_ = MakeRunnerFor<Forwarder>(config_, broker_, storage_, kTestModuleTimeout); }

void TestSlog::AddSequencer() { sequencer_ = MakeRunnerFor<Sequencer>(config_, broker_, kTestModuleTimeout); }

void TestSlog::AddInterleaver() { interleaver_ = MakeRunnerFor<Interleaver>(config_, broker_, kTestModuleTimeout); }

void TestSlog::AddScheduler() { scheduler_ = MakeRunnerFor<Scheduler>(config_, broker_, storage_, kTestModuleTimeout); }

void TestSlog::AddLocalPaxos() { local_paxos_ = MakeRunnerFor<LocalPaxos>(config_, broker_, kTestModuleTimeout); }

void TestSlog::AddGlobalPaxos() { global_paxos_ = MakeRunnerFor<GlobalPaxos>(config_, broker_, kTestModuleTimeout); }

void TestSlog::AddMultiHomeOrderer() {
  multi_home_orderer_ = MakeRunnerFor<MultiHomeOrderer>(config_, broker_, kTestModuleTimeout);
}

void TestSlog::AddOutputChannel(Channel channel) {
  broker_->AddChannel(channel);

  zmq::socket_t socket(*broker_->context(), ZMQ_PULL);
  socket.bind(MakeInProcChannelAddress(channel));
  channels_.insert_or_assign(channel, std::move(socket));
}

zmq::pollitem_t TestSlog::GetPollItemForChannel(Channel channel) {
  auto it = channels_.find(channel);
  if (it == channels_.end()) {
    LOG(FATAL) << "Channel " << channel << " does not exist";
  }
  return {static_cast<void*>(it->second), 0, /* fd */
          ZMQ_POLLIN, 0 /* revent */};
}

unique_ptr<Sender> TestSlog::NewSender() { return std::make_unique<Sender>(broker_->config(), broker_->context()); }

void TestSlog::StartInNewThreads() {
  broker_->StartInNewThreads();
  if (server_) {
    server_->StartInNewThread();
    string endpoint = "tcp://localhost:" + to_string(config_->server_port());
    client_socket_.connect(endpoint);
  }
  if (forwarder_) {
    forwarder_->StartInNewThread();
  }
  if (sequencer_) {
    sequencer_->StartInNewThread();
  }
  if (interleaver_) {
    interleaver_->StartInNewThread();
  }
  if (scheduler_) {
    scheduler_->StartInNewThread();
  }
  if (local_paxos_) {
    local_paxos_->StartInNewThread();
  }
  if (global_paxos_) {
    global_paxos_->StartInNewThread();
  }
  if (multi_home_orderer_) {
    multi_home_orderer_->StartInNewThread();
  }
}

void TestSlog::SendTxn(Transaction* txn) {
  CHECK(server_ != nullptr) << "TestSlog does not have a server";
  api::Request request;
  auto txn_req = request.mutable_txn();
  txn_req->set_allocated_txn(txn);
  SendSerializedProtoWithEmptyDelim(client_socket_, request);
}

Transaction TestSlog::RecvTxnResult() {
  api::Response res;
  if (!RecvDeserializedProtoWithEmptyDelim(client_socket_, res)) {
    LOG(FATAL) << "Malformed response to client transaction.";
    return Transaction();
  }
  const auto& txn = res.txn().txn();
  LOG(INFO) << "Received response. Stream id: " << res.stream_id();
  return txn;
}

}  // namespace slog
