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

#include "mongo/bson/bsonelement_comparator.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_path.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/stdx/memory.h"
#include "mongo/stdx/unordered_map.h"

namespace pcrecpp {
class RE;
}  // namespace pcrecpp;

namespace mongo {

class CollatorInterface;

/*
Expression_geo.h (src\mongo\db\matcher):class GeoMatchExpression : public LeafMatchExpression {
Expression_geo.h (src\mongo\db\matcher):class GeoNearMatchExpression : public LeafMatchExpression {
Expression_internal_schema_eq.h (src\mongo\db\matcher\schema):class InternalSchemaEqMatchExpression final : public LeafMatchExpression {
Expression_internal_schema_fmod.h (src\mongo\db\matcher\schema):class InternalSchemaFmodMatchExpression final : public LeafMatchExpression {
Expression_internal_schema_str_length.h (src\mongo\db\matcher\schema):class InternalSchemaStrLengthMatchExpression : public LeafMatchExpression {
Expression_leaf.h (src\mongo\db\matcher):class ComparisonMatchExpression : public LeafMatchExpression {
Expression_leaf.h (src\mongo\db\matcher):class RegexMatchExpression : public LeafMatchExpression {
Expression_leaf.h (src\mongo\db\matcher):class ModMatchExpression : public LeafMatchExpression {
Expression_leaf.h (src\mongo\db\matcher):class ExistsMatchExpression : public LeafMatchExpression {
Expression_leaf.h (src\mongo\db\matcher):class InMatchExpression : public LeafMatchExpression {
Expression_leaf.h (src\mongo\db\matcher):class BitTestMatchExpression : public LeafMatchExpression {
Expression_text_base.h (src\mongo\db\matcher):class TextMatchExpressionBase : public LeafMatchExpression {
Expression_type.h (src\mongo\db\matcher):class TypeMatchExpressionBase : public LeafMatchExpression {
Geo_near.cpp (src\mongo\db\exec):class TwoDPtInAnnulusExpression : public LeafMatchExpression {
*/  

//ComparisonMatchExpression继承LeafMatchExpression，LeafMatchExpression继承PathMatchExpression
//ComparisonMatchExpression(EQ, LTE, LT, GT, GTE)等继承该类，见上面的注释
class LeafMatchExpression : public PathMatchExpression {
public:
    explicit LeafMatchExpression(MatchType matchType) : PathMatchExpression(matchType) {}

    virtual ~LeafMatchExpression() {}

    size_t numChildren() const override {
        return 0;
    }

    MatchExpression* getChild(size_t i) const override {
        MONGO_UNREACHABLE;
    }

    std::vector<MatchExpression*>* getChildVector() override {
        return nullptr;
    }

    bool shouldExpandLeafArray() const override {
        return true;
    }

    MatchCategory getCategory() const override {
        return MatchCategory::kLeaf;
    }
};

/**
 * EQ, LTE, LT, GT, GTE subclass from ComparisonMatchExpression.
 上面的subclass分别对应以下类:
 GTEMatchExpression GTMatchExpression LTMatchExpression LTEMatchExpression EqualityMatchExpression
 */ //parseComparison中会使用
class ComparisonMatchExpression : public LeafMatchExpression {
public:
    explicit ComparisonMatchExpression(MatchType type) : LeafMatchExpression(type) {}

    Status init(StringData path, const BSONElement& rhs);

    virtual ~ComparisonMatchExpression() {}

    bool matchesSingleElement(const BSONElement&, MatchDetails* details = nullptr) const final;

    virtual void debugString(StringBuilder& debug, int level = 0) const;

    virtual void serialize(BSONObjBuilder* out) const;

    virtual bool equivalent(const MatchExpression* other) const;

    const BSONElement& getData() const {
        return _rhs;
    }

    const CollatorInterface* getCollator() const {
        return _collator;
    }

    /**
     * Returns true if the MatchExpression is a ComparisonMatchExpression.
     */
    static bool isComparisonMatchExpression(const MatchExpression* expr) {
        switch (expr->matchType()) {
            case MatchExpression::LT:
            case MatchExpression::LTE:
            case MatchExpression::EQ:
            case MatchExpression::GTE:
            case MatchExpression::GT:
                return true;
            default:
                return false;
        }
    }

protected:
    /**
     * 'collator' must outlive the ComparisonMatchExpression and any clones made of it.
     */
    virtual void _doSetCollator(const CollatorInterface* collator) {
        _collator = collator;
    }

