/**
 *    Copyright (C) 2016 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/base/disallow_copying.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/range_arithmetic.h"
#include "mongo/db/s/collection_metadata.h"
#include "mongo/db/s/collection_range_deleter.h"
#include "mongo/db/service_context.h"
#include "mongo/executor/task_executor.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/util/concurrency/notification.h"

#include "mongo/stdx/memory.h"

#include <list>

namespace mongo {

class ScopedCollectionMetadata;

class MetadataManager {
    MONGO_DISALLOW_COPYING(MetadataManager);

public:
    MetadataManager(ServiceContext*, NamespaceString nss, executor::TaskExecutor* rangeDeleter);
    ~MetadataManager();

    /**
     * An ActiveMetadata must be set before this function can be called.
     *
     * Increments the usage counter of the active metadata and returns an RAII object, which
     * contains the currently active metadata.  When the usageCounter goes to zero, the RAII
     * object going out of scope will call _removeMetadata.
     */
    ScopedCollectionMetadata getActiveMetadata();

    /**
     * Returns the number of CollectionMetadata objects being maintained on behalf of running
     * queries.  The actual number may vary after it returns, so this is really only useful for unit
     * tests.
     */
    size_t numberOfMetadataSnapshots();

    /**
     * Uses the contents of the specified metadata as a way to purge any pending chunks.
     */
    void refreshActiveMetadata(std::unique_ptr<CollectionMetadata> newMetadata);

    void toBSONPending(BSONArrayBuilder& bb) const;

    /**
     * Appends information on all the chunk ranges in rangesToClean to builder.
     */
    void append(BSONObjBuilder* builder);

    /**
     * Returns a map to the set of chunks being migrated in.
     */
    RangeMap const& getReceiveMap() const {
        return _receivingChunks;
    }

    /**
     * If no running queries can depend on documents in the range, schedules any such documents for
     * immediate cleanup. Otherwise, returns false.
     */
    bool beginReceive(ChunkRange const& range);

    /**
     * Removes the range from the pending list, and schedules any documents in the range for
     * immediate cleanup.  Assumes no active queries can see any local documents in the range.
     */
    void forgetReceive(const ChunkRange& range);

    /**
     * Initiates cleanup of the orphaned documents as if a chunk has been migrated out. If any
     * documents in the range might still be in use by running queries, queues cleanup to begin
     * after they have all terminated.  Otherwise, schedules documents for immediate cleanup.
     *
     * Must be called with the collection locked for writing.  To monitor completion, use
     * trackCleanup or CollectionShardingState::waitForClean.
     */
    void cleanUpRange(ChunkRange const& range);

    /**
     * Returns true if the specified key is in a range being received.
     */
    bool keyIsPending(const BSONObj& key) const;

    /**
     * Returns the number of ranges scheduled to be cleaned, exclusive of such ranges that might
     * still be in use by running queries.  Outside of test drivers, the actual number may vary
     * after it returns, so this is really only useful for unit tests.
     */
    size_t numberOfRangesToClean();

    /**
     * Returns the number of ranges scheduled to be cleaned once all queries that could depend on
     * them have terminated. The actual number may vary after it returns, so this is really only
     * useful for unit tests.
     */
    size_t numberOfRangesToCleanStillInUse();

    using CleanupNotification = CollectionRangeDeleter::DeleteNotification;
    /**
     * Reports whether the argument range is still scheduled for deletion. If not, returns nullptr.
     * Otherwise, returns a notification n such that n->get(opCtx) will wake when deletion of a
     * range (possibly the one of interest) is completed.
     */
    CleanupNotification trackCleanup(ChunkRange const& orphans);

    /**
     * Called for the range deletion executor, actually deletes a scheduled range of documents
     * immediately.  Returns true if it should be scheduled to be called again.
     */
    bool cleanUpNextRange(OperationContext* opCtx,
                          Collection* collection,
                          BSONObj const& keyPattern,
                          int maxToDelete);

