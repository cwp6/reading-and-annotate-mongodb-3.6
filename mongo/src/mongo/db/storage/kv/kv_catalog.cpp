// kv_catalog.cpp

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
/*
数据目录结构：
├── admin
│?? ├── collection-11-4403473224283218525.wt
│?? ├── collection-17-4403473224283218525.wt
│?? ├── collection-57-8046959287074256623.wt
│?? ├── index-12-4403473224283218525.wt
│?? ├── index-18-4403473224283218525.wt
│?? └── index-19-4403473224283218525.wt
├── config
│?? ├── collection-15-4403473224283218525.wt
│?? ├── collection-2-2800236074169171616.wt
│?? ├── collection-2-8046959287074256623.wt
│?? ├── collection-4-8046959287074256623.wt
│?? ├── collection-6-8046959287074256623.wt
│?? ├── index-16-4403473224283218525.wt
│?? ├── index-3-8046959287074256623.wt
│?? ├── index-5-8046959287074256623.wt
│?? ├── index-7-8046959287074256623.wt
│?? ├── index-8-8046959287074256623.wt
│?? └── index-9-8046959287074256623.wt
├── diagnostic.data
│?? ├── metrics.2021-03-05T00-12-34Z-00000
│?? ├── metrics.2021-03-05T12-22-34Z-00000
│?? ├── metrics.2021-03-06T00-32-34Z-00000
│?? ├── metrics.2021-03-06T12-47-34Z-00000
│?? ├── metrics.2021-03-07T00-57-34Z-00000
│?? ├── metrics.2021-03-07T13-12-34Z-00000
│?? ├── metrics.2021-03-08T01-22-34Z-00000
│?? ├── metrics.2021-03-08T13-37-34Z-00000
│?? ├── metrics.2021-03-09T01-47-34Z-00000
│?? ├── metrics.2021-03-09T13-57-34Z-00000
│?? ├── metrics.2021-03-10T02-07-34Z-00000
│?? ├── metrics.2021-03-10T14-02-44Z-00000
│?? ├── metrics.2021-03-11T02-07-44Z-00000
│?? ├── metrics.2021-03-11T14-07-44Z-00000
│?? ├── metrics.2021-03-12T02-17-44Z-00000
│?? ├── metrics.2021-03-12T14-27-44Z-00000
│?? ├── metrics.2021-03-13T02-37-44Z-00000
│?? ├── metrics.2021-03-13T14-47-44Z-00000
│?? ├── metrics.2021-03-14T02-57-44Z-00000
│?? ├── metrics.2021-03-14T15-14-20Z-00000
│?? ├── metrics.2021-03-15T03-44-20Z-00000
│?? └── metrics.interim
├── journal
│?? ├── WiredTigerLog.0000172834
│?? ├── WiredTigerPreplog.0000172482
│?? ├── WiredTigerPreplog.0000172489
│?? ├── WiredTigerPreplog.0000172499
│?? ├── WiredTigerPreplog.0000172500
│?? ├── WiredTigerPreplog.0000172502
│?? ├── WiredTigerPreplog.0000172504
│?? ├── WiredTigerPreplog.0000172511
│?? ├── WiredTigerPreplog.0000172512
│?? ├── WiredTigerPreplog.0000172513
│?? ├── WiredTigerPreplog.0000172515
│?? ├── WiredTigerPreplog.0000172521
│?? ├── WiredTigerPreplog.0000172523
│?? ├── WiredTigerPreplog.0000172524
│?? ├── WiredTigerPreplog.0000172528
│?? ├── WiredTigerPreplog.0000172535
│?? └── WiredTigerPreplog.0000172539
├── local
│?? ├── collection-0-4403473224283218525.wt
│?? ├── collection-0-8046959287074256623.wt
│?? ├── collection-13-4403473224283218525.wt
│?? ├── collection-2-4403473224283218525.wt
│?? ├── collection-4-4403473224283218525.wt
│?? ├── collection-56-8046959287074256623.wt
│?? ├── collection-6-4403473224283218525.wt
│?? ├── collection-8-4403473224283218525.wt
│?? ├── collection-9-4403473224283218525.wt
│?? ├── index-10-4403473224283218525.wt
│?? ├── index-1-4403473224283218525.wt
│?? ├── index-14-4403473224283218525.wt
│?? ├── index-1-8046959287074256623.wt
│?? ├── index-3-4403473224283218525.wt
│?? ├── index-5-4403473224283218525.wt
│?? └── index-7-4403473224283218525.wt
├── _mdb_catalog.wt
├── mongod.lock
├── myCxxoll
│?? ├── collection-4-2800236074169171616.wt
│?? ├── collection-6-2800236074169171616.wt
│?? ├── index-5-2800236074169171616.wt
│?? └── index-7-2800236074169171616.wt
├── xx_monitor_stat   这个是库
│?? ├── collection-12-8046959287074256623.wt        一个集合对应一个wt文件
│?? ├── collection-16-8046959287074256623.wt
│?? ├── collection-18-8046959287074256623.wt
│?? ├── collection-20-8046959287074256623.wt
│?? ├── collection-22-8046959287074256623.wt
│?? ├── collection-24-8046959287074256623.wt
│?? ├── collection-28-8046959287074256623.wt
│?? ├── collection-29-8046959287074256623.wt
│?? ├── collection-31-8046959287074256623.wt
│?? ├── collection-33-8046959287074256623.wt
│?? ├── collection-35-8046959287074256623.wt
│?? ├── collection-37-8046959287074256623.wt
│?? ├── collection-39-8046959287074256623.wt
│?? ├── collection-42-8046959287074256623.wt
│?? ├── collection-44-8046959287074256623.wt
│?? ├── collection-46-8046959287074256623.wt
│?? ├── collection-48-8046959287074256623.wt
│?? ├── collection-50-8046959287074256623.wt
│?? ├── collection-52-8046959287074256623.wt
│?? ├── collection-54-8046959287074256623.wt
│?? ├── index-0-2800236074169171616.wt                              每个索引对应一个wt文件
│?? ├── index-1-2800236074169171616.wt
│?? ├── index-13-8046959287074256623.wt
│?? ├── index-17-8046959287074256623.wt
│?? ├── index-19-8046959287074256623.wt
│?? ├── index-21-8046959287074256623.wt
│?? ├── index-23-8046959287074256623.wt
│?? ├── index-25-8046959287074256623.wt
│?? ├── index-30-8046959287074256623.wt
│?? ├── index-3-2800236074169171616.wt
│?? ├── index-32-8046959287074256623.wt
│?? ├── index-34-8046959287074256623.wt
│?? ├── index-36-8046959287074256623.wt
│?? ├── index-38-8046959287074256623.wt
│?? ├── index-40-8046959287074256623.wt
│?? ├── index-43-8046959287074256623.wt
│?? ├── index-45-8046959287074256623.wt
│?? ├── index-47-8046959287074256623.wt
│?? ├── index-49-8046959287074256623.wt
│?? ├── index-51-8046959287074256623.wt
│?? ├── index-53-8046959287074256623.wt
│?? └── index-55-8046959287074256623.wt
├── xx_stat  这个是库
│?? ├── collection-10-8046959287074256623.wt         一个集合对应一个wt文件
│?? ├── collection-14-8046959287074256623.wt
│?? ├── collection-26-8046959287074256623.wt
│?? ├── collection-41-8046959287074256623.wt
│?? ├── index-11-8046959287074256623.wt                              每个索引对应一个wt文件
│?? ├── index-13-2800236074169171616.wt
│?? ├── index-14-2800236074169171616.wt
│?? ├── index-15-2800236074169171616.wt
│?? ├── index-15-8046959287074256623.wt
│?? ├── index-17-2800236074169171616.wt
│?? └── index-27-8046959287074256623.wt
├── rollback
│?? └── xx_monitor_stat.device_activity_longconnect2.2020-02-13T13-53-30.0.bson
├── sizeStorer.wt
├── storage.bson
├── test
│?? ├── collection-10-2800236074169171616.wt
│?? ├── collection-8-2800236074169171616.wt
│?? ├── index-11-2800236074169171616.wt
│?? └── index-9-2800236074169171616.wt
├── _tmp
├── WiredTiger
├── WiredTigerLAS.wt
├── WiredTiger.lock
├── WiredTiger.turtle
└── WiredTiger.wt

{
	md: {
		ns: "test.mycol",
		options: {
			uuid: UUID("75591c22-bd0f-4a56-ac95-ef90224cf3df"),
			capped: true,
			size: 6142976,
			max: 10000,
			autoIndexId: true
		},
		indexes: [{
			spec: {
				v: 2,
				key: {
					_id: 1
				},
				name: "_id_",
				ns: "test.mycol"
			},
			ready: false,
			multikey: false,
			multikeyPaths: {
				_id: BinData(0, 00)
			},
			head: 0,
			prefix: -1
		}],
		prefix: -1
	},
	idxIdent: {  //索引ident，再KVCollectionCatalogEntry中管理起来
		_id_: "test/index/2--6948813758302814892"
	},
	ns: "test.mycol",
	//表ident   ，记录到KVCatalog._idents中，
	ident: "test/collection/1--6948813758302814892"
}

{
	md: {
		ns: "test.test",
		options: {
			uuid: UUID("520904ec-0432-4c00-a15d-788e2f5d707b")
			//建表参数都在这里
		},
		indexes: [{
			spec: {
				v: 2,
				key: {
					_id: 1
				},
				name: "_id_",
				ns: "test.test"
			},
			ready: true,
			multikey: false,
			multikeyPaths: {
				_id: BinData(0, 00)
			},
			head: 0,
			prefix: -1
		}, {
			spec: {
				v: 2,
				key: {
					name: 1.0,
					age: 1.0
				},
				name: "name_1_age_1",
				ns: "test.test",
				background: true
			},
			ready: true,
			multikey: false,
			multikeyPaths: {
				name: BinData(0, 00),
				age: BinData(0, 00)
			},
			head: 0,
			prefix: -1
		}, {
			spec: {
				v: 2,
				key: {
					zipcode: 1.0
				},
				name: "zipcode_1",
				ns: "test.test",
				background: true
			},
			ready: true,
			multikey: false,
			multikeyPaths: {
				zipcode: BinData(0, 00)
			},
			head: 0,
			prefix: -1
		}],
		prefix: -1
	},
	idxIdent: {
		_id_: "test/index/8-380857198902467499",
		name_1_age_1: "test/index/0--6948813758302814892",
		zipcode_1: "test/index/3--6948813758302814892"
	},
	ns: "test.test",
	ident: "test/collection/7-380857198902467499"
}

*/
#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/db/storage/kv/kv_catalog.h"

