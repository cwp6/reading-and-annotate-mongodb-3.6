/**
 *    Copyright (C) 2013 10gen Inc.
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

#include <memory>

#include "mongo/bson/bsonobj_comparator_interface.h"
#include "mongo/db/fts/fts_query.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/query/index_bounds.h"
#include "mongo/db/query/plan_cache.h"
#include "mongo/db/query/stage_types.h"

namespace mongo {

class GeoNearExpression;

/**
 * This is an abstract representation of a query plan.  It can be transcribed into a tree of
 * PlanStages, which can then be handed to a PlanRunner for execution.
 */ 
 
//QueryPlannerAccess::buildIndexedDataAccess和QueryPlannerAnalysis::analyzeDataAccess
//一起生成QuerySolutionNode tree, 并存储倒querySolution.root(也就是这个QuerySolutionNode tree)

 
//所有的QuerySolution.root是一颗tree,所有的树节点类型为QuerySolutionNode，这些节点就挂载到这棵树中
//QuerySolution.root树种的各个节点真实类型为QuerySolutionNode的继承类，例如CollectionScanNode、AndHashNode等
//QuerySolutionNode的具体继承类实现见本文件后面

//QueryPlannerAccess::buildIndexedDataAccess中生成该类    
struct QuerySolutionNode { 
//QuerySolution.root为该类型   
//实际上在该文件后面的FetchNode CollectionScanNode  AndHashNode等类中继承该类,
    QuerySolutionNode() {}
    virtual ~QuerySolutionNode() {
        for (size_t i = 0; i < children.size(); ++i) {
            delete children[i];
        }
    }

    /**
     * Return a std::string representation of this node and any children.
     */ //QuerySolutionNode::toString 
    std::string toString() const;

    /**
     * What stage should this be transcribed to?  See stage_types.h.
     */ //例如CollectionScanNode赋值参考CollectionScanNode::getType
    virtual StageType getType() const = 0;

    /**
     * Internal function called by toString()
     *
     * TODO: Consider outputting into a BSONObj or builder thereof.
     */
    virtual void appendToString(mongoutils::str::stream* ss, int indent) const = 0;

    //
    // Computed properties
    //

    /**
     * Must be called before any properties are examined.
     */
    virtual void computeProperties() {
        for (size_t i = 0; i < children.size(); ++i) {
            children[i]->computeProperties();
        }
    }

    /**
     * If true, one of these are true:
     *          1. All outputs are already fetched, or
     *          2. There is a projection in place and a fetch is not required.
     *
     * If false, a fetch needs to be placed above the root in order to provide results.
     *
     * Usage: To determine if every possible result that might reach the root
     * will be fully-fetched or not.  We don't want any surplus fetches.
     */
    virtual bool fetched() const = 0;

    /**
     * Returns true if the tree rooted at this node provides data with the field name 'field'.
     * This data can come from any of the types of the WSM.
     *
     * Usage: If an index-only plan has all the fields we're interested in, we don't
     * have to fetch to show results with those fields.
     *
     * TODO: 'field' is probably more appropriate as a FieldRef or string.
     */
    virtual bool hasField(const std::string& field) const = 0;

    /**
     * Returns true if the tree rooted at this node provides data that is sorted by the
     * its location on disk.
     * 如果以该节点为根的树提供了按其在磁盘上的位置排序的数据，则返回true。
     *
     * Usage: If all the children of an STAGE_AND_HASH have this property, we can compute the
     * AND faster by replacing the STAGE_AND_HASH with STAGE_AND_SORTED.
     */
    virtual bool sortedByDiskLoc() const = 0;

    /**
     * Return a BSONObjSet representing the possible sort orders of the data stream from this
     * node.  If the data is not sorted in any particular fashion, returns an empty set.
     *
     * Usage:
     * 1. If our plan gives us a sort order, we don't have to add a sort stage.
     * 2. If all the children of an OR have the same sort order, we can maintain that
     *    sort order with a STAGE_SORT_MERGE instead of STAGE_OR.
     */
    virtual const BSONObjSet& getSort() const = 0;

