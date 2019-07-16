/// @file

#pragma once

#include "database/distributed/graph_db.hpp"
#include "durability/distributed/version.hpp"

namespace database {
class Master final : public GraphDb {
 public:
  explicit Master(Config config = Config());
  ~Master();

  std::unique_ptr<GraphDbAccessor> Access() override;
  std::unique_ptr<GraphDbAccessor> Access(tx::TransactionId) override;

  Storage &storage() override;
  durability::WriteAheadLog &wal() override;
  tx::Engine &tx_engine() override;
  storage::ConcurrentIdMapper<storage::Label> &label_mapper() override;
  storage::ConcurrentIdMapper<storage::EdgeType> &edge_type_mapper() override;
  storage::ConcurrentIdMapper<storage::Property> &property_mapper() override;
  void CollectGarbage() override;
  int WorkerId() const override;
  std::vector<int> GetWorkerIds() const override;
  bool MakeSnapshot(GraphDbAccessor &accessor) override;
  void ReinitializeStorage() override;

  /** Gets this master's endpoint. */
  io::network::Endpoint endpoint() const;
  /** Gets the endpoint of the worker with the given id. */
  // TODO make const once Coordination::GetEndpoint is const.
  io::network::Endpoint GetEndpoint(int worker_id);

  void Start();
  bool AwaitShutdown(std::function<void(void)> call_before_shutdown = [] {});
  void Shutdown();

  distributed::BfsRpcClients &bfs_subcursor_clients() override;
  distributed::DataRpcClients &data_clients() override;
  distributed::UpdatesRpcServer &updates_server() override;
  distributed::UpdatesRpcClients &updates_clients() override;
  distributed::DataManager &data_manager() override;

  distributed::PullRpcClients &pull_clients();
  distributed::PlanDispatcher &plan_dispatcher();
  distributed::IndexRpcClients &index_rpc_clients();

 private:
  std::unique_ptr<impl::Master> impl_;

  utils::Scheduler transaction_killer_;
  std::unique_ptr<utils::Scheduler> snapshot_creator_;
};

class Worker final : public GraphDb {
 public:
  explicit Worker(Config config = Config());
  ~Worker();

  std::unique_ptr<GraphDbAccessor> Access() override;
  std::unique_ptr<GraphDbAccessor> Access(tx::TransactionId) override;

  Storage &storage() override;
  durability::WriteAheadLog &wal() override;
  tx::Engine &tx_engine() override;
  storage::ConcurrentIdMapper<storage::Label> &label_mapper() override;
  storage::ConcurrentIdMapper<storage::EdgeType> &edge_type_mapper() override;
  storage::ConcurrentIdMapper<storage::Property> &property_mapper() override;
  void CollectGarbage() override;
  int WorkerId() const override;
  std::vector<int> GetWorkerIds() const override;
  bool MakeSnapshot(GraphDbAccessor &accessor) override;
  void ReinitializeStorage() override;
  void RecoverWalAndIndexes(durability::RecoveryData *recovery_data);

  /** Gets this worker's endpoint. */
  io::network::Endpoint endpoint() const;
  /** Gets the endpoint of the worker with the given id. */
  // TODO make const once Coordination::GetEndpoint is const.
  io::network::Endpoint GetEndpoint(int worker_id);

  void Start();
  bool AwaitShutdown(std::function<void(void)> call_before_shutdown = [] {});
  void Shutdown();

  distributed::BfsRpcClients &bfs_subcursor_clients() override;
  distributed::DataRpcClients &data_clients() override;
  distributed::UpdatesRpcServer &updates_server() override;
  distributed::UpdatesRpcClients &updates_clients() override;
  distributed::DataManager &data_manager() override;

  distributed::PlanConsumer &plan_consumer();

 private:
  std::unique_ptr<impl::Worker> impl_;

  utils::Scheduler transaction_killer_;
};

/// Creates a new Vertex on the given worker.
/// It is NOT allowed to call this function with this worker's id.
VertexAccessor InsertVertexIntoRemote(
    GraphDbAccessor *dba, int worker_id,
    const std::vector<storage::Label> &labels,
    const std::unordered_map<storage::Property, PropertyValue> &properties,
    std::optional<int64_t> cypher_id);

}  // namespace database