#include <stdlib.h>

#include "mongo/bson/util/bson_extract.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/kv/kv_catalog_feature_tracker.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/platform/bits.h"
#include "mongo/platform/random.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace {
// This is a global resource, which protects accesses to the catalog metadata (instance-wide).
// It is never used with KVEngines that support doc-level locking so this should never conflict
// with anything else.

const char kIsFeatureDocumentFieldName[] = "isFeatureDoc";
const char kNamespaceFieldName[] = "ns";
const char kNonRepairableFeaturesFieldName[] = "nonRepairable";
const char kRepairableFeaturesFieldName[] = "repairable";

void appendPositionsOfBitsSet(uint64_t value, StringBuilder* sb) {
    invariant(sb);

    *sb << "[ ";
    bool firstIteration = true;
    while (value) {
        const int lowestSetBitPosition = countTrailingZeros64(value);
        if (!firstIteration) {
            *sb << ", ";
        }
        *sb << lowestSetBitPosition;
        value ^= (1ULL << lowestSetBitPosition);
        firstIteration = false;
    }
    *sb << " ]";
}
}

using std::unique_ptr;
using std::string;

class KVCatalog::AddIdentChange : public RecoveryUnit::Change {
public:
    AddIdentChange(KVCatalog* catalog, StringData ident)
        : _catalog(catalog), _ident(ident.toString()) {}

