#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "communication.hpp"

const int NUM_WORKERS = 1;

class Txn : public SenderMessage {
 public:
  Txn(ChannelRefT channel, int64_t id) : SenderMessage(channel), id_(id) {}
  int64_t id() const { return id_; }

  template <class Archive>
  void serialize(Archive &archive) {
    archive(cereal::base_class<SenderMessage>(this), id_);
  }

 private:
  int64_t id_;
};

class CreateNodeTxn : public Txn {
 public:
  CreateNodeTxn(ChannelRefT channel, int64_t id) : Txn(channel, id) {}

  template <class Archive>
  void serialize(Archive &archive) {
    archive(cereal::base_class<Txn>(this));
  }
};

class CountNodesTxn : public Txn {
 public:
  CountNodesTxn(ChannelRefT channel, int64_t id) : Txn(channel, id) {}

  template <class Archive>
  void serialize(Archive &archive) {
    archive(cereal::base_class<Txn>(this));
  }
};

class CountNodesTxnResult : public Message {
 public:
  CountNodesTxnResult(int64_t count) : count_(count) {}
  int64_t count() const { return count_; }

  template <class Archive>
  void serialize(Archive &archive) {
    archive(count_);
  }

 private:
  int64_t count_;
};

class CommitRequest : public SenderMessage {
 public:
  CommitRequest(ChannelRefT sender, int64_t worker_id)
      : SenderMessage(sender), worker_id_(worker_id) {}
  int64_t worker_id() { return worker_id_; }

  template <class Archive>
  void serialize(Archive &archive) {
    archive(cereal::base_class<SenderMessage>(this), worker_id_);
  }

 private:
  int64_t worker_id_;
};

class AbortRequest : public SenderMessage {
 public:
  AbortRequest(ChannelRefT sender, int64_t worker_id)
      : SenderMessage(sender), worker_id_(worker_id) {}
  int64_t worker_id() { return worker_id_; }

  template <class Archive>
  void serialize(Archive &archive) {
    archive(cereal::base_class<SenderMessage>(this), worker_id_);
  }

 private:
  int64_t worker_id_;
};

class CommitDirective : public Message {
  template <class Archive>
  void serialize(Archive &archive) {
    archive(cereal::base_class<Message>(this));
  }
};

class AbortDirective : public Message {
  template <class Archive>
  void serialize(Archive &archive) {
    archive(cereal::base_class<Message>(this));
  }
};

class Query : public Message {
 public:
  Query(std::string query) : Message(), query_(query) {}
  std::string query() const { return query_; }

  template <class Archive>
  void serialize(Archive &archive) {
    archive(cereal::base_class<Message>(this), query_);
  }

 private:
  std::string query_;
};

class Quit : public Message {
  template <class Archive>
  void serialize(Archive &archive) {
    archive(cereal::base_class<Message>(this));
  }
};

class Master : public Reactor {
 public:
  Master(System *system, std::string name) : Reactor(system, name), next_xid_(1) {}

  virtual void Run() {
    auto stream = main_.first;
    FindWorkers();

    std::cout << "Master is active" << std::endl;
    while (true) {
      auto m = stream->AwaitEvent();
      if (Query *query = dynamic_cast<Query *>(m.get())) {
        ProcessQuery(query);
        break;  // process only the first query
      } else {
        std::cerr << "unknown message\n";
        exit(1);
      }
    }

    stream->OnEvent([this](const Message &msg, EventStream::Subscription& subscription) {
      std::cout << "Processing Query via Callback" << std::endl;
      const Query &query =
          dynamic_cast<const Query &>(msg);  // exception bad_cast
      ProcessQuery(&query);
      subscription.unsubscribe();
    });
  }

 private:
  void ProcessQuery(const Query *query) {
    if (query->query() == "create node") {
      PerformCreateNode();
    } else if (query->query() == "count nodes") {
      PerformCountNodes();
    } else {
      std::cout << "got query: " << query->query() << std::endl;
    }
  }

