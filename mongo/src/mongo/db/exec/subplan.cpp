/**
 *    Copyright (C) 2013-2014 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/db/exec/subplan.h"

#include <memory>
#include <vector>

#include "mongo/client/dbclientinterface.h"
#include "mongo/db/exec/multi_plan.h"
#include "mongo/db/exec/scoped_timer.h"
#include "mongo/db/matcher/extensions_callback_real.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/planner_access.h"
#include "mongo/db/query/planner_analysis.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/query_planner_common.h"
#include "mongo/db/query/stage_builder.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/transitional_tools_do_not_use/vector_spooling.h"

namespace mongo {

using std::endl;
using std::unique_ptr;
using std::vector;
using stdx::make_unique;

const char* SubplanStage::kStageType = "SUBPLAN";

SubplanStage::SubplanStage(OperationContext* opCtx,
                           Collection* collection,
                           WorkingSet* ws,
                           const QueryPlannerParams& params,
                           CanonicalQuery* cq)
    : PlanStage(kStageType, opCtx),
      _collection(collection),
      _ws(ws),
      _plannerParams(params),
      _query(cq) {
    invariant(_collection);
}

namespace {

/**
 * Returns true if 'expr' is an AND that contains a single OR child.
 */
bool isContainedOr(const MatchExpression* expr) {
    if (MatchExpression::AND != expr->matchType()) {
        return false;
    }

    size_t numOrs = 0;
    for (size_t i = 0; i < expr->numChildren(); ++i) {
        if (MatchExpression::OR == expr->getChild(i)->matchType()) {
            ++numOrs;
        }
    }

    return (numOrs == 1U);
}

}  // namespace

//如果root为OR类型，则返回false
bool SubplanStage::canUseSubplanning(const CanonicalQuery& query) {
    const QueryRequest& qr = query.getQueryRequest();
    const MatchExpression* expr = query.root();

    // Hint provided   强制索引返回false
    if (!qr.getHint().isEmpty()) {
        return false;
    }

    // Min provided      Min queries 返回false
    // Min queries are a special case of hinted queries.
    if (!qr.getMin().isEmpty()) {
        return false;
    }

    // Max provided  max queries 返回false
    // Similar to min, max queries are a special case of hinted queries.
    if (!qr.getMax().isEmpty()) {
        return false;
    }

    // Tailable cursors won't get cached, just turn into collscans.
    if (query.getQueryRequest().isTailable()) {
        return false;
    }

    // Snapshot is really a hint.
    if (query.getQueryRequest().isSnapshot()) {
        return false;
    }

    // TODO: For now we only allow rooted OR. We should consider also allowing contained OR that
    // does not have a TEXT or GEO_NEAR node.
    return MatchExpression::OR == expr->matchType();
}

std::unique_ptr<MatchExpression> SubplanStage::rewriteToRootedOr(
    std::unique_ptr<MatchExpression> root) {
    dassert(isContainedOr(root.get()));

    // Detach the OR from the root.
    std::vector<MatchExpression*>& rootChildren = *root->getChildVector();
    std::unique_ptr<MatchExpression> orChild;
    for (size_t i = 0; i < rootChildren.size(); ++i) {
        if (MatchExpression::OR == rootChildren[i]->matchType()) {
            orChild.reset(rootChildren[i]);
            rootChildren.erase(rootChildren.begin() + i);
            break;
        }
    }

    // We should have found an OR, and the OR should have at least 2 children.
    invariant(orChild);
    invariant(orChild->getChildVector());
    invariant(orChild->getChildVector()->size() > 1U);

    // AND the existing root with each OR child.
    std::vector<MatchExpression*>& orChildren = *orChild->getChildVector();
    for (size_t i = 0; i < orChildren.size(); ++i) {
        std::unique_ptr<AndMatchExpression> ama = stdx::make_unique<AndMatchExpression>();
        ama->add(orChildren[i]);
        ama->add(root->shallowClone().release());
        orChildren[i] = ama.release();
    }

    // Normalize and sort the resulting match expression.
    orChild = MatchExpression::optimize(std::move(orChild));
    CanonicalQuery::sortTree(orChild.get());

    return orChild;
}

