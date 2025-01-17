/**
 *    Copyright (C) 2015 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/migration_source_manager.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/s/migration_chunk_cloner_source_legacy.h"
#include "mongo/db/s/migration_util.h"
#include "mongo/db/s/shard_metadata_util.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/s/sharding_state_recovery.h"
#include "mongo/executor/task_executor.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_shard_collection.h"
#include "mongo/s/catalog_cache_loader.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/commit_chunk_migration_request_type.h"
#include "mongo/s/set_shard_version_request.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/s/stale_exception.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/elapsed_tracker.h"
#include "mongo/util/exit.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

using namespace shardmetadatautil;

namespace {

// Wait at most this much time for the recipient to catch up sufficiently so critical section can be
// entered moveChunk全量迁移阶段最长时间
const Hours kMaxWaitToEnterCriticalSectionTimeout(6);
const char kMigratedChunkVersionField[] = "migratedChunkVersion";
const char kControlChunkVersionField[] = "controlChunkVersion";
const char kWriteConcernField[] = "writeConcern";
const WriteConcernOptions kMajorityWriteConcern(WriteConcernOptions::kMajority,
                                                WriteConcernOptions::SyncMode::UNSET,
                                                Seconds(15));

/**
 * Best-effort attempt to ensure the recipient shard has refreshed its routing table to
 * 'newCollVersion'. Fires and forgets an asychronous remote setShardVersion command.
 */
void refreshRecipientRoutingTable(OperationContext* opCtx,
                                  const NamespaceString& nss,
                                  ShardId toShard,
                                  const HostAndPort& toShardHost,
                                  const ChunkVersion& newCollVersion) {
    SetShardVersionRequest ssv = SetShardVersionRequest::makeForVersioningNoPersist(
        Grid::get(opCtx)->shardRegistry()->getConfigServerConnectionString(),
        toShard,
        ConnectionString(toShardHost),
        nss,
        newCollVersion,
        false);

    const executor::RemoteCommandRequest request(
        toShardHost,
        NamespaceString::kAdminDb.toString(),
        ssv.toBSON(),
        ReadPreferenceSetting{ReadPreference::PrimaryOnly}.toContainingBSON(),
        opCtx,
        executor::RemoteCommandRequest::kNoTimeout);

    executor::TaskExecutor* const executor =
        Grid::get(opCtx)->getExecutorPool()->getFixedExecutor();
    Status s =
        executor
            ->scheduleRemoteCommand(
                request, [](const executor::TaskExecutor::RemoteCommandCallbackArgs& args) {})
            .getStatus();
    std::move(s).ignore();
}

Status checkCollectionEpochMatches(const ScopedCollectionMetadata& metadata, OID expectedEpoch) {
    if (metadata && metadata->getCollVersion().epoch() == expectedEpoch)
        return Status::OK();

    return {ErrorCodes::IncompatibleShardingMetadata,
            str::stream() << "The collection was dropped or recreated since the migration began. "
                          << "Expected collection epoch: "
                          << expectedEpoch.toString()
                          << ", but found: "
                          << (metadata ? metadata->getCollVersion().epoch().toString()
                                       : "unsharded collection.")};
}

}  // namespace

MONGO_FP_DECLARE(doNotRefreshRecipientAfterCommit);
MONGO_FP_DECLARE(failMigrationCommit);
MONGO_FP_DECLARE(hangBeforeLeavingCriticalSection);
MONGO_FP_DECLARE(migrationCommitNetworkError);