    virtual void commit() {}
    virtual void rollback() {
        stdx::lock_guard<stdx::mutex> lk(_catalog->_identsLock);
        _catalog->_idents.erase(_ident);
    }

    KVCatalog* const _catalog;
    const std::string _ident;
};

class KVCatalog::RemoveIdentChange : public RecoveryUnit::Change {
public:
    RemoveIdentChange(KVCatalog* catalog, StringData ident, const Entry& entry)
        : _catalog(catalog), _ident(ident.toString()), _entry(entry) {}

    virtual void commit() {}
    virtual void rollback() {
        stdx::lock_guard<stdx::mutex> lk(_catalog->_identsLock);
        _catalog->_idents[_ident] = _entry;
    }

    KVCatalog* const _catalog;
    const std::string _ident;
    const Entry _entry;
};

bool KVCatalog::FeatureTracker::isFeatureDocument(BSONObj obj) {
    BSONElement firstElem = obj.firstElement();
    if (firstElem.fieldNameStringData() == kIsFeatureDocumentFieldName) {
        return firstElem.booleanSafe();
    }
    return false;
}

Status KVCatalog::FeatureTracker::isCompatibleWithCurrentCode(OperationContext* opCtx) const {
    FeatureBits versionInfo = getInfo(opCtx);

    uint64_t unrecognizedNonRepairableFeatures =
        versionInfo.nonRepairableFeatures & ~_usedNonRepairableFeaturesMask;
    if (unrecognizedNonRepairableFeatures) {
        StringBuilder sb;
        sb << "The data files use features not recognized by this version of mongod; the NR feature"
              " bits in positions ";
        appendPositionsOfBitsSet(unrecognizedNonRepairableFeatures, &sb);
        sb << " aren't recognized by this version of mongod";
        return {ErrorCodes::MustUpgrade, sb.str()};
    }

    uint64_t unrecognizedRepairableFeatures =
        versionInfo.repairableFeatures & ~_usedRepairableFeaturesMask;
    if (unrecognizedRepairableFeatures) {
        StringBuilder sb;
        sb << "The data files use features not recognized by this version of mongod; the R feature"
              " bits in positions ";
        appendPositionsOfBitsSet(unrecognizedRepairableFeatures, &sb);
        sb << " aren't recognized by this version of mongod";
        return {ErrorCodes::CanRepairToDowngrade, sb.str()};
    }

    return Status::OK();
}

std::unique_ptr<KVCatalog::FeatureTracker> KVCatalog::FeatureTracker::get(OperationContext* opCtx,
                                                                          KVCatalog* catalog,
                                                                          RecordId rid) {
    auto record = catalog->_rs->dataFor(opCtx, rid);
    BSONObj obj = record.toBson();
    invariant(isFeatureDocument(obj));
    return std::unique_ptr<KVCatalog::FeatureTracker>(new KVCatalog::FeatureTracker(catalog, rid));
}

std::unique_ptr<KVCatalog::FeatureTracker> KVCatalog::FeatureTracker::create(
    OperationContext* opCtx, KVCatalog* catalog) {
    return std::unique_ptr<KVCatalog::FeatureTracker>(
        new KVCatalog::FeatureTracker(catalog, RecordId()));
}

bool KVCatalog::FeatureTracker::isNonRepairableFeatureInUse(OperationContext* opCtx,
                                                            NonRepairableFeature feature) const {
    FeatureBits versionInfo = getInfo(opCtx);
    return versionInfo.nonRepairableFeatures & static_cast<NonRepairableFeatureMask>(feature);
}

void KVCatalog::FeatureTracker::markNonRepairableFeatureAsInUse(OperationContext* opCtx,
                                                                NonRepairableFeature feature) {
    FeatureBits versionInfo = getInfo(opCtx);
    versionInfo.nonRepairableFeatures |= static_cast<NonRepairableFeatureMask>(feature);
    putInfo(opCtx, versionInfo);
}

