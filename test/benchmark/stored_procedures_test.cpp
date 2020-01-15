#include <gtest/gtest.h>

#include "common/proto_utils.h"
#include "benchmark/stored_procedures.h"

using namespace std;
using namespace slog;

TEST(StoredProceduresTest, KeyValue) {
  auto txn = MakeTransaction(
      {},
      {"key1", "key2", "key3"},
      "SET key1 value1\n"
      "SET key2 value2\n"
      "DEL key3");

  KeyValueStoredProcedures proc;
  proc.Execute(txn);
  ASSERT_EQ(txn.status(), TransactionStatus::COMMITTED);
  ASSERT_EQ(txn.write_set_size(), 3);
  ASSERT_EQ(txn.write_set().at("key1"), "value1");
  ASSERT_EQ(txn.write_set().at("key2"), "value2");
  ASSERT_EQ(txn.delete_set_size(), 1);
  ASSERT_EQ(txn.delete_set(0), "key3");
}

TEST(StoredProceduresTest, KeyValueAborted) {
  // Invalid code
  auto txn = MakeTransaction(
      {},
      {"key1", "key2", "key3"},
      "SET key1");

  KeyValueStoredProcedures proc;
  proc.Execute(txn);
  ASSERT_EQ(txn.status(), TransactionStatus::ABORTED);
}

TEST(StoredProceduresTest, KeyValueOnlyWritesKeysInWriteSet) {
  auto txn = MakeTransaction(
      {},
      {"key1"},
      "SET key1 value1\n"
      "SET key2 value2\n"
      "DEL key3");

  KeyValueStoredProcedures proc;
  proc.Execute(txn);
  ASSERT_EQ(txn.status(), TransactionStatus::COMMITTED);
  ASSERT_EQ(txn.write_set_size(), 1);
  ASSERT_EQ(txn.write_set().at("key1"), "value1");
}