//源分片迁移管理模块初始化
MigrationSourceManager::MigrationSourceManager(OperationContext* opCtx,
                                               MoveChunkRequest request,
                                               ConnectionString donorConnStr,
                                               HostAndPort recipientHost)
	//moveChunk命令参数
    : _args(std::move(request)), 
    //源分片地址信息
      _donorConnStr(std::move(donorConnStr)),
    //目的分片信息
      _recipientHost(std::move(recipientHost)) {
      //必须预先获得锁(注意不是断言)
    invariant(!opCtx->lockState()->isLocked());

    // Disallow moving a chunk to ourselves
    //源和目的不能是同一个分片
    uassert(ErrorCodes::InvalidOptions,
            "Destination shard cannot be the same as source",
            _args.getFromShardId() != _args.getToShardId());

    log() << "Starting chunk migration " << redact(_args.toString())
          << " with expected collection version epoch " << _args.getVersionEpoch();

    // Force refresh of the metadata to ensure we have the latest
    {
    	//获取shardstat状态信息，也就是db.runCommand("shardingState")
        auto const shardingState = ShardingState::get(opCtx);

        ChunkVersion unusedShardVersion;
        Status refreshStatus = 
			//获取nss对应shardversion //获取nss对应的最新ChunkVersion
            shardingState->refreshMetadataNow(opCtx, getNss(), &unusedShardVersion);
        uassert(refreshStatus.code(),
                str::stream() << "cannot start migrate of chunk " << _args.toString() << " due to "
                              << refreshStatus.reason(),
                refreshStatus.isOK());
    }

    // Snapshot the committed metadata from the time the migration starts
    const auto collectionMetadataAndUUID = [&] {
        AutoGetCollection autoColl(opCtx, getNss(), MODE_IS);
        uassert(ErrorCodes::InvalidOptions,
                "cannot move chunks for a collection that doesn't exist",
                autoColl.getCollection());

        UUID collectionUUID;
        if (autoColl.getCollection()->uuid()) {
			//AutoGetCollection::getCollection
            collectionUUID = autoColl.getCollection()->uuid().value();
        }

		//CollectionShardingState::getMetadata 获取元数据信息ScopedCollectionMetadata
        auto metadata = CollectionShardingState::get(opCtx, getNss())->getMetadata();
        uassert(ErrorCodes::IncompatibleShardingMetadata,
                str::stream() << "cannot move chunks for an unsharded collection",
                metadata);

        return std::make_tuple(std::move(metadata), std::move(collectionUUID));
    }();

    const auto& collectionMetadata = std::get<0>(collectionMetadataAndUUID);

	//CollectionMetadata::getCollVersion
    const auto collectionVersion = collectionMetadata->getCollVersion();
	//CollectionMetadata::getShardVersion
    const auto shardVersion = collectionMetadata->getShardVersion();

    // If the shard major version is zero, this means we do not have any chunks locally to migrate
    //没有需要迁移的块
    uassert(ErrorCodes::IncompatibleShardingMetadata,
            str::stream() << "cannot move chunk " << _args.toString()
                          << " because the shard doesn't contain any chunks",
            shardVersion.majorVersion() > 0);
	//集合已经删除
    uassert(ErrorCodes::StaleEpoch,
            str::stream() << "cannot move chunk " << _args.toString()
                          << " because collection may have been dropped. "
                          << "current epoch: "
                          << collectionVersion.epoch()
                          << ", cmd epoch: "
                          << _args.getVersionEpoch(),
            _args.getVersionEpoch() == collectionVersion.epoch());

    ChunkType chunkToMove;
    chunkToMove.setMin(_args.getMinKey());
    chunkToMove.setMax(_args.getMaxKey());

	//collectionMetadata::checkChunkIsValid  chunk检查
    Status chunkValidateStatus = collectionMetadata->checkChunkIsValid(chunkToMove);
    uassert(chunkValidateStatus.code(),
            str::stream() << "Unable to move chunk with arguments '" << redact(_args.toString())
                          << "' due to error "
                          << redact(chunkValidateStatus.reason()),
            chunkValidateStatus.isOK());

	//ChunkVersion::epoch,获取_epoch
    _collectionEpoch = collectionVersion.epoch();
	//获取collectionUUID信息
    _collectionUuid = std::get<1>(collectionMetadataAndUUID);
}

MigrationSourceManager::~MigrationSourceManager() {
    invariant(!_cloneDriver);
}

NamespaceString MigrationSourceManager::getNss() const {
    return _args.getNss();
}