    /**
     * Make a deep copy.
     */ //cloneBaseData调用，对应CollectionScanNode::clone  AndHashNode::clone等
    virtual QuerySolutionNode* clone() const = 0;

    /**
     * Copy base query solution data from 'this' to 'other'.
     */
    void cloneBaseData(QuerySolutionNode* other) const {
        for (size_t i = 0; i < this->children.size(); i++) {
            other->children.push_back(this->children[i]->clone());
        }
        if (NULL != this->filter) {
            other->filter = this->filter->shallowClone();
        }
    }

    // These are owned here.
    std::vector<QuerySolutionNode*> children;

    // If a stage has a non-NULL filter all values outputted from that stage must pass that
    // filter.
    std::unique_ptr<MatchExpression> filter;  

protected:
    /**
     * Formatting helper used by toString().
     */
    static void addIndent(mongoutils::str::stream* ss, int level);

    /**
     * Every solution node has properties and this adds the debug info for the
     * properties.
     */
    void addCommon(mongoutils::str::stream* ss, int indent) const;

private:
    MONGO_DISALLOW_COPYING(QuerySolutionNode);
};

/**
 * A QuerySolution must be entirely self-contained and own everything inside of it.
 *
 * A tree of stages may be built from a QuerySolution.  The QuerySolution must outlive the tree
 * of stages. 每个查询计划QuerySolution对应一个计划阶段PlanStage. 见getExecutor
 */ 
//QueryPlannerAccess::buildIndexedDataAccess和QueryPlannerAnalysis::analyzeDataAccess
//一起生成QuerySolutionNode tree, 并存储到querySolution.root(也就是这个QuerySolutionNode tree)

 
 //参考https://yq.aliyun.com/articles/215016?spm=a2c4e.11155435.0.0.21ad5df01WAL0E

//QueryPlannerAnalysis::analyzeDataAccess中生成QuerySolution
//PlanExecutor的主要作用是选出最佳的QuerySolution， 并且执行该solution
//每个索引对应一种执行计划，在MongoDB中叫解决方案，参考querysolution.txt日志理解
struct QuerySolution { //执行计划，可以参考https://yq.aliyun.com/articles/647563?spm=a2c4e.11155435.0.0.7cb74df3gUVck4
    QuerySolution() : hasBlockingStage(false), indexFilterApplied(false) {}

    //QueryPlannerAccess::buildIndexedDataAccess和QueryPlannerAnalysis::analyzeDataAccess
    //一起生成QuerySolutionNode tree, 并存储倒querySolution.root(也就是这个QuerySolutionNode tree)
    // Owned here. 
    std::unique_ptr<QuerySolutionNode> root;

    // Any filters in root or below point into this object.  Must be owned.
    BSONObj filterData;

    // There are two known scenarios in which a query solution might potentially block:
    //
    // Sort stage:
    // If the solution has a sort stage, the sort wasn't provided by an index, so we might want
    // to scan an index to provide that sort in a non-blocking fashion.
    //
    // Hashed AND stage:
    // The hashed AND stage buffers data from multiple index scans and could block. In that case,
    // we would want to fall back on an alternate non-blocking solution.
    //QueryPlannerAnalysis::analyzeDataAccess中可能赋值为true, 默认false
    //如果有soln->hasBlockingStage = hasSortStage || hasAndHashStage;这两个stage则为blockstage
    bool hasBlockingStage;

    // Runner executing this solution might be interested in knowing
    // if the planning process for this solution was based on filtered indices.
    bool indexFilterApplied;

    // Owned here. Used by the plan cache.
    //缓存该solution对应索引信息，参考QueryPlanner::plan
    std::unique_ptr<SolutionCacheData> cacheData;

    /**
     * Output a human-readable std::string representing the plan.
     */
    std::string toString() {
        if (NULL == root) {
            return "empty query solution";
        }

        mongoutils::str::stream ss;
        root->appendToString(&ss, 0);
        return ss;
    }

private:
    MONGO_DISALLOW_COPYING(QuerySolution);
};

struct TextNode : public QuerySolutionNode {
    TextNode(IndexEntry index)
        : _sort(SimpleBSONObjComparator::kInstance.makeBSONObjSet()), index(std::move(index)) {}