    //parseComparison调用，path也就是{ aa : 0.99 }或者{ aa: { $lt: "0.99" } },
    //rhs为path对应的原始bson
    BSONElement _rhs;

    // Collator used to compare elements. By default, simple binary comparison will be used.
    const CollatorInterface* _collator = nullptr;

private:
    ExpressionOptimizerFunc getOptimizer() const final {
        //直接返回，不做任何优化
        return [](std::unique_ptr<MatchExpression> expression) { return expression; };
    }
};

//MatchExpressionParser::parse中调用
class EqualityMatchExpression : public ComparisonMatchExpression {
public:
    EqualityMatchExpression() : ComparisonMatchExpression(EQ) {}
    virtual std::unique_ptr<MatchExpression> shallowClone() const {
        std::unique_ptr<ComparisonMatchExpression> e = stdx::make_unique<EqualityMatchExpression>();
        invariantOK(e->init(path(), _rhs));
        if (getTag()) {
            e->setTag(getTag()->clone());
        }
        e->setCollator(_collator);
        return std::move(e);
    }
};

class LTEMatchExpression : public ComparisonMatchExpression {
public:
    LTEMatchExpression() : ComparisonMatchExpression(LTE) {}
    virtual std::unique_ptr<MatchExpression> shallowClone() const {
        std::unique_ptr<ComparisonMatchExpression> e = stdx::make_unique<LTEMatchExpression>();
        invariantOK(e->init(path(), _rhs));
        if (getTag()) {
            e->setTag(getTag()->clone());
        }
        e->setCollator(_collator);
        return std::move(e);
    }
};

class LTMatchExpression : public ComparisonMatchExpression {
public:
    LTMatchExpression() : ComparisonMatchExpression(LT) {}
    virtual std::unique_ptr<MatchExpression> shallowClone() const {
        std::unique_ptr<ComparisonMatchExpression> e = stdx::make_unique<LTMatchExpression>();
        invariantOK(e->init(path(), _rhs));
        if (getTag()) {
            e->setTag(getTag()->clone());
        }
        e->setCollator(_collator);
        return std::move(e);
    }
};

class GTMatchExpression : public ComparisonMatchExpression {
public:
    GTMatchExpression() : ComparisonMatchExpression(GT) {}
    virtual std::unique_ptr<MatchExpression> shallowClone() const {
        std::unique_ptr<ComparisonMatchExpression> e = stdx::make_unique<GTMatchExpression>();
        invariantOK(e->init(path(), _rhs));
        if (getTag()) {
            e->setTag(getTag()->clone());
        }
        e->setCollator(_collator);
        return std::move(e);
    }
};

class GTEMatchExpression : public ComparisonMatchExpression {
public:
    GTEMatchExpression() : ComparisonMatchExpression(GTE) {}
    virtual std::unique_ptr<MatchExpression> shallowClone() const {
        std::unique_ptr<ComparisonMatchExpression> e = stdx::make_unique<GTEMatchExpression>();
        invariantOK(e->init(path(), _rhs));
        if (getTag()) {
            e->setTag(getTag()->clone());
        }
        e->setCollator(_collator);
        return std::move(e);
    }
};

class RegexMatchExpression : public LeafMatchExpression {
public:
    RegexMatchExpression();
    ~RegexMatchExpression();

    Status init(StringData path, StringData regex, StringData options);
    Status init(StringData path, const BSONElement& e);

    virtual std::unique_ptr<MatchExpression> shallowClone() const {
        std::unique_ptr<RegexMatchExpression> e = stdx::make_unique<RegexMatchExpression>();
        invariantOK(e->init(path(), _regex, _flags));
        if (getTag()) {
            e->setTag(getTag()->clone());
        }
        return std::move(e);
    }

    bool matchesSingleElement(const BSONElement&, MatchDetails* details = nullptr) const final;

    virtual void debugString(StringBuilder& debug, int level) const;

    virtual void serialize(BSONObjBuilder* out) const;

    void serializeToBSONTypeRegex(BSONObjBuilder* out) const;

    void shortDebugString(StringBuilder& debug) const;

    virtual bool equivalent(const MatchExpression* other) const;

    const std::string& getString() const {
        return _regex;
    }
    const std::string& getFlags() const {
        return _flags;
    }

private:
    ExpressionOptimizerFunc getOptimizer() const final {
        return [](std::unique_ptr<MatchExpression> expression) { return expression; };
    }

    std::string _regex;
    std::string _flags;
    std::unique_ptr<pcrecpp::RE> _re;
};

class ModMatchExpression : public LeafMatchExpression {
public:
    ModMatchExpression() : LeafMatchExpression(MOD) {}

