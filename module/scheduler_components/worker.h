#pragma once

#include <functional>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <zmq.hpp>

#include "common/configuration.h"
#include "common/txn_holder.h"
#include "common/types.h"
#include "module/base/networked_module.h"
#include "module/scheduler_components/commands.h"
#include "proto/internal.pb.h"
#include "proto/transaction.pb.h"
#include "storage/storage.h"

namespace slog {

struct TransactionState {
  enum class Phase { READ_LOCAL_STORAGE, WAIT_REMOTE_READ, EXECUTE, COMMIT, FINISH };

  TransactionState(TxnHolder* txn_holder)
      : txn_holder(txn_holder), remote_reads_waiting_on(0), phase(Phase::READ_LOCAL_STORAGE) {}
  TxnHolder* txn_holder;
  uint32_t remote_reads_waiting_on;
  Phase phase;
};

/**
 * A worker executes and commits transactions. Every time it receives from
 * the scheduler a message pertaining to a transaction X, it will either
 * initializes the state for X if X is a new transaction or try to advance
 * X to the subsequent phases as much as possible.
 */
class Worker : public NetworkedModule {
 public:
  Worker(const ConfigurationPtr& config, const std::shared_ptr<Broker>& broker, Channel channel,
         const std::shared_ptr<Storage<Key, Record>>& storage,
         std::chrono::milliseconds poll_timeout_ms = kModuleTimeout);

  static Channel MakeChannel(int worker_num) { return kMaxChannel + worker_num; }

 protected:
  void Initialize() final;
  /**
   * Applies remote read for transactions that are in the WAIT_REMOTE_READ phase.
   * When all remote reads are received, the transaction is moved to the EXECUTE phase.
   */
  void OnInternalRequestReceived(EnvelopePtr&& env) final;

  /**
   * Receives new transaction from the scheduler
   */
  bool OnCustomSocket() final;

 private:
  /**
   * Drives most of the phase transition of a transaction
   */
  void AdvanceTransaction(TxnId txn_id);

  /**
   * Checks master metadata information and reads local data to the transaction
   * buffer, then broadcast local data to other partitions
   */
  void ReadLocalStorage(TxnId txn_id);

  /**
   * Executes the code inside the transaction
   */
  void Execute(TxnId txn_id);

  /**
   * Applies the writes to local storage
   */
  void Commit(TxnId txn_id);

  /**
   * Returns the result back to the scheduler and cleans up the transaction state
   */
  void Finish(TxnId txn_id);

  void NotifyOtherPartitions(TxnId txn_id);

  void SendToCoordinatingServer(TxnId txn_id);

  // Precondition: txn_id must exists in txn states table
  TransactionState& TxnState(TxnId txn_id);

  ConfigurationPtr config_;
  std::shared_ptr<Storage<Key, Record>> storage_;
  std::unique_ptr<Commands> commands_;

  std::unordered_map<TxnId, TransactionState> txn_states_;
};

}  // namespace slog