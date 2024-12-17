// Copyright (c) 2015-present, Qihoo, Inc.  All rights reserved.
// This source code is licensed under the BSD-style license found in the
// LICENSE file in the root directory of this source tree. An additional grant
// of patent rights can be found in the PATENTS file in the same directory.

#ifndef PIKA_CLIENT_CONN_H_
#define PIKA_CLIENT_CONN_H_

#include <bitset>
#include <utility>
#include <variant>

#include "acl.h"
#include "include/pika_command.h"
#include "include/pika_define.h"
#include "net/src/memoey_pool.h"

// TODO: stat time costing in write out data to connfd
struct TimeStat {
  TimeStat() = default;
  void Reset() {
    enqueue_ts_ = dequeue_ts_ = 0;
    process_done_ts_ = 0;
    before_queue_ts_ = 0;
  }

  uint64_t start_ts() const { return enqueue_ts_; }

  uint64_t total_time() const { return process_done_ts_ > enqueue_ts_ ? process_done_ts_ - enqueue_ts_ : 0; }

  uint64_t queue_time() const { return dequeue_ts_ > enqueue_ts_ ? dequeue_ts_ - enqueue_ts_ : 0; }

  uint64_t process_time() const { return process_done_ts_ > dequeue_ts_ ? process_done_ts_ - dequeue_ts_ : 0; }

  uint64_t before_queue_time() const { return process_done_ts_ > dequeue_ts_ ? before_queue_ts_ - enqueue_ts_ : 0; }

  uint64_t enqueue_ts_;
  uint64_t dequeue_ts_;
  uint64_t before_queue_ts_;
  uint64_t process_done_ts_;
};

class PikaClientConn : public net::RedisConn {
 public:
  using WriteCompleteCallback = std::function<void()>;

  struct BgTaskArg {
    std::shared_ptr<Cmd> cmd_ptr;
    std::shared_ptr<PikaClientConn> conn_ptr;
    std::vector<net::RedisCmdArgsType> redis_cmds;
    std::shared_ptr<std::string> resp_ptr;
    LogOffset offset;
    std::string db_name;
    bool cache_miss_in_rtc_;
  };

  struct TxnStateBitMask {
   public:
    static constexpr uint8_t Start = 0;
    static constexpr uint8_t InitCmdFailed = 1;
    static constexpr uint8_t WatchFailed = 2;
    static constexpr uint8_t Execing = 3;
  };

  PikaClientConn(int fd, const std::string& ip_port, net::Thread* server_thread, net::NetMultiplexer* mpx,
                 const net::HandleType& handle_type, int max_conn_rbuf_size);
  ~PikaClientConn() = default;

  bool IsInterceptedByRTC(std::string& opt);

  void ProcessRedisCmds(const std::vector<net::RedisCmdArgsType>& argvs, bool async, std::string* response) override;

  bool ReadCmdInCache(const net::RedisCmdArgsType& argv, const std::string& opt);
  void BatchExecRedisCmd(const std::vector<net::RedisCmdArgsType>& argvs, bool cache_miss_in_rtc);
  int DealMessage(const net::RedisCmdArgsType& argv, std::string* response) override { return 0; }
  static void DoBackgroundTask(void* arg);

  bool IsPubSub() { return is_pubsub_; }
  void SetIsPubSub(bool is_pubsub) { is_pubsub_ = is_pubsub; }
  void SetCurrentDb(const std::string& db_name) { current_db_ = db_name; }
  void SetWriteCompleteCallback(WriteCompleteCallback cb) { write_completed_cb_ = std::move(cb); }
  const std::string& GetCurrentTable() override { return current_db_; }

  void DoAuth(const std::shared_ptr<User>& user);

  void UnAuth(const std::shared_ptr<User>& user);

  bool IsAuthed() const;
  void InitUser();
  bool AuthRequired() const;

  std::string UserName() const;

  // Txn
  std::queue<std::shared_ptr<Cmd>> GetTxnCmdQue();
  void PushCmdToQue(std::shared_ptr<Cmd> cmd);
  void ClearTxnCmdQue();
  void SetTxnWatchFailState(bool is_failed);
  void SetTxnInitFailState(bool is_failed);
  void SetTxnStartState(bool is_start);
  void AddKeysToWatch(const std::vector<std::string>& db_keys);
  void RemoveWatchedKeys();
  void SetTxnFailedFromKeys(const std::vector<std::string>& db_keys);
  void SetTxnFailedIfKeyExists(const std::string target_db_name = "");
  void ExitTxn();
  bool IsInTxn();
  bool IsTxnInitFailed();
  bool IsTxnWatchFailed();
  bool IsTxnExecing(void);

  net::ServerThread* server_thread() { return server_thread_; }
  void ClientInfoToString(std::string* info, const std::string& cmdName);

  std::atomic<int> resp_num;
  std::vector<std::shared_ptr<std::string>> resp_array;

  std::shared_ptr<TimeStat> time_stat_;

  void SetMemoryPool(net::MemoryPool *memory_pool) override{ memory_pool_ = memory_pool; }

 private:
  net::ServerThread* const server_thread_;
  std::string current_db_;
  WriteCompleteCallback write_completed_cb_;
  bool is_pubsub_ = false;
  std::queue<std::shared_ptr<Cmd>> txn_cmd_que_;
  std::bitset<16> txn_state_;
  std::unordered_set<std::string> watched_db_keys_;
  std::mutex txn_state_mu_;

  bool authenticated_ = false;
  std::shared_ptr<User> user_;

  net::MemoryPool *memory_pool_;

  std::variant<std::shared_ptr<Cmd>, Cmd*> DoCmd(const PikaCmdArgsType& argv, const std::string& opt,
                             const std::shared_ptr<std::string>& resp_ptr, bool cache_miss_in_rtc);

  void ProcessSlowlog(const PikaCmdArgsType& argv, uint64_t do_duration);
  void ProcessMonitor(const PikaCmdArgsType& argv);

  void ExecRedisCmd(const PikaCmdArgsType& argv, std::shared_ptr<std::string>& resp_ptr, bool cache_miss_in_rtc);
  void TryWriteResp();
};

struct ClientInfo {
  int fd;
  std::string ip_port;
  int64_t last_interaction = 0;
  std::shared_ptr<PikaClientConn> conn;
};

extern bool AddrCompare(const ClientInfo& lhs, const ClientInfo& rhs);
extern bool IdleCompare(const ClientInfo& lhs, const ClientInfo& rhs);

#endif