  void PerformCreateNode() {
    int worker_id = rand() % NUM_WORKERS;
    int64_t xid = GetTransactionId();
    std::string txn_channel_name = GetTxnName(xid);
    auto connector = Open(txn_channel_name);
    auto stream = connector.first;

    auto create_node_txn =
        std::make_unique<CreateNodeTxn>(connector.second, xid);
    channels_[worker_id]->Send(std::move(create_node_txn));
    auto m = stream->AwaitEvent();
    if (CommitRequest *req = dynamic_cast<CommitRequest *>(m.get())) {
      req->sender()->Send(std::make_unique<CommitDirective>());
    } else if (AbortRequest *req = dynamic_cast<AbortRequest *>(m.get())) {
      req->sender()->Send(std::make_unique<AbortDirective>());
    } else {
      std::cerr << "unknown message\n";
      exit(1);
    }
    Close(txn_channel_name);
  }

  void PerformCountNodes() {
    int64_t xid = GetTransactionId();
    std::string txn_channel_name = GetTxnName(xid);
    auto connector = Open(txn_channel_name);
    auto stream = connector.first;
    for (int w_id = 0; w_id < NUM_WORKERS; ++w_id)
      channels_[w_id]->Send(
          std::make_unique<CountNodesTxn>(connector.second, xid));

    std::vector<ChannelRefT> txn_channels;
    txn_channels.resize(NUM_WORKERS, nullptr);
    bool commit = true;
    for (int responds = 0; responds < NUM_WORKERS; ++responds) {
      auto m = stream->AwaitEvent();
      if (CommitRequest *req = dynamic_cast<CommitRequest *>(m.get())) {
        txn_channels[req->worker_id()] = req->sender();
        commit &= true;
      } else if (AbortRequest *req = dynamic_cast<AbortRequest *>(m.get())) {
        txn_channels[req->worker_id()] = req->sender();
        commit = false;
      } else {
        std::cerr << "unknown message\n";
        exit(1);
      }
    }

    if (commit) {
      for (int w_id = 0; w_id < NUM_WORKERS; ++w_id)
        txn_channels[w_id]->Send(std::make_unique<CommitDirective>());
    } else {
      for (int w_id = 0; w_id < NUM_WORKERS; ++w_id)
        txn_channels[w_id]->Send(std::make_unique<AbortDirective>());
    }

    int64_t count = 0;
    for (int responds = 0; responds < NUM_WORKERS; ++responds) {
      auto m = stream->AwaitEvent();
      if (CountNodesTxnResult *cnt =
              dynamic_cast<CountNodesTxnResult *>(m.get())) {
        count += cnt->count();
      } else {
        std::cerr << "unknown message\n";
        exit(1);
      }
    }

    Close(txn_channel_name);
    std::cout << "graph has " << count << " vertices" << std::endl;
  }

  int64_t GetTransactionId() { return next_xid_++; }

  std::string GetWorkerName(int worker_id) {
    return "worker" + std::to_string(worker_id);
  }

  std::string GetTxnName(int txn_id) { return "txn" + std::to_string(txn_id); }

  void FindWorkers() {
    channels_.resize(NUM_WORKERS, nullptr);
    int workers_found = 0;
    while (workers_found < NUM_WORKERS) {
      for (int64_t w_id = 0; w_id < NUM_WORKERS; ++w_id) {
        if (channels_[w_id] == nullptr) {
          // TODO: Resolve worker channel using the network service.
          channels_[w_id] = system_->FindChannel(GetWorkerName(w_id), "main");
          if (channels_[w_id] != nullptr) ++workers_found;
        }
      }
      if (workers_found < NUM_WORKERS)
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
  }

  // TODO: Why is master atomic, it should be unique?
  std::atomic<int64_t> next_xid_;
  std::vector<std::shared_ptr<Channel>> channels_;
};

class Worker : public Reactor {
 public:
  Worker(System *system, std::string name, int64_t id) : Reactor(system, name),
    worker_id_(id) {}

