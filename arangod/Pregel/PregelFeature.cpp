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
/// @author Simon Grätzer
////////////////////////////////////////////////////////////////////////////////

#include "PregelFeature.h"

#include <atomic>

#include "ApplicationFeatures/ApplicationServer.h"
#include "Basics/MutexLocker.h"
#include "Basics/NumberOfCores.h"
#include "Basics/StringUtils.h"
#include "Cluster/ClusterFeature.h"
#include "Cluster/ClusterInfo.h"
#include "Cluster/ServerState.h"
#include "FeaturePhases/V8FeaturePhase.h"
#include "Pregel/AlgoRegistry.h"
#include "Pregel/Conductor.h"
#include "Pregel/Recovery.h"
#include "Pregel/Utils.h"
#include "Pregel/Worker.h"
#include "Scheduler/Scheduler.h"
#include "Scheduler/SchedulerFeature.h"
#include "Utils/ExecContext.h"
#include "VocBase/LogicalCollection.h"
#include "VocBase/ticks.h"

namespace {
bool authorized(std::string const& user) {
  auto const& exec = arangodb::ExecContext::current();
  if (exec.isSuperuser()) {
    return true;
  }
  return (user == exec.user());
}

bool authorized(std::pair<std::string, std::shared_ptr<arangodb::pregel::Conductor>> const& conductor) {
  return ::authorized(conductor.first);
}

bool authorized(std::pair<std::string, std::shared_ptr<arangodb::pregel::IWorker>> const& worker) {
  return ::authorized(worker.first);
}

/// @brief custom deleter for the PregelFeature.
/// it does nothing, i.e. doesn't delete it. This is because the ApplicationServer
/// is managing the PregelFeature, but we need a shared_ptr to it here as well. The
/// shared_ptr we are using here just tracks the refcount, but doesn't manage the
/// memory
struct NonDeleter {
  void operator()(arangodb::pregel::PregelFeature*) {}
};

}  // namespace

using namespace arangodb;
using namespace arangodb::pregel;

