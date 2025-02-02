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

#pragma once
#ifndef ARANGOD_ROCKSDB_ENGINE_ROCKSDB_META_COLLECTION_H
#define ARANGOD_ROCKSDB_ENGINE_ROCKSDB_META_COLLECTION_H 1

#include "Basics/Common.h"
#include "Basics/ReadWriteLock.h"
#include "Basics/ResultT.h"
#include "Containers/MerkleTree.h"
#include "RocksDBEngine/RocksDBCommon.h"
#include "RocksDBEngine/RocksDBMetadata.h"
#include "StorageEngine/PhysicalCollection.h"
#include "VocBase/AccessMode.h"
#include "VocBase/LogicalCollection.h"

#include <functional>

namespace arangodb {

class RocksDBMetaCollection : public PhysicalCollection {
 public:
  explicit RocksDBMetaCollection(LogicalCollection& collection,
                                 arangodb::velocypack::Slice const& info);
  RocksDBMetaCollection(LogicalCollection& collection,
                        PhysicalCollection const*);  // use in cluster only!!!!!
  virtual ~RocksDBMetaCollection() = default;
  
  std::string const& path() const override final;

  void deferDropCollection(std::function<bool(LogicalCollection&)> const&) override final;

  /// @brief report extra memory used by indexes etc.
  size_t memory() const override final { return 0; }
  uint64_t objectId() const { return _objectId.load(); }

  RocksDBMetadata& meta() { return _meta; }
  RocksDBMetadata const& meta() const { return _meta; }

  RevisionId revision(arangodb::transaction::Methods* trx) const override final;
  uint64_t numberDocuments(transaction::Methods* trx) const override final;

  ErrorCode lockWrite(double timeout = 0.0);
  void unlockWrite();
  ErrorCode lockRead(double timeout = 0.0);
  void unlockRead();
  
  /// recalculate counts for collection in case of failure, blocks other writes for a short period
  uint64_t recalculateCounts() override;
 
  /// @brief compact-data operation
  /// triggers rocksdb compaction for documentDB and indexes
  void compact() override final;
  
  /// estimate size of collection and indexes
  void estimateSize(velocypack::Builder& builder);

  void setRevisionTree(std::unique_ptr<containers::RevisionTree>&& tree, uint64_t seq);
  std::unique_ptr<containers::RevisionTree> revisionTree(transaction::Methods& trx) override;
  std::unique_ptr<containers::RevisionTree> revisionTree(uint64_t batchId) override;

  bool needToPersistRevisionTree(rocksdb::SequenceNumber maxCommitSeq) const;
  rocksdb::SequenceNumber lastSerializedRevisionTree(rocksdb::SequenceNumber maxCommitSeq);
  rocksdb::SequenceNumber serializeRevisionTree(std::string& output,
                                                rocksdb::SequenceNumber commitSeq,
                                                bool force);

  Result rebuildRevisionTree() override;
  void rebuildRevisionTree(std::unique_ptr<rocksdb::Iterator>& iter);

  void revisionTreeSummary(VPackBuilder& builder, bool fromCollection);
  void revisionTreePendingUpdates(VPackBuilder& builder);

  void placeRevisionTreeBlocker(TransactionId transactionId) override;
  void removeRevisionTreeBlocker(TransactionId transactionId) override;

  /**
   * @brief Buffer updates to this collection to be applied when appropriate
   *
   * Buffers updates associated with a given commit seq/tick. Will hold updates
   * until all previous blockers have been removed to ensure a consistent state
   * for sync/recovery and avoid any missed updates.
   *
   * @param  seq      The seq/tick post-commit, prior to call
   * @param  inserts  Vector of revisions to insert
   * @param  removals Vector of revisions to remove
   */
  void bufferUpdates(rocksdb::SequenceNumber seq, std::vector<std::uint64_t>&& inserts,
                     std::vector<std::uint64_t>&& removals);

  Result bufferTruncate(rocksdb::SequenceNumber seq);

  /// @brief sends the collection's revision tree to hibernation
  void hibernateRevisionTree();

  /// @brief return bounds for all documents
  virtual RocksDBKeyBounds bounds() const = 0;
  
  /// @brief produce a revision tree from the documents in the collection
  ResultT<std::pair<std::unique_ptr<containers::RevisionTree>, rocksdb::SequenceNumber>> revisionTreeFromCollection();

#ifdef ARANGODB_ENABLE_FAILURE_TESTS
  void corruptRevisionTree(std::uint64_t count, std::uint64_t hash);
#endif
  
 protected:
  
  /// @brief track the usage of waitForSync option in an operation
  void trackWaitForSync(arangodb::transaction::Methods* trx, OperationOptions& options);