    virtual ~TextNode() {}

    virtual StageType getType() const {
        return STAGE_TEXT;
    }

    virtual void appendToString(mongoutils::str::stream* ss, int indent) const;

    // Text's return is LOC_AND_OBJ so it's fetched and has all fields.
    bool fetched() const {
        return true;
    }
    bool hasField(const std::string& field) const {
        return true;
    }
    bool sortedByDiskLoc() const {
        return false;
    }
    const BSONObjSet& getSort() const {
        return _sort;
    }

    QuerySolutionNode* clone() const;

    BSONObjSet _sort;

    IndexEntry index;
    std::unique_ptr<fts::FTSQuery> ftsQuery;

    // The number of fields in the prefix of the text index. For example, if the key pattern is
    //
    //   { a: 1, b: 1, _fts: "text", _ftsx: 1, c: 1 }
    //
    // then the number of prefix fields is 2, because of "a" and "b".
    size_t numPrefixFields = 0u;

    // "Prefix" fields of a text index can handle equality predicates.  We group them with the
    // text node while creating the text leaf node and convert them into a BSONObj index prefix
    // when we finish the text leaf node.
    BSONObj indexPrefix;
};

//QueryPlannerAccess::makeCollectionScan中构造使用
struct CollectionScanNode : public QuerySolutionNode {
    CollectionScanNode();
    virtual ~CollectionScanNode() {}

    virtual StageType getType() const {
        return STAGE_COLLSCAN;
    }

    virtual void appendToString(mongoutils::str::stream* ss, int indent) const;

    bool fetched() const {
        return true;
    }
    bool hasField(const std::string& field) const {
        return true;
    }
    bool sortedByDiskLoc() const {
        return false;
    }
    const BSONObjSet& getSort() const {
        return _sort;
    }

    QuerySolutionNode* clone() const;

    BSONObjSet _sort;

    // Name of the namespace.
    std::string name;

    // Should we make a tailable cursor?
    bool tailable;

    // Should we keep track of the timestamp of the latest oplog entry we've seen? This information
    // is needed to merge cursors from the oplog in order of operation time when reading the oplog
    // across a sharded cluster.
    bool shouldTrackLatestOplogTimestamp = false;

    //增排序 还是减排序
    int direction;

    // maxScan option to .find() limits how many docs we look at.
    //db.collection.find( { $query: { <query> }, $maxScan: <number> } )
    int maxScan;
};


/*
例如如下场景会进入： 同一个查询，每个字段各有索引满足条件
2021-02-10T09:54:03.666+0800 D QUERY    [conn-4] About to build solntree(QuerySolution tree) from tagged tree:
$and
    age == 99.0  || Selected Index #1 pos 0 combine 1
    name == "yangyazhou2"  || Selected Index #2 pos 0 combine 1
2021-02-10T09:54:03.666+0800 D QUERY    [conn-4] About to build solntree(QuerySolution tree) from tagged tree, after prepareForAccessPlanning:
$and
    age == 99.0  || Selected Index #1 pos 0 combine 1
    name == "yangyazhou2"  || Selected Index #2 pos 0 combine 1
2021-02-10T09:54:03.666+0800 D QUERY    [conn-4] Can't build index intersection solution: AND_SORTED is not possible and AND_HASH is disabled.
*/

//QueryPlannerAccess::buildIndexedAnd中构造使用
struct AndHashNode : public QuerySolutionNode {
    AndHashNode();
    virtual ~AndHashNode();

    virtual StageType getType() const {
        return STAGE_AND_HASH;
    }

    virtual void appendToString(mongoutils::str::stream* ss, int indent) const;

    bool fetched() const;
    bool hasField(const std::string& field) const;
    bool sortedByDiskLoc() const {
        return false;
    }
    const BSONObjSet& getSort() const {
        return children.back()->getSort();
    }

    QuerySolutionNode* clone() const;

    BSONObjSet _sort;
};