private:
    struct CollectionMetadataTracker {
        /**
         * Creates a new CollectionMetadataTracker with the usageCounter initialized to zero.
         */
        CollectionMetadataTracker(std::unique_ptr<CollectionMetadata> m);

    private:
        std::unique_ptr<CollectionMetadata> metadata;
        uint32_t usageCounter{0};
        boost::optional<ChunkRange> orphans{boost::none};

        friend class ScopedCollectionMetadata;  // for access to usageCounter:
        friend class MetadataManager;
    };

    /**
     * Atomically decrements scoped.tracker->usageCount under our own mutex. At zero, retires any
     * metadata that has fallen out of use, and pushes any orphan ranges found in them to the list
     * of ranges actively being cleaned up.  Calling with a null scoped.tracker is a no-op.
     */
    void _decrementTrackerUsage(ScopedCollectionMetadata const& scoped);

    /**
     * Pushes current set of chunks, if any, to _metadataInUse, replaces it with newMetadata.
     */
    void _setActiveMetadata_inlock(std::unique_ptr<CollectionMetadata> newMetadata);

    /**
     * Returns true if the specified range overlaps any chunk that might be currently in use by a
     * running query.
     *
     * must be called locked.
     */

    bool _overlapsInUseChunk(ChunkRange const& range);

    /**
     * Returns true if any range (possibly) still in use, but scheduled for cleanup, overlaps
     * the argument range.
     *
     * Must be called locked.
     */
    bool _overlapsInUseCleanups(ChunkRange const& range);

    /**
     * Deletes ranges, in background, until done, normally using a task executor attached to the
     * ShardingState.
     *
     * Each time it completes cleaning up a range, it wakes up clients waiting on completion of
     * that range, which may then verify their range has no more deletions scheduled, and proceed.
     */
    static void _scheduleCleanup(executor::TaskExecutor*, NamespaceString nss);

    /**
     * Adds the range to the list of ranges scheduled for immediate deletion, and schedules a
     * a background task to perform the work.
     *
     * Must be called locked.
     */
    void _pushRangeToClean(ChunkRange const& range);

    /**
     * Adds a range from the receiving map, so getNextOrphanRange will skip ranges migrating in.
     */
    void _addToReceiving(ChunkRange const& range);

    /**
     * Removes a range from the receiving map after a migration failure. range.minKey() must
     * exactly match an element of _receivingChunks.
     */
    void _removeFromReceiving(ChunkRange const& range);

    /**
     * Wakes up any clients waiting on a range to leave _metadataInUse
     *
     * Must be called locked.
     */
    void _notifyInUse();

    // data members

    const NamespaceString _nss;

    // ServiceContext from which to obtain instances of global support objects.
    ServiceContext* _serviceContext;

    // Mutex to protect the state below
    stdx::mutex _managerLock;

    // The currently active collection metadata
    std::unique_ptr<CollectionMetadataTracker> _activeMetadataTracker;

    // Previously active collection metadata instances still in use by active server operations or
    // cursors
    std::list<std::unique_ptr<CollectionMetadataTracker>> _metadataInUse;

    // Chunk ranges which are currently assumed to be transferred to the shard. Indexed by the min
    // key of the range.
    RangeMap _receivingChunks;

    // Clients can sleep on copies of _notification while waiting for their orphan ranges to fall
    // out of use.
    std::shared_ptr<Notification<Status>> _notification;

    // The background task that deletes documents from orphaned chunk ranges.
    executor::TaskExecutor* _executor;

    // Ranges being deleted, or scheduled to be deleted, by a background task
    CollectionRangeDeleter _rangesToClean;

    friend class ScopedCollectionMetadata;  // Its op=() and dtor call _decrementTrackerUsage().
};

class ScopedCollectionMetadata {
    MONGO_DISALLOW_COPYING(ScopedCollectionMetadata);

public:
    /**
     * Creates an empty ScopedCollectionMetadata. Using the default constructor means that no
     * metadata is available.
     */
    ScopedCollectionMetadata();

    // Must be called with owning MetadataManager unlocked
    ~ScopedCollectionMetadata();

    // must be called with owning MetadataManager unlocked
    ScopedCollectionMetadata(ScopedCollectionMetadata&& other);
    ScopedCollectionMetadata& operator=(ScopedCollectionMetadata&& other);

    /**
     * Dereferencing the ScopedCollectionMetadata dereferences the internal CollectionMetadata.
     */
    CollectionMetadata* operator->() const;
    CollectionMetadata* getMetadata() const;

    /**
     * True if the ScopedCollectionMetadata stores a metadata (is not empty)
     */
    operator bool() const;

private:
    /**
     * Increments the refcount in the specified tracker.
     *
     * Must be called with specified *manager locked.
     */
    ScopedCollectionMetadata(MetadataManager* manager,
                             MetadataManager::CollectionMetadataTracker* tracker);

    MetadataManager* _manager{nullptr};
    MetadataManager::CollectionMetadataTracker* _tracker{nullptr};

    friend ScopedCollectionMetadata MetadataManager::getActiveMetadata();  // uses our private ctor
    friend void MetadataManager::_decrementTrackerUsage(ScopedCollectionMetadata const&);
};

}  // namespace mongo
