//
// Created by lars on 16/04/2021.
//

#include <velocypack/Iterator.h>
#include <velocypack/Parser.h>

#include <Cluster/ServerState.h>
#include <Network/ConnectionPool.h>
#include <Network/Methods.h>
#include <Network/NetworkFeature.h>
#include <Replication2/ReplicatedLog.h>
#include <velocypack/velocypack-aliases.h>

#include "Basics/overload.h"
#include "RestLogHandler.h"

using namespace arangodb;
using namespace arangodb::replication2;

RestStatus RestLogHandler::execute() {
  switch (_request->requestType()) {
    case rest::RequestType::GET:
      return handleGetRequest();
    case rest::RequestType::POST:
      return handlePostRequest();
    case rest::RequestType::DELETE_REQ:
      return handleDeleteRequest();
    default:
      generateError(rest::ResponseCode::METHOD_NOT_ALLOWED, TRI_ERROR_HTTP_METHOD_NOT_ALLOWED);
  }
  return RestStatus::DONE;
}

struct FakeLogFollower : AbstractFollower {
  explicit FakeLogFollower(network::ConnectionPool* pool, ParticipantId id,
                           std::string database, LogId logId)
      :  pool(pool), id(std::move(id)), database(database), logId(logId) {}
  auto getParticipantId() const noexcept -> ParticipantId const& override { return id; }
  auto appendEntries(AppendEntriesRequest request)
      -> arangodb::futures::Future<AppendEntriesResult> override {
    VPackBufferUInt8  buffer;
    {
      VPackBuilder builder(buffer);
      request.toVelocyPack(builder);
    }

    auto path = "_api/log/" + std::to_string(logId.id()) + "/appendEntries";

    network::RequestOptions opts;
    opts.database = database;
    LOG_DEVEL << "sending append entries to " << id << " with payload " << VPackSlice(buffer.data()).toJson();
    auto f = network::sendRequest(pool, "server:" + id, fuerte::RestVerb::Post, path, std::move(buffer), opts);

    return std::move(f).thenValue([this](network::Response result) -> AppendEntriesResult {
      LOG_DEVEL << "Append entries for " << id << " returned, fuerte ok = " << result.ok();
      if (result.fail()) {
        return AppendEntriesResult{false, LogTerm(0)};
      }
      LOG_DEVEL << "Result for " << id << " is " << result.slice().toJson();
      TRI_ASSERT(result.slice().get("error").isFalse()); // TODO
      return AppendEntriesResult::fromVelocyPack(result.slice().get("result"));
    });

  }

  network::ConnectionPool *pool;
  ParticipantId id;
  std::string database;
  LogId logId;
};

RestStatus RestLogHandler::handlePostRequest() {

  std::vector<std::string> const& suffixes = _request->decodedSuffixes();

  bool parseSuccess = false;
  VPackSlice body = this->parseVPackBody(parseSuccess);
  if (!parseSuccess) {  // error message generated in parseVPackBody
    return RestStatus::DONE;
  }

  if (suffixes.empty()) {
    // create a new log
    LogId id{body.get("id").getNumericValue<uint64_t>()};

    auto result = _vocbase.createReplicatedLog(id);
    if (result.ok()) {
      generateOk(rest::ResponseCode::OK, VPackSlice::emptyObjectSlice());
    } else {
      generateError(result.result());
    }
    return RestStatus::DONE;
  }

  if (suffixes.size() != 2) {
    generateError(rest::ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
                  "expect GET /_api/log/<log-id>");
    return RestStatus::DONE;
  }

  LogId logId{basics::StringUtils::uint64(suffixes[0])};

  if (auto& verb = suffixes[1]; verb == "insert") {
    auto log = _vocbase.getReplicatedLogLeaderById(logId);
    auto idx = log->insert(LogPayload{body.toJson()});

    auto f = log->waitFor(idx).thenValue([this, idx](std::shared_ptr<QuorumData>&& quorum) {
      VPackBuilder response;
      {
        VPackObjectBuilder ob(&response);
        response.add("index", VPackValue(quorum->index.value));
        response.add("term", VPackValue(quorum->term.value));
        VPackArrayBuilder ab(&response, "quorum");
        for (auto& part : quorum->quorum) {
          response.add(VPackValue(part));
        }
      }
      LOG_DEVEL << "insert completed idx = " << idx.value;
      generateOk(rest::ResponseCode::ACCEPTED, response.slice());
    });

    log->runAsyncStep(); // TODO
    return waitForFuture(std::move(f));

  } else if (verb == "insertBabies") {
    auto log = _vocbase.getReplicatedLogLeaderById(logId);

    if (!body.isArray()) {
      generateError(rest::ResponseCode::NOT_FOUND, TRI_ERROR_HTTP_NOT_FOUND,
                    "expected array");
      return RestStatus::DONE;
    }

    auto lastIndex = LogIndex{0};

    for (auto entry : VPackArrayIterator(body)) {
      lastIndex = log->insert(LogPayload{entry.toJson()});
    }

    auto f = log->waitFor(lastIndex).thenValue([this, lastIndex](std::shared_ptr<QuorumData>&& quorum) {
      VPackBuilder response;
      {
        VPackObjectBuilder ob(&response);
        response.add("index", VPackValue(quorum->index.value));
        response.add("term", VPackValue(quorum->term.value));
        VPackArrayBuilder ab(&response, "quorum");
        for (auto& part : quorum->quorum) {
          response.add(VPackValue(part));
        }
      }
      LOG_DEVEL << "insert completed idx = " << lastIndex.value;
      generateOk(rest::ResponseCode::ACCEPTED, response.slice());
    });

    log->runAsyncStep(); // TODO
    return waitForFuture(std::move(f));

  } else if(verb == "becomeLeader") {
    auto& log = _vocbase.getReplicatedLogById(logId);

    auto term = LogTerm{body.get("term").getNumericValue<uint64_t>()};
    auto writeConcern = body.get("writeConcern").getNumericValue<std::size_t>();

    std::vector<std::shared_ptr<AbstractFollower>> follower;
    for (auto const& part : VPackArrayIterator(body.get("follower"))) {
      auto partId = part.copyString();
      follower.emplace_back(std::make_shared<FakeLogFollower>(server().getFeature<NetworkFeature>().pool(), partId, _vocbase.name(), logId));
    }

    log.becomeLeader(ServerState::instance()->getId(), term, follower, writeConcern);
    generateOk(rest::ResponseCode::ACCEPTED, VPackSlice::emptyObjectSlice());
  } else if (verb == "becomeFollower") {
    auto& log = _vocbase.getReplicatedLogById(logId);
    auto term = LogTerm{body.get("term").getNumericValue<uint64_t>()};
    auto leaderId = body.get("leader").copyString();
    log.becomeFollower(ServerState::instance()->getId(), term, leaderId);
    generateOk(rest::ResponseCode::ACCEPTED, VPackSlice::emptyObjectSlice());

  } else if (verb == "appendEntries") {
    auto log = _vocbase.getReplicatedLogFollowerById(logId);
    auto request = AppendEntriesRequest::fromVelocyPack(body);
    auto f = log->appendEntries(std::move(request)).thenValue([this](AppendEntriesResult&& res) {
      VPackBuilder builder;
      res.toVelocyPack(builder);
      generateOk(rest::ResponseCode::ACCEPTED, builder.slice());
    });

    return waitForFuture(std::move(f));
  } else {
    generateError(
        rest::ResponseCode::NOT_FOUND, TRI_ERROR_HTTP_NOT_FOUND,
        "expecting one of the resources 'insert', ");

  }
  return RestStatus::DONE;
}