//MoveChunkCommand::_runImpl调用 
//发送_recvChunkStart后进入kCloning状态，目的收到命令后进行真正的数据迁移，数据迁移由目的触发从源拉取
Status MigrationSourceManager::startClone(OperationContext* opCtx) {
    invariant(!opCtx->lockState()->isLocked());
    invariant(_state == kCreated);
    auto scopedGuard = MakeGuard([&] { cleanupOnError(opCtx); });

	/*
	2020-07-20T12:24:46.778+0800 I SHARDING [conn84350] about to log metadata event into changelog: 
	{ _id: "bjcp4983-2020-07-20T12:24:46.778+0800-5f151c8e31b53b31fd10c0d1", server: "bjcp4983", 
	clientAddr: "10.64.54.5:44022", time: new Date(1595219086778), what: "moveChunk.start", 
	ns: "ocloud_cold_data_db.ocloud_cold_data_t", details: { min: { user_id: "472908302", module: "album", 
	md5: "1816ADAEB60E49DA9A4EF633FD5BE84C" }, max: { user_id: "472909051", module: "album", md5: "859526FAF8E58683284FE04F5484AC7A" }, 
	from: "ocloud_WbUiXohI_shard_4", to: "ocloud_WbUiXohI_shard_9" } }
	*/
	//记录logChange到cfg的config.changelog
    Grid::get(opCtx)
        ->catalogClient()
        ->logChange(opCtx,
                    "moveChunk.start",
                    getNss().ns(),
                    BSON("min" << _args.getMinKey() << "max" << _args.getMaxKey() << "from"
                               << _args.getFromShardId()
                               << "to"
                               << _args.getToShardId()),
                    ShardingCatalogClient::kMajorityWriteConcern)
        .ignore();

    {
        // Register for notifications from the replication subsystem
        AutoGetCollection autoColl(opCtx, getNss(), MODE_IX, MODE_X);
        auto css = CollectionShardingState::get(opCtx, getNss().ns());

        const auto metadata = css->getMetadata();
		//epoch检查
        Status status = checkCollectionEpochMatches(metadata, _collectionEpoch);
        if (!status.isOK())
            return status;

        // Having the metadata manager registered on the collection sharding state is what indicates
        // that a chunk on that collection is being migrated. With an active migration, write
        // operations require the cloner to be present in order to track changes to the chunk which
        // needs to be transmitted to the recipient.
        //构造_cloneDriver
        _cloneDriver = stdx::make_unique<MigrationChunkClonerSourceLegacy>(
            _args, metadata->getKeyPattern(), _donorConnStr, _recipientHost);

        css->setMigrationSourceManager(opCtx, this);
    }

	//MigrationChunkClonerSourceLegacy::startClone //发送_recvChunkStart后进入kCloning状态
    Status startCloneStatus = _cloneDriver->startClone(opCtx);
    if (!startCloneStatus.isOK()) {
        return startCloneStatus;
    }

    _state = kCloning;
    scopedGuard.Dismiss();
    return Status::OK();
}

//MoveChunkCommand::_runImpl 循环检查目的分片全量迁移过程是否完成
Status MigrationSourceManager::awaitToCatchUp(OperationContext* opCtx) {
    invariant(!opCtx->lockState()->isLocked());
    invariant(_state == kCloning);
    auto scopedGuard = MakeGuard([&] { cleanupOnError(opCtx); });

    // Block until the cloner deems it appropriate to enter the critical section.
    //循环查询目的分片从原分片拉取全量数据是否完成，默认最多等待6小时
    Status catchUpStatus = _cloneDriver->awaitUntilCriticalSectionIsAppropriate(
        opCtx, kMaxWaitToEnterCriticalSectionTimeout);
    if (!catchUpStatus.isOK()) {
        return catchUpStatus;
    }

	//进入_recvChunkStatus状态
    _state = kCloneCaughtUp;
    scopedGuard.Dismiss();
    return Status::OK();
}

