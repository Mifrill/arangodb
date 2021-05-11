////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2021 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Lars Maier
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "Futures/Future.h"
#include "RestHandler/RestVocbaseBaseHandler.h"

#include <velocypack/Slice.h>

#include <map>
#include <utility>

namespace arangodb {

class RestAdminClusterHandler : public RestVocbaseBaseHandler {
 public:
  RestAdminClusterHandler(application_features::ApplicationServer&,
                          GeneralRequest*, GeneralResponse*);
  ~RestAdminClusterHandler() override = default;

 public:
  RestStatus execute() override;
  char const* name() const override final { return "RestAdminClusterHandler"; }
  RequestLane lane() const override final { return RequestLane::CLIENT_SLOW; }

 private:
  static std::string const Health;
  static std::string const NumberOfServers;
  static std::string const Maintenance;
  static std::string const NodeVersion;
  static std::string const NodeStatistics;
  static std::string const NodeEngine;
  static std::string const Statistics;
  static std::string const ShardDistribution;
  static std::string const CollectionShardDistribution;
  static std::string const CleanoutServer;
  static std::string const ResignLeadership;
  static std::string const MoveShard;
  static std::string const QueryJobStatus;
  static std::string const RemoveServer;
  static std::string const RebalanceShards;
  static std::string const ShardStatistics;

  RestStatus handleHealth();
  RestStatus handleNumberOfServers();
  RestStatus handleMaintenance();

  RestStatus setMaintenance(bool wantToActivate);
  RestStatus handlePutMaintenance();
  RestStatus handleGetMaintenance();

  RestStatus handleGetNumberOfServers();
  RestStatus handlePutNumberOfServers();

  RestStatus handleNodeVersion();
  RestStatus handleNodeStatistics();
  RestStatus handleNodeEngine();
  RestStatus handleStatistics();

  RestStatus handleShardDistribution();
  RestStatus handleCollectionShardDistribution();
  RestStatus handleShardStatistics();

  RestStatus handleCleanoutServer();
  RestStatus handleResignLeadership();
  RestStatus handleMoveShard();
  RestStatus handleQueryJobStatus();

  RestStatus handleRemoveServer();
  RestStatus handleRebalanceShards();

 private:


  RestStatus handleSingleServerJob(std::string const& job);
  RestStatus handleCreateSingleServerJob(std::string const& job, std::string const& server);

  typedef std::chrono::steady_clock clock;
  typedef futures::Future<futures::Unit> FutureVoid;


  RestStatus handleProxyGetRequest(std::string const& url, std::string const& serverFromParameter);
  RestStatus handleGetCollectionShardDistribution(std::string const& collection);

  RestStatus handlePostRemoveServer(std::string const& server);

  std::string resolveServerNameID(std::string const&);

 public:
  struct CollectionShardPair {
    std::string collection;
    std::string shard;
    bool isLeader;

    bool operator==(CollectionShardPair const& other) const {
      return collection == other.collection && shard == other.shard &&
             isLeader == other.isLeader;
    }
  };
  void getShardDistribution(std::map<std::string, std::unordered_set<CollectionShardPair>>& distr);

  struct MoveShardDescription {
    std::string collection;
    std::string shard;
    std::string from;
    std::string to;
    bool isLeader;
  };

  using ShardMap = std::map<std::string, std::unordered_set<CollectionShardPair>>;
  using ReshardAlgorithm =
      std::function<void(ShardMap&, std::vector<MoveShardDescription>&)>;

 private:
  FutureVoid handlePostRebalanceShards(const ReshardAlgorithm&);
};
}  // namespace arangodb

