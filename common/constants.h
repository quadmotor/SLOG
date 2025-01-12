#pragma once

#include <string>

#include "common/types.h"

namespace slog {

const auto kModuleTimeout = 1000ms;

const Channel kServerChannel = 1;
const Channel kForwarderChannel = 2;
const Channel kSequencerChannel = 3;
const Channel kMultiHomeOrdererChannel = 4;
const Channel kInterleaverChannel = 5;
const Channel kSchedulerChannel = 6;
const Channel kLocalPaxos = 7;
const Channel kGlobalPaxos = 8;
const Channel kWorkerChannel = 9;
// Broker channels range from kBrokerChannel to kMaxChannel - 1
const Channel kBrokerChannel = 10;
const Channel kMaxChannel = 15;

const uint32_t kMaxNumMachines = 1000;

const uint32_t kPaxosDefaultLeaderPosition = 0;

const size_t kLockTableSizeLimit = 1000000;

const int kRecvRetries = 1000;

/****************************
 *      Statistic Keys
 ****************************/

/* Server */
const char TXN_ID_COUNTER[] = "txn_id_counter";
const char NUM_PENDING_RESPONSES[] = "num_pending_responses";
const char NUM_PARTIALLY_COMPLETED_TXNS[] = "num_partially_completed_txns";
const char PENDING_RESPONSES[] = "pending_responses";
const char PARTIALLY_COMPLETED_TXNS[] = "partially_completed_txns";

/* Forwarder */
const char FORW_BATCH_SIZE_PCTLS[] = "forw_batch_size_pctls";
const char FORW_BATCH_DURATION_MS_PCTLS[] = "forw_batch_duration_ms_pctls";

/* Multi-home orderer */
const char MHO_BATCH_SIZE_PCTLS[] = "mho_batch_size_pctls";
const char MHO_BATCH_DURATION_MS_PCTLS[] = "mho_batch_duration_ms_pctls";

/* Sequencer */
const char SEQ_BATCH_SIZE_PCTLS[] = "seq_batch_size_pctls";
const char SEQ_BATCH_DURATION_MS_PCTLS[] = "seq_batch_duration_ms_pctls";

/* Scheduler */
const char ALL_TXNS[] = "all_txns";
const char NUM_ALL_TXNS[] = "num_all_txns";
const char NUM_LOCKED_KEYS[] = "num_locked_keys";
const char LOCK_MANAGER_TYPE[] = "lock_manager_type";
const char NUM_TXNS_WAITING_FOR_LOCK[] = "num_txns_waiting_for_lock";
const char NUM_WAITING_FOR_PER_TXN[] = "num_waiting_for_per_txn";
const char LOCK_TABLE[] = "lock_table";
const char WAITED_BY_GRAPH[] = "waited_by_graph";
const char TXN_ID[] = "id";
const char TXN_DONE[] = "done";
const char TXN_ABORTING[] = "aborting";
const char TXN_NUM_LO[] = "num_lo";
const char TXN_EXPECTED_NUM_LO[] = "expected_num_lo";

}  // namespace slog