std::pair<Result, uint64_t> PregelFeature::startExecution(
    TRI_vocbase_t& vocbase, std::string algorithm,
    std::vector<std::string> const& vertexCollections,
    std::vector<std::string> const& edgeCollections, 
    std::unordered_map<std::string, std::vector<std::string>> const& edgeCollectionRestrictions,
    VPackSlice const& params) {
  if (isStopping()) {
    return std::make_pair(Result{TRI_ERROR_SHUTTING_DOWN,
                                 "pregel system not available"}, 0);
  }

  ServerState* ss = ServerState::instance();

  // check the access rights to collections
  ExecContext const& exec = ExecContext::current();
  if (!exec.isSuperuser()) {
    TRI_ASSERT(params.isObject());
    VPackSlice storeSlice = params.get("store");
    bool storeResults = !storeSlice.isBool() || storeSlice.getBool();
    for (std::string const& vc : vertexCollections) {
      bool canWrite = exec.canUseCollection(vc, auth::Level::RW);
      bool canRead = exec.canUseCollection(vc, auth::Level::RO);
      if ((storeResults && !canWrite) || !canRead) {
        return std::make_pair(Result{TRI_ERROR_FORBIDDEN}, 0);
      }
    }
    for (std::string const& ec : edgeCollections) {
      bool canWrite = exec.canUseCollection(ec, auth::Level::RW);
      bool canRead = exec.canUseCollection(ec, auth::Level::RO);
      if ((storeResults && !canWrite) || !canRead) {
        return std::make_pair(Result{TRI_ERROR_FORBIDDEN}, 0);
      }
    }
  }

  for (std::string const& name : vertexCollections) {
    if (ss->isCoordinator()) {
      try {
        auto& ci = vocbase.server().getFeature<ClusterFeature>().clusterInfo();
        auto coll = ci.getCollection(vocbase.name(), name);

        if (coll->system()) {
          return std::make_pair(
              Result{TRI_ERROR_BAD_PARAMETER,
                     "Cannot use pregel on system collection"},
              0);
        }

        if (coll->status() == TRI_VOC_COL_STATUS_DELETED || coll->deleted()) {
          return std::make_pair(Result{TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND, name}, 0);
        }
      } catch (...) {
        return std::make_pair(Result{TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND, name}, 0);
      }
    } else if (ss->getRole() == ServerState::ROLE_SINGLE) {
      auto coll = vocbase.lookupCollection(name);

      if (coll == nullptr || coll->status() == TRI_VOC_COL_STATUS_DELETED ||
          coll->deleted()) {
        return std::make_pair(Result{TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND, name}, 0);
      }
    } else {
      return std::make_pair(Result{TRI_ERROR_INTERNAL}, 0);
    }
  }

  std::vector<CollectionID> edgeColls;

  // load edge collection
  for (std::string const& name : edgeCollections) {
    if (ss->isCoordinator()) {
      try {
        auto& ci = vocbase.server().getFeature<ClusterFeature>().clusterInfo();
        auto coll = ci.getCollection(vocbase.name(), name);

        if (coll->system()) {
          return std::make_pair(
              Result{TRI_ERROR_BAD_PARAMETER,
                     "Cannot use pregel on system collection"},
              0);
        }

        if (!coll->isSmart()) {
          std::vector<std::string> eKeys = coll->shardKeys();

          std::string shardKeyAttribute = "vertex";
          if (params.hasKey("shardKeyAttribute")) {
           shardKeyAttribute =  params.get("shardKeyAttribute").copyString();
          }

          if (eKeys.size() != 1 || eKeys[0] != shardKeyAttribute) {
            return std::make_pair(Result{TRI_ERROR_BAD_PARAMETER,
                                         "Edge collection needs to be sharded "
                                         "by shardKeyAttribute parameter ('"
                                         + shardKeyAttribute
                                         + "'), or use SmartGraphs. The current shardKey is: "
                                         + (eKeys.empty() ? "undefined" : "'" + eKeys[0] + "'")

                                         },
                                  0);
          }
        }

        if (coll->status() == TRI_VOC_COL_STATUS_DELETED || coll->deleted()) {
          return std::make_pair(Result{TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND, name}, 0);
        }

        // smart edge collections contain multiple actual collections
        std::vector<std::string> actual = coll->realNamesForRead();

        edgeColls.insert(edgeColls.end(), actual.begin(), actual.end());
      } catch (...) {
        return std::make_pair(Result{TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND, name}, 0);
      }
    } else if (ss->getRole() == ServerState::ROLE_SINGLE) {
      auto coll = vocbase.lookupCollection(name);

      if (coll == nullptr || coll->deleted()) {
        return std::make_pair(Result{TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND, name}, 0);
      }
      std::vector<std::string> actual = coll->realNamesForRead();
      edgeColls.insert(edgeColls.end(), actual.begin(), actual.end());
    } else {
      return std::make_pair(Result{TRI_ERROR_INTERNAL}, 0);
    }
  }

  uint64_t en = createExecutionNumber();
  auto c = std::make_shared<pregel::Conductor>(en, vocbase, vertexCollections,
                                               edgeColls, edgeCollectionRestrictions,
                                               algorithm, params, *this);
  addConductor(std::move(c), en);
  TRI_ASSERT(conductor(en));
  conductor(en)->start();

  return std::make_pair(Result{}, en);
}

uint64_t PregelFeature::createExecutionNumber() {
  return TRI_NewServerSpecificTick();
}

PregelFeature::PregelFeature(application_features::ApplicationServer& server)
    : application_features::ApplicationFeature(server, "Pregel") {
  setOptional(true);
  startsAfter<application_features::V8FeaturePhase>();
}

PregelFeature::~PregelFeature() {
  TRI_ASSERT(_conductors.empty());
  TRI_ASSERT(_workers.empty());
}