void KVCatalog::FeatureTracker::markNonRepairableFeatureAsNotInUse(OperationContext* opCtx,
                                                                   NonRepairableFeature feature) {
    FeatureBits versionInfo = getInfo(opCtx);
    versionInfo.nonRepairableFeatures &= ~static_cast<NonRepairableFeatureMask>(feature);
    putInfo(opCtx, versionInfo);
}

bool KVCatalog::FeatureTracker::isRepairableFeatureInUse(OperationContext* opCtx,
                                                         RepairableFeature feature) const {
    FeatureBits versionInfo = getInfo(opCtx);
    return versionInfo.repairableFeatures & static_cast<RepairableFeatureMask>(feature);
}

void KVCatalog::FeatureTracker::markRepairableFeatureAsInUse(OperationContext* opCtx,
                                                             RepairableFeature feature) {
    FeatureBits versionInfo = getInfo(opCtx);
    versionInfo.repairableFeatures |= static_cast<RepairableFeatureMask>(feature);
    putInfo(opCtx, versionInfo);
}

void KVCatalog::FeatureTracker::markRepairableFeatureAsNotInUse(OperationContext* opCtx,
                                                                RepairableFeature feature) {
    FeatureBits versionInfo = getInfo(opCtx);
    versionInfo.repairableFeatures &= ~static_cast<RepairableFeatureMask>(feature);
    putInfo(opCtx, versionInfo);
}

KVCatalog::FeatureTracker::FeatureBits KVCatalog::FeatureTracker::getInfo(
    OperationContext* opCtx) const {
    if (_rid.isNull()) {
        return {};
    }

    auto record = _catalog->_rs->dataFor(opCtx, _rid);
    BSONObj obj = record.toBson();
    invariant(isFeatureDocument(obj));

    BSONElement nonRepairableFeaturesElem;
    auto nonRepairableFeaturesStatus = bsonExtractTypedField(
        obj, kNonRepairableFeaturesFieldName, BSONType::NumberLong, &nonRepairableFeaturesElem);
    fassert(40111, nonRepairableFeaturesStatus);

    BSONElement repairableFeaturesElem;
    auto repairableFeaturesStatus = bsonExtractTypedField(
        obj, kRepairableFeaturesFieldName, BSONType::NumberLong, &repairableFeaturesElem);
    fassert(40112, repairableFeaturesStatus);

    FeatureBits versionInfo;
    versionInfo.nonRepairableFeatures =
        static_cast<NonRepairableFeatureMask>(nonRepairableFeaturesElem.numberLong());
    versionInfo.repairableFeatures =
        static_cast<RepairableFeatureMask>(repairableFeaturesElem.numberLong());
    return versionInfo;
}

void KVCatalog::FeatureTracker::putInfo(OperationContext* opCtx, const FeatureBits& versionInfo) {
    BSONObjBuilder bob;
    bob.appendBool(kIsFeatureDocumentFieldName, true);
    // We intentionally include the "ns" field with a null value in the feature document to prevent
    // older versions that do 'obj["ns"].String()' from starting up. This way only versions that are
    // aware of the feature document's existence can successfully start up.
    bob.appendNull(kNamespaceFieldName);
    bob.append(kNonRepairableFeaturesFieldName,
               static_cast<long long>(versionInfo.nonRepairableFeatures));
    bob.append(kRepairableFeaturesFieldName,
               static_cast<long long>(versionInfo.repairableFeatures));
    BSONObj obj = bob.done();

    if (_rid.isNull()) {
        // This is the first time a feature is being marked as in-use or not in-use, so we must
        // insert the feature document rather than update it.
        const bool enforceQuota = false;
        // TODO SERVER-30638: using timestamp 0 for these inserts
        auto rid = _catalog->_rs->insertRecord(
            opCtx, obj.objdata(), obj.objsize(), Timestamp(), enforceQuota);
        fassert(40113, rid.getStatus());
        _rid = rid.getValue();
    } else {
        const bool enforceQuota = false;
        UpdateNotifier* notifier = nullptr;
        auto status = _catalog->_rs->updateRecord(
            opCtx, _rid, obj.objdata(), obj.objsize(), enforceQuota, notifier);
        fassert(40114, status);
    }
}

////KVStorageEngine::KVStorageEngine中构造初始化
KVCatalog::KVCatalog(RecordStore* rs, bool directoryPerDb, bool directoryForIndexes)
    : _rs(rs), //对应WiredTigerRecordStore,对应_mdb_catalog.wt元数据文件操作
      _directoryPerDb(directoryPerDb),
      _directoryForIndexes(directoryForIndexes),
      _rand(_newRand()) {}

KVCatalog::~KVCatalog() {
    _rs = NULL;
}

//生成随机数
std::string KVCatalog::_newRand() {
    return str::stream() << std::unique_ptr<SecureRandom>(SecureRandom::create())->nextInt64();
}

//ident文件是否以指定随机数结尾，必须把最大的随机数记录下来，以后有新表等从该随机数自动加1，避免重复
bool KVCatalog::_hasEntryCollidingWithRand() const {
    // Only called from init() so don't need to lock.
    for (NSToIdentMap::const_iterator it = _idents.begin(); it != _idents.end(); ++it) {
        if (StringData(it->first).endsWith(_rand))
            return true;
    }
    return false;
}