/*
db.test.find( {$or : [{ $and : [ { name : "0.99" }, { "age" : 99 } ] },{ $or : [ {	name : "cc" }, { "xx" : 3} ] } ]} ).sort({"name":1}).limit(7)
上面查询最终会下面的MatchExpression tree
		     $or	------这层对应stage为SubplanStage
		  /   \	  \
		 /	  \     \
		/	name:cc  \		
	  $and			 \ 
	  /   \ 			  \
	/	  \ 			 "xx" : 3(也就是"xx":{$eq:3})
name:0.99	age:99
参考目录中的querysolution.txt,配合该日志阅读  
*/

//SubplanStage::pickBestPlan调用，配合prepareExecution生成SubplanStage阅读
Status SubplanStage::planSubqueries() {
	//克隆该root expression
    _orExpression = _query->root()->shallowClone();
    if (isContainedOr(_orExpression.get())) {
        _orExpression = rewriteToRootedOr(std::move(_orExpression));
        invariant(CanonicalQuery::isValid(_orExpression.get(), _query->getQueryRequest()).isOK());
    }

    for (size_t i = 0; i < _plannerParams.indices.size(); ++i) {
        const IndexEntry& ie = _plannerParams.indices[i];
        _indexMap[ie.name] = i;
		//打印所有的索引信息
        LOG(5) << "Subplanner: index " << i << " is " << ie;
    }

    for (size_t i = 0; i < _orExpression->numChildren(); ++i) {
        // We need a place to shove the results from planning this branch.
        _branchResults.push_back(stdx::make_unique<BranchPlanningResult>());
        BranchPlanningResult* branchResult = _branchResults.back().get();

        MatchExpression* orChild = _orExpression->getChild(i);

		//OR下面得子tree各自统一规范化
        // Turn the i-th child into its own query.
        auto statusWithCQ = CanonicalQuery::canonicalize(getOpCtx(), *_query, orChild);
        if (!statusWithCQ.isOK()) {
            mongoutils::str::stream ss;
            ss << "Can't canonicalize subchild " << orChild->toString() << " "
               << statusWithCQ.getStatus().reason();
			LOG(5) << "Can't canonicalize subchild " << orChild->toString() << " "
               << statusWithCQ.getStatus().reason();
            return Status(ErrorCodes::BadValue, ss);
        }

        branchResult->canonicalQuery = std::move(statusWithCQ.getValue());

        // Plan the i-th child. We might be able to find a plan for the i-th child in the plan
        // cache. If there's no cached plan, then we generate and rank plans using the MPS.
        CachedSolution* rawCS;
		//线程缓存中获取
        if (PlanCache::shouldCacheQuery(*branchResult->canonicalQuery) &&
            _collection->infoCache()
                ->getPlanCache()
                //PlanCache::get从缓存的planCache中获取solution
                ->get(*branchResult->canonicalQuery, &rawCS)
                .isOK()) {
            // We have a CachedSolution. Store it for later.
            LOG(5) << "Subplanner: cached plan found for child " << i << " of "
                   << _orExpression->numChildren();

            branchResult->cachedSolution.reset(rawCS);
        } else {
            // No CachedSolution found. We'll have to plan from scratch.
            //OR下的第几个分支请求
            LOG(5) << "Subplanner: planning child " << i << " of " << _orExpression->numChildren();

            // We don't set NO_TABLE_SCAN because peeking at the cache data will keep us from
            // considering any plan that's a collscan.
            invariant(branchResult->solutions.empty());
            std::vector<QuerySolution*> rawSolutions;
			//获取该OR 子tree对应得QuerySolution
            Status status =
                QueryPlanner::plan(*branchResult->canonicalQuery, _plannerParams, &rawSolutions);
            branchResult->solutions = transitional_tools_do_not_use::spool_vector(rawSolutions);

            if (!status.isOK()) {
                mongoutils::str::stream ss;
                ss << "Can't plan for subchild " << branchResult->canonicalQuery->toString() << " "
                   << status.reason();
				LOG(5) << "Can't plan for subchild " << branchResult->canonicalQuery->toString() << " "
                   << status.reason();
                return Status(ErrorCodes::BadValue, ss);
            }
			//该OR下面的分支对应的indexed solutions.
            LOG(5) << "Subplanner: got " << branchResult->solutions.size() << " solutions";

            if (0 == branchResult->solutions.size()) {
                // If one child doesn't have an indexed solution, bail out.
                mongoutils::str::stream ss;
                ss << "No solutions for subchild " << branchResult->canonicalQuery->toString();
				LOG(5) << "No solutions for subchild " << branchResult->canonicalQuery->toString();
				return Status(ErrorCodes::BadValue, ss);
            }
        }
    }

    return Status::OK();
}