//加互斥锁保证表不会有新数据写入  
//MoveChunkCommand::_runImpl
Status MigrationSourceManager::enterCriticalSection(OperationContext* opCtx) {
    invariant(!opCtx->lockState()->isLocked());
    invariant(_state == kCloneCaughtUp);
    auto scopedGuard = MakeGuard([&] { cleanupOnError(opCtx); });

    {
        const auto metadata = [&] {
            AutoGetCollection autoColl(opCtx, _args.getNss(), MODE_IS);
            return CollectionShardingState::get(opCtx, _args.getNss())->getMetadata();
        }();

        Status status = checkCollectionEpochMatches(metadata, _collectionEpoch);
        if (!status.isOK())
            return status;

        _notifyChangeStreamsOnRecipientFirstChunk(opCtx, metadata);
    }

    // Mark the shard as running critical operation, which requires recovery on crash.
    //
    // NOTE: The 'migrateChunkToNewShard' oplog message written by the above call to
    // '_notifyChangeStreamsOnRecipientFirstChunk' depends on this majority write to carry its local
    // write to majority committed.
    Status status = ShardingStateRecovery::startMetadataOp(opCtx);
    if (!status.isOK()) {
        return status;
    }

    {
        // The critical section must be entered with collection X lock in order to ensure there are
        // no writes which could have entered and passed the version check just before we entered
        // the crticial section, but managed to complete after we left it.
        //保证表不会有新的写入
        AutoGetCollection autoColl(opCtx, getNss(), MODE_IX, MODE_X);

        // IMPORTANT: After this line, the critical section is in place and needs to be signaled
        _critSecSignal = std::make_shared<Notification<void>>();
    }

    _state = kCriticalSection;

    // Persist a signal to secondaries that we've entered the critical section. This is will cause
    // secondaries to refresh their routing table when next accessed, which will block behind the
    // critical section. This ensures causal consistency by preventing a stale mongos with a cluster
    // time inclusive of the migration config commit update from accessing secondary data.
    // Note: this write must occur after the critSec flag is set, to ensure the secondary refresh
    // will stall behind the flag.
    Status signalStatus =
        updateShardCollectionsEntry(opCtx,
                                    BSON(ShardCollectionType::ns() << getNss().ns()),
                                    BSONObj(),
                                    BSON(ShardCollectionType::enterCriticalSectionCounter() << 1),
                                    false /*upsert*/);
    if (!signalStatus.isOK()) {
        return {
            ErrorCodes::OperationFailed,
            str::stream() << "Failed to persist critical section signal for secondaries due to: "
                          << signalStatus.toString()};
    }

    log() << "Migration successfully entered critical section";

    scopedGuard.Dismiss();
    return Status::OK();
}

//发送_recvChunkCommit到目的分片，通知目的分片原分片已经停止写入，可以拉取增量数据了
Status MigrationSourceManager::commitChunkOnRecipient(OperationContext* opCtx) {
    invariant(!opCtx->lockState()->isLocked());
    invariant(_state == kCriticalSection);
    auto scopedGuard = MakeGuard([&] { cleanupOnError(opCtx); });

    // Tell the recipient shard to fetch the latest changes.
    //MigrationChunkClonerSourceLegacy::commitClone
    //发送_recvChunkCommit到目的分片，通知目的分片原分片已经停止写入，可以拉取增量数据了
    Status commitCloneStatus = _cloneDriver->commitClone(opCtx);

    if (MONGO_FAIL_POINT(failMigrationCommit) && commitCloneStatus.isOK()) {
        commitCloneStatus = {ErrorCodes::InternalError,
                             "Failing _recvChunkCommit due to failpoint."};
    }

    if (!commitCloneStatus.isOK()) {
        return {commitCloneStatus.code(),
                str::stream() << "commit clone failed due to " << commitCloneStatus.toString()};
    }

    _state = kCloneCompleted;
    scopedGuard.Dismiss();
    return Status::OK();
}

