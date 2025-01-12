#include "module/consensus.h"

#include "common/proto_utils.h"

namespace slog {

using internal::Request;

namespace {

vector<MachineId> GetMembers(const ConfigurationPtr& config) {
  auto local_rep = config->local_replica();
  vector<MachineId> members;
  // Enlist all machines in the same region as members
  for (uint32_t part = 0; part < config->num_partitions(); part++) {
    members.push_back(config->MakeMachineId(local_rep, part));
  }
  return members;
}

}  // namespace

GlobalPaxos::GlobalPaxos(const ConfigurationPtr& config, const shared_ptr<Broker>& broker,
                         std::chrono::milliseconds poll_timeout)
    : SimulatedMultiPaxos(kGlobalPaxos, broker, GetMembers(config), config->local_machine_id(), poll_timeout) {
  for (uint32_t rep = 0; rep < config->num_replicas(); rep++) {
    multihome_orderers_.push_back(config->MakeMachineId(rep, config->leader_partition_for_multi_home_ordering()));
  }
}

void GlobalPaxos::OnCommit(uint32_t slot, uint32_t value, bool is_leader) {
  if (!is_leader) {
    return;
  }
  auto env = NewEnvelope();
  auto order = env->mutable_request()->mutable_forward_batch()->mutable_batch_order();
  order->set_slot(slot);
  order->set_batch_id(value);
  Send(std::move(env), multihome_orderers_, kMultiHomeOrdererChannel);
}

LocalPaxos::LocalPaxos(const ConfigurationPtr& config, const shared_ptr<Broker>& broker,
                       std::chrono::milliseconds poll_timeout)
    : SimulatedMultiPaxos(kLocalPaxos, broker, GetMembers(config), config->local_machine_id(), poll_timeout) {}

void LocalPaxos::OnCommit(uint32_t slot, uint32_t value, bool) {
  auto env = NewEnvelope();
  auto order = env->mutable_request()->mutable_local_queue_order();
  order->set_queue_id(value);
  order->set_slot(slot);
  Send(std::move(env), kInterleaverChannel);
}

}  // namespace slog