//KVCatalog::newCollection   KVCatalog::putMetaData中调用
//_newUniqueIdent()函数获得collection对应的文件名字（ident），一个集合对应一个wt文件
//注意，这里是生成一个唯一的ident，这里的算法值得学习
std::string KVCatalog::_newUniqueIdent(StringData ns, const char* kind) {
    // If this changes to not put _rand at the end, _hasEntryCollidingWithRand will need fixing.
    StringBuilder buf;
    if (_directoryPerDb) {
        buf << NamespaceString::escapeDbName(nsToDatabaseSubstring(ns)) << '/';
    }
    buf << kind;
    buf << (_directoryForIndexes ? '/' : '-');
    buf << _next.fetchAndAdd(1) << '-' << _rand;

	//yang test ......... KVCatalog::_newUniqueIdent, bufstr:test/collection/7--3550907941469880053
	log() << "yang test ......... KVCatalog::_newUniqueIdent, bufstr:" << buf.str();
    return buf.str();
}

//KVStorageEngine::KVStorageEngine中调用，KVStorageEngine::KVStorageEngine初始化构造的时候就从
//元数据文件_mdb_catalog.wt中获取元数据信息
void KVCatalog::init(OperationContext* opCtx) {
    // No locking needed since called single threaded.
    auto cursor = _rs->getCursor(opCtx);
    while (auto record = cursor->next()) {
        BSONObj obj = record->data.releaseToBson();

        if (FeatureTracker::isFeatureDocument(obj)) {
            // There should be at most one version document in the catalog.
            invariant(!_featureTracker);

            // Initialize the feature tracker and skip over the version document because it doesn't
            // correspond to a namespace entry.
            _featureTracker = FeatureTracker::get(opCtx, this, record->id);
            continue;
        }

        // No rollback since this is just loading already committed data.
        //表及其对应的ident信息
        string ns = obj["ns"].String();
        string ident = obj["ident"].String();
        _idents[ns] = Entry(ident, record->id);
    }

    if (!_featureTracker) {
        // If there wasn't a feature document, then just an initialize a feature tracker that
        // doesn't manage a feature document yet.
        _featureTracker = KVCatalog::FeatureTracker::create(opCtx, this);
    }

    // In the unlikely event that we have used this _rand before generate a new one.
    while (_hasEntryCollidingWithRand()) {
        _rand = _newRand();
    }
}

/*
//由于每个collection创建时都会存储到元数据文件_mdb_catalog中，
//因此，可以直接从这个文件中得到所有已创建的collection。

例如实例重启，需要通过_mdb_catalog.wt获取表元数据信息

*/ //KVStorageEngine::KVStorageEngine调用
void KVCatalog::getAllCollections(std::vector<std::string>* out) const {
    stdx::lock_guard<stdx::mutex> lk(_identsLock);
    for (NSToIdentMap::const_iterator it = _idents.begin(); it != _idents.end(); ++it) {
        out->push_back(it->first);
    }
}

/*
KVCatalog::newCollection()函数会调用_newUniqueIdent()函数获得collection对应的文件名字（ident），并且，
会将collection的ns、ident存储到元数据文件_mdb_catalog中。
*/ 
//insertBatchAndHandleErrors->makeCollection->mongo::userCreateNS->mongo::userCreateNSImpl
//->DatabaseImpl::createCollection->Collection* createCollection->KVDatabaseCatalogEntryBase::createCollection

//CollectionImpl::_insertDocuments(插入数据)   KVCatalog::newCollection(记录集合元数据到_mdb_catalog.wt)中执行  
//KVDatabaseCatalogEntryBase::createCollection中执行    

