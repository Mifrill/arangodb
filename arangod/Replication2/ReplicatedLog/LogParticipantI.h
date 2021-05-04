////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2021-2021 ArangoDB GmbH, Cologne, Germany
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
/// @author Tobias Gödderz
////////////////////////////////////////////////////////////////////////////////

#pragma once

#if (_MSC_VER >= 1)
// suppress warnings:
#pragma warning(push)
// conversion from 'size_t' to 'immer::detail::rbts::count_t', possible loss of data
#pragma warning(disable : 4267)
// result of 32-bit shift implicitly converted to 64 bits (was 64-bit shift intended?)
#pragma warning(disable : 4334)
#endif
#include <immer/map.hpp>
#include <immer/flex_vector.hpp>
#if (_MSC_VER >= 1)
#pragma warning(pop)
#endif

#include <Basics/Guarded.h>
#include <Basics/voc-errors.h>
#include <Futures/Future.h>
#include <Replication2/ReplicatedLog.h>
#include <velocypack/Builder.h>
#include <velocypack/SharedSlice.h>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>
#include "Replication2/ReplicatedLog/Common.h"
#include "Replication2/ReplicatedLog/InMemoryLog.h"
#include "Replication2/ReplicatedLog/LogCore.h"
#include "Replication2/ReplicatedLog/PersistedLog.h"

namespace arangodb::replication2::replicated_log {
struct LogParticipantI {
  [[nodiscard]] virtual auto getStatus() const -> LogStatus = 0;
  virtual ~LogParticipantI() = default;
  [[nodiscard]] virtual auto resign() && -> std::unique_ptr<LogCore> = 0;

  using WaitForPromise = futures::Promise<std::shared_ptr<QuorumData>>;
  using WaitForFuture = futures::Future<std::shared_ptr<QuorumData>>;
  using WaitForQueue = std::multimap<LogIndex, WaitForPromise>;

  [[nodiscard]] virtual auto waitFor(LogIndex index) -> WaitForFuture = 0;

  using WaitForIteratorFuture = futures::Future<std::unique_ptr<LogIterator>>;
  [[nodiscard]] virtual auto waitForIterator(LogIndex index) -> WaitForIteratorFuture;
};
}  // namespace arangodb::replication2::replicated_log