namespace {

/**
 * On success, applies the index tags from 'branchCacheData' (which represent the winning
 * plan for 'orChild') to 'compositeCacheData'.
 */
//SubplanStage::choosePlanForSubqueries调用
//填充filter的_tagData信息，把MatchExpression tree和索引tree信息关联起来
Status tagOrChildAccordingToCache(PlanCacheIndexTree* compositeCacheData,
                                  SolutionCacheData* branchCacheData,
                                  MatchExpression* orChild,
                                  const std::map<StringData, size_t>& indexMap) {
    invariant(compositeCacheData);

    // We want a well-formed *indexed* solution.
    if (NULL == branchCacheData) {
        // For example, we don't cache things for 2d indices.
        mongoutils::str::stream ss;
        ss << "No cache data for subchild " << orChild->toString();
		LOG(5) << "No cache data for subchild " << orChild->toString();
        return Status(ErrorCodes::BadValue, ss);
    }

	//该branch没有适合的候选索引，则直接打印报错
    if (SolutionCacheData::USE_INDEX_TAGS_SOLN != branchCacheData->solnType) {
        mongoutils::str::stream ss;
        ss << "No indexed cache data for subchild " << orChild->toString();
		LOG(5) << "No indexed cache data for subchild " << orChild->toString();
        return Status(ErrorCodes::BadValue, ss);
    }

    // Add the index assignments to our original query.
    Status tagStatus =
        QueryPlanner::tagAccordingToCache(orChild, branchCacheData->tree.get(), indexMap);

    if (!tagStatus.isOK()) {
        mongoutils::str::stream ss;
        ss << "Failed to extract indices from subchild " << orChild->toString();
		LOG(5) << "Failed to extract indices from subchild " << orChild->toString();
        return Status(ErrorCodes::BadValue, ss);
    }

    // Add the child's cache data to the cache data we're creating for the main query.
    //生成PlanCacheIndexTree tree树
    compositeCacheData->children.push_back(branchCacheData->tree->clone());

    return Status::OK();
}

}  // namespace

// Use the multi plan stage to select a winning plan for each branch, and then construct
// the overall winning plan from the resulting index tags.

