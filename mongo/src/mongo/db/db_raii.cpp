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

#include "mongo/platform/basic.h"

#include "mongo/db/db_raii.h"

#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/uuid_catalog.h"
#include "mongo/db/client.h"
#include "mongo/db/curop.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/stats/top.h"
#include "mongo/util/fail_point_service.h"

namespace mongo {

namespace {
MONGO_FP_DECLARE(setAutoGetCollectionWait);
}  // namespace

//AutoGetDb::AutoGetDb或者AutoGetOrCreateDb::AutoGetOrCreateDb->DatabaseHolderImpl::get从DatabaseHolderImpl._dbs数组查找获取Database
//DatabaseImpl::createCollection创建collection的表全部添加到DatabaseImpl._collections数组中
//AutoGetCollection::AutoGetCollection通过Database::getCollection或者UUIDCatalog::lookupCollectionByUUID(从UUIDCatalog._catalog数组通过查找uuid可以获取collection表信息)
//注意AutoGetCollection::AutoGetCollection构造函数可以是uuid，也有一个构造函数是nss，也就是可以通过uuid查找，也可以通过nss查找

AutoGetDb::AutoGetDb(OperationContext* opCtx, StringData ns, LockMode mode)
    : _dbLock(opCtx, ns, mode), 
      //DatabaseHolderImpl::get
      _db(dbHolder().get(opCtx, ns)) {}

//AutoGetDb::AutoGetDb从db数组获取db信息
AutoGetDb::AutoGetDb(OperationContext* opCtx, StringData ns, Lock::DBLock lock)
    : _dbLock(std::move(lock)), 
	//DatabaseHolderImpl::get从DatabaseHolderImpl._dbs数组查找获取DB
	_db(dbHolder().get(opCtx, ns)) {}

//AutoGetDb::AutoGetDb或者AutoGetOrCreateDb::AutoGetOrCreateDb->DatabaseHolderImpl::get从DatabaseHolderImpl._dbs数组查找获取Database
//DatabaseImpl::createCollection创建collection的表全部添加到DatabaseImpl._collections数组中
//AutoGetCollection::AutoGetCollection通过Database::getCollection或者UUIDCatalog::lookupCollectionByUUID(从UUIDCatalog._catalog数组通过查找uuid可以获取collection表信息)
//注意AutoGetCollection::AutoGetCollection构造函数可以是uuid，也有一个构造函数是nss，也就是可以通过uuid查找，也可以通过nss查找


//注意AutoGetCollection::AutoGetCollection构造函数可以是uuid，也有一个构造函数是nss，也就是可以通过uuid查找，也可以通过nss查找
//insertBatchAndHandleErrors会使用
AutoGetCollection::AutoGetCollection(OperationContext* opCtx,
                                     const NamespaceString& nss,
                                     const UUID& uuid,
                                     LockMode modeAll)
    : _viewMode(ViewMode::kViewsForbidden),
      _autoDb(opCtx, nss.db(), Lock::DBLock(opCtx, nss.db(), modeAll)),
      _collLock(opCtx->lockState(), nss.ns(), modeAll),
      //通过UUIDCatalog::lookupCollectionByUUID判断uuid是否存在，可能返回为NULL，表示该uuid不存在
      _coll(UUIDCatalog::get(opCtx).lookupCollectionByUUID(uuid)) {
    // Wait for a configured amount of time after acquiring locks if the failpoint is enabled.
    MONGO_FAIL_POINT_BLOCK(setAutoGetCollectionWait, customWait) {
        const BSONObj& data = customWait.getData();
        sleepFor(Milliseconds(data["waitForMillis"].numberInt()));
    }
}
//insertBatchAndHandleErrors
AutoGetCollection::AutoGetCollection(OperationContext* opCtx,
                                     const NamespaceString& nss,
                                     LockMode modeDB,
                                     LockMode modeColl,
                                     ViewMode viewMode)
    //这里先构造库锁，库锁Lock::DBLock::DBLock->_globalLock里面会构造全局锁
    : AutoGetCollection(opCtx, nss, modeColl, viewMode, Lock::DBLock(opCtx, nss.db(), modeDB)) {}

//AutoGetDb::AutoGetDb或者AutoGetOrCreateDb::AutoGetOrCreateDb->DatabaseHolderImpl::get从DatabaseHolderImpl._dbs数组查找获取Database
//DatabaseImpl::createCollection创建collection的表全部添加到DatabaseImpl._collections数组中
//AutoGetCollection::AutoGetCollection通过Database::getCollection或者UUIDCatalog::lookupCollectionByUUID(从UUIDCatalog._catalog数组通过查找uuid可以获取collection表信息)
//注意AutoGetCollection::AutoGetCollection构造函数可以是uuid，也有一个构造函数是nss，也就是可以通过uuid查找，也可以通过nss查找

//上面的AutoGetCollection::AutoGetCollection调用
//库表的初始化赋值都在这里
AutoGetCollection::AutoGetCollection(OperationContext* opCtx,
                                     const NamespaceString& nss,
                                     LockMode modeColl,
                                     ViewMode viewMode,
                                     Lock::DBLock lock)
    : _viewMode(viewMode),
      _autoDb(opCtx, nss.db(), std::move(lock)),
      //这里构造集合锁
      _collLock(opCtx->lockState(), nss.ns(), modeColl),
      //nss为db.collection，确定db和collection是否存在，collection不存在直接返回null
      //Database::getCollection
      _coll(_autoDb.getDb() ? _autoDb.getDb()->getCollection(opCtx, nss) : nullptr) {
    Database* db = _autoDb.getDb();
    // If the database exists, but not the collection, check for views.
    if (_viewMode == ViewMode::kViewsForbidden && db && !_coll &&
        db->getViewCatalog()->lookup(opCtx, nss.ns()))
        uasserted(ErrorCodes::CommandNotSupportedOnView,
                  str::stream() << "Namespace " << nss.ns() << " is a view, not a collection");

    // Wait for a configured amount of time after acquiring locks if the failpoint is enabled.
    MONGO_FAIL_POINT_BLOCK(setAutoGetCollectionWait, customWait) {
        const BSONObj& data = customWait.getData();
        sleepFor(Milliseconds(data["waitForMillis"].numberInt()));
    }
}

AutoGetCollectionOrView::AutoGetCollectionOrView(OperationContext* opCtx,
                                                 const NamespaceString& nss,
                                                 LockMode modeAll)
    : _autoColl(opCtx, nss, modeAll, modeAll, AutoGetCollection::ViewMode::kViewsPermitted),
      _view(_autoColl.getDb() && !_autoColl.getCollection()
                ? _autoColl.getDb()->getViewCatalog()->lookup(opCtx, nss.ns())
                : nullptr) {}


//AutoGetDb::AutoGetDb或者AutoGetOrCreateDb::AutoGetOrCreateDb->DatabaseHolderImpl::get从DatabaseHolderImpl._dbs数组查找获取Database
//DatabaseImpl::createCollection创建collection的表全部添加到DatabaseImpl._collections数组中
//AutoGetCollection::AutoGetCollection通过Database::getCollection或者UUIDCatalog::lookupCollectionByUUID(从UUIDCatalog._catalog数组通过查找uuid可以获取collection表信息)
//注意AutoGetCollection::AutoGetCollection构造函数可以是uuid，也有一个构造函数是nss，也就是可以通过uuid查找，也可以通过nss查找
AutoGetOrCreateDb::AutoGetOrCreateDb(OperationContext* opCtx, StringData ns, LockMode mode)
    : _dbLock(opCtx, ns, mode), 
		//DatabaseHolderImpl::get
		_db(dbHolder().get(opCtx, ns)) {
    invariant(mode == MODE_IX || mode == MODE_X);
    _justCreated = false;
    // If the database didn't exist, relock in MODE_X
    if (_db == NULL) {
        if (mode != MODE_X) {
            _dbLock.relockWithMode(MODE_X);
        }
		//	//DatabaseHolderImpl::openDb
        _db = dbHolder().openDb(opCtx, ns);
        _justCreated = true;
    }
}

AutoStatsTracker::AutoStatsTracker(OperationContext* opCtx,
                                   const NamespaceString& nss,
                                   Top::LockType lockType,
                                   boost::optional<int> dbProfilingLevel)
    : _opCtx(opCtx), _lockType(lockType) {
    if (!dbProfilingLevel) {
        // No profiling level was determined, attempt to read the profiling level from the Database
        // object.
        AutoGetDb autoDb(_opCtx, nss.db(), MODE_IS);
        if (autoDb.getDb()) {
            dbProfilingLevel = autoDb.getDb()->getProfilingLevel();
        }
    }
    stdx::lock_guard<Client> clientLock(*_opCtx->getClient());
    CurOp::get(_opCtx)->enter_inlock(nss.ns().c_str(), dbProfilingLevel);
}

AutoStatsTracker::~AutoStatsTracker() {
    auto curOp = CurOp::get(_opCtx);
    Top::get(_opCtx->getServiceContext())
        .record(_opCtx,
                curOp->getNS(),
                curOp->getLogicalOp(),
                _lockType,
                //这里把消耗的时间记录下来
                durationCount<Microseconds>(curOp->elapsedTimeExcludingPauses()),
                curOp->isCommand(),
                curOp->getReadWriteType());
}

AutoGetCollectionForRead::AutoGetCollectionForRead(OperationContext* opCtx,
                                                   const StringData dbName,
                                                   const UUID& uuid) {
    // Lock the database since a UUID will always be in the same database even though its
    // collection name may change.
    Lock::DBLock dbSLock(opCtx, dbName, MODE_IS);

    auto nss = UUIDCatalog::get(opCtx).lookupNSSByUUID(uuid);

    // If the UUID doesn't exist, we leave _autoColl to be boost::none.
    if (!nss.isEmpty()) {
        _autoColl.emplace(
            opCtx, nss, MODE_IS, AutoGetCollection::ViewMode::kViewsForbidden, std::move(dbSLock));

        // Note: this can yield.
        _ensureMajorityCommittedSnapshotIsValid(nss, opCtx);
    }
}

AutoGetCollectionForRead::AutoGetCollectionForRead(OperationContext* opCtx,
                                                   const NamespaceString& nss,
                                                   AutoGetCollection::ViewMode viewMode) {
    _autoColl.emplace(opCtx, nss, MODE_IS, MODE_IS, viewMode);

    // Note: this can yield.
    _ensureMajorityCommittedSnapshotIsValid(nss, opCtx);
}

AutoGetCollectionForRead::AutoGetCollectionForRead(OperationContext* opCtx,
                                                   const NamespaceString& nss,
                                                   AutoGetCollection::ViewMode viewMode,
                                                   Lock::DBLock lock) {
    _autoColl.emplace(opCtx, nss, MODE_IS, viewMode, std::move(lock));

    // Note: this can yield.
    _ensureMajorityCommittedSnapshotIsValid(nss, opCtx);
}
void AutoGetCollectionForRead::_ensureMajorityCommittedSnapshotIsValid(const NamespaceString& nss,
                                                                       OperationContext* opCtx) {
    while (true) {
        auto coll = _autoColl->getCollection();
        if (!coll) {
            return;
        }
        auto minSnapshot = coll->getMinimumVisibleSnapshot();
        if (!minSnapshot) {
            return;
        }
        auto mySnapshot = opCtx->recoveryUnit()->getMajorityCommittedSnapshot();
        if (!mySnapshot) {
            return;
        }
        if (mySnapshot >= minSnapshot) {
            return;
        }

        // Yield locks.
        _autoColl = boost::none;

        repl::ReplicationCoordinator::get(opCtx)->waitUntilSnapshotCommitted(opCtx, *minSnapshot);

        uassertStatusOK(opCtx->recoveryUnit()->setReadFromMajorityCommittedSnapshot());

        {
            stdx::lock_guard<Client> lk(*opCtx->getClient());
            CurOp::get(opCtx)->yielded();
        }

        // Relock.
        _autoColl.emplace(opCtx, nss, MODE_IS);
    }
}

AutoGetCollectionForReadCommand::AutoGetCollectionForReadCommand(
    OperationContext* opCtx,
    const NamespaceString& nss,
    AutoGetCollection::ViewMode viewMode,
    Lock::DBLock lock) {
    _autoCollForRead.emplace(opCtx, nss, viewMode, std::move(lock));
    const int doNotChangeProfilingLevel = 0;
    _statsTracker.emplace(opCtx,
                          nss,
                          Top::LockType::ReadLocked,
                          _autoCollForRead->getDb() ? _autoCollForRead->getDb()->getProfilingLevel()
                                                    : doNotChangeProfilingLevel);

    // We have both the DB and collection locked, which is the prerequisite to do a stable shard
    // version check, but we'd like to do the check after we have a satisfactory snapshot.
    auto css = CollectionShardingState::get(opCtx, nss);
    css->checkShardVersionOrThrow(opCtx);
}

AutoGetCollectionForReadCommand::AutoGetCollectionForReadCommand(
    OperationContext* opCtx, const NamespaceString& nss, AutoGetCollection::ViewMode viewMode)
    : AutoGetCollectionForReadCommand(
          opCtx, nss, viewMode, Lock::DBLock(opCtx, nss.db(), MODE_IS)) {}

//FindCmd::run中构造使用
AutoGetCollectionOrViewForReadCommand::AutoGetCollectionOrViewForReadCommand(
    OperationContext* opCtx, const NamespaceString& nss)
    : AutoGetCollectionForReadCommand(opCtx, nss, AutoGetCollection::ViewMode::kViewsPermitted),
      _view(_autoCollForRead->getDb() && !getCollection()
                ? _autoCollForRead->getDb()->getViewCatalog()->lookup(opCtx, nss.ns())
                : nullptr) {}

AutoGetCollectionOrViewForReadCommand::AutoGetCollectionOrViewForReadCommand(
    OperationContext* opCtx, const NamespaceString& nss, Lock::DBLock lock)
    : AutoGetCollectionForReadCommand(
          opCtx, nss, AutoGetCollection::ViewMode::kViewsPermitted, std::move(lock)),
      _view(_autoCollForRead->getDb() && !getCollection()
                ? _autoCollForRead->getDb()->getViewCatalog()->lookup(opCtx, nss.ns())
                : nullptr) {}

AutoGetCollectionForReadCommand::AutoGetCollectionForReadCommand(OperationContext* opCtx,
                                                                 const StringData dbName,
                                                                 const UUID& uuid) {
    _autoCollForRead.emplace(opCtx, dbName, uuid);
    if (_autoCollForRead->getCollection()) {
        _statsTracker.emplace(opCtx,
                              _autoCollForRead->getCollection()->ns(),
                              Top::LockType::ReadLocked,
                              _autoCollForRead->getDb()->getProfilingLevel());

        // We have both the DB and collection locked, which is the prerequisite to do a stable shard
        // version check, but we'd like to do the check after we have a satisfactory snapshot.
        auto css = CollectionShardingState::get(opCtx, _autoCollForRead->getCollection()->ns());
        css->checkShardVersionOrThrow(opCtx);
    }
}

void AutoGetCollectionOrViewForReadCommand::releaseLocksForView() noexcept {
    invariant(_view);
    _view = nullptr;
    _autoCollForRead = boost::none;
}

OldClientContext::OldClientContext(OperationContext* opCtx,
                                   const std::string& ns,
                                   Database* db,
                                   bool justCreated)
    : _justCreated(justCreated), _doVersion(true), _ns(ns), _db(db), _opCtx(opCtx) {
    _finishInit();
}

OldClientContext::OldClientContext(OperationContext* opCtx,
                                   const std::string& ns,
                                   bool doVersion)
    : _justCreated(false),  // set for real in finishInit
      _doVersion(doVersion),
      _ns(ns),
      //_finishInit()中赋值
      _db(NULL),
      _opCtx(opCtx) {
     //如果该Db在内存中不存在，则调用DatabaseImpl::openDb创建对应DB信息
    _finishInit();
}

//获取对应的db信息，上面的OldClientContext::OldClientContext中调用
void OldClientContext::_finishInit() {
    _db = dbHolder().get(_opCtx, _ns);
    if (_db) {
        _justCreated = false;
    } else {
        invariant(_opCtx->lockState()->isDbLockedForMode(nsToDatabaseSubstring(_ns), MODE_X));
		//调用DatabaseImpl::openDb创建对应DB信息
		_db = dbHolder().openDb(_opCtx, _ns, &_justCreated);
        invariant(_db);
    }

    if (_doVersion) {
        _checkNotStale();
    }

    stdx::lock_guard<Client> lk(*_opCtx->getClient());
    CurOp::get(_opCtx)->enter_inlock(_ns.c_str(), _db->getProfilingLevel());
}

void OldClientContext::_checkNotStale() const {
    switch (CurOp::get(_opCtx)->getNetworkOp()) {
        case dbGetMore:  // getMore is special and should be handled elsewhere.
        case dbUpdate:   // update & delete check shard version in instance.cpp, so don't check
        case dbDelete:   // here as well.
            break;
        default:
            auto css = CollectionShardingState::get(_opCtx, _ns);
            css->checkShardVersionOrThrow(_opCtx);
    }
}

OldClientContext::~OldClientContext() {
    // Lock must still be held
    invariant(_opCtx->lockState()->isLocked());

    auto currentOp = CurOp::get(_opCtx);
    Top::get(_opCtx->getClient()->getServiceContext())
        .record(_opCtx,
                currentOp->getNS(),
                currentOp->getLogicalOp(),
                _opCtx->lockState()->isWriteLocked() ? Top::LockType::WriteLocked
                                                     : Top::LockType::ReadLocked,
                _timer.micros(),
                currentOp->isCommand(),
                currentOp->getReadWriteType());
}


OldClientWriteContext::OldClientWriteContext(OperationContext* opCtx, const std::string& ns)
    : _opCtx(opCtx),
      _nss(ns),
      _autodb(opCtx, _nss.db(), MODE_IX),
      _collk(opCtx->lockState(), ns, MODE_IX),
      _c(opCtx, ns, _autodb.getDb(), _autodb.justCreated()) {
    _collection = _c.db()->getCollection(opCtx, ns);
    if (!_collection && !_autodb.justCreated()) {
        // relock database in MODE_X to allow collection creation
        _collk.relockAsDatabaseExclusive(_autodb.lock());
        Database* db = dbHolder().get(_opCtx, ns);
        invariant(db == _c.db());
    }
}

}  // namespace mongo
