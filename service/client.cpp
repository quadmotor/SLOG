#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>

#include "common/constants.h"
#include "common/json_utils.h"
#include "common/proto_utils.h"
#include "connection/zmq_utils.h"
#include "proto/api.pb.h"
#include "rapidjson/document.h"
#include "rapidjson/istreamwrapper.h"
#include "rapidjson/prettywriter.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"
#include "service/service_utils.h"

DEFINE_string(host, "localhost", "Hostname of the SLOG server to connect to");
DEFINE_uint32(port, 2023, "Port number of the SLOG server to connect to");
DEFINE_uint32(repeat, 1, "Used with \"txn\" command. Send the txn multiple times");
DEFINE_bool(no_wait, false, "Used with \"txn\" command. Don't wait for reply");
DEFINE_int32(truncate, 50, "Number of lines to truncate the output at");

using namespace slog;
using namespace std;

zmq::context_t context(1);
zmq::socket_t server_socket(context, ZMQ_DEALER);

/***********************************************
                Txn Command
***********************************************/

void ExecuteTxn(const char* txn_file) {
  // 1. Read txn from file
  ifstream ifs(txn_file, ios_base::in);
  if (!ifs.is_open()) {
    LOG(ERROR) << "Could not open file " << txn_file;
    return;
  }
  rapidjson::IStreamWrapper json_stream(ifs);
  rapidjson::Document d;
  d.ParseStream(json_stream);
  if (d.HasParseError()) {
    LOG(ERROR) << "Could not parse json in " << txn_file;
    return;
  }

  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  d.Accept(writer);
  LOG(INFO) << "Parsed JSON: " << buffer.GetString();

  vector<KeyEntry> keys;

  // 2. Construct a request
  auto write_set_arr = d["write_set"].GetArray();
  vector<string> write_set;
  for (const auto& v : write_set_arr) {
    keys.emplace_back(v.GetString(), KeyType::WRITE);
  }

  auto read_set_arr = d["read_set"].GetArray();
  for (const auto& v : read_set_arr) {
    keys.emplace_back(v.GetString(), KeyType::READ);
  }

  Transaction* txn;
  if (d.HasMember("new_master")) {
    txn = MakeTransaction(keys, d["new_master"].GetInt());
  } else {
    txn = MakeTransaction(keys, d["code"].GetString());
  }

  api::Request req;
  req.mutable_txn()->set_allocated_txn(txn);

  // 3. Send to the server
  for (uint32_t i = 0; i < FLAGS_repeat; i++) {
    SendSerializedProtoWithEmptyDelim(server_socket, req);
  }

  // 4. Wait and print response
  if (!FLAGS_no_wait) {
    for (uint32_t i = 0; i < FLAGS_repeat; i++) {
      api::Response res;
      if (!RecvDeserializedProtoWithEmptyDelim(server_socket, res)) {
        LOG(FATAL) << "Malformed response";
      } else {
        const auto& txn = res.txn().txn();
        cout << txn;
        if (!txn.internal().events().empty()) {
          cout << left << setw(33) << "Tracing event";
          cout << right << setw(8) << "Machine" << setw(20) << "Time"
               << "\n";
          for (int i = 0; i < txn.internal().events_size(); ++i) {
            cout << left << setw(33) << ENUM_NAME(txn.internal().events(i), TransactionEvent);
            cout << right << setw(8) << txn.internal().event_machines(i) << setw(20) << txn.internal().event_times(i)
                 << "\n";
          }
        }
      }
    }
  }
}

/***********************************************
                Stats Command
***********************************************/
#define TRUNCATED_COUNTER counter##__LINE__
#define TRUNCATED_FOR_EACH(ITER, ARRAY)          \
  int TRUNCATED_COUNTER = 0;                     \
  for (auto& ITER : ARRAY)                       \
    if (++TRUNCATED_COUNTER >= FLAGS_truncate) { \
      std::cout << "(truncated)\n";              \
      break;                                     \
    } else