size_t PregelFeature::availableParallelism() {
  const size_t procNum = NumberOfCores::getValue();
  return procNum < 1 ? 1 : procNum;
}

void PregelFeature::start() {
  if (ServerState::instance()->isAgent()) {
    return;
  }

  if (ServerState::instance()->isCoordinator()) {
    auto& ci = server().getFeature<ClusterFeature>().clusterInfo();
    _recoveryManager = std::make_unique<RecoveryManager>(ci);
    _recoveryManagerPtr.store(_recoveryManager.get(), std::memory_order_release);
  }
}

void PregelFeature::beginShutdown() {
  TRI_ASSERT(isStopping());
  
  MUTEX_LOCKER(guard, _mutex);
  // cancel all conductors and workers
  for (auto& it : _conductors) {
    it.second.second->cancel();
  }
  for (auto it : _workers) {
    it.second.second->cancelGlobalStep(VPackSlice());
  }
}

void PregelFeature::stop() {}

void PregelFeature::unprepare() {
  MUTEX_LOCKER(guard, _mutex);
  decltype(_conductors) cs = std::move(_conductors);
  decltype(_workers) ws = std::move(_workers);
  guard.unlock();

  // all pending tasks should have been finished by now, and all references
  // to conductors and workers been dropped!
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  for (auto& it : cs) {
    TRI_ASSERT(it.second.second.use_count() == 1);
  }
  
  for (auto it : _workers) {
    TRI_ASSERT(it.second.second.use_count() == 1);
  }
#endif
}

bool PregelFeature::isStopping() const noexcept {
  return server().isStopping();
}

void PregelFeature::addConductor(std::shared_ptr<Conductor>&& c, uint64_t executionNumber) {
  if (isStopping()) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_SHUTTING_DOWN);
  }
  
  std::string user = ExecContext::current().user();
  MUTEX_LOCKER(guard, _mutex);
  _conductors.try_emplace(executionNumber,
                      std::move(user), std::move(c));
}

std::shared_ptr<Conductor> PregelFeature::conductor(uint64_t executionNumber) {
  MUTEX_LOCKER(guard, _mutex);
  auto it = _conductors.find(executionNumber);
  return (it != _conductors.end() && ::authorized(it->second)) ? it->second.second : nullptr;
}

void PregelFeature::addWorker(std::shared_ptr<IWorker>&& w, uint64_t executionNumber) {
  if (isStopping()) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_SHUTTING_DOWN);
  }
  
  std::string user = ExecContext::current().user();
  MUTEX_LOCKER(guard, _mutex);
  _workers.try_emplace(executionNumber,
                   std::move(user), std::move(w));
}

std::shared_ptr<IWorker> PregelFeature::worker(uint64_t executionNumber) {
  MUTEX_LOCKER(guard, _mutex);
  auto it = _workers.find(executionNumber);
  return (it != _workers.end() && ::authorized(it->second)) ? it->second.second : nullptr;
}

void PregelFeature::cleanupConductor(uint64_t executionNumber) {
  MUTEX_LOCKER(guard, _mutex);
  _conductors.erase(executionNumber);
}

void PregelFeature::cleanupWorker(uint64_t executionNumber) {
  // unmapping etc might need a few seconds
  TRI_ASSERT(SchedulerFeature::SCHEDULER != nullptr);
  Scheduler* scheduler = SchedulerFeature::SCHEDULER;
  bool queued = scheduler->queue(RequestLane::INTERNAL_LOW, [this, executionNumber] {
    MUTEX_LOCKER(guard, _mutex);
    _workers.erase(executionNumber);
  });
  if (!queued) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_QUEUE_FULL,
                                   "No thread available to queue cleanup.");
  }
}