/* 例如下面的情况会满足使用
2021-02-10T09:54:03.666+0800 D QUERY    [conn-4] About to build solntree(QuerySolution tree) from tagged tree:
$and
    age == 99.0  || Selected Index #1 pos 0 combine 1
    name == "yangyazhou2"  || Selected Index #0 pos 0 combine 1
2021-02-10T09:54:03.666+0800 D QUERY    [conn-4] About to build solntree(QuerySolution tree) from tagged tree, after prepareForAccessPlanning:
$and
    name == "yangyazhou2"  || Selected Index #0 pos 0 combine 1
    age == 99.0  || Selected Index #1 pos 0 combine 1
2021-02-10T09:54:03.666+0800 D QUERY    [conn-4] Planner: adding QuerySolutionNode:
FETCH
---filter:
        $and
            name == "yangyazhou2"  || Selected Index #0 pos 0 combine 1
            age == 99.0  || Selected Index #1 pos 0 combine 1
---fetched = 1
---sortedByDiskLoc = 1
---getSort = []
---Child:
------AND_SORTED
---------fetched = 0
---------sortedByDiskLoc = 1
---------getSort = []
---------Child 0:
---------IXSCAN
------------indexName = name_1
keyPattern = { name: 1.0 }
------------direction = 1
------------bounds = field #0['name']: ["yangyazhou2", "yangyazhou2"]
------------fetched = 0
------------sortedByDiskLoc = 1
------------getSort = []
---------Child 1:
---------IXSCAN
------------indexName = age_1
keyPattern = { age: 1.0 }
------------direction = 1
------------bounds = field #0['age']: [99.0, 99.0]
------------fetched = 0
------------sortedByDiskLoc = 1
------------getSort = []
也就是把每个IXSCAN的排序后做合并，例如db.test.find({ name : "yangyazhou2" , "age" : 99 }),
这个查询对应test表只建了name:1索引和age:1索引，则两个索引都满足候选索引条件，则分别利用两个索引排序，
然后再对这两个排序好的数据进行AND_SORTED合并排序
*/
//QueryPlannerAccess::buildIndexedAnd中构造使用
struct AndSortedNode : public QuerySolutionNode {
    AndSortedNode();
    virtual ~AndSortedNode();
    //对应QuerySolutionNode为AndSortedNode
    virtual StageType getType() const {
        return STAGE_AND_SORTED;
    }

    virtual void appendToString(mongoutils::str::stream* ss, int indent) const;

    bool fetched() const;
    bool hasField(const std::string& field) const;
    bool sortedByDiskLoc() const {
        return true;
    }
    const BSONObjSet& getSort() const {
        return _sort;
    }

    QuerySolutionNode* clone() const;

    BSONObjSet _sort;
};

struct OrNode : public QuerySolutionNode {
    OrNode();
    virtual ~OrNode();
    //对应QuerySolutionNode为OrNode
    virtual StageType getType() const {
        return STAGE_OR;
    }

    virtual void appendToString(mongoutils::str::stream* ss, int indent) const;

    bool fetched() const;
    bool hasField(const std::string& field) const;
    bool sortedByDiskLoc() const {
        // Even if our children are sorted by their diskloc or other fields, we don't maintain
        // any order on the output.
        return false;
    }
    const BSONObjSet& getSort() const {
        return _sort;
    }

    QuerySolutionNode* clone() const;

    BSONObjSet _sort;

    bool dedup;
};

struct MergeSortNode : public QuerySolutionNode {
    MergeSortNode();
    virtual ~MergeSortNode();
    //对应QuerySolutionNode为MergeSortNode
    virtual StageType getType() const {
        return STAGE_SORT_MERGE;
    }

    virtual void appendToString(mongoutils::str::stream* ss, int indent) const;

    bool fetched() const;
    bool hasField(const std::string& field) const;
    bool sortedByDiskLoc() const {
        return false;
    }

    const BSONObjSet& getSort() const {
        return _sorts;
    }

    QuerySolutionNode* clone() const;

    virtual void computeProperties() {
        for (size_t i = 0; i < children.size(); ++i) {
            children[i]->computeProperties();
        }
        _sorts.clear();
        _sorts.insert(sort);
    }

    BSONObjSet _sorts;

    BSONObj sort;
    bool dedup;
};