struct StatsModule {
  ModuleId api_enum;
  function<void(const rapidjson::Document&, uint32_t level)> print_func;
};

void PrintServerStats(const rapidjson::Document& stats, uint32_t level) {
  cout << "Txn id counter: " << stats[TXN_ID_COUNTER].GetUint() << "\n";
  cout << "Pending responses: " << stats[NUM_PENDING_RESPONSES].GetUint() << "\n";
  if (level >= 1) {
    cout << "List of pending responses (txn_id, stream_id):\n";
    TRUNCATED_FOR_EACH(entry, stats[PENDING_RESPONSES].GetArray()) {
      cout << "(" << entry.GetArray()[0].GetUint() << ", " << entry.GetArray()[1].GetUint() << ")\n";
    }
    cout << "\n";
  }
  cout << "Partially completed txns: " << stats[NUM_PARTIALLY_COMPLETED_TXNS].GetUint() << "\n";
  if (level >= 1) {
    cout << "List of partially completed txns: ";
    TRUNCATED_FOR_EACH(txn_id, stats[PARTIALLY_COMPLETED_TXNS].GetArray()) { cout << txn_id.GetUint() << " "; }
    cout << "\n";
  }
  cout << endl;
}

void PrintForwarderStats(const rapidjson::Document& stats, uint32_t) {
  const auto& batch_duration_ms_pctls = stats[FORW_BATCH_DURATION_MS_PCTLS].GetArray();
  const auto& batch_size_pctls = stats[FORW_BATCH_SIZE_PCTLS].GetArray();
  cout << "Batch duration percentiles (ms)\n";
  if (batch_duration_ms_pctls.Empty()) {
    cout << "\tNo data\n";
  } else {
    cout << fixed << setprecision(3);
    for (size_t i = 0; i < kPctlLevels.size(); ++i) {
      cout << setw(4) << kPctlLevels[i] << ": " << batch_duration_ms_pctls[i].GetFloat() << "\n";
    }
  }
  cout << "\n";
  cout << "Batch size percentiles\n";
  if (batch_size_pctls.Empty()) {
    cout << "\tNo data\n";
  } else {
    for (size_t i = 0; i < kPctlLevels.size(); ++i) {
      cout << setw(4) << kPctlLevels[i] << ": " << batch_size_pctls[i].GetInt() << "\n";
    }
  }
}

void PrintMHOrdererStats(const rapidjson::Document& stats, uint32_t) {
  const auto& batch_duration_ms_pctls = stats[MHO_BATCH_DURATION_MS_PCTLS].GetArray();
  const auto& batch_size_pctls = stats[MHO_BATCH_SIZE_PCTLS].GetArray();
  cout << "Batch duration percentiles (ms)\n";
  if (batch_duration_ms_pctls.Empty()) {
    cout << "\tNo data\n";
  } else {
    cout << fixed << setprecision(3);
    for (size_t i = 0; i < kPctlLevels.size(); ++i) {
      cout << setw(4) << kPctlLevels[i] << ": " << batch_duration_ms_pctls[i].GetFloat() << "\n";
    }
  }
  cout << "\n";
  cout << "Batch size percentiles\n";
  if (batch_size_pctls.Empty()) {
    cout << "\tNo data\n";
  } else {
    for (size_t i = 0; i < kPctlLevels.size(); ++i) {
      cout << setw(4) << kPctlLevels[i] << ": " << batch_size_pctls[i].GetInt() << "\n";
    }
  }
}

