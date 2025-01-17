/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#include "mongo/platform/basic.h"

#include "mongo/db/repl/replication_coordinator_external_state_impl.h"

#include <string>

#include "mongo/base/init.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/catalog/coll_mod.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/logical_time_metadata_hook.h"
#include "mongo/db/logical_time_validator.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/repair_database.h"
#include "mongo/db/repl/bgsync.h"
#include "mongo/db/repl/drop_pending_collection_reaper.h"
#include "mongo/db/repl/isself.h"
#include "mongo/db/repl/last_vote.h"
#include "mongo/db/repl/master_slave.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/noop_writer.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_buffer_blocking_queue.h"
#include "mongo/db/repl/oplog_buffer_collection.h"
#include "mongo/db/repl/oplog_buffer_proxy.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/repl/replication_process.h"
#include "mongo/db/repl/rs_sync.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/s/balancer/balancer.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/s/sharding_state_recovery.h"
#include "mongo/db/server_options.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session_catalog.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/executor/network_connection_hook.h"
#include "mongo/executor/network_interface.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/rpc/metadata/egress_metadata_hook_list.h"
#include "mongo/s/catalog/sharding_catalog_manager.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/catalog_cache_loader.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/cluster_identity_loader.h"
#include "mongo/s/grid.h"
#include "mongo/stdx/functional.h"
#include "mongo/stdx/memory.h"
#include "mongo/stdx/thread.h"
#include "mongo/transport/service_entry_point.h"
#include "mongo/transport/session.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/exit.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/net/listen.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
namespace repl {

namespace {
using UniqueLock = stdx::unique_lock<stdx::mutex>;
using LockGuard = stdx::lock_guard<stdx::mutex>;

const char localDbName[] = "local";
const char configCollectionName[] = "local.system.replset";
const auto configDatabaseName = localDbName;
const char lastVoteCollectionName[] = "local.replset.election";
const auto lastVoteDatabaseName = localDbName;
const char meCollectionName[] = "local.me";
const auto meDatabaseName = localDbName;
const char tsFieldName[] = "ts";

const char kCollectionOplogBufferName[] = "collection";
const char kBlockingQueueOplogBufferName[] = "inMemoryBlockingQueue";

// Set this to specify whether to use a collection to buffer the oplog on the destination server
// during initial sync to prevent rolling over the oplog.
MONGO_EXPORT_STARTUP_SERVER_PARAMETER(initialSyncOplogBuffer,
                                      std::string,
                                      kCollectionOplogBufferName);

// Set this to specify size of read ahead buffer in the OplogBufferCollection.
MONGO_EXPORT_STARTUP_SERVER_PARAMETER(initialSyncOplogBufferPeekCacheSize, int, 10000);

// Set this to specify maximum number of times the oplog fetcher will consecutively restart the
// oplog tailing query on non-cancellation errors.
server_parameter_storage_type<int, ServerParameterType::kStartupAndRuntime>::value_type
    oplogFetcherMaxFetcherRestarts(3);
class ExportedOplogFetcherMaxFetcherRestartsServerParameter
    : public ExportedServerParameter<int, ServerParameterType::kStartupAndRuntime> {
public:
    ExportedOplogFetcherMaxFetcherRestartsServerParameter();
    Status validate(const int& potentialNewValue) override;
} _exportedOplogFetcherMaxFetcherRestartsServerParameter;

ExportedOplogFetcherMaxFetcherRestartsServerParameter::
    ExportedOplogFetcherMaxFetcherRestartsServerParameter()
    : ExportedServerParameter<int, ServerParameterType::kStartupAndRuntime>(
          ServerParameterSet::getGlobal(),
          "oplogFetcherMaxFetcherRestarts",
          &oplogFetcherMaxFetcherRestarts) {}

Status ExportedOplogFetcherMaxFetcherRestartsServerParameter::validate(
    const int& potentialNewValue) {
    if (potentialNewValue < 0) {
        return Status(ErrorCodes::BadValue,
                      "oplogFetcherMaxFetcherRestarts must be greater than or equal to 0");
    }
    return Status::OK();
}

MONGO_INITIALIZER(initialSyncOplogBuffer)(InitializerContext*) {
    if ((initialSyncOplogBuffer != kCollectionOplogBufferName) &&
        (initialSyncOplogBuffer != kBlockingQueueOplogBufferName)) {
        return Status(ErrorCodes::BadValue,
                      "unsupported initial sync oplog buffer option: " + initialSyncOplogBuffer);
    }
    return Status::OK();
}

/**
 * Returns new thread pool for thread pool task executor.
 */
std::unique_ptr<ThreadPool> makeThreadPool() {
    ThreadPool::Options threadPoolOptions;
    threadPoolOptions.poolName = "replication";
    threadPoolOptions.onCreateThread = [](const std::string& threadName) {
        Client::initThread(threadName.c_str());
    };
    return stdx::make_unique<ThreadPool>(threadPoolOptions);
}

/**
 * Schedules a task using the executor. This task is always run unless the task executor is shutting
 * down.
 */
void scheduleWork(executor::TaskExecutor* executor,
                  const executor::TaskExecutor::CallbackFn& work) {
    auto cbh = executor->scheduleWork([work](const executor::TaskExecutor::CallbackArgs& args) {
        if (args.status == ErrorCodes::CallbackCanceled) {
            return;
        }
        work(args);
    });
    if (cbh == ErrorCodes::ShutdownInProgress) {
        return;
    }
    fassertStatusOK(40460, cbh);
}

}  // namespace

ReplicationCoordinatorExternalStateImpl::ReplicationCoordinatorExternalStateImpl(
    ServiceContext* service,
    DropPendingCollectionReaper* dropPendingCollectionReaper,
    StorageInterface* storageInterface,
    ReplicationProcess* replicationProcess)
    : _service(service),
      _dropPendingCollectionReaper(dropPendingCollectionReaper),
      _storageInterface(storageInterface),
      _replicationProcess(replicationProcess) {
    uassert(ErrorCodes::BadValue, "A StorageInterface is required.", _storageInterface);
}
ReplicationCoordinatorExternalStateImpl::~ReplicationCoordinatorExternalStateImpl() {}

bool ReplicationCoordinatorExternalStateImpl::isInitialSyncFlagSet(OperationContext* opCtx) {
    return _replicationProcess->getConsistencyMarkers()->getInitialSyncFlag(opCtx);
}

// This function acquires the LockManager locks on oplog, so it cannot be called while holding
// ReplicationCoordinatorImpl's mutex.
void ReplicationCoordinatorExternalStateImpl::startSteadyStateReplication(
    OperationContext* opCtx, ReplicationCoordinator* replCoord) {

    // Initialize the cached pointer to the oplog collection, for writing to the oplog.
    acquireOplogCollectionForLogging(opCtx);

    LockGuard lk(_threadMutex);

    // We've shut down the external state, don't start again.
    if (_inShutdown)
        return;

    invariant(replCoord);
    invariant(!_bgSync);
    log() << "Starting replication fetcher thread";
    _bgSync = stdx::make_unique<BackgroundSync>(
        this, _replicationProcess, makeSteadyStateOplogBuffer(opCtx));
    _bgSync->startup(opCtx);

    log() << "Starting replication applier thread";
    invariant(!_applierThread);
    _applierThread.reset(new RSDataSync{_bgSync.get(), replCoord});
    _applierThread->startup();
    log() << "Starting replication reporter thread";
    invariant(!_syncSourceFeedbackThread);
    _syncSourceFeedbackThread.reset(new stdx::thread(stdx::bind(&SyncSourceFeedback::run,
                                                                &_syncSourceFeedback,
                                                                _taskExecutor.get(),
                                                                _bgSync.get(),
                                                                replCoord)));
}

void ReplicationCoordinatorExternalStateImpl::stopDataReplication(OperationContext* opCtx) {
    UniqueLock lk(_threadMutex);
    _stopDataReplication_inlock(opCtx, &lk);
}

void ReplicationCoordinatorExternalStateImpl::_stopDataReplication_inlock(OperationContext* opCtx,
                                                                          UniqueLock* lock) {
    // Make sue no other _stopDataReplication calls are in progress.
    _dataReplicationStopped.wait(*lock, [this]() { return !_stoppingDataReplication; });
    _stoppingDataReplication = true;

    auto oldSSF = std::move(_syncSourceFeedbackThread);
    auto oldBgSync = std::move(_bgSync);
    auto oldApplier = std::move(_applierThread);
    lock->unlock();

    if (oldSSF) {
        log() << "Stopping replication reporter thread";
        _syncSourceFeedback.shutdown();
        oldSSF->join();
    }

    if (oldBgSync) {
        log() << "Stopping replication fetcher thread";
        oldBgSync->shutdown(opCtx);
    }

    if (oldApplier) {
        log() << "Stopping replication applier thread";
        oldApplier->join();
    }

    if (oldBgSync) {
        oldBgSync->join(opCtx);
    }

    lock->lock();
    _stoppingDataReplication = false;
    _dataReplicationStopped.notify_all();
}


void ReplicationCoordinatorExternalStateImpl::startThreads(const ReplSettings& settings) {
    stdx::lock_guard<stdx::mutex> lk(_threadMutex);
    if (_startedThreads) {
        return;
    }

    log() << "Starting replication storage threads";
    _service->getGlobalStorageEngine()->setJournalListener(this);

    auto hookList = stdx::make_unique<rpc::EgressMetadataHookList>();
    hookList->addHook(stdx::make_unique<rpc::LogicalTimeMetadataHook>(_service));
    _taskExecutor = stdx::make_unique<executor::ThreadPoolTaskExecutor>(
        makeThreadPool(),
        executor::makeNetworkInterface("NetworkInterfaceASIO-RS", nullptr, std::move(hookList)));
    _taskExecutor->startup();

    _writerPool = SyncTail::makeWriterPool();

    _startedThreads = true;
}

void ReplicationCoordinatorExternalStateImpl::startMasterSlave(OperationContext* opCtx) {
    repl::startMasterSlave(opCtx);
}

void ReplicationCoordinatorExternalStateImpl::shutdown(OperationContext* opCtx) {
    UniqueLock lk(_threadMutex);
    if (!_startedThreads) {
        return;
    }

    _inShutdown = true;
    _stopDataReplication_inlock(opCtx, &lk);

    if (_noopWriter) {
        LOG(1) << "Stopping noop writer";
        _noopWriter->stopWritingPeriodicNoops();
    }

    log() << "Stopping replication storage threads";
    _taskExecutor->shutdown();
    _taskExecutor->join();
    lk.unlock();

    // Perform additional shutdown steps below that must be done outside _threadMutex.

    if (_replicationProcess->getConsistencyMarkers()->getOplogTruncateAfterPoint(opCtx).isNull() &&
        loadLastOpTime(opCtx) ==
            _replicationProcess->getConsistencyMarkers()->getAppliedThrough(opCtx)) {
        // Clear the appliedThrough marker to indicate we are consistent with the top of the
        // oplog.
        _replicationProcess->getConsistencyMarkers()->setAppliedThrough(opCtx, {});
    }
}

executor::TaskExecutor* ReplicationCoordinatorExternalStateImpl::getTaskExecutor() const {
    return _taskExecutor.get();
}

OldThreadPool* ReplicationCoordinatorExternalStateImpl::getDbWorkThreadPool() const {
    return _writerPool.get();
}

Status ReplicationCoordinatorExternalStateImpl::runRepairOnLocalDB(OperationContext* opCtx) {
    try {
        Lock::GlobalWrite globalWrite(opCtx);
        StorageEngine* engine = getGlobalServiceContext()->getGlobalStorageEngine();

        if (!engine->isMmapV1()) {
            return Status::OK();
        }

        UnreplicatedWritesBlock uwb(opCtx);
        Status status = repairDatabase(opCtx, engine, localDbName, false, false);

        // Open database before returning
        dbHolder().openDb(opCtx, localDbName);
    } catch (const DBException& ex) {
        return ex.toStatus();
    }
    return Status::OK();
}

Status ReplicationCoordinatorExternalStateImpl::initializeReplSetStorage(OperationContext* opCtx,
                                                                         const BSONObj& config) {
    try {
        createOplog(opCtx);
        const auto& kRsOplogNamespace = NamespaceString::kRsOplogNamespace;

        writeConflictRetry(opCtx,
                           "initiate oplog entry",
                           kRsOplogNamespace.toString(),
                           [this, &opCtx, &config, &kRsOplogNamespace] {
                               Lock::GlobalWrite globalWrite(opCtx);

                               WriteUnitOfWork wuow(opCtx);
                               Helpers::putSingleton(opCtx, configCollectionName, config);
                               const auto msgObj = BSON("msg"
                                                        << "initiating set");
                               _service->getOpObserver()->onOpMessage(opCtx, msgObj);
                               wuow.commit();
                               // ReplSetTest assumes that immediately after the replSetInitiate
                               // command returns, it can allow other nodes to initial sync with no
                               // retries and they will succeed.  Unfortunately, initial sync will
                               // fail if it finds its sync source has an empty oplog.  Thus, we
                               // need to wait here until the seed document is visible in our oplog.
                               AutoGetCollection oplog(opCtx, kRsOplogNamespace, MODE_IS);
                               waitForAllEarlierOplogWritesToBeVisible(opCtx);
                           });

        // Set UUIDs for all non-replicated collections. This is necessary for independent replica
        // sets and config server replica sets started with no data files because collections in
        // local are created prior to the featureCompatibilityVersion being set to 3.6, so the
        // collections are not created with UUIDs. This is not an issue for shard servers because
        // the config server sends a setFeatureCompatibilityVersion command with the
        // featureCompatibilityVersion equal to the cluster's featureCompatibilityVersion during
        // addShard, which will add UUIDs to all collections that do not already have them. Here,
        // we add UUIDs to the non-replicated collections on the primary. We add them on the
        // secondaries during InitialSync.
        if (serverGlobalParams.clusterRole != ClusterRole::ShardServer &&
            FeatureCompatibilityVersion::isCleanStartUp()) {
            auto schemaStatus = updateUUIDSchemaVersionNonReplicated(opCtx, true);
            if (!schemaStatus.isOK()) {
                return schemaStatus;
            }
        }
        FeatureCompatibilityVersion::setIfCleanStartup(opCtx, _storageInterface);
    } catch (const DBException& ex) {
        return ex.toStatus();
    }
    return Status::OK();
}

void ReplicationCoordinatorExternalStateImpl::waitForAllEarlierOplogWritesToBeVisible(
    OperationContext* opCtx) {
    AutoGetCollection oplog(opCtx, NamespaceString::kRsOplogNamespace, MODE_IS);
    oplog.getCollection()->getRecordStore()->waitForAllEarlierOplogWritesToBeVisible(opCtx);
}

void ReplicationCoordinatorExternalStateImpl::onDrainComplete(OperationContext* opCtx) {
    invariant(!opCtx->lockState()->isLocked());

    // If this is a config server node becoming a primary, ensure the balancer is ready to start.
    if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {
        // We must ensure the balancer has stopped because it may still be in the process of
        // stopping if this node was previously primary.
        Balancer::get(opCtx)->waitForBalancerToStop();
    }
}

OpTime ReplicationCoordinatorExternalStateImpl::onTransitionToPrimary(OperationContext* opCtx,
                                                                      bool isV1ElectionProtocol) {
    invariant(opCtx->lockState()->isW());

    // Clear the appliedThrough marker so on startup we'll use the top of the oplog. This must be
    // done before we add anything to our oplog.
    invariant(
        _replicationProcess->getConsistencyMarkers()->getOplogTruncateAfterPoint(opCtx).isNull());
    _replicationProcess->getConsistencyMarkers()->setAppliedThrough(opCtx, {});

    if (isV1ElectionProtocol) {
        writeConflictRetry(opCtx, "logging transition to primary to oplog", "local.oplog.rs", [&] {
            WriteUnitOfWork wuow(opCtx);
            opCtx->getClient()->getServiceContext()->getOpObserver()->onOpMessage(
                opCtx,
                BSON("msg"
                     << "new primary"));
            wuow.commit();
        });
    }
    const auto opTimeToReturn = fassertStatusOK(28665, loadLastOpTime(opCtx));

    _shardingOnTransitionToPrimaryHook(opCtx);
    _dropAllTempCollections(opCtx);

    serverGlobalParams.validateFeaturesAsMaster.store(true);

    return opTimeToReturn;
}

/*
MongoDB 采用了信息转发的方式来解决这个问题，当 S1 收到 S2 汇报过来的 replSetUpdatePosition 命令，
进行处理时（processReplSetUpdatePosition()），如果发现自己不是 Primary 角色，会立刻触发一个 
forwardSlaveProgress 任务，即，把自己的 Oplog Apply 信息，连同自己的 Secondary 汇报过来的，
构造一个 replSetUpdatePosition 命令，发往上游，从而保证，当任一个 Secondary 节点的 Oplog Apply 
进度推进，Primary 都能够及时的收到消息，尽可能降低 w>1 时，因 writeConcern 而带来的写操作延迟。
*/
void ReplicationCoordinatorExternalStateImpl::forwardSlaveProgress() {
    _syncSourceFeedback.forwardSlaveProgress();
}

OID ReplicationCoordinatorExternalStateImpl::ensureMe(OperationContext* opCtx) {
    std::string myname = getHostName();
    OID myRID;
    {
        Lock::DBLock lock(opCtx, meDatabaseName, MODE_X);

        BSONObj me;
        // local.me is an identifier for a server for getLastError w:2+
        // TODO: handle WriteConflictExceptions below
        if (!Helpers::getSingleton(opCtx, meCollectionName, me) || !me.hasField("host") ||
            me["host"].String() != myname) {
            myRID = OID::gen();

            // clean out local.me
            Helpers::emptyCollection(opCtx, NamespaceString(meCollectionName));

            // repopulate
            BSONObjBuilder b;
            b.append("_id", myRID);
            b.append("host", myname);
            Helpers::putSingleton(opCtx, meCollectionName, b.done());
        } else {
            myRID = me["_id"].OID();
        }
    }
    return myRID;
}

StatusWith<BSONObj> ReplicationCoordinatorExternalStateImpl::loadLocalConfigDocument(
    OperationContext* opCtx) {
    try {
        return writeConflictRetry(opCtx, "load replica set config", configCollectionName, [opCtx] {
            BSONObj config;
            if (!Helpers::getSingleton(opCtx, configCollectionName, config)) {
                return StatusWith<BSONObj>(
                    ErrorCodes::NoMatchingDocument,
                    str::stream() << "Did not find replica set configuration document in "
                                  << configCollectionName);
            }
            return StatusWith<BSONObj>(config);
        });
    } catch (const DBException& ex) {
        return StatusWith<BSONObj>(ex.toStatus());
    }
}

Status ReplicationCoordinatorExternalStateImpl::storeLocalConfigDocument(OperationContext* opCtx,
                                                                         const BSONObj& config) {
    try {
        writeConflictRetry(opCtx, "save replica set config", configCollectionName, [&] {
            Lock::DBLock dbWriteLock(opCtx, configDatabaseName, MODE_X);
            Helpers::putSingleton(opCtx, configCollectionName, config);
        });

        return Status::OK();
    } catch (const DBException& ex) {
        return ex.toStatus();
    }
}

StatusWith<LastVote> ReplicationCoordinatorExternalStateImpl::loadLocalLastVoteDocument(
    OperationContext* opCtx) {
    try {
        return writeConflictRetry(
            opCtx, "load replica set lastVote", lastVoteCollectionName, [opCtx] {
                BSONObj lastVoteObj;
                if (!Helpers::getSingleton(opCtx, lastVoteCollectionName, lastVoteObj)) {
                    return StatusWith<LastVote>(
                        ErrorCodes::NoMatchingDocument,
                        str::stream() << "Did not find replica set lastVote document in "
                                      << lastVoteCollectionName);
                }
                return LastVote::readFromLastVote(lastVoteObj);
            });
    } catch (const DBException& ex) {
        return StatusWith<LastVote>(ex.toStatus());
    }
}

Status ReplicationCoordinatorExternalStateImpl::storeLocalLastVoteDocument(
    OperationContext* opCtx, const LastVote& lastVote) {
    BSONObj lastVoteObj = lastVote.toBSON();
    try {
        Status status =
            writeConflictRetry(opCtx, "save replica set lastVote", lastVoteCollectionName, [&] {
                Lock::DBLock dbWriteLock(opCtx, lastVoteDatabaseName, MODE_X);

                // If there is no last vote document, we want to store one. Otherwise, we only want
                // to replace it if the new last vote document would have a higher term. We both
                // check the term of the current last vote document and insert the new document
                // under the DBLock to synchronize the two operations.
                BSONObj result;
                bool exists = Helpers::getSingleton(opCtx, lastVoteCollectionName, result);
                if (!exists) {
                    Helpers::putSingleton(opCtx, lastVoteCollectionName, lastVoteObj);
                } else {
                    StatusWith<LastVote> oldLastVoteDoc = LastVote::readFromLastVote(result);
                    if (!oldLastVoteDoc.isOK()) {
                        return oldLastVoteDoc.getStatus();
                    }
                    if (lastVote.getTerm() > oldLastVoteDoc.getValue().getTerm()) {
                        Helpers::putSingleton(opCtx, lastVoteCollectionName, lastVoteObj);
                    }
                }

                return Status::OK();
            });

        if (!status.isOK()) {
            return status;
        }

        opCtx->recoveryUnit()->waitUntilDurable();

        return Status::OK();
    } catch (const DBException& ex) {
        return ex.toStatus();
    }
}

void ReplicationCoordinatorExternalStateImpl::setGlobalTimestamp(ServiceContext* ctx,
                                                                 const Timestamp& newTime) {
    setNewTimestamp(ctx, newTime);
}

StatusWith<OpTime> ReplicationCoordinatorExternalStateImpl::loadLastOpTime(
    OperationContext* opCtx) {
    // TODO: handle WriteConflictExceptions below
    try {
        // If we are doing an initial sync do not read from the oplog.
        if (_replicationProcess->getConsistencyMarkers()->getInitialSyncFlag(opCtx)) {
            return {ErrorCodes::InitialSyncFailure, "In the middle of an initial sync."};
        }

        BSONObj oplogEntry;
        if (!Helpers::getLast(opCtx, NamespaceString::kRsOplogNamespace.ns().c_str(), oplogEntry)) {
            return StatusWith<OpTime>(ErrorCodes::NoMatchingDocument,
                                      str::stream() << "Did not find any entries in "
                                                    << NamespaceString::kRsOplogNamespace.ns());
        }
        BSONElement tsElement = oplogEntry[tsFieldName];
        if (tsElement.eoo()) {
            return StatusWith<OpTime>(ErrorCodes::NoSuchKey,
                                      str::stream() << "Most recent entry in "
                                                    << NamespaceString::kRsOplogNamespace.ns()
                                                    << " missing \""
                                                    << tsFieldName
                                                    << "\" field");
        }
        if (tsElement.type() != bsonTimestamp) {
            return StatusWith<OpTime>(ErrorCodes::TypeMismatch,
                                      str::stream() << "Expected type of \"" << tsFieldName
                                                    << "\" in most recent "
                                                    << NamespaceString::kRsOplogNamespace.ns()
                                                    << " entry to have type Timestamp, but found "
                                                    << typeName(tsElement.type()));
        }
        return OpTime::parseFromOplogEntry(oplogEntry);
    } catch (const DBException& ex) {
        return StatusWith<OpTime>(ex.toStatus());
    }
}

bool ReplicationCoordinatorExternalStateImpl::isSelf(const HostAndPort& host, ServiceContext* ctx) {
    return repl::isSelf(host, ctx);
}

HostAndPort ReplicationCoordinatorExternalStateImpl::getClientHostAndPort(
    const OperationContext* opCtx) {
    return HostAndPort(opCtx->getClient()->clientAddress(true));
}

void ReplicationCoordinatorExternalStateImpl::closeConnections() {
    _service->getServiceEntryPoint()->endAllSessions(transport::Session::kKeepOpen);
}

void ReplicationCoordinatorExternalStateImpl::killAllUserOperations(OperationContext* opCtx) {
    ServiceContext* environment = opCtx->getServiceContext();
    environment->killAllUserOperations(opCtx, ErrorCodes::InterruptedDueToReplStateChange);
}

void ReplicationCoordinatorExternalStateImpl::shardingOnStepDownHook() {
    if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {
        Balancer::get(_service)->interruptBalancer();
    } else if (ShardingState::get(_service)->enabled()) {
        invariant(serverGlobalParams.clusterRole == ClusterRole::ShardServer);
        ShardingState::get(_service)->interruptChunkSplitter();
        CatalogCacheLoader::get(_service).onStepDown();
    }

    ShardingState::get(_service)->markCollectionsNotShardedAtStepdown();

    if (auto validator = LogicalTimeValidator::get(_service)) {
        auto opCtx = cc().getOperationContext();

        if (opCtx != nullptr) {
            validator->enableKeyGenerator(opCtx, false);
        } else {
            auto opCtxPtr = cc().makeOperationContext();
            validator->enableKeyGenerator(opCtxPtr.get(), false);
        }
    }
}

void ReplicationCoordinatorExternalStateImpl::_shardingOnTransitionToPrimaryHook(
    OperationContext* opCtx) {
    auto status = ShardingStateRecovery::recover(opCtx);

    if (ErrorCodes::isShutdownError(status.code())) {
        // Note: callers of this method don't expect exceptions, so throw only unexpected fatal
        // errors.
        return;
    }

    fassertStatusOK(40107, status);

    if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {
        status = ShardingCatalogManager::get(opCtx)->initializeConfigDatabaseIfNeeded(opCtx);
        if (!status.isOK() && status != ErrorCodes::AlreadyInitialized) {
            if (ErrorCodes::isShutdownError(status.code())) {
                // Don't fassert if we're mid-shutdown, let the shutdown happen gracefully.
                return;
            }

            fassertFailedWithStatus(40184,
                                    Status(status.code(),
                                           str::stream()
                                               << "Failed to initialize config database on config "
                                                  "server's first transition to primary"
                                               << causedBy(status)));
        }

        if (status.isOK()) {
            // Load the clusterId into memory. Use local readConcern, since we can't use majority
            // readConcern in drain mode because the global lock prevents replication. This is
            // safe, since if the clusterId write is rolled back, any writes that depend on it will
            // also be rolled back.
            // Since we *just* wrote the cluster ID to the config.version document (via
            // ShardingCatalogManager::initializeConfigDatabaseIfNeeded), this should always
            // succeed.
            status = ClusterIdentityLoader::get(opCtx)->loadClusterId(
                opCtx, repl::ReadConcernLevel::kLocalReadConcern);

            if (ErrorCodes::isShutdownError(status.code())) {
                // Don't fassert if we're mid-shutdown, let the shutdown happen gracefully.
                return;
            }

            fassertStatusOK(40217, status);
        }

        // Free any leftover locks from previous instantiations.
        auto distLockManager = Grid::get(opCtx)->catalogClient()->getDistLockManager();
        distLockManager->unlockAll(opCtx, distLockManager->getProcessID());

        // If this is a config server node becoming a primary, start the balancer
        Balancer::get(opCtx)->initiateBalancer(opCtx);

        if (auto validator = LogicalTimeValidator::get(_service)) {
            validator->enableKeyGenerator(opCtx, true);
        }
    } else if (ShardingState::get(opCtx)->enabled()) {
        invariant(serverGlobalParams.clusterRole == ClusterRole::ShardServer);

        const auto configsvrConnStr =
            Grid::get(opCtx)->shardRegistry()->getConfigShard()->getConnString();
        auto status = ShardingState::get(opCtx)->updateShardIdentityConfigString(
            opCtx, configsvrConnStr.toString());
        if (!status.isOK()) {
            warning() << "error encountered while trying to update config connection string to "
                      << configsvrConnStr << causedBy(status);
        }

        CatalogCacheLoader::get(_service).onStepUp();
        ShardingState::get(_service)->initiateChunkSplitter();
    } else {  // unsharded
        if (auto validator = LogicalTimeValidator::get(_service)) {
            validator->enableKeyGenerator(opCtx, true);
        }
    }

    SessionCatalog::get(_service)->onStepUp(opCtx);

    // There is a slight chance that some stale metadata might have been loaded before the latest
    // optime has been recovered, so throw out everything that we have up to now
    ShardingState::get(opCtx)->markCollectionsNotShardedAtStepdown();
}

void ReplicationCoordinatorExternalStateImpl::signalApplierToChooseNewSyncSource() {
    LockGuard lk(_threadMutex);
    if (_bgSync) {
        _bgSync->clearSyncTarget();
    }
}

void ReplicationCoordinatorExternalStateImpl::stopProducer() {
    LockGuard lk(_threadMutex);
    if (_bgSync) {
        _bgSync->stop(false);
    }
}

void ReplicationCoordinatorExternalStateImpl::startProducerIfStopped() {
    LockGuard lk(_threadMutex);
    if (_bgSync) {
        _bgSync->startProducerIfStopped();
    }
}

void ReplicationCoordinatorExternalStateImpl::_dropAllTempCollections(OperationContext* opCtx) {
    std::vector<std::string> dbNames;
    StorageEngine* storageEngine = _service->getGlobalStorageEngine();
    storageEngine->listDatabases(&dbNames);

    for (std::vector<std::string>::iterator it = dbNames.begin(); it != dbNames.end(); ++it) {
        // The local db is special because it isn't replicated. It is cleared at startup even on
        // replica set members.
        if (*it == "local")
            continue;
        LOG(2) << "Removing temporary collections from " << *it;
        Database* db = dbHolder().get(opCtx, *it);
        // Since we must be holding the global lock during this function, if listDatabases
        // returned this dbname, we should be able to get a reference to it - it can't have
        // been dropped.
        invariant(db);
        db->clearTmpCollections(opCtx);
    }
}

void ReplicationCoordinatorExternalStateImpl::dropAllSnapshots() {
    if (auto manager = _service->getGlobalStorageEngine()->getSnapshotManager())
        manager->dropAllSnapshots();
}

void ReplicationCoordinatorExternalStateImpl::updateCommittedSnapshot(
    const OpTime& newCommitPoint) {
    auto manager = _service->getGlobalStorageEngine()->getSnapshotManager();
    if (manager) {
        manager->setCommittedSnapshot(newCommitPoint.getTimestamp());
    }
    notifyOplogMetadataWaiters(newCommitPoint);
}

bool ReplicationCoordinatorExternalStateImpl::snapshotsEnabled() const {
    return _service->getGlobalStorageEngine()->getSnapshotManager() != nullptr;
}

void ReplicationCoordinatorExternalStateImpl::notifyOplogMetadataWaiters(
    const OpTime& committedOpTime) {
    signalOplogWaiters();

    // Notify the DropPendingCollectionReaper if there are any drop-pending collections with drop
    // optimes before or at the committed optime.
    if (auto earliestDropOpTime = _dropPendingCollectionReaper->getEarliestDropOpTime()) {
        if (committedOpTime >= *earliestDropOpTime) {
            auto reaper = _dropPendingCollectionReaper;
            scheduleWork(
                _taskExecutor.get(),
                [committedOpTime, reaper](const executor::TaskExecutor::CallbackArgs& args) {
                    auto opCtx = cc().makeOperationContext();
                    reaper->dropCollectionsOlderThan(opCtx.get(), committedOpTime);
                });
        }
    }
}

double ReplicationCoordinatorExternalStateImpl::getElectionTimeoutOffsetLimitFraction() const {
    return replElectionTimeoutOffsetLimitFraction;
}

bool ReplicationCoordinatorExternalStateImpl::isReadCommittedSupportedByStorageEngine(
    OperationContext* opCtx) const {
    auto storageEngine = opCtx->getServiceContext()->getGlobalStorageEngine();
    // This should never be called if the storage engine has not been initialized.
    invariant(storageEngine);
    return storageEngine->getSnapshotManager();
}

StatusWith<OpTime> ReplicationCoordinatorExternalStateImpl::multiApply(
    OperationContext* opCtx,
    MultiApplier::Operations ops,
    MultiApplier::ApplyOperationFn applyOperation) {
    return repl::multiApply(opCtx, _writerPool.get(), std::move(ops), applyOperation);
}

Status ReplicationCoordinatorExternalStateImpl::multiSyncApply(MultiApplier::OperationPtrs* ops) {
    // SyncTail* argument is not used by repl::multiSyncApply().
    repl::multiSyncApply(ops, nullptr);
    // multiSyncApply() will throw or abort on error, so we hardcode returning OK.
    return Status::OK();
}

Status ReplicationCoordinatorExternalStateImpl::multiInitialSyncApply(
    MultiApplier::OperationPtrs* ops, const HostAndPort& source, AtomicUInt32* fetchCount) {
    // repl::multiInitialSyncApply uses SyncTail::shouldRetry() (and implicitly getMissingDoc())
    // to fetch missing documents during initial sync. Therefore, it is fine to construct SyncTail
    // with invalid BackgroundSync, MultiSyncApplyFunc and writerPool arguments because we will not
    // be accessing any SyncTail functionality that require these constructor parameters.
    SyncTail syncTail(nullptr, SyncTail::MultiSyncApplyFunc(), nullptr);
    syncTail.setHostname(source.toString());
    return repl::multiInitialSyncApply(ops, &syncTail, fetchCount);
}

std::unique_ptr<OplogBuffer> ReplicationCoordinatorExternalStateImpl::makeInitialSyncOplogBuffer(
    OperationContext* opCtx) const {
    if (initialSyncOplogBuffer == kCollectionOplogBufferName) {
        invariant(initialSyncOplogBufferPeekCacheSize >= 0);
        OplogBufferCollection::Options options;
        options.peekCacheSize = std::size_t(initialSyncOplogBufferPeekCacheSize);
        return stdx::make_unique<OplogBufferProxy>(
            stdx::make_unique<OplogBufferCollection>(StorageInterface::get(opCtx), options));
    } else {
        return stdx::make_unique<OplogBufferBlockingQueue>();
    }
}

std::unique_ptr<OplogBuffer> ReplicationCoordinatorExternalStateImpl::makeSteadyStateOplogBuffer(
    OperationContext* opCtx) const {
    return stdx::make_unique<OplogBufferBlockingQueue>();
}

std::size_t ReplicationCoordinatorExternalStateImpl::getOplogFetcherMaxFetcherRestarts() const {
    return oplogFetcherMaxFetcherRestarts.load();
}

JournalListener::Token ReplicationCoordinatorExternalStateImpl::getToken() {
    return repl::getGlobalReplicationCoordinator()->getMyLastAppliedOpTime();
}

void ReplicationCoordinatorExternalStateImpl::onDurable(const JournalListener::Token& token) {
    repl::getGlobalReplicationCoordinator()->setMyLastDurableOpTimeForward(token);
}

/*
featdoc_1:PRIMARY> 
featdoc_1:PRIMARY> db.oplog.rs.find().sort({"ts":-1}).limit(1)
{ "ts" : Timestamp(1565771474, 1), "t" : NumberLong(5), "h" : NumberLong("-6325785047449693380"), "v" : 2, "op" : "n", "ns" : "", "wall" : ISODate("2019-08-14T08:31:14.623Z"), "o" : { "msg" : "periodic noop" } }
*/
//主从可以通过在oplog中定期增加periodic noop来进行同步
//ReplicationCoordinatorImpl::signalDrainComplete
void ReplicationCoordinatorExternalStateImpl::startNoopWriter(OpTime opTime) {
    invariant(_noopWriter);
    _noopWriter->startWritingPeriodicNoops(opTime).transitional_ignore();
}

void ReplicationCoordinatorExternalStateImpl::stopNoopWriter() {
    invariant(_noopWriter);
    _noopWriter->stopWritingPeriodicNoops();
}

void ReplicationCoordinatorExternalStateImpl::setupNoopWriter(Seconds waitTime) {
    invariant(!_noopWriter);

    _noopWriter = stdx::make_unique<NoopWriter>(waitTime);
}
}  // namespace repl
}  // namespace mongo