struct FetchNode : public QuerySolutionNode {
    FetchNode();
    virtual ~FetchNode() {}
    //对应QuerySolutionNode为FetchNode 
    virtual StageType getType() const {
        return STAGE_FETCH;
    }

    virtual void appendToString(mongoutils::str::stream* ss, int indent) const;

    bool fetched() const {
        return true;
    }
    bool hasField(const std::string& field) const {
        return true;
    }
    bool sortedByDiskLoc() const {
        return children[0]->sortedByDiskLoc();
    }
    const BSONObjSet& getSort() const {
        return children[0]->getSort();
    }

    QuerySolutionNode* clone() const;

    BSONObjSet _sorts;
};

//QueryPlannerAccess::makeLeafNode构造使用
struct IndexScanNode : public QuerySolutionNode {
    IndexScanNode(IndexEntry index);
    virtual ~IndexScanNode() {}

    virtual void computeProperties();
    //对应QuerySolutionNode为IndexScanNode
    virtual StageType getType() const {
        return STAGE_IXSCAN;
    }

    virtual void appendToString(mongoutils::str::stream* ss, int indent) const;

    bool fetched() const {
        return false;
    }
    bool hasField(const std::string& field) const;
    bool sortedByDiskLoc() const;
    const BSONObjSet& getSort() const {
        return _sorts;
    }

    QuerySolutionNode* clone() const;

    bool operator==(const IndexScanNode& other) const;

    /**
     * This function extracts a list of field names from 'indexKeyPattern' whose corresponding index
     * bounds in 'bounds' can contain strings.  This is the case if there are intervals containing
     * String, Object, or Array values.
     */
    static std::set<StringData> getFieldsWithStringBounds(const IndexBounds& bounds,
                                                          const BSONObj& indexKeyPattern);
    //例如索引{ name: 1.0, male: 1.0 }对应的getSort就是
    //[{ male: -1 }, { name: -1 }, { name: -1, male: -1 }, ]
    //IndexScanNode::computeProperties()赋值
    BSONObjSet _sorts;
    //也就是索引信息，例如{ name: 1.0, male: 1.0 }
    IndexEntry index;

    int direction;

    // maxScan option to .find() limits how many docs we look at.
    int maxScan; //db.collection.find( { $query: { <query> }, $maxScan: <number> } 

    // If there's a 'returnKey' projection we add key metadata.
    bool addKeyMetadata;
    //例如类似bounds = field #0['name']: ["yangyazhou2", "yangyazhou2"], field #1['male']: [MaxKey, MinKey]
    IndexBounds bounds; 

    const CollatorInterface* queryCollator;

    // The set of paths in the index key pattern which have at least one multikey path component, or
    // empty if the index either is not multikey or does not have path-level multikeyness metadata.
    //
    // The correct set of paths is computed and stored here by computeProperties().
    std::set<StringData> multikeyFields;
};

struct ProjectionNode : public QuerySolutionNode {
    /**
     * We have a few implementations of the projection functionality.  The most general
     * implementation 'DEFAULT' is much slower than the fast-path implementations
     * below.  We only really have all the information available to choose a projection
     * implementation at planning time.
     */
    enum ProjectionType {
        // This is the most general implementation of the projection functionality.  It handles
        // every case.
        DEFAULT,

        // This is a fast-path for when the projection is fully covered by one index.
        COVERED_ONE_INDEX,

        // This is a fast-path for when the projection only has inclusions on non-dotted fields.
        SIMPLE_DOC,
    };

    ProjectionNode(ParsedProjection proj)
        : _sorts(SimpleBSONObjComparator::kInstance.makeBSONObjSet()),
          fullExpression(NULL),
          projType(DEFAULT),
          parsed(proj) {}

    virtual ~ProjectionNode() {}
    //对应QuerySolutionNode为ProjectionNode
    virtual StageType getType() const {
        return STAGE_PROJECTION;
    }

    virtual void computeProperties();

    virtual void appendToString(mongoutils::str::stream* ss, int indent) const;

    /**
     * Data from the projection node is considered fetch iff the child provides fetched data.
     */
    bool fetched() const {
        return children[0]->fetched();
    }