//SubplanStage::pickBestPlan调用
//选择子查询对应的plan
Status SubplanStage::choosePlanForSubqueries(PlanYieldPolicy* yieldPolicy) {
    // This is the skeleton of index selections that is inserted into the cache.
    std::unique_ptr<PlanCacheIndexTree> cacheData(new PlanCacheIndexTree());

	//遍历各个OR子树
    for (size_t i = 0; i < _orExpression->numChildren(); ++i) {
        MatchExpression* orChild = _orExpression->getChild(i);
        BranchPlanningResult* branchResult = _branchResults[i].get();

        if (branchResult->cachedSolution.get()) {
            // We can get the index tags we need out of the cache.
            Status tagStatus = tagOrChildAccordingToCache(
                cacheData.get(), branchResult->cachedSolution->plannerData[0], orChild, _indexMap);
            if (!tagStatus.isOK()) {
				LOG(2) << "SubplanStage::choosePlanForSubqueries 1";
				mongoutils::str::stream ss;
                ss << "SubplanStage::choosePlanForSubqueries 2";
                return tagStatus;
            }
        } else if (1 == branchResult->solutions.size()) {
            QuerySolution* soln = branchResult->solutions.front().get();
            Status tagStatus = tagOrChildAccordingToCache(
                cacheData.get(), soln->cacheData.get(), orChild, _indexMap);
            if (!tagStatus.isOK()) {
				mongoutils::str::stream ss;
                ss << "SubplanStage::choosePlanForSubqueries 4";
				LOG(5) << "No indexed cache data for subchild " << orChild->toString();
                return tagStatus;
            }
        } else {
            // N solutions, rank them.

            // We already checked for zero solutions in planSubqueries(...).
            invariant(!branchResult->solutions.empty());

            _ws->clear();

            // We pass the SometimesCache option to the MPS because the SubplanStage currently does
            // not use the CachedPlanStage's eviction mechanism. We therefore are more conservative
            // about putting a potentially bad plan into the cache in the subplan path.
            // We temporarily add the MPS to _children to ensure that we pass down all
            // save/restore/invalidate messages that can be generated if pickBestPlan yields.
            invariant(_children.empty());
            _children.emplace_back(
                stdx::make_unique<MultiPlanStage>(getOpCtx(),
                                                  _collection,
                                                  branchResult->canonicalQuery.get(),
                                                  MultiPlanStage::CachingMode::SometimesCache));
            ON_BLOCK_EXIT([&] {
                invariant(_children.size() == 1);  // Make sure nothing else was added to _children.
                _children.pop_back();
            });
            MultiPlanStage* multiPlanStage = static_cast<MultiPlanStage*>(child().get());

            // Dump all the solutions into the MPS.
            for (size_t ix = 0; ix < branchResult->solutions.size(); ++ix) {
                PlanStage* nextPlanRoot;
                invariant(StageBuilder::build(getOpCtx(),
                                              _collection,
                                              *branchResult->canonicalQuery,
                                              *branchResult->solutions[ix],
                                              _ws,
                                              &nextPlanRoot));

                // Takes ownership of solution with index 'ix' and 'nextPlanRoot'.
                multiPlanStage->addPlan(branchResult->solutions[ix].release(), nextPlanRoot, _ws);
            }

            Status planSelectStat = multiPlanStage->pickBestPlan(yieldPolicy);
            if (!planSelectStat.isOK()) {
				LOG(2) << "SubplanStage::choosePlanForSubqueries 5";
				mongoutils::str::stream ss;
                ss << "SubplanStage::choosePlanForSubqueries 6";
                return planSelectStat;
            }

            if (!multiPlanStage->bestPlanChosen()) {
                mongoutils::str::stream ss;
                ss << "Failed to pick best plan for subchild "
                   << branchResult->canonicalQuery->toString();
				LOG(5) << "Failed to pick best plan for subchild "
                   << branchResult->canonicalQuery->toString();
                return Status(ErrorCodes::BadValue, ss);
            }

            QuerySolution* bestSoln = multiPlanStage->bestSolution();

            // Check that we have good cache data. For example, we don't cache things
            // for 2d indices.
            if (NULL == bestSoln->cacheData.get()) {
                mongoutils::str::stream ss;
                ss << "No cache data for subchild " << orChild->toString();
				LOG(5) << "No cache data for subchild " << orChild->toString();
                return Status(ErrorCodes::BadValue, ss);
            }

            if (SolutionCacheData::USE_INDEX_TAGS_SOLN != bestSoln->cacheData->solnType) {
                mongoutils::str::stream ss;
                ss << "No indexed cache data for subchild " << orChild->toString();
				LOG(5) << "No indexed cache data for subchild " << orChild->toString();
                return Status(ErrorCodes::BadValue, ss);
            }

            // Add the index assignments to our original query.
            Status tagStatus = QueryPlanner::tagAccordingToCache(
                orChild, bestSoln->cacheData->tree.get(), _indexMap);

            if (!tagStatus.isOK()) {
                mongoutils::str::stream ss;
                ss << "Failed to extract indices from subchild " << orChild->toString();
				LOG(5) << "Failed to extract indices from subchild " << orChild->toString();
                return Status(ErrorCodes::BadValue, ss);
            }

            cacheData->children.push_back(bestSoln->cacheData->tree->clone());
        }
    }

    // Must do this before using the planner functionality.
    prepareForAccessPlanning(_orExpression.get());

    // Use the cached index assignments to build solnRoot. Takes ownership of '_orExpression'.
    std::unique_ptr<QuerySolutionNode> solnRoot(QueryPlannerAccess::buildIndexedDataAccess(
        *_query, _orExpression.release(), false, _plannerParams.indices, _plannerParams));

    if (!solnRoot) {
        mongoutils::str::stream ss;
        ss << "Failed to build indexed data path for subplanned query\n";
		LOG(5) << "Failed to build indexed data path for subplanned query\n";
        return Status(ErrorCodes::BadValue, ss);
    }

    LOG(5) << "Subplanner: fully tagged tree is " << redact(solnRoot->toString());
	LOG(2) << "Subplanner: fully tagged tree is " << redact(solnRoot->toString());

    // Takes ownership of 'solnRoot'
    _compositeSolution.reset(
        QueryPlannerAnalysis::analyzeDataAccess(*_query, _plannerParams, std::move(solnRoot)));

    if (NULL == _compositeSolution.get()) {
        mongoutils::str::stream ss;
        ss << "Failed to analyze subplanned query";
		LOG(2) << "Failed to analyze subplanned query";
        return Status(ErrorCodes::BadValue, ss);
    }

    LOG(5) << "Subplanner: Composite solution is " << redact(_compositeSolution->toString());

    // Use the index tags from planning each branch to construct the composite solution,
    // and set that solution as our child stage.
    _ws->clear();
    PlanStage* root;
    invariant(StageBuilder::build(
        getOpCtx(), _collection, *_query, *_compositeSolution.get(), _ws, &root));
    invariant(_children.empty());
    _children.emplace_back(root);

    return Status::OK();
}