void PregelFeature::handleConductorRequest(TRI_vocbase_t& vocbase,
                                           std::string const& path,
                                           VPackSlice const& body,
                                           VPackBuilder& outBuilder) {
  if (isStopping()) {
    return;  // shutdown ongoing
  }

  VPackSlice sExecutionNum = body.get(Utils::executionNumberKey);
  if (!sExecutionNum.isInteger() && !sExecutionNum.isString()) {
    LOG_TOPIC("8410a", ERR, Logger::PREGEL) << "Invalid execution number";
  }
  uint64_t exeNum = 0;
  if (sExecutionNum.isInteger()) {
    exeNum = sExecutionNum.getUInt();
  } else if (sExecutionNum.isString()) {
    exeNum = basics::StringUtils::uint64(sExecutionNum.copyString());
  }
  std::shared_ptr<Conductor> co = conductor(exeNum);
  if (!co) {
    THROW_ARANGO_EXCEPTION_MESSAGE(
        TRI_ERROR_CURSOR_NOT_FOUND, "Conductor not found, invalid execution number: " + std::to_string(exeNum));
  }

  if (path == Utils::finishedStartupPath) {
    co->finishedWorkerStartup(body);
  } else if (path == Utils::finishedWorkerStepPath) {
    outBuilder = co->finishedWorkerStep(body);
  } else if (path == Utils::finishedWorkerFinalizationPath) {
    co->finishedWorkerFinalize(body);
  } else if (path == Utils::finishedRecoveryPath) {
    co->finishedRecoveryStep(body);
  }
}

void PregelFeature::handleWorkerRequest(TRI_vocbase_t& vocbase,
                                        std::string const& path,
                                        VPackSlice const& body,
                                        VPackBuilder& outBuilder) {
  if (isStopping() && path != Utils::finalizeExecutionPath) {
    return;  // shutdown ongoing
  }

  VPackSlice sExecutionNum = body.get(Utils::executionNumberKey);

  if (!sExecutionNum.isInteger()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(
        TRI_ERROR_INTERNAL, "Worker not found, invalid execution number");
  }

  uint64_t exeNum = sExecutionNum.getUInt();
  std::shared_ptr<IWorker> w = worker(exeNum);

  // create a new worker instance if necessary
  if (path == Utils::startExecutionPath) {
    if (w) {
      THROW_ARANGO_EXCEPTION_MESSAGE(
          TRI_ERROR_INTERNAL,
          "Worker with this execution number already exists.");
    }

    addWorker(AlgoRegistry::createWorker(vocbase, body, *this), exeNum);
    worker(exeNum)->setupWorker();  // will call conductor

    return;
  } else if (path == Utils::startRecoveryPath) {
    if (!w) {
      addWorker(AlgoRegistry::createWorker(vocbase, body, *this), exeNum);
    }

    worker(exeNum)->startRecovery(body);
    return;
  } else if (!w) {
    // any other call should have a working worker instance
    LOG_TOPIC("41788", WARN, Logger::PREGEL)
        << "Handling " << path << ", worker " << exeNum << " does not exist";
    THROW_ARANGO_EXCEPTION_FORMAT(
        TRI_ERROR_CURSOR_NOT_FOUND,
        "Handling request %s, but worker %lld does not exist.", path.c_str(), exeNum);
  }

  if (path == Utils::prepareGSSPath) {
    w->prepareGlobalStep(body, outBuilder);
  } else if (path == Utils::startGSSPath) {
    w->startGlobalStep(body);
  } else if (path == Utils::messagesPath) {
    w->receivedMessages(body);
  } else if (path == Utils::cancelGSSPath) {
    w->cancelGlobalStep(body);
  } else if (path == Utils::finalizeExecutionPath) {
    w->finalizeExecution(body, [this, exeNum]() {
      cleanupWorker(exeNum);
    });
  } else if (path == Utils::continueRecoveryPath) {
    w->compensateStep(body);
  } else if (path == Utils::finalizeRecoveryPath) {
    w->finalizeRecovery(body);
  } else if (path == Utils::aqlResultsPath) {
    bool withId = false;
    if (body.isObject()) {
      VPackSlice slice = body.get("withId");
      withId = slice.isBoolean() && slice.getBool();
    }
    w->aqlResult(outBuilder, withId);
  }
}