//更新_idents，记录下集合对应元数据信息，也就是集合路径  集合uuid 集合索引，以及在元数据_mdb_catalog.wt中的位置
//KVDatabaseCatalogEntryBase::createCollection->KVCatalog::newCollection
Status KVCatalog::newCollection(OperationContext* opCtx,
                                StringData ns,
                                const CollectionOptions& options,
                                KVPrefix prefix) {
    invariant(opCtx->lockState()->isDbLockedForMode(nsToDatabaseSubstring(ns), MODE_X));

	//一个集合对应一个wt文件
    const string ident = _newUniqueIdent(ns, "collection");

    stdx::lock_guard<stdx::mutex> lk(_identsLock);
	//在下面跟新
	//更新_idents，记录下集合对应元数据信息，也就是集合路径  集合uuid 集合索引，以及在元数据_mdb_catalog.wt中的位置
    Entry& old = _idents[ns.toString()];
    if (!old.ident.empty()) {
        return Status(ErrorCodes::NamespaceExists, "collection already exists");
    }

    opCtx->recoveryUnit()->registerChange(new AddIdentChange(this, ns));

    BSONObj obj;
    {
        BSONObjBuilder b;
        b.append("ns", ns);
        b.append("ident", ident);
        BSONCollectionCatalogEntry::MetaData md;
        md.ns = ns.toString();
        md.options = options;
        md.prefix = prefix;
        b.append("md", md.toBSON());
        obj = b.obj();
    }
    const bool enforceQuota = false;
    // TODO SERVER-30638: using timestamp 0 for these inserts.
    StatusWith<RecordId> res = 
    	//WiredTigerRecordStore::_insertRecords  记录collection元数据到_mdb_catalog.wt，数据内容如下:
    	/*
        yang test ...WiredTigerRecordStore::_insertRecords . _uri:table:_mdb_catalog key:RecordId(5) value:{ ns: "test.yangyazhou", ident: "test/collection/7--3550907941469880053", md: { ns: "test.yangyazhou", options: { uuid: UUID("13867264-68f8-422d-a13c-2d92a0d43e8e") }, indexes: [], prefix: -1 } }
    	
		stored meta data for test.coll @ RecordId(4) 
		obj:{ ns: "test.coll", ident: "test/collection/4-7637131936287447509", md: { ns: "test.coll", options: { uuid: UUID("440d1a2b-5122-40e9-b0c0-7ec58f072055") }, indexes: [], prefix: -1 } }
	    */
	    //WiredTigerRecordStore::_insertRecords  记录collection元数据到_mdb_catalog.wt，数据内容如上
	    //WiredTigerRecordStore::insertRecord
        _rs->insertRecord(opCtx, obj.objdata(), obj.objsize(), Timestamp(), enforceQuota);
    if (!res.isOK())
        return res.getStatus();

	//更新_idents，记录下集合对应元数据信息，也就是集合路径  集合uuid 集合索引，以及在元数据_mdb_catalog.wt中的位置
    old = Entry(ident, res.getValue());
	//2021-05-01T11:12:42.394+0800 D STORAGE  [conn-1] stored meta data for test.mycol @ RecordId(6) 
	//obj:{ ns: "test.mycol", ident: "test/collection/1--6948813758302814892", md: { ns: "test.mycol", 
	//options: { uuid: UUID("75591c22-bd0f-4a56-ac95-ef90224cf3df"), capped: true, size: 6142976, max: 10000, autoIndexId: true }, 
	//indexes: [], prefix: -1 } }
	//集合元数据信息存入_mdb_catalog.wt
    LOG(1) << "stored meta data for " << ns << " @ " << res.getValue() << " obj:" << redact(obj);;
    return Status::OK();
}

//获取ns表对应的元数据信息，内容可以参考KVCatalog::newCollection
std::string KVCatalog::getCollectionIdent(StringData ns) const {
    stdx::lock_guard<stdx::mutex> lk(_identsLock);
    NSToIdentMap::const_iterator it = _idents.find(ns.toString());
    invariant(it != _idents.end());
    return it->second.ident;
}

//KVCollectionCatalogEntry::prepareForIndexBuild调用
//获取索引对应文件目录数据
std::string KVCatalog::getIndexIdent(OperationContext* opCtx,
                                     StringData ns,
                                     StringData idxName) const {
    BSONObj obj = _findEntry(opCtx, ns);
    BSONObj idxIdent = obj["idxIdent"].Obj();
    return idxIdent[idxName].String();
}

//_mdb_catalog.wt中查找   下面的KVCatalog::getMetaData中执行
//获取ns表对应元数据信息，再mdb_catalog的第几行，内容是什么
BSONObj KVCatalog::_findEntry(OperationContext* opCtx, StringData ns, RecordId* out) const {
    RecordId dl;
    {
        stdx::lock_guard<stdx::mutex> lk(_identsLock);
        NSToIdentMap::const_iterator it = _idents.find(ns.toString());
        invariant(it != _idents.end());
        dl = it->second.storedLoc;
    }

/*
2021-04-19T14:55:07.391+0800 D STORAGE  [conn-2] KVCatalog::_findEntry looking up metadata for: yyztest.account2 @ RecordId(32)
2021-04-19T14:55:07.391+0800 D STORAGE  [conn-2] KVCatalog::_findEntry looking up metadata for: yyztest.account2 @ RecordId(32) data:{ md: { ns: "yyztest.account2", options: { uuid: UUID("8cf59aef-c3d2-4bc7-8385-96d8807f41f9") }, indexes: [ { spec: { v: 2, key: { _id: 1 }, name: "_id_", ns: "yyztest.account2" }, ready: true, multikey: false, multikeyPaths: { _id: BinData(0, 00) }, head: 0, prefix: -1 }, { spec: { v: 2, unique: true, key: { game: 1.0, plat: 1.0, serverAccounts.roles.roleId: 1.0 }, name: "game_1_plat_1_serverAccounts.roles.roleId_1", ns: "yyztest.account2", background: true }, ready: true, multikey: true, multikeyPaths: { game: BinData(0, 00), plat: BinData(0, 00), serverAccounts.roles.roleId: BinData(0, 010100) }, head: 0, prefix: -1 } ], prefix: -1 }, idxIdent: { _id_: "yyztest/index/10-5463452279079700935", game_1_plat_1_serverAccounts.roles.roleId_1: "yyztest/index/11-5463452279079700935" }, ns: "yyztest.account2", ident: "yyztest/collection/9-5463452279079700935" }
*/
    LOG(3) << "KVCatalog::_findEntry looking up metadata for: " << ns << " @ " << dl;
    RecordData data;
    if (!_rs->findRecord(opCtx, dl, &data)) {
        // since the in memory meta data isn't managed with mvcc
        // its possible for different transactions to see slightly
        // different things, which is ok via the locking above.
        return BSONObj();
    }

    if (out)
        *out = dl;
	LOG(3) << "KVCatalog::_findEntry looking up metadata for: " << ns << " @ " << dl << " data:" << data.releaseToBson();

    return data.releaseToBson().getOwned();
}