//chunk迁移完成，修改config集群元数据信息，同时删除源分片已经迁移走得chunk数据
////构造"_configsvrCommitChunkMigration"命令，提交相关数据给config服务器
Status MigrationSourceManager::commitChunkMetadataOnConfig(OperationContext* opCtx) {
    invariant(!opCtx->lockState()->isLocked());
    invariant(_state == kCloneCompleted);
    auto scopedGuard = MakeGuard([&] { cleanupOnError(opCtx); });

    // If we have chunks left on the FROM shard, bump the version of one of them as well. This will
    // change the local collection major version, which indicates to other processes that the chunk
    // metadata has changed and they should refresh.
    BSONObjBuilder builder;

    {
        const auto metadata = [&] {
            AutoGetCollection autoColl(opCtx, _args.getNss(), MODE_IS);
            return CollectionShardingState::get(opCtx, _args.getNss())->getMetadata();
        }();

        Status status = checkCollectionEpochMatches(metadata, _collectionEpoch);
        if (!status.isOK())
            return status;

        boost::optional<ChunkType> controlChunkType = boost::none;
        if (metadata->getNumChunks() > 1) {
            ChunkType differentChunk;
            invariant(metadata->getDifferentChunk(_args.getMinKey(), &differentChunk));
            invariant(differentChunk.getMin().woCompare(_args.getMinKey()) != 0);
            controlChunkType = std::move(differentChunk);
        } else {
            log() << "Moving last chunk for the collection out";
        }

        ChunkType migratedChunkType;
        migratedChunkType.setMin(_args.getMinKey());
        migratedChunkType.setMax(_args.getMaxKey());

		//构造_configsvrCommitChunkMigration命令
        CommitChunkMigrationRequest::appendAsCommand(&builder,
                                                     getNss(),
                                                     _args.getFromShardId(),
                                                     _args.getToShardId(),
                                                     migratedChunkType,
                                                     controlChunkType,
                                                     metadata->getCollVersion());

        builder.append(kWriteConcernField, kMajorityWriteConcern.toBSON());
    }

    // Read operations must begin to wait on the critical section just before we send the commit
    // operation to the config server
    {
        AutoGetCollection autoColl(opCtx, getNss(), MODE_IX, MODE_X);
        _readsShouldWaitOnCritSec = true;
    }

	//发送命令到config
    auto commitChunkMigrationResponse =
        Grid::get(opCtx)->shardRegistry()->getConfigShard()->runCommandWithFixedRetryAttempts(
            opCtx,
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            "admin",
            builder.obj(),
            Shard::RetryPolicy::kIdempotent);

    if (MONGO_FAIL_POINT(migrationCommitNetworkError)) {
        commitChunkMigrationResponse = Status(
            ErrorCodes::InternalError, "Failpoint 'migrationCommitNetworkError' generated error");
    }

    const Status migrationCommitStatus =
        (commitChunkMigrationResponse.isOK() ? commitChunkMigrationResponse.getValue().commandStatus
                                             : commitChunkMigrationResponse.getStatus());
	//跟新config元数据失败
    if (!migrationCommitStatus.isOK()) {
        // Need to get the latest optime in case the refresh request goes to a secondary --
        // otherwise the read won't wait for the write that _configsvrCommitChunkMigration may have
        // done
        log() << "Error occurred while committing the migration. Performing a majority write "
                 "against the config server to obtain its latest optime"
              << causedBy(redact(migrationCommitStatus));

        Status status = Grid::get(opCtx)->catalogClient()->logChange(
            opCtx,
            "moveChunk.validating",
            getNss().ns(),
            BSON("min" << _args.getMinKey() << "max" << _args.getMaxKey() << "from"
                       << _args.getFromShardId()
                       << "to"
                       << _args.getToShardId()),
            ShardingCatalogClient::kMajorityWriteConcern);

        if ((ErrorCodes::isInterruption(status.code()) ||
             ErrorCodes::isShutdownError(status.code()) ||
             status == ErrorCodes::CallbackCanceled) &&
            globalInShutdownDeprecated()) {
            // Since the server is already doing a clean shutdown, this call will just join the
            // previous shutdown call
            shutdown(waitForShutdown());
        }

        fassertStatusOK(
            40137,
            {status.code(),
             str::stream() << "Failed to commit migration for chunk " << _args.toString()
                           << " due to "
                           << redact(migrationCommitStatus)
                           << ". Updating the optime with a write before refreshing the "
                           << "metadata also failed with "
                           << redact(status)});
    }

	//跟新config元数据成功

	
    // Do a best effort attempt to incrementally refresh the metadata before leaving the critical
    // section. It is okay if the refresh fails because that will cause the metadata to be cleared
    // and subsequent callers will try to do a full refresh.
    ChunkVersion unusedShardVersion;
    Status refreshStatus =
        ShardingState::get(opCtx)->refreshMetadataNow(opCtx, getNss(), &unusedShardVersion);

    if (!refreshStatus.isOK()) {
        AutoGetCollection autoColl(opCtx, getNss(), MODE_IX, MODE_X);

        CollectionShardingState::get(opCtx, getNss())->refreshMetadata(opCtx, nullptr);

        log() << "Failed to refresh metadata after a "
              << (migrationCommitStatus.isOK() ? "failed commit attempt" : "successful commit")
              << ". Metadata was cleared so it will get a full refresh when accessed again."
              << causedBy(redact(refreshStatus));

        // migrationCommitStatus may be OK or an error. The migration is considered a success at
        // this point if the commit succeeded. The metadata refresh either occurred or the metadata
        // was safely cleared.
        return {migrationCommitStatus.code(),
                str::stream() << "Orphaned range not cleaned up. Failed to refresh metadata after"
                                 " migration commit due to '"
                              << refreshStatus.toString()
                              << "', and commit failed due to '"
                              << migrationCommitStatus.toString()
                              << "'"};
    }

    auto refreshedMetadata = [&] {
        AutoGetCollection autoColl(opCtx, getNss(), MODE_IS);
        return CollectionShardingState::get(opCtx, getNss())->getMetadata();
    }();

    if (!refreshedMetadata) {
        return {ErrorCodes::NamespaceNotSharded,
                str::stream() << "Chunk move failed because collection '" << getNss().ns()
                              << "' is no longer sharded. The migration commit error was: "
                              << migrationCommitStatus.toString()};
    }

    if (refreshedMetadata->keyBelongsToMe(_args.getMinKey())) {
        // The chunk modification was not applied, so report the original error
        return {migrationCommitStatus.code(),
                str::stream() << "Chunk move was not successful due to "
                              << migrationCommitStatus.reason()};
    }

    // Migration succeeded
    log() << "Migration succeeded and updated collection version to "
          << refreshedMetadata->getCollVersion();

    MONGO_FAIL_POINT_PAUSE_WHILE_SET(hangBeforeLeavingCriticalSection);

    scopedGuard.Dismiss();

    // Exit critical section, clear old scoped collection metadata.
    _cleanup(opCtx);

	//记录changelog
    Grid::get(opCtx)
        ->catalogClient()
        ->logChange(opCtx,
                    "moveChunk.commit",
                    getNss().ns(),
                    BSON("min" << _args.getMinKey() << "max" << _args.getMaxKey() << "from"
                               << _args.getFromShardId()
                               << "to"
                               << _args.getToShardId()),
                    ShardingCatalogClient::kMajorityWriteConcern)
        .ignore();

    // Wait for the metadata update to be persisted before attempting to delete orphaned documents
    // so that metadata changes propagate to secondaries first
    CatalogCacheLoader::get(opCtx).waitForCollectionFlush(opCtx, getNss());

    const ChunkRange range(_args.getMinKey(), _args.getMaxKey());

	//源分片删除已经迁移得chunk数据
    auto notification = [&] {
        auto const whenToClean = _args.getWaitForDelete() ? CollectionShardingState::kNow
                                                          : CollectionShardingState::kDelayed;
        AutoGetCollection autoColl(opCtx, getNss(), MODE_IS);
        return CollectionShardingState::get(opCtx, getNss())->cleanUpRange(range, whenToClean);
    }();

    if (!MONGO_FAIL_POINT(doNotRefreshRecipientAfterCommit)) {
        // Best-effort make the recipient refresh its routing table to the new collection version.
        refreshRecipientRoutingTable(opCtx,
                                     getNss(),
                                     _args.getToShardId(),
                                     _recipientHost,
                                     refreshedMetadata->getCollVersion());
    }

    if (_args.getWaitForDelete()) {
        log() << "Waiting for cleanup of " << getNss().ns() << " range "
              << redact(range.toString());
        return notification.waitStatus(opCtx);
    }

	//等待源分片删除已经迁移走得chunk数据完毕
    if (notification.ready() && !notification.waitStatus(opCtx).isOK()) {
        warning() << "Failed to initiate cleanup of " << getNss().ns() << " range "
                  << redact(range.toString())
                  << " due to: " << redact(notification.waitStatus(opCtx));
    } else {
        log() << "Leaving cleanup of " << getNss().ns() << " range " << redact(range.toString())
              << " to complete in background";
        notification.abandon();
    }

    return Status::OK();
}