    Status init(StringData path, int divisor, int remainder);

    virtual std::unique_ptr<MatchExpression> shallowClone() const {
        std::unique_ptr<ModMatchExpression> m = stdx::make_unique<ModMatchExpression>();
        invariantOK(m->init(path(), _divisor, _remainder));
        if (getTag()) {
            m->setTag(getTag()->clone());
        }
        return std::move(m);
    }

    bool matchesSingleElement(const BSONElement&, MatchDetails* details = nullptr) const final;

    virtual void debugString(StringBuilder& debug, int level) const;

    virtual void serialize(BSONObjBuilder* out) const;

    virtual bool equivalent(const MatchExpression* other) const;

    int getDivisor() const {
        return _divisor;
    }
    int getRemainder() const {
        return _remainder;
    }

private:
    ExpressionOptimizerFunc getOptimizer() const final {
        return [](std::unique_ptr<MatchExpression> expression) { return expression; };
    }

    int _divisor;
    int _remainder;
};

class ExistsMatchExpression : public LeafMatchExpression {
public:
    ExistsMatchExpression() : LeafMatchExpression(EXISTS) {}

    Status init(StringData path);

    virtual std::unique_ptr<MatchExpression> shallowClone() const {
        std::unique_ptr<ExistsMatchExpression> e = stdx::make_unique<ExistsMatchExpression>();
        invariantOK(e->init(path()));
        if (getTag()) {
            e->setTag(getTag()->clone());
        }
        return std::move(e);
    }

    bool matchesSingleElement(const BSONElement&, MatchDetails* details = nullptr) const final;

    virtual void debugString(StringBuilder& debug, int level) const;

    virtual void serialize(BSONObjBuilder* out) const;

    virtual bool equivalent(const MatchExpression* other) const;

private:
    ExpressionOptimizerFunc getOptimizer() const final {
        return [](std::unique_ptr<MatchExpression> expression) { return expression; };
    }
};

/**
 * query operator: $in
 */
class InMatchExpression : public LeafMatchExpression {
public:
    InMatchExpression()
        : LeafMatchExpression(MATCH_IN),
          _eltCmp(BSONElementComparator::FieldNamesMode::kIgnore, _collator),
          _equalitySet(_eltCmp.makeBSONEltFlatSet(_originalEqualityVector)) {}

    Status init(StringData path);

    virtual std::unique_ptr<MatchExpression> shallowClone() const;

    bool matchesSingleElement(const BSONElement&, MatchDetails* details = nullptr) const final;

    virtual void debugString(StringBuilder& debug, int level) const;

    virtual void serialize(BSONObjBuilder* out) const;

    virtual bool equivalent(const MatchExpression* other) const;

    /**
     * 'collator' must outlive the InMatchExpression and any clones made of it.
     */
    virtual void _doSetCollator(const CollatorInterface* collator);

    Status setEqualities(std::vector<BSONElement> equalities);

    Status addRegex(std::unique_ptr<RegexMatchExpression> expr);

    const BSONEltFlatSet& getEqualities() const {
        return _equalitySet;
    }

    const std::vector<std::unique_ptr<RegexMatchExpression>>& getRegexes() const {
        return _regexes;
    }

    const CollatorInterface* getCollator() const {
        return _collator;
    }

    bool hasNull() const {
        return _hasNull;
    }

    bool hasEmptyArray() const {
        return _hasEmptyArray;
    }

private:
    ExpressionOptimizerFunc getOptimizer() const final;

    // Whether or not '_equalities' has a jstNULL element in it.
    bool _hasNull = false;

    // Whether or not '_equalities' has an empty array element in it.
    bool _hasEmptyArray = false;

    // Collator used to construct '_eltCmp';
    const CollatorInterface* _collator = nullptr;

    // Comparator used to compare elements. By default, simple binary comparison will be used.
    BSONElementComparator _eltCmp;

    // Original container of equality elements, including duplicates. Needed for re-computing
    // '_equalitySet' in case '_collator' changes after elements have been added.
    std::vector<BSONElement> _originalEqualityVector;

    // Set of equality elements associated with this expression. '_eltCmp' is used as a comparator
    // for this set.
    BSONEltFlatSet _equalitySet;

