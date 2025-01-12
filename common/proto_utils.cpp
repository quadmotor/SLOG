#include "common/proto_utils.h"

#include <glog/logging.h>

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>

using std::get_if;
using std::string;
namespace slog {

Transaction* MakeTransaction(const std::vector<KeyEntry>& keys, const std::variant<string, int>& proc,
                             MachineId coordinating_server) {
  Transaction* txn = new Transaction();
  for (const auto& key : keys) {
    ValueEntry val;
    val.set_type(key.type);
    if (key.metadata.has_value()) {
      val.mutable_metadata()->set_master(key.metadata->master);
      val.mutable_metadata()->set_counter(key.metadata->counter);
    }
    txn->mutable_keys()->insert({key.key, std::move(val)});
  }
  if (auto code = get_if<string>(&proc); code) {
    txn->set_code(*code);
  } else {
    txn->mutable_remaster()->set_new_master(std::get<int>(proc));
  }
  txn->set_status(TransactionStatus::NOT_STARTED);
  txn->mutable_internal()->set_id(1000);
  txn->mutable_internal()->set_coordinating_server(coordinating_server);

  SetTransactionType(*txn);

  PopulateInvolvedReplicas(*txn);

  return txn;
}

TransactionType SetTransactionType(Transaction& txn) {
  auto txn_internal = txn.mutable_internal();

  bool master_metadata_is_complete = !txn.keys().empty();
  for (const auto& kv : txn.keys()) {
    if (!kv.second.has_metadata()) {
      master_metadata_is_complete = false;
      break;
    }
  }

  if (!master_metadata_is_complete) {
    txn_internal->set_type(TransactionType::UNKNOWN);
    return txn_internal->type();
  }

  bool is_single_home = true;
  auto home = txn.keys().begin()->second.metadata().master();
  for (const auto& kv : txn.keys()) {
    if (kv.second.metadata().master() != home) {
      is_single_home = false;
      break;
    }
  }

#ifdef REMASTER_PROTOCOL_COUNTERLESS
  // Remaster txn will become multi-home
  if (txn.procedure_case() == Transaction::kRemaster) {
    is_single_home = false;
  }
#endif /* REMASTER_PROTOCOL_COUNTERLESS */
  if (is_single_home) {
    txn_internal->set_type(TransactionType::SINGLE_HOME);
    txn_internal->set_home(home);
  } else {
    txn_internal->set_type(TransactionType::MULTI_HOME_OR_LOCK_ONLY);
    txn_internal->set_home(-1);
  }
  return txn_internal->type();
}

Transaction* GenerateLockOnlyTxn(Transaction* txn, uint32_t lo_master, bool in_place) {
  Transaction* lock_only_txn = txn;
  if (!in_place) {
    lock_only_txn = new Transaction(*txn);
  }

  lock_only_txn->mutable_internal()->set_home(lo_master);

#ifdef REMASTER_PROTOCOL_COUNTERLESS
  if (lock_only_txn->procedure_case() == Transaction::kRemaster &&
      lock_only_txn->remaster().new_master() == lo_master) {
    lock_only_txn->mutable_remaster()->set_is_new_master_lock_only(true);
    // For remaster txn, there is only one key in the metadata, and we want to keep that key there
    // in this case, so we return here.
    return lock_only_txn;
  }
#endif /* REMASTER_PROTOCOL_COUNTERLESS */

  return lock_only_txn;
}

Transaction* GeneratePartitionedTxn(const ConfigurationPtr& config, Transaction* txn, uint32_t partition,
                                    bool in_place) {
  Transaction* new_txn = txn;
  if (!in_place) {
    new_txn = new Transaction(*txn);
  }

  vector<bool> involved_replicas(config->num_replicas(), false);

  // Check if the generated subtxn does not intend to lock any key in its home region
  // If this is a remaster txn, it is never redundant
  bool is_redundant = !new_txn->has_remaster();

  // Remove keys that are not in the target partition
  for (auto it = new_txn->mutable_keys()->begin(); it != new_txn->mutable_keys()->end();) {
    if (config->partition_of_key(it->first) != partition) {
      it = new_txn->mutable_keys()->erase(it);
    } else {
      auto master = it->second.metadata().master();
      involved_replicas[master] = true;
      is_redundant &= static_cast<int>(master) != new_txn->internal().home();

      ++it;
    }
  }

  // Shortcut for when the key set is empty or there is no key mastered at the home region
  if (new_txn->keys().empty() || is_redundant) {
    delete new_txn;
    return nullptr;
  }

  // Update involved replica list if needed
  if (new_txn->internal().type() == TransactionType::MULTI_HOME_OR_LOCK_ONLY) {
    new_txn->mutable_internal()->mutable_involved_replicas()->Clear();
    for (size_t r = 0; r < involved_replicas.size(); ++r) {
      if (involved_replicas[r]) {
        new_txn->mutable_internal()->add_involved_replicas(r);
      }
    }
  }

  return new_txn;
}

void PopulateInvolvedReplicas(Transaction& txn) {
  if (txn.internal().type() == TransactionType::UNKNOWN) {
    return;
  }

  if (txn.internal().type() == TransactionType::SINGLE_HOME) {
    txn.mutable_internal()->mutable_involved_replicas()->Clear();
    CHECK(txn.keys().begin()->second.has_metadata());
    txn.mutable_internal()->add_involved_replicas(txn.keys().begin()->second.metadata().master());
    return;
  }

  vector<uint32_t> involved_replicas;
  for (const auto& kv : txn.keys()) {
    CHECK(kv.second.has_metadata());
    involved_replicas.push_back(kv.second.metadata().master());
  }

#ifdef REMASTER_PROTOCOL_COUNTERLESS
  if (txn.procedure_case() == Transaction::kRemaster) {
    involved_replicas.push_back(txn.remaster().new_master());
  }
#endif

  sort(involved_replicas.begin(), involved_replicas.end());
  auto last = unique(involved_replicas.begin(), involved_replicas.end());
  txn.mutable_internal()->mutable_involved_replicas()->Add(involved_replicas.begin(), last);
}

void PopulateInvolvedPartitions(const ConfigurationPtr& config, Transaction& txn) {
  vector<bool> involved_partitions(config->num_partitions(), false);
  vector<bool> active_partitions(config->num_partitions(), false);
  for (const auto& [key, value] : txn.keys()) {
    auto partition = config->partition_of_key(key);
    involved_partitions[partition] = true;
    if (value.type() == KeyType::WRITE) {
      active_partitions[partition] = true;
    }
  }

  for (size_t p = 0; p < involved_partitions.size(); ++p) {
    if (involved_partitions[p]) {
      txn.mutable_internal()->add_involved_partitions(p);
    }
  }

  for (size_t p = 0; p < active_partitions.size(); ++p) {
    if (active_partitions[p]) {
      txn.mutable_internal()->add_active_partitions(p);
    }
  }
}

void MergeTransaction(Transaction& txn, const Transaction& other) {
  if (txn.internal().id() != other.internal().id()) {
    std::ostringstream oss;
    oss << "Cannot merge transactions with different IDs: " << txn.internal().id() << " vs. " << other.internal().id();
    throw std::runtime_error(oss.str());
  }
  if (txn.internal().type() != other.internal().type()) {
    std::ostringstream oss;
    oss << "Cannot merge transactions with different types: " << txn.internal().type() << " vs. "
        << other.internal().type();
    throw std::runtime_error(oss.str());
  }

  if (other.status() == TransactionStatus::ABORTED) {
    txn.set_status(TransactionStatus::ABORTED);
    txn.set_abort_reason(other.abort_reason());
  } else if (txn.status() != TransactionStatus::ABORTED) {
    for (const auto& kv : other.keys()) {
      txn.mutable_keys()->insert(kv);
    }
  }

  txn.mutable_internal()->mutable_events()->MergeFrom(other.internal().events());
  txn.mutable_internal()->mutable_event_times()->MergeFrom(other.internal().event_times());
  txn.mutable_internal()->mutable_event_machines()->MergeFrom(other.internal().event_machines());
}

std::ostream& operator<<(std::ostream& os, const Transaction& txn) {
  os << "Transaction ID: " << txn.internal().id() << "\n";
  os << "Status: " << ENUM_NAME(txn.status(), TransactionStatus) << "\n";
  if (txn.status() == TransactionStatus::ABORTED) {
    os << "Abort reason: " << txn.abort_reason() << "\n";
  }
  os << "Key set:\n";
  os << std::setfill(' ');
  for (const auto& [k, v] : txn.keys()) {
    os << "[" << ENUM_NAME(v.type(), KeyType) << "] " << k << "\n";
    os << "\tValue: " << v.value() << "\n";
    if (v.type() == KeyType::WRITE) {
      os << "\tNew value: " << v.new_value() << "\n";
    }
    os << "\tMetadata: " << v.metadata() << "\n";
  }
  if (!txn.deleted_keys().empty()) {
    os << "Deleted keys: ";
    for (const auto& k : txn.deleted_keys()) {
      os << "\t" << k << "\n";
    }
  }
  os << "Type: " << ENUM_NAME(txn.internal().type(), TransactionType) << "\n";
  if (txn.procedure_case() == Transaction::ProcedureCase::kCode) {
    os << "Code: " << txn.code() << "\n";
  } else {
    os << "New master: " << txn.remaster().new_master() << "\n";
  }
  os << "Coordinating server: " << txn.internal().coordinating_server() << "\n";
  os << "Involved partitions: ";
  for (auto p : txn.internal().involved_partitions()) {
    os << p << " ";
  }
  os << "\n";
  os << "Involved replicas: ";
  for (auto r : txn.internal().involved_replicas()) {
    os << r << " ";
  }
  os << std::endl;
  return os;
}

std::ostream& operator<<(std::ostream& os, const MasterMetadata& metadata) {
  os << "(" << metadata.master() << ", " << metadata.counter() << ")";
  return os;
}

bool operator==(const Transaction& txn1, const Transaction txn2) {
  return txn1.status() == txn2.status() && txn1.keys() == txn2.keys() &&
         txn1.procedure_case() == txn2.procedure_case() && txn1.abort_reason() == txn2.abort_reason() &&
         txn1.internal().id() == txn2.internal().id() && txn1.internal().type() == txn2.internal().type();
}

bool operator==(const MasterMetadata& metadata1, const MasterMetadata& metadata2) {
  return metadata1.master() == metadata2.master() && metadata1.counter() == metadata2.counter();
}

bool operator==(const ValueEntry& val1, const ValueEntry& val2) {
  return val1.value() == val2.value() && val1.new_value() == val2.new_value() && val1.type() == val2.type() &&
         val1.metadata() == val2.metadata();
}

vector<Transaction*> Unbatch(internal::Batch* batch) {
  auto transactions = batch->mutable_transactions();

  vector<Transaction*> buffer(transactions->size());

  for (int i = transactions->size() - 1; i >= 0; i--) {
    auto txn = transactions->ReleaseLast();
    auto txn_internal = txn->mutable_internal();

    // Transfer recorded events from batch to each txn in the batch
    txn_internal->mutable_events()->MergeFrom(batch->events());
    txn_internal->mutable_event_times()->MergeFrom(batch->event_times());
    txn_internal->mutable_event_machines()->MergeFrom(batch->event_machines());

    buffer[i] = txn;
  }

  return buffer;
}

}  // namespace slog