    bool hasField(const std::string& field) const {
        // TODO: Returning false isn't always the right answer -- we may either be including
        // certain fields, or we may be dropping fields (in which case hasField returns true).
        //
        // Given that projection sits on top of everything else in .find() it doesn't matter
        // what we do here.
        return false;
    }

    bool sortedByDiskLoc() const {
        // Projections destroy the RecordId.  By returning true here, this kind of implies that a
        // fetch could still be done upstream.
        //
        // Perhaps this should be false to not imply that there *is* a RecordId?  Kind of a
        // corner case.
        return children[0]->sortedByDiskLoc();
    }

    const BSONObjSet& getSort() const {
        return _sorts;
    }

    QuerySolutionNode* clone() const;

    BSONObjSet _sorts;

    // The full query tree.  Needed when we have positional operators.
    // Owned in the CanonicalQuery, not here.
    MatchExpression* fullExpression;

    // Given that we don't yet have a MatchExpression analogue for the expression language, we
    // use a BSONObj.
    BSONObj projection;

    // What implementation of the projection algorithm should we use?
    ProjectionType projType;

    ParsedProjection parsed;

    // Only meaningful if projType == COVERED_ONE_INDEX.  This is the key pattern of the index
    // supplying our covered data.  We can pre-compute which fields to include and cache that
    // data for later if we know we only have one index.
    BSONObj coveredKeyObj;
};

struct SortKeyGeneratorNode : public QuerySolutionNode {
    //对应QuerySolutionNode为SortKeyGeneratorNode
    StageType getType() const final {
        return STAGE_SORT_KEY_GENERATOR;
    }

    bool fetched() const final {
        return children[0]->fetched();
    }

    bool hasField(const std::string& field) const final {
        return children[0]->hasField(field);
    }

    bool sortedByDiskLoc() const final {
        return children[0]->sortedByDiskLoc();
    }

    const BSONObjSet& getSort() const final {
        return children[0]->getSort();
    }

    QuerySolutionNode* clone() const final;

    void appendToString(mongoutils::str::stream* ss, int indent) const final;

    // The user-supplied sort pattern.
    BSONObj sortSpec;
};

//QueryPlannerAnalysis::analyzeSort中构造使用
struct SortNode : public QuerySolutionNode {
    SortNode() : _sorts(SimpleBSONObjComparator::kInstance.makeBSONObjSet()), limit(0) {}

    virtual ~SortNode() {}
    //对应QuerySolutionNode为SortNode
    virtual StageType getType() const {
        return STAGE_SORT;
    }

    virtual void appendToString(mongoutils::str::stream* ss, int indent) const;

    bool fetched() const {
        return children[0]->fetched();
    }
    bool hasField(const std::string& field) const {
        return children[0]->hasField(field);
    }
    bool sortedByDiskLoc() const {
        return false;
    }

    const BSONObjSet& getSort() const {
        return _sorts;
    }

    QuerySolutionNode* clone() const;

    virtual void computeProperties() {
        for (size_t i = 0; i < children.size(); ++i) {
            children[i]->computeProperties();
        }
        _sorts.clear();
        _sorts.insert(pattern);
    }

    BSONObjSet _sorts;

    BSONObj pattern;

    // Sum of both limit and skip count in the parsed query.
    size_t limit;
};

struct LimitNode : public QuerySolutionNode {
    LimitNode() {}
    virtual ~LimitNode() {}
    //对应QuerySolutionNode为LimitNode
    virtual StageType getType() const {
        return STAGE_LIMIT;
    }

    virtual void appendToString(mongoutils::str::stream* ss, int indent) const;

    bool fetched() const {
        return children[0]->fetched();
    }
    bool hasField(const std::string& field) const {
        return children[0]->hasField(field);
    }
    bool sortedByDiskLoc() const {
        return children[0]->sortedByDiskLoc();
    }
    const BSONObjSet& getSort() const {
        return children[0]->getSort();
    }

    QuerySolutionNode* clone() const;

    long long limit;
};

struct SkipNode : public QuerySolutionNode {
    SkipNode() {}
    virtual ~SkipNode() {}
    //对应QuerySolutionNode为SkipNode
    virtual StageType getType() const {
        return STAGE_SKIP;
    }
    virtual void appendToString(mongoutils::str::stream* ss, int indent) const;