  virtual void Run() {
    std::cout << "worker " << worker_id_ << " is active" << std::endl;
    auto stream = main_.first;
    FindMaster();
    while (true) {
      auto m = stream->AwaitEvent();
      if (Txn *txn = dynamic_cast<Txn *>(m.get())) {
        HandleTransaction(txn);
      } else {
        std::cerr << "unknown message\n";
        exit(1);
      }
    }
  }

 private:
  void HandleTransaction(Txn *txn) {
    if (CreateNodeTxn *create_txn = dynamic_cast<CreateNodeTxn *>(txn)) {
      HandleCreateNode(create_txn);
    } else if (CountNodesTxn *cnt_txn = dynamic_cast<CountNodesTxn *>(txn)) {
      HandleCountNodes(cnt_txn);
    } else {
      std::cerr << "unknown transaction\n";
      exit(1);
    }
  }

  void HandleCreateNode(CreateNodeTxn *txn) {
    auto connector = Open(GetTxnChannelName(txn->id()));
    auto stream = connector.first;
    auto masterChannel = txn->sender();
    // TODO: Do the actual commit.
    masterChannel->Send(
        std::make_unique<CommitRequest>(connector.second, worker_id_));
    auto m = stream->AwaitEvent();
    if (dynamic_cast<CommitDirective *>(m.get())) {
      // TODO: storage_.CreateNode();
    } else if (dynamic_cast<AbortDirective *>(m.get())) {
      // TODO: Rollback.
    } else {
      std::cerr << "unknown message\n";
      exit(1);
    }
    Close(GetTxnChannelName(txn->id()));
  }

  void HandleCountNodes(CountNodesTxn *txn) {
    auto connector = Open(GetTxnChannelName(txn->id()));
    auto stream = connector.first;
    auto masterChannel = txn->sender();

    // TODO: Fix this hack -- use the storage.
    int num = 123;

    masterChannel->Send(
        std::make_unique<CommitRequest>(connector.second, worker_id_));
    auto m = stream->AwaitEvent();
    if (dynamic_cast<CommitDirective *>(m.get())) {
      masterChannel->Send(std::make_unique<CountNodesTxnResult>(num));
    } else if (dynamic_cast<AbortDirective *>(m.get())) {
      // send nothing
    } else {
      std::cerr << "unknown message\n";
      exit(1);
    }
    Close(GetTxnChannelName(txn->id()));
  }

  // TODO: Don't repeat code from Master.
  std::string GetTxnChannelName(int64_t transaction_id) {
    return "txn" + std::to_string(transaction_id);
  }

  void FindMaster() {
    // TODO: Replace with network service and channel resolution.
    while (!(master_channel_ = system_->FindChannel("master", "main")))
      std::this_thread::sleep_for(std::chrono::seconds(1));
  }

  std::shared_ptr<Channel> master_channel_ = nullptr;
  int worker_id_;
  // Storage storage_;
};

void ClientMain(System *system) {
  std::shared_ptr<Channel> channel = nullptr;
  // TODO: Replace this with network channel resolution.
  while (!(channel = system->FindChannel("master", "main")))
    std::this_thread::sleep_for(std::chrono::seconds(1));
  std::cout << "I/O Client Main active" << std::endl;

  bool active = true;
  while (active) {
    std::string s;
    std::getline(std::cin, s);
    if (s == "quit") {
      active = false;
      channel->Send(std::make_unique<Quit>());
    } else {
      channel->Send(std::make_unique<Query>(s));
    }
  }
}

int main() {
  System system;
  system.Spawn<Master>("master");
  std::thread client(ClientMain, &system);
  for (int i = 0; i < NUM_WORKERS; ++i)
    system.Spawn<Worker>("worker" + std::to_string(i), i);
  system.AwaitShutdown();
  return 0;
}