//从元数据文件中查找对应的集合相关的数据文件 索引文件等信息
//KVCollectionCatalogEntry::_getMetaData  KVDatabaseCatalogEntryBase::initCollection
//KVDatabaseCatalogEntryBase::renameCollection  
//KVStorageEngine::reconcileCatalogAndIdents    KVStorageEngine::KVStorageEngine

//获取MetaData信息，KVCollectionCatalogEntry::_getMetaData调用
//获取表ns的元数据信息
const BSONCollectionCatalogEntry::MetaData KVCatalog::getMetaData(OperationContext* opCtx,
                                                                  StringData ns) {
    BSONObj obj = _findEntry(opCtx, ns);
//[conn1] returning metadata: md: { ns: "test.yangyazhou", options: { uuid: UUID("38145b44-6a9d-4a50-8b03-a0dfedc7597f") }, 
//indexes: [ { spec: { v: 2, key: { _id: 1 }, name: "_id_", ns: "test.yangyazhou" }, ready: true, multikey: false, multikeyPaths: { _id: BinData(0, 00) }, head: 0, prefix: -1 } ], prefix: -1 }
    LOG(3) << " fetched CCE metadata: " << obj;
    BSONCollectionCatalogEntry::MetaData md;
    const BSONElement mdElement = obj["md"];
    if (mdElement.isABSONObj()) {
        LOG(3) << "returning metadata: " << mdElement;
        md.parse(mdElement.Obj());
    }
    return md;
}


//集合相关的元数据信息记录到_mdb_catalog.wt 如创建某个集合对应的数据文件在哪里，索引文件在哪里

//KVCollectionCatalogEntry类的如下相关接口完成对MetaData的更新:updateValidator   updateFlags  setIsTemp  removeUUID  addUUID  updateTTLSetting  indexBuildSuccess
//KVCollectionCatalogEntry类的相关接口调用，完成MetaData相关成员更新

//更新某个表的元数据信息  KVCollectionCatalogEntry类的相关接口调用，完成MetaData相关成员更新
void KVCatalog::putMetaData(OperationContext* opCtx,
                            StringData ns,
                            BSONCollectionCatalogEntry::MetaData& md) {
    RecordId loc;
    BSONObj obj = _findEntry(opCtx, ns, &loc);

    {
        // rebuilt doc
        BSONObjBuilder b;
        b.append("md", md.toBSON());

        BSONObjBuilder newIdentMap;
        BSONObj oldIdentMap;
        if (obj["idxIdent"].isABSONObj())
            oldIdentMap = obj["idxIdent"].Obj();

        // fix ident map
        /* 例如已经有_id_索引，和name_1_age_1索引，新键db.test.createIndex( { zipcode: 1}, {background: true} )
		idxIdent: {
			_id_: "test/index/8-380857198902467499",   注意test对应的是库
			name_1_age_1: "test/index/0--6948813758302814892",
			zipcode_1: "test/index/3--6948813758302814892"
		},
		*/
        for (size_t i = 0; i < md.indexes.size(); i++) {
            string name = md.indexes[i].name();
            BSONElement e = oldIdentMap[name];
			//先把原有的找出来存入newIdentMap
            if (e.type() == String) {
                newIdentMap.append(e);
                continue;
            }
            // missing, create new
            //新加的索引也追加进去
            newIdentMap.append(name, _newUniqueIdent(ns, "index"));
        }
        b.append("idxIdent", newIdentMap.obj());

        // add whatever is left
        b.appendElementsUnique(obj);
        obj = b.obj();
    }
	
	//[initandlisten] recording new metadata: { md: { ns: "admin.system.version", options: 
	//{ uuid: UUID("d24324d6-5465-4634-9f8a-3d6c6f6af801") }, indexes: [ { spec: { v: 2, key: { _id: 1 }, 
	//name: "_id_", ns: "admin.system.version" }, ready: true, multikey: false, multikeyPaths: 
	//{ _id: BinData(0, 00) }, head: 0, prefix: -1 } ], prefix: -1 }, idxIdent: { _id_: "admin/index/1--9034870482849730886" }, 
	//ns: "admin.system.version", ident: "admin/collection/0--9034870482849730886" }

	//建一个新表的元数据打印:db.createCollection("mycol", { capped : true, autoIndexId : true, size :    6142800, max : 10000 } )
	//{ md: { ns: "test.mycol", options: { uuid: UUID("75591c22-bd0f-4a56-ac95-ef90224cf3df"), capped: true, size: 6142976, max: 10000, autoIndexId: true }, 
	//indexes: [ { spec: { v: 2, key: { _id: 1 }, name: "_id_", ns: "test.mycol" }, ready: false, multikey: false, 
	//multikeyPaths: { _id: BinData(0, 00) }, head: 0, prefix: -1 } ], prefix: -1 }, idxIdent: { _id_: "test/index/2--6948813758302814892" }, 
	//ns: "test.mycol", ident: "test/collection/1--6948813758302814892" }

    LOG(3) << "recording new metadata: " << obj;
    Status status = _rs->updateRecord(opCtx, loc, obj.objdata(), obj.objsize(), false, NULL);
    fassert(28521, status.isOK());
}