    bool fetched() const {
        return children[0]->fetched();
    }
    bool hasField(const std::string& field) const {
        return children[0]->hasField(field);
    }
    bool sortedByDiskLoc() const {
        return children[0]->sortedByDiskLoc();
    }
    const BSONObjSet& getSort() const {
        return children[0]->getSort();
    }

    QuerySolutionNode* clone() const;

    long long skip;
};

// This is a standalone stage.
struct GeoNear2DNode : public QuerySolutionNode {
    GeoNear2DNode(IndexEntry index)
        : _sorts(SimpleBSONObjComparator::kInstance.makeBSONObjSet()),
          index(std::move(index)),
          addPointMeta(false),
          addDistMeta(false) {}

    virtual ~GeoNear2DNode() {}
    //对应QuerySolutionNode为GeoNear2DNode
    virtual StageType getType() const {
        return STAGE_GEO_NEAR_2D;
    }
    virtual void appendToString(mongoutils::str::stream* ss, int indent) const;

    bool fetched() const {
        return true;
    }
    bool hasField(const std::string& field) const {
        return true;
    }
    bool sortedByDiskLoc() const {
        return false;
    }
    const BSONObjSet& getSort() const {
        return _sorts;
    }

    QuerySolutionNode* clone() const;

    BSONObjSet _sorts;

    // Not owned here
    const GeoNearExpression* nq;
    IndexBounds baseBounds;

    IndexEntry index;
    bool addPointMeta;
    bool addDistMeta;
};

// This is actually its own standalone stage.
struct GeoNear2DSphereNode : public QuerySolutionNode {
    GeoNear2DSphereNode(IndexEntry index)
        : _sorts(SimpleBSONObjComparator::kInstance.makeBSONObjSet()),
          index(std::move(index)),
          addPointMeta(false),
          addDistMeta(false) {}

    virtual ~GeoNear2DSphereNode() {}
    //对应QuerySolutionNode为GeoNear2DSphereNode
    virtual StageType getType() const {
        return STAGE_GEO_NEAR_2DSPHERE;
    }
    virtual void appendToString(mongoutils::str::stream* ss, int indent) const;

    bool fetched() const {
        return true;
    }
    bool hasField(const std::string& field) const {
        return true;
    }
    bool sortedByDiskLoc() const {
        return false;
    }
    const BSONObjSet& getSort() const {
        return _sorts;
    }

    QuerySolutionNode* clone() const;

    BSONObjSet _sorts;

    // Not owned here
    const GeoNearExpression* nq;
    IndexBounds baseBounds;

    IndexEntry index;
    bool addPointMeta;
    bool addDistMeta;
};

//
// Internal nodes used to provide functionality
//

/**
 * If we're answering a query on a sharded cluster, docs must be checked against the shard key
 * to ensure that we don't return data that shouldn't be there.  This must be done prior to
 * projection, and in fact should be done as early as possible to avoid propagating stale data
 * through the pipeline.
 */
//QueryPlannerAnalysis::analyzeDataAccess中使用
struct ShardingFilterNode : public QuerySolutionNode {
    ShardingFilterNode() {}
    virtual ~ShardingFilterNode() {}
    //对应QuerySolutionNode为ShardingFilterNode
    virtual StageType getType() const {
        return STAGE_SHARDING_FILTER;
    }
    virtual void appendToString(mongoutils::str::stream* ss, int indent) const;

    bool fetched() const {
        return children[0]->fetched();
    }
    bool hasField(const std::string& field) const {
        return children[0]->hasField(field);
    }
    bool sortedByDiskLoc() const {
        return children[0]->sortedByDiskLoc();
    }
    const BSONObjSet& getSort() const {
        return children[0]->getSort();
    }

    QuerySolutionNode* clone() const;
};

/**
 * If documents mutate or are deleted during a query, we can (in some cases) fetch them
 * and still return them.  This stage merges documents that have been mutated or deleted
 * into the query result stream.
 */