void MigrationSourceManager::cleanupOnError(OperationContext* opCtx) {
    if (_state == kDone) {
        return;
    }

    Grid::get(opCtx)
        ->catalogClient()
        ->logChange(opCtx,
                    "moveChunk.error",
                    getNss().ns(),
                    BSON("min" << _args.getMinKey() << "max" << _args.getMaxKey() << "from"
                               << _args.getFromShardId()
                               << "to"
                               << _args.getToShardId()),
                    ShardingCatalogClient::kMajorityWriteConcern)
        .ignore();

    _cleanup(opCtx);
}

void MigrationSourceManager::_notifyChangeStreamsOnRecipientFirstChunk(
    OperationContext* opCtx, const ScopedCollectionMetadata& metadata) {
    // Change streams are only supported in 3.6 and above
    if (serverGlobalParams.featureCompatibility.getVersion() !=
        ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo36)
        return;

    // If this is not the first donation, there is nothing to be done
    if (metadata->getChunkManager()->getVersion(_args.getToShardId()).isSet())
        return;

    const std::string dbgMessage = str::stream()
        << "Migrating chunk from shard " << _args.getFromShardId() << " to shard "
        << _args.getToShardId() << " with no chunks for this collection";

    // The message expected by change streams
    const auto o2Message = BSON("type"
                                << "migrateChunkToNewShard"
                                << "from"
                                << _args.getFromShardId()
                                << "to"
                                << _args.getToShardId());

    auto const serviceContext = opCtx->getClient()->getServiceContext();

    AutoGetCollection autoColl(opCtx, NamespaceString::kRsOplogNamespace, MODE_IX);
    writeConflictRetry(
        opCtx, "migrateChunkToNewShard", NamespaceString::kRsOplogNamespace.ns(), [&] {
            WriteUnitOfWork uow(opCtx);
            serviceContext->getOpObserver()->onInternalOpMessage(
                opCtx, getNss(), _collectionUuid, BSON("msg" << dbgMessage), o2Message);
            uow.commit();
        });
}