//表重命名后元数据也要更新，包括表名 表ident 索引也修改
//KVDatabaseCatalogEntryBase::renameCollection调用
Status KVCatalog::renameCollection(OperationContext* opCtx,
                                   StringData fromNS,
                                   StringData toNS,
                                   bool stayTemp) {
    RecordId loc;
    BSONObj old = _findEntry(opCtx, fromNS, &loc).getOwned();
    {
		//存储新表元数据信息
        BSONObjBuilder b;

		//新表名
        b.append("ns", toNS);

        BSONCollectionCatalogEntry::MetaData md;
		//解析处原表的md信息
        md.parse(old["md"].Obj());
		//BSONCollectionCatalogEntry::MetaData::rename 表对应索引元数据也需要修改
        md.rename(toNS);
        if (!stayTemp)
            md.options.temp = false;
		//把修改后的md组装到b中
        b.append("md", md.toBSON());

        b.appendElementsUnique(old);

        BSONObj obj = b.obj();
		//把表对应元数据信息更新为替换后的新的
        Status status = _rs->updateRecord(opCtx, loc, obj.objdata(), obj.objsize(), false, NULL);
        fassert(28522, status.isOK());
    }

    stdx::lock_guard<stdx::mutex> lk(_identsLock);
    const NSToIdentMap::iterator fromIt = _idents.find(fromNS.toString());
    invariant(fromIt != _idents.end());

    opCtx->recoveryUnit()->registerChange(new RemoveIdentChange(this, fromNS, fromIt->second));
    opCtx->recoveryUnit()->registerChange(new AddIdentChange(this, toNS));

    _idents.erase(fromIt);
    _idents[toNS.toString()] = Entry(old["ident"].String(), loc);

    return Status::OK();
}

//
//drop删表CmdDrop::errmsgRun->dropCollection->DatabaseImpl::dropCollectionEvenIfSystem->DatabaseImpl::_finishDropCollection
//    ->DatabaseImpl::_finishDropCollection->KVDatabaseCatalogEntryBase::dropCollection->KVCatalog::dropCollection
//配合KVDatabaseCatalogEntryBase::createCollection->KVCatalog::newCollection阅读
//删除表后需要从元数据中删除该表
Status KVCatalog::dropCollection(OperationContext* opCtx, StringData ns) {
    invariant(opCtx->lockState()->isDbLockedForMode(nsToDatabaseSubstring(ns), MODE_X));
    stdx::lock_guard<stdx::mutex> lk(_identsLock);
    const NSToIdentMap::iterator it = _idents.find(ns.toString());
    if (it == _idents.end()) {
        return Status(ErrorCodes::NamespaceNotFound, "collection not found");
    }

    opCtx->recoveryUnit()->registerChange(new RemoveIdentChange(this, ns, it->second));

    LOG(1) << "deleting metadata for " << ns << " @ " << it->second.storedLoc;
	//WiredTigerRecordStore::deleteRecord
    _rs->deleteRecord(opCtx, it->second.storedLoc);
    _idents.erase(it);

    return Status::OK();
}

//获取某个DB下面所有表的元数据信息
std::vector<std::string> KVCatalog::getAllIdentsForDB(StringData db) const {
    std::vector<std::string> v;

    {
        stdx::lock_guard<stdx::mutex> lk(_identsLock);
        for (NSToIdentMap::const_iterator it = _idents.begin(); it != _idents.end(); ++it) {
            NamespaceString ns(it->first);
            if (ns.db() != db)
                continue;
            v.push_back(it->second.ident);
        }
    }

    return v;
}

//WiredTigerKVEngine::getAllIdents和KVCatalog::getAllIdents区别：
// 1. WiredTigerKVEngine::getAllIdents对应WiredTiger.wt元数据文件，由wiredtiger存储引擎自己维护
// 2. KVCatalog::getAllIdents对应_mdb_catalog.wt，由mongodb server层storage模块维护
// 3. 这两个元数据相比较，冲突的时候collection以_mdb_catalog.wt为准，该表下面的索引以WiredTiger.wt为准
//	  参考KVStorageEngine::reconcileCatalogAndIdents

//KVStorageEngine::reconcileCatalogAndIdents调用
//获取集群所有的ident信息，包括表的，也包括index的
std::vector<std::string> KVCatalog::getAllIdents(OperationContext* opCtx) const {
    std::vector<std::string> v;

    auto cursor = _rs->getCursor(opCtx);
    while (auto record = cursor->next()) {
        BSONObj obj = record->data.releaseToBson();
        if (FeatureTracker::isFeatureDocument(obj)) {
            // Skip over the version document because it doesn't correspond to a namespace entry and
            // therefore doesn't refer to any idents.
            continue;
        }
        v.push_back(obj["ident"].String());

        BSONElement e = obj["idxIdent"];
        if (!e.isABSONObj())
            continue;
        BSONObj idxIdent = e.Obj();

        BSONObjIterator sub(idxIdent);
        while (sub.more()) {
            BSONElement e = sub.next();
            v.push_back(e.String());
        }
    }

    return v;
}

//用户数据包括普通表数据和索引数据
bool KVCatalog::isUserDataIdent(StringData ident) const {
    return ident.find("index-") != std::string::npos || ident.find("index/") != std::string::npos ||
        ident.find("collection-") != std::string::npos ||
        ident.find("collection/") != std::string::npos;
}
}