RestStatus RestLogHandler::handleGetRequest() {
  std::vector<std::string> const& suffixes = _request->decodedSuffixes();
  if (suffixes.empty()) {

    generateError(rest::ResponseCode::NOT_IMPLEMENTED, TRI_ERROR_NOT_IMPLEMENTED);
    return RestStatus::DONE;
  }

  LogId logId{basics::StringUtils::uint64(suffixes[0])};

  if (suffixes.size() == 1) {
    replicated_log::ReplicatedLog& log = _vocbase.getReplicatedLogById(logId);
    VPackBuilder buffer;
    std::visit([&](auto const& status) { status.toVelocyPack(buffer); }, log._participant->getStatus());
    generateOk(rest::ResponseCode::OK, buffer.slice());
    return RestStatus::DONE;
  }

  auto const& verb = suffixes[1];

  if (verb == "dump") {
    if (suffixes.size() != 2) {
      generateError(rest::ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
                    "expect GET /_api/log/<log-id>/dump");
      return RestStatus::DONE;
    }

    // dump log
    VPackBuilder result;

    {
      VPackObjectBuilder ob(&result);
      result.add("logId", VPackValue(logId.id()));
      // result.add("index", VPackValue(idx.value));
    }

    generateOk(rest::ResponseCode::OK, result.slice());
  } else if (verb == "readEntry") {
    auto log = _vocbase.getReplicatedLogLeaderById(logId);
    if (suffixes.size() != 3) {
      generateError(rest::ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
                    "expect GET /_api/log/<log-id>/readEntry/<id>");
      return RestStatus::DONE;
    }
    LogIndex logIdx{basics::StringUtils::uint64(suffixes[2])};

    auto entry = log->readReplicatedEntryByIndex(logIdx);
    if (entry) {
      VPackBuilder result;
      {
        VPackObjectBuilder builder(&result);
        result.add("index", VPackValue(entry->logIndex().value));
        result.add("term", VPackValue(entry->logTerm().value));

        {
          VPackParser parser; // TODO remove parser and store vpack
          parser.parse(entry->logPayload().dummy);
          auto parserResult = parser.steal();
          result.add("payload", parserResult->slice());
        }


      }
      generateOk(rest::ResponseCode::OK, result.slice());

    } else {
      generateError(rest::ResponseCode::NOT_FOUND, TRI_ERROR_HTTP_NOT_FOUND, "log index not found");
    }

  } else {
    generateError(rest::ResponseCode::NOT_FOUND, TRI_ERROR_HTTP_NOT_FOUND,
                  "expecting one of the resources 'dump', 'readEntry'");
  }
  return RestStatus::DONE;
}

RestStatus RestLogHandler::handleDeleteRequest() {

  std::vector<std::string> const& suffixes = _request->decodedSuffixes();

  if (suffixes.size() != 1) {
    generateError(rest::ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
                  "expect DELETE /_api/log/<log-id>");
    return RestStatus::DONE;
  }

  LogId logId{basics::StringUtils::uint64(suffixes[0])};
  auto result = _vocbase.dropReplicatedLog(logId);
  if (!result.ok()) {
    generateError(result);
  } else {
    generateOk(rest::ResponseCode::ACCEPTED, VPackSlice::emptyObjectSlice());
  }

  return RestStatus::DONE;
}

RestLogHandler::RestLogHandler(application_features::ApplicationServer& server,
                               GeneralRequest* req, GeneralResponse* resp)
    : RestVocbaseBaseHandler(server, req, resp) {}
RestLogHandler::~RestLogHandler() = default;