    // Container of regex elements this object owns.
    std::vector<std::unique_ptr<RegexMatchExpression>> _regexes;
};

/**
 * Bit test query operators include $bitsAllSet, $bitsAllClear, $bitsAnySet, and $bitsAnyClear.
 */
class BitTestMatchExpression : public LeafMatchExpression {
public:
    explicit BitTestMatchExpression(MatchType type) : LeafMatchExpression(type) {}
    virtual ~BitTestMatchExpression() {}

    /**
     * Initialize with either bit positions, a 64-bit numeric bitmask, or a char array
     * bitmask.
     */
    Status init(StringData path, std::vector<uint32_t> bitPositions);
    Status init(StringData path, uint64_t bitMask);
    Status init(StringData path, const char* bitMaskBinary, uint32_t bitMaskLen);

    bool matchesSingleElement(const BSONElement&, MatchDetails* details = nullptr) const final;

    virtual void debugString(StringBuilder& debug, int level) const;

    virtual void serialize(BSONObjBuilder* out) const;

    virtual bool equivalent(const MatchExpression* other) const;

    size_t numBitPositions() const {
        return _bitPositions.size();
    }

    const std::vector<uint32_t>& getBitPositions() const {
        return _bitPositions;
    }

protected:
    /**
     * Used to copy this match expression to another BitTestMatchExpression. Does not take
     * ownership.
     */
    void initClone(BitTestMatchExpression* clone) const {
        invariantOK(clone->init(path(), _bitPositions));
        if (getTag()) {
            clone->setTag(getTag()->clone());
        }
    }

private:
    ExpressionOptimizerFunc getOptimizer() const final {
        return [](std::unique_ptr<MatchExpression> expression) { return expression; };
    }

    /**
     * Performs bit test using bit positions on 'eValue' and returns whether or not the bit test
     * passes.
     */
    bool performBitTest(long long eValue) const;

    /**
     * Performs bit test using bit positions on 'eBinary' with length (in bytes) 'eBinaryLen' and
     * returns whether or not the bit test passes.
     */
    bool performBitTest(const char* eBinary, uint32_t eBinaryLen) const;

    /**
     * Helper function for performBitTest(...).
     *
     * needFurtherBitTests() determines if the result of a bit-test ('isBitSet') is enough
     * information to skip the rest of the bit tests.
     **/
    bool needFurtherBitTests(bool isBitSet) const;

    // Vector of bit positions to test, with bit position 0 being the least significant bit.
    // Used to perform bit tests against BinData.
    std::vector<uint32_t> _bitPositions;

    // Used to perform bit tests against numbers using a single bitwise operation.
    uint64_t _bitMask = 0;
};

class BitsAllSetMatchExpression : public BitTestMatchExpression {
public:
    BitsAllSetMatchExpression() : BitTestMatchExpression(BITS_ALL_SET) {}
    virtual std::unique_ptr<MatchExpression> shallowClone() const {
        std::unique_ptr<BitTestMatchExpression> bitTestMatchExpression =
            stdx::make_unique<BitsAllSetMatchExpression>();
        initClone(bitTestMatchExpression.get());
        return std::move(bitTestMatchExpression);
    }
};

class BitsAllClearMatchExpression : public BitTestMatchExpression {
public:
    BitsAllClearMatchExpression() : BitTestMatchExpression(BITS_ALL_CLEAR) {}
    virtual std::unique_ptr<MatchExpression> shallowClone() const {
        std::unique_ptr<BitTestMatchExpression> bitTestMatchExpression =
            stdx::make_unique<BitsAllClearMatchExpression>();
        initClone(bitTestMatchExpression.get());
        return std::move(bitTestMatchExpression);
    }
};

class BitsAnySetMatchExpression : public BitTestMatchExpression {
public:
    BitsAnySetMatchExpression() : BitTestMatchExpression(BITS_ANY_SET) {}
    virtual std::unique_ptr<MatchExpression> shallowClone() const {
        std::unique_ptr<BitTestMatchExpression> bitTestMatchExpression =
            stdx::make_unique<BitsAnySetMatchExpression>();
        initClone(bitTestMatchExpression.get());
        return std::move(bitTestMatchExpression);
    }
};

class BitsAnyClearMatchExpression : public BitTestMatchExpression {
public:
    BitsAnyClearMatchExpression() : BitTestMatchExpression(BITS_ANY_CLEAR) {}
    virtual std::unique_ptr<MatchExpression> shallowClone() const {
        std::unique_ptr<BitTestMatchExpression> bitTestMatchExpression =
            stdx::make_unique<BitsAnyClearMatchExpression>();
        initClone(bitTestMatchExpression.get());
        return std::move(bitTestMatchExpression);
    }
};

}  // namespace mongo