  Result applyUpdatesForTransaction(containers::RevisionTree& tree,
                                    rocksdb::SequenceNumber commitSeq) const;

 private:
  ErrorCode doLock(double timeout, AccessMode::Type mode);
  bool haveBufferedOperations() const;
  std::unique_ptr<containers::RevisionTree> allocateEmptyRevisionTree(std::size_t depth) const;
  void applyUpdates(rocksdb::SequenceNumber commitSeq);
  void ensureRevisionTree();

  // helper function to build revision trees
  std::unique_ptr<containers::RevisionTree> revisionTree(
    std::function<std::unique_ptr<containers::RevisionTree>(std::unique_ptr<containers::RevisionTree>)> const& callback);

 protected:
  RocksDBMetadata _meta;     /// collection metadata
  /// @brief collection lock used for write access
  mutable basics::ReadWriteLock _exclusiveLock;
  /// @brief collection lock used for recalculation count values
  mutable std::mutex _recalculationLock;

  /// @brief depth for all revision trees. 
  /// depth is large from the beginning so that the trees are always
  /// large enough to handle large collections and do not need resizing.
  /// as the combined RAM usage for all such trees would be prohibitive,
  /// we may hold some of the trees in memory only in a compressed variant.
  static constexpr std::size_t revisionTreeDepth = 6;

 private:
  std::atomic<uint64_t> _objectId;  /// rocksdb-specific object id for collection

  /// @brief helper class for accessing revision trees in a compressed or
  /// uncompressed state. this class alone is not thread-safe. it must be
  /// used with external synchronization
  class RevisionTreeAccessor {
   public:
    RevisionTreeAccessor(RevisionTreeAccessor const&) = delete;
    RevisionTreeAccessor& operator=(RevisionTreeAccessor const&) = delete;

    explicit RevisionTreeAccessor(std::unique_ptr<containers::RevisionTree> tree,
                                  LogicalCollection const& collection);

    void insert(std::vector<std::uint64_t> const& keys);
    void remove(std::vector<std::uint64_t> const& keys);
    void clear();
    std::unique_ptr<containers::RevisionTree> clone() const;
    std::uint64_t count() const;
    std::uint64_t rootValue() const;
    std::uint64_t maxDepth() const;

    // potentially expensive! only call when necessary
    std::uint64_t compressedSize() const;
    
    void checkConsistency() const;
    void serializeBinary(std::string& output) const;

    // turn the full-blown revision tree into a potentially smaller
    // compressed representation
    void hibernate(bool force);

#ifdef ARANGODB_ENABLE_FAILURE_TESTS
    void corrupt(std::uint64_t count, std::uint64_t hash);
#endif

   private:
    /// @brief make sure we have a value in _tree. unfortunately we
    /// need to declare this const although it can mutate the internal
    /// state of _tree
    void ensureTree() const;

    /// @brief compressed representation of tree. we will either have
    /// this attribute populated or _tree
    std::string mutable _compressed;

    /// @brief the actual tree. we will either have this populated or 
    ///_compressed
    std::unique_ptr<containers::RevisionTree> mutable _tree;

    /// @brief collecion object, used only for context in log messages
    LogicalCollection const& _logicalCollection;

    /// @brief maxDepth of tree. supposed to never change
    std::uint64_t const _maxDepth;
  
    /// @brief number of hibernation requests received (we may ignore the
    /// first few ones)
    std::uint32_t mutable _hibernationRequests;

    /// @brief whether or not we should attempt to compress the tree
    bool _compressible;
  };

  /// @revision tree management for replication
  std::unique_ptr<RevisionTreeAccessor> _revisionTree;
  std::atomic<rocksdb::SequenceNumber> _revisionTreeApplied;
  rocksdb::SequenceNumber _revisionTreeCreationSeq;
  rocksdb::SequenceNumber _revisionTreeSerializedSeq;
  std::chrono::steady_clock::time_point _revisionTreeSerializedTime;
  mutable std::mutex _revisionTreeLock;

  // if the types of these containers are changed to some other type, please check the new
  // type's iterator invalidation rules first and if iterators are invalidated when new 
  // elements get inserted
  std::multimap<rocksdb::SequenceNumber, std::vector<std::uint64_t>> _revisionInsertBuffers;
  std::multimap<rocksdb::SequenceNumber, std::vector<std::uint64_t>> _revisionRemovalBuffers;
  std::set<rocksdb::SequenceNumber> _revisionTruncateBuffer;
  mutable std::mutex _revisionBufferLock;
};

} // namespace arangodb
  
#endif