//SubplanStage::pickBestPlan调用
Status SubplanStage::choosePlanWholeQuery(PlanYieldPolicy* yieldPolicy) {
    // Clear out the working set. We'll start with a fresh working set.
    _ws->clear();

    // Use the query planning module to plan the whole query.
    std::vector<QuerySolution*> rawSolutions;
    Status status = QueryPlanner::plan(*_query, _plannerParams, &rawSolutions);
    std::vector<std::unique_ptr<QuerySolution>> solutions =
        transitional_tools_do_not_use::spool_vector(rawSolutions);
    if (!status.isOK()) {
        return Status(ErrorCodes::BadValue,
                      "error processing query: " + _query->toString() +
                          " planner returned error: " + status.reason());
    }

    // We cannot figure out how to answer the query.  Perhaps it requires an index
    // we do not have?
    if (0 == solutions.size()) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "error processing query: " << _query->toString()
                                    << " No query solutions");
    }

    if (1 == solutions.size()) {
        PlanStage* root;
        // Only one possible plan.  Run it.  Build the stages from the solution.
        verify(StageBuilder::build(getOpCtx(), _collection, *_query, *solutions[0], _ws, &root));
        invariant(_children.empty());
        _children.emplace_back(root);

        // This SubplanStage takes ownership of the query solution.
        _compositeSolution = std::move(solutions.back());
        solutions.pop_back();

        return Status::OK();
    } else {
        // Many solutions. Create a MultiPlanStage to pick the best, update the cache,
        // and so on. The working set will be shared by all candidate plans.
        invariant(_children.empty());
        _children.emplace_back(new MultiPlanStage(getOpCtx(), _collection, _query));
        MultiPlanStage* multiPlanStage = static_cast<MultiPlanStage*>(child().get());

        for (size_t ix = 0; ix < solutions.size(); ++ix) {
            if (solutions[ix]->cacheData.get()) {
                solutions[ix]->cacheData->indexFilterApplied = _plannerParams.indexFiltersApplied;
            }

            // version of StageBuild::build when WorkingSet is shared
            PlanStage* nextPlanRoot;
            verify(StageBuilder::build(
                getOpCtx(), _collection, *_query, *solutions[ix], _ws, &nextPlanRoot));

            // Takes ownership of 'solutions[ix]' and 'nextPlanRoot'.
            multiPlanStage->addPlan(solutions[ix].release(), nextPlanRoot, _ws);
        }

        // Delegate the the MultiPlanStage's plan selection facility.
        Status planSelectStat = multiPlanStage->pickBestPlan(yieldPolicy);
        if (!planSelectStat.isOK()) {
            return planSelectStat;
        }

        return Status::OK();
    }
}