void PrintSequencerStats(const rapidjson::Document& stats, uint32_t) {
  const auto& batch_duration_ms_pctls = stats[SEQ_BATCH_DURATION_MS_PCTLS].GetArray();
  const auto& batch_size_pctls = stats[SEQ_BATCH_SIZE_PCTLS].GetArray();
  cout << "Batch duration percentiles (ms)\n";
  if (batch_duration_ms_pctls.Empty()) {
    cout << "\tNo data\n";
  } else {
    cout << fixed << setprecision(3);
    for (size_t i = 0; i < kPctlLevels.size(); ++i) {
      cout << setw(4) << kPctlLevels[i] << ": " << batch_duration_ms_pctls[i].GetFloat() << "\n";
    }
  }
  cout << "\n";
  cout << "Batch size percentiles\n";
  if (batch_size_pctls.Empty()) {
    cout << "\tNo data\n";
  } else {
    for (size_t i = 0; i < kPctlLevels.size(); ++i) {
      cout << setw(4) << kPctlLevels[i] << ": " << batch_size_pctls[i].GetInt() << "\n";
    }
  }
}

string LockModeStr(LockMode mode) {
  switch (mode) {
    case LockMode::UNLOCKED:
      return "UNLOCKED";
    case LockMode::READ:
      return "READ";
    case LockMode::WRITE:
      return "WRITE";
  }
  return "<error>";
}

void PrintSchedulerStats(const rapidjson::Document& stats, uint32_t level) {
  cout << "Number of active txns: " << stats[NUM_ALL_TXNS].GetUint() << "\n";
  cout << "\nACTIVE TRANSACTIONS\n\n";
  if (level == 0) {
    TRUNCATED_FOR_EACH(txn_id, stats[ALL_TXNS].GetArray()) { cout << txn_id.GetUint() << " "; }
  } else if (level >= 1) {
    TRUNCATED_FOR_EACH(txn, stats[ALL_TXNS].GetArray()) {
      cout << "\t";
      cout << TXN_ID << ": " << txn[TXN_ID].GetUint() << ", ";
      cout << TXN_DONE << ": " << txn[TXN_DONE].GetBool() << ", ";
      cout << TXN_ABORTING << ": " << txn[TXN_ABORTING].GetBool() << ", ";
      cout << TXN_NUM_LO << ": " << txn[TXN_NUM_LO].GetInt() << ", ";
      cout << TXN_EXPECTED_NUM_LO << ": " << txn[TXN_EXPECTED_NUM_LO].GetInt() << "\n";
    }
  }

  cout << "\n";
  cout << "Waiting txns: " << stats[NUM_TXNS_WAITING_FOR_LOCK].GetUint() << "\n";

  // 0: OLD or RMA. 1: DDR
  auto lock_man_type = stats[LOCK_MANAGER_TYPE].GetInt();

  if (lock_man_type == 0) {
    cout << "Locked keys: " << stats[NUM_LOCKED_KEYS].GetUint() << "\n";
  }

  if (level >= 1) {
    cout << "\n\nTRANSACTION DEPENDENCIES\n\n";
    if (lock_man_type == 0) {
      cout << setw(10) << "Txn" << setw(18) << "# waiting for"
           << "\n";
      TRUNCATED_FOR_EACH(it, stats[NUM_WAITING_FOR_PER_TXN].GetArray()) {
        const auto& entry = it.GetArray();
        cout << setw(10) << entry[0].GetUint() << setw(18) << entry[1].GetInt() << "\n";
      }
    } else {
      cout << setw(10) << "Txn"
           << "\tTxns waiting for this txn"
           << "\n";
      TRUNCATED_FOR_EACH(it, stats[WAITED_BY_GRAPH].GetArray()) {
        const auto& entry = it.GetArray();
        cout << setw(10) << entry[0].GetUint() << "\t";
        TRUNCATED_FOR_EACH(e, entry[1].GetArray()) { cout << e.GetUint() << " "; }
        cout << "\n";
      }
    }
  }

  if (level >= 2) {
    cout << "\n\nLOCK TABLE\n\n";
    TRUNCATED_FOR_EACH(it, stats[LOCK_TABLE].GetArray()) {
      const auto& entry = it.GetArray();
      if (lock_man_type == 0) {
        auto lock_mode = static_cast<LockMode>(entry[1].GetUint());
        cout << "Key: " << entry[0].GetString() << ". Mode: " << LockModeStr(lock_mode) << "\n";
        cout << "\tHolders: ";
        for (const auto& holder : entry[2].GetArray()) {
          cout << holder.GetUint() << " ";
        }
        cout << "\n";

        cout << "\tWaiters: ";
        TRUNCATED_FOR_EACH(waiter, entry[3].GetArray()) {
          auto txn_and_mode = waiter.GetArray();
          cout << "(" << txn_and_mode[0].GetUint() << ", "
               << LockModeStr(static_cast<LockMode>(txn_and_mode[1].GetUint())) << ") ";
        }
      } else {
        cout << "Key: " << entry[0].GetString() << "\n";
        cout << "\tWrite: " << entry[1].GetUint() << "\n";
        cout << "\tReads: ";
        TRUNCATED_FOR_EACH(requester, entry[2].GetArray()) { cout << requester.GetUint() << " "; }
      }
      cout << "\n";
    }
  }
  cout << endl;
}