void MigrationSourceManager::_cleanup(OperationContext* opCtx) {
    invariant(_state != kDone);

    auto cloneDriver = [&]() {
        // Unregister from the collection's sharding state
        AutoGetCollection autoColl(opCtx, getNss(), MODE_IX, MODE_X);

        auto css = CollectionShardingState::get(opCtx, getNss().ns());

        // The migration source manager is not visible anymore after it is unregistered from the
        // collection
        css->clearMigrationSourceManager(opCtx);

        // Leave the critical section.
        if (_critSecSignal) {
            _critSecSignal->set();
        }

        return std::move(_cloneDriver);
    }();

    // Decrement the metadata op counter outside of the collection lock in order to hold it for as
    // short as possible.
    if (_state == kCriticalSection || _state == kCloneCompleted) {
        ShardingStateRecovery::endMetadataOp(opCtx);
    }

    if (cloneDriver) {
        cloneDriver->cancelClone(opCtx);
    }

    _state = kDone;
}

std::shared_ptr<Notification<void>> MigrationSourceManager::getMigrationCriticalSectionSignal(
    bool isForReadOnlyOperation) const {
    if (!isForReadOnlyOperation) {
        return _critSecSignal;
    }

    if (_readsShouldWaitOnCritSec) {
        return _critSecSignal;
    }

    return nullptr;
}

//ActiveMigrationsRegistry::getActiveMigrationStatusReport
BSONObj MigrationSourceManager::getMigrationStatusReport() const {
    return migrationutil::makeMigrationStatusDocument(getNss(),
                                                      _args.getFromShardId(),
                                                      _args.getToShardId(),
                                                      true,
                                                      _args.getMinKey(),
                                                      _args.getMaxKey());
}

}  // namespace mongo
