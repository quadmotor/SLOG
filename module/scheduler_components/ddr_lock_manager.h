#pragma once

// Prevent mixing with other versions
#ifdef LOCK_MANAGER
#error "Only one lock manager can be included"
#endif
#define LOCK_MANAGER

#include <list>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "common/configuration.h"
#include "common/constants.h"
#include "common/json_utils.h"
#include "common/transaction_holder.h"
#include "common/types.h"

using std::list;
using std::optional;
using std::pair;
using std::shared_ptr;
using std::unordered_map;
using std::unordered_set;
using std::vector;

namespace slog {

/**
 * An object of this class represents the tail of the lock queue.
 * We don't update this structure when a transaction releases its
 * locks. Therefore, this structure might contain released transactions
 * so we need to verify any result returned from it.
 */
class LockQueueTail {
 public:
  optional<TxnId> AcquireReadLock(TxnId txn_id);
  vector<TxnId> AcquireWriteLock(TxnId txn_id);

  /* For debugging */
  optional<TxnId> write_lock_requester() const { return write_lock_requester_; }

  /* For debugging */
  vector<TxnId> read_lock_requesters() const { return read_lock_requesters_; }

 private:
  optional<TxnId> write_lock_requester_;
  vector<TxnId> read_lock_requesters_;
};

struct TxnInfo {
  vector<TxnId> waited_by;
  int waiting_for_cnt = 0;
  int pending_parts = 0;

  bool is_ready() const { return waiting_for_cnt == 0 && pending_parts == 0; }
};

/**
 * This is a deterministic lock manager which grants locks for transactions
 * in the order that they request. If transaction X, appears before
 * transaction Y in the log, X always gets all locks before Y.
 *
 * DDR stands for Deterministic Deadlock Resolving. This lock manager is
 * remaster-aware like the RMA lock manager. However, for each lock wait
 * queue, it only keeps track of the tail of the queue. The dependencies
 * between the txns are tracked in a graph, which can be used to deterministically
 * detect and resolve deadlocks.
 *
 * Remastering:
 * Locks are taken on the tuple <key, replica>, using the transaction's
 * master metadata. The masters are checked in the worker, so if two
 * transactions hold separate locks for the same key, then one has an
 * incorrect master and will be aborted. Remaster transactions request the
 * locks for both <key, old replica> and <key, new replica>.
 *
 * TODO: aborts can be detected here, before transactions are dispatched
 */
class DDRLockManager {
 public:
  /**
   * Counts the number of locks a txn needs.
   *
   * For MULTI_HOME txns, the number of needed locks before
   * calling this method can be negative due to its LockOnly
   * txn. Calling this function would bring the number of waited
   * locks back to 0, meaning all locks are granted.
   *
   * @param txn_holder Holder of the transaction to be registered.
   * @return    true if all locks are acquired, false if not and
   *            the transaction is queued up.
   */
  bool AcceptTransaction(const TransactionHolder& txn_holder);

  /**
   * Tries to acquire all locks for a given transaction. If not
   * all locks are acquired, the transaction is queued up to wait
   * for the current holders to release.
   *
   * @param txn_holder Holder of the transaction whose locks are acquired.
   * @return    true if all locks are acquired, false if not and
   *            the transaction is queued up.
   */
  AcquireLocksResult AcquireLocks(const TransactionHolder& txn_holder);

  /**
   * Convenient method to perform txn registration and
   * lock acquisition at the same time.
   */
  AcquireLocksResult AcceptTxnAndAcquireLocks(const TransactionHolder& txn_holder);

  /**
   * Releases all locks that a transaction is holding or waiting for.
   *
   * @param txn_holder Holder of the transaction whose locks are released.
   *            LockOnly txn is not accepted.
   * @return    A set of IDs of transactions that are able to obtain
   *            all of their locks thanks to this release.
   */
  vector<TxnId> ReleaseLocks(const TransactionHolder& txn_holder);

  /**
   * Gets current statistics of the lock manager
   *
   * @param stats A JSON object where the statistics are stored into
   */
  void GetStats(rapidjson::Document& stats, uint32_t level) const;

 private:
  unordered_map<KeyReplica, LockQueueTail> lock_table_;
  unordered_map<TxnId, TxnInfo> txn_info_;
};

}  // namespace slog