//PlanExecutor::pickBestPlan中调用
Status SubplanStage::pickBestPlan(PlanYieldPolicy* yieldPolicy) {
    // Adds the amount of time taken by pickBestPlan() to executionTimeMillis. There's lots of
    // work that happens here, so this is needed for the time accounting to make sense.
    ScopedTimer timer(getClock(), &_commonStats.executionTimeMillis);

	/*
	db.test.find( {$or : [{ $and : [ { name : "0.99" }, { "age" : 99 } ] },{ $or : [ {  name : "cc" }, { "xx" : 3} ] } ]} ).sort({"name":1}).limit(7)
	上面查询最终会下面的MatchExpression tree
	              $or   ------这层对应stage为SubplanStage
			  /  \    \
			 /    \    \
			/   name:cc \	    
		  $and		    \ 
		  /   \ 		     \
		/	  \ 	        "xx" : 3(也就是"xx":{$eq:3})
	name:0.99   age:99
	参考目录中的querysolution.txt  
	*/
	//类似上面得or回走这里面,为每个子分支生成各自的querySolutions
    // Plan each branch of the $or.
    Status subplanningStatus = planSubqueries();
    if (!subplanningStatus.isOK()) {
        if (subplanningStatus == ErrorCodes::QueryPlanKilled ||
            subplanningStatus == ErrorCodes::ExceededTimeLimit) {
            // Query planning cannot continue if the plan for one of the subqueries was killed
            // because the collection or a candidate index may have been dropped, or if we've
            // exceeded the operation's time limit.
            return subplanningStatus;
        }
        return choosePlanWholeQuery(yieldPolicy);
    }

    // Use the multi plan stage to select a winning plan for each branch, and then construct
    // the overall winning plan from the resulting index tags.
    //选择整个OR查询的最优索引
    Status subplanSelectStat = choosePlanForSubqueries(yieldPolicy);
    if (!subplanSelectStat.isOK()) {
        if (subplanSelectStat == ErrorCodes::QueryPlanKilled ||
            subplanSelectStat == ErrorCodes::ExceededTimeLimit) {
            // Query planning cannot continue if the plan for one of the subqueries was killed
            // because the collection or a candidate index may have been dropped, or if we've
            // exceeded the operation's time limit.
            return subplanSelectStat;
        }
        return choosePlanWholeQuery(yieldPolicy);
    }

    return Status::OK();
}

bool SubplanStage::isEOF() {
    // If we're running we best have a runner.
    invariant(child());
    return child()->isEOF();
}
/*
db.test.find( {$or : [{ $and : [ { name : "0.99" }, { "age" : 99 } ] },{ $or : [ {  name : "cc" }, { "xx" : 3} ] } ]} ).sort({"name":1}).limit(7)
上面查询最终会下面的MatchExpression tree
              $or   ------这层对应stage为SubplanStage
		  /  \    \
		 /    \    \
		/   name:cc \	    
	  $and		    \ 
	  /   \ 		     \
	/	  \ 	        "xx" : 3(也就是"xx":{$eq:3})
name:0.99   age:99
参考目录中的querysolution.txt  
*/
PlanStage::StageState SubplanStage::doWork(WorkingSetID* out) {
    if (isEOF()) {
        return PlanStage::IS_EOF;
    }

    invariant(child());
    return child()->work(out);
}

unique_ptr<PlanStageStats> SubplanStage::getStats() {
    _commonStats.isEOF = isEOF();
    unique_ptr<PlanStageStats> ret = make_unique<PlanStageStats>(_commonStats, STAGE_SUBPLAN);
    ret->children.emplace_back(child()->getStats());
    return ret;
}

//判断该子分支是否有缓存的solution
bool SubplanStage::branchPlannedFromCache(size_t i) const {
    return NULL != _branchResults[i]->cachedSolution.get();
}

const SpecificStats* SubplanStage::getSpecificStats() const {
    return NULL;
}

}  // namespace mongo