const unordered_map<string, StatsModule> STATS_MODULES = {
    {"server", {ModuleId::SERVER, PrintServerStats}},
    {"forwarder", {ModuleId::FORWARDER, PrintForwarderStats}},
    {"mhorderer", {ModuleId::MHORDERER, PrintMHOrdererStats}},
    {"sequencer", {ModuleId::SEQUENCER, PrintSequencerStats}},
    {"scheduler", {ModuleId::SCHEDULER, PrintSchedulerStats}}};

void ExecuteStats(const char* module, uint32_t level) {
  auto stats_module_it = STATS_MODULES.find(string(module));
  if (stats_module_it == STATS_MODULES.end()) {
    LOG(ERROR) << "Invalid module: " << module;
    return;
  }
  auto& stats_module = stats_module_it->second;

  // 1. Construct a request for stats
  api::Request req;
  req.mutable_stats()->set_module(stats_module.api_enum);
  req.mutable_stats()->set_level(level);

  // 2. Send to the server
  SendSerializedProtoWithEmptyDelim(server_socket, req);

  // 3. Wait and print response
  api::Response res;
  if (!RecvDeserializedProtoWithEmptyDelim(server_socket, res)) {
    LOG(FATAL) << "Malformed response";
  } else {
    rapidjson::Document stats;
    stats.Parse(res.stats().stats_json().c_str());

    rapidjson::StringBuffer buf;
    rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buf);
    stats.Accept(writer);
    VLOG(1) << "Stats object: " << buf.GetString();

    stats_module.print_func(stats, level);
  }
}

int main(int argc, char* argv[]) {
  slog::InitializeService(&argc, &argv);
  string endpoint = "tcp://" + FLAGS_host + ":" + to_string(FLAGS_port);
  LOG(INFO) << "Connecting to " << endpoint;
  server_socket.connect(endpoint);
  auto cmd_argc = argc - 1;
  if (cmd_argc == 0) {
    LOG(ERROR) << "Please specify a command";
    return 1;
  }

  if (strcmp(argv[1], "txn") == 0) {
    if (cmd_argc != 2) {
      LOG(ERROR) << "Invalid number of arguments for the \"txn\" command:\n"
                 << "Usage: txn <txn_file>";
      return 1;
    }
    ExecuteTxn(argv[2]);
  } else if (strcmp(argv[1], "stats") == 0) {
    if (cmd_argc < 2 || cmd_argc > 3) {
      LOG(ERROR) << "Invalid number of arguments for the \"stats\" command:\n"
                 << "Usage: stats <module> [<level>]";
      return 1;
    }
    uint32_t level = 0;
    if (cmd_argc == 3) {
      level = std::stoul(argv[3]);
    }
    ExecuteStats(argv[2], level);
  } else {
    LOG(ERROR) << "Invalid command: " << argv[1];
  }
  return 0;
}