struct KeepMutationsNode : public QuerySolutionNode {
    KeepMutationsNode() : sorts(SimpleBSONObjComparator::kInstance.makeBSONObjSet()) {}

    virtual ~KeepMutationsNode() {}
    //对应QuerySolutionNode为KeepMutationsNode
    virtual StageType getType() const {
        return STAGE_KEEP_MUTATIONS;
    }
    virtual void appendToString(mongoutils::str::stream* ss, int indent) const;

    // Any flagged results are OWNED_OBJ and therefore we're covered if our child is.
    bool fetched() const {
        return children[0]->fetched();
    }

    // Any flagged results are OWNED_OBJ and as such they'll have any field we need.
    bool hasField(const std::string& field) const {
        return children[0]->hasField(field);
    }

    bool sortedByDiskLoc() const {
        return false;
    }
    const BSONObjSet& getSort() const {
        return sorts;
    }

    QuerySolutionNode* clone() const;

    // Since we merge in flagged results we have no sort order.
    BSONObjSet sorts;
};

/**
 * Distinct queries only want one value for a given field.  We run an index scan but
 * *always* skip over the current key to the next key.
 */
struct DistinctNode : public QuerySolutionNode {
    DistinctNode(IndexEntry index)
        : sorts(SimpleBSONObjComparator::kInstance.makeBSONObjSet()), index(std::move(index)) {}

    virtual ~DistinctNode() {}
    //对应QuerySolutionNode为DistinctNode
    virtual StageType getType() const {
        return STAGE_DISTINCT_SCAN;
    }
    virtual void appendToString(mongoutils::str::stream* ss, int indent) const;

    // This stage is created "on top" of normal planning and as such the properties
    // below don't really matter.
    bool fetched() const {
        return false;
    }
    bool hasField(const std::string& field) const {
        return !index.keyPattern[field].eoo();
    }
    bool sortedByDiskLoc() const {
        return false;
    }
    const BSONObjSet& getSort() const {
        return sorts;
    }

    QuerySolutionNode* clone() const;

    BSONObjSet sorts;

    IndexEntry index;
    int direction;
    IndexBounds bounds;
    // We are distinct-ing over the 'fieldNo'-th field of 'index.keyPattern'.
    int fieldNo;
};

/**
 * Some count queries reduce to counting how many keys are between two entries in a
 * Btree.
 */
struct CountScanNode : public QuerySolutionNode {
    CountScanNode(IndexEntry index)
        : sorts(SimpleBSONObjComparator::kInstance.makeBSONObjSet()), index(std::move(index)) {}

    virtual ~CountScanNode() {}
    //对应QuerySolutionNode为CountScanNode
    virtual StageType getType() const {
        return STAGE_COUNT_SCAN;
    }
    virtual void appendToString(mongoutils::str::stream* ss, int indent) const;

    bool fetched() const {
        return false;
    }
    bool hasField(const std::string& field) const {
        return true;
    }
    bool sortedByDiskLoc() const {
        return false;
    }
    const BSONObjSet& getSort() const {
        return sorts;
    }

    QuerySolutionNode* clone() const;

    BSONObjSet sorts;

    IndexEntry index;

    BSONObj startKey;
    bool startKeyInclusive;

    BSONObj endKey;
    bool endKeyInclusive;
};

/**
 * This stage drops results that are out of sorted order.
 */

struct EnsureSortedNode : public QuerySolutionNode {
    EnsureSortedNode() {}
    virtual ~EnsureSortedNode() {}
    //对应QuerySolutionNode为EnsureSortedNode
    virtual StageType getType() const {
        return STAGE_ENSURE_SORTED;
    }

    virtual void appendToString(mongoutils::str::stream* ss, int indent) const;

    bool fetched() const {
        return children[0]->fetched();
    }
    bool hasField(const std::string& field) const {
        return children[0]->hasField(field);
    }
    bool sortedByDiskLoc() const {
        return children[0]->sortedByDiskLoc();
    }
    const BSONObjSet& getSort() const {
        return children[0]->getSort();
    }

    QuerySolutionNode* clone() const;

    // The pattern that the results should be sorted by.
    BSONObj pattern;
};

}  // namespace mongo
