// Copyright 2023 PingCAP, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <Columns/ColumnsNumber.h>
#include <Columns/IColumn.h>

#include <cstring>

namespace DB
{
static_assert(std::is_same_v<NullMap, ColumnUInt8::Container>);

/// Class that specifies nullable columns. A nullable column represents
/// a column, which may have any type, provided with the possibility of
/// storing NULL values. For this purpose, a ColumNullable object stores
/// an ordinary column along with a special column, namely a byte map,
/// whose type is ColumnUInt8. The latter column indicates whether the
/// value of a given row is a NULL or not. Such a design is preferred
/// over a bitmap because columns are usually stored on disk as compressed
/// files. In this regard, using a bitmap instead of a byte map would
/// greatly complicate the implementation with little to no benefits.
class ColumnNullable final : public COWPtrHelper<IColumn, ColumnNullable>
{
private:
    friend class COWPtrHelper<IColumn, ColumnNullable>;

    ColumnNullable(MutableColumnPtr && nested_column_, MutableColumnPtr && null_map_);
    ColumnNullable(const ColumnNullable &) = default;

public:
    /** Create immutable column using immutable arguments. This arguments may be shared with other columns.
      * Use IColumn::mutate in order to make mutable column and mutate shared nested columns.
      */
    using Base = COWPtrHelper<IColumn, ColumnNullable>;
    static Ptr create(const ColumnPtr & nested_column_, const ColumnPtr & null_map_)
    {
        return ColumnNullable::create(nested_column_->assumeMutable(), null_map_->assumeMutable());
    }

    template <typename... Args, typename = typename std::enable_if<IsMutableColumns<Args...>::value>::type>
    static MutablePtr create(Args &&... args)
    {
        return Base::create(std::forward<Args>(args)...);
    }

    const char * getFamilyName() const override { return "Nullable"; }
    std::string getName() const override { return "Nullable(" + nested_column->getName() + ")"; }
    MutableColumnPtr cloneResized(size_t size) const override;
    size_t size() const override { return static_cast<const ColumnUInt8 &>(*null_map).size(); }
    bool isNullAt(size_t n) const override
    {
        return DB::isNullAt(static_cast<const ColumnUInt8 &>(*null_map).getData(), n);
    }
    Field operator[](size_t n) const override;
    void get(size_t n, Field & res) const override;
    UInt64 get64(size_t n) const override { return nested_column->get64(n); }
    StringRef getDataAt(size_t) const override;
    /// Will insert null value if pos=nullptr
    void insertData(const char * pos, size_t length) override;
    bool decodeTiDBRowV2Datum(size_t cursor, const String & raw_value, size_t length, bool force_decode) override;
    void insertFromDatumData(const char *, size_t) override;
    StringRef serializeValueIntoArena(
        size_t n,
        Arena & arena,
        char const *& begin,
        const TiDB::TiDBCollatorPtr &,
        String &) const override;
    const char * deserializeAndInsertFromArena(const char * pos, const TiDB::TiDBCollatorPtr &) override;

    size_t serializeByteSize() const override;

    void countSerializeByteSize(PaddedPODArray<size_t> & byte_size) const override;
    void countSerializeByteSizeForCmp(
        PaddedPODArray<size_t> & byte_size,
        const NullMap * nullmap,
        const TiDB::TiDBCollatorPtr & collator) const override;

    void countSerializeByteSizeForColumnArray(
        PaddedPODArray<size_t> & byte_size,
        const IColumn::Offsets & array_offsets) const override;
    void countSerializeByteSizeForCmpColumnArray(
        PaddedPODArray<size_t> & byte_size,
        const IColumn::Offsets & array_offsets,
        const NullMap * nullmap,
        const TiDB::TiDBCollatorPtr & collator) const override;

    void serializeToPos(PaddedPODArray<char *> & pos, size_t start, size_t length, bool has_null) const override;
    void serializeToPosForCmp(
        PaddedPODArray<char *> & pos,
        size_t start,
        size_t length,
        bool has_null,
        const NullMap * nullmap,
        const TiDB::TiDBCollatorPtr & collator,
        String * sort_key_container) const override;

    void serializeToPosForColumnArray(
        PaddedPODArray<char *> & pos,
        size_t start,
        size_t length,
        bool has_null,
        const IColumn::Offsets & array_offsets) const override;
    void serializeToPosForCmpColumnArray(
        PaddedPODArray<char *> & pos,
        size_t start,
        size_t length,
        bool has_null,
        const NullMap * nullmap,
        const IColumn::Offsets & array_offsets,
        const TiDB::TiDBCollatorPtr & collator,
        String * sort_key_container) const override;

    void deserializeAndInsertFromPos(PaddedPODArray<char *> & pos, bool use_nt_align_buffer) override;

    void deserializeAndInsertFromPosForColumnArray(
        PaddedPODArray<char *> & pos,
        const IColumn::Offsets & array_offsets,
        bool use_nt_align_buffer) override;

    void flushNTAlignBuffer() override;

    void deserializeAndAdvancePos(PaddedPODArray<char *> & pos) const override;

    void deserializeAndAdvancePosForColumnArray(PaddedPODArray<char *> & pos, const IColumn::Offsets & array_offsets)
        const override;

    void insertRangeFrom(const IColumn & src, size_t start, size_t length) override;

    void insert(const Field & x) override;
    void insertFrom(const IColumn & src, size_t n) override;
    void insertManyFrom(const IColumn & src, size_t n, size_t length) override;
    void insertSelectiveRangeFrom(const IColumn & src, const Offsets & selective_offsets, size_t start, size_t length)
        override;

    void insertDefault() override
    {
        getNestedColumn().insertDefault();
        getNullMapData().push_back(1);
    }

    void insertManyDefaults(size_t length) override
    {
        getNestedColumn().insertManyDefaults(length);
        auto & map = getNullMapData();
        size_t old_size = map.size();
        map.resize(old_size + length);
        memset(map.data() + old_size, 1, length);
    }

    void popBack(size_t n) override;
    ColumnPtr filter(const Filter & filt, ssize_t result_size_hint) const override;
    ColumnPtr permute(const Permutation & perm, size_t limit) const override;
    std::tuple<bool, int> compareAtCheckNull(size_t n, size_t m, const ColumnNullable & rhs, int null_direction_hint)
        const;
    int compareAt(size_t n, size_t m, const IColumn & rhs_, int null_direction_hint) const override;
    int compareAt(
        size_t n,
        size_t m,
        const IColumn & rhs_,
        int null_direction_hint,
        const TiDB::ITiDBCollator & collator) const override;
    void getPermutation(bool reverse, size_t limit, int null_direction_hint, Permutation & res) const override;
    void getPermutation(
        const TiDB::ITiDBCollator & collator,
        bool reverse,
        size_t limit,
        int null_direction_hint,
        Permutation & res) const override;
    void adjustPermutationWithNullDirection(bool reverse, size_t limit, int null_direction_hint, Permutation & res)
        const;
    void reserve(size_t n) override;
    void reserveAlign(size_t n, size_t alignment) override;
    void reserveWithTotalMemoryHint(size_t n, Int64 total_memory_hint) override;
    void reserveAlignWithTotalMemoryHint(size_t n, Int64 total_memory_hint, size_t alignment) override;
    size_t byteSize() const override;
    size_t byteSize(size_t offset, size_t limit) const override;
    size_t allocatedBytes() const override;
    ColumnPtr replicateRange(size_t start_row, size_t end_row, const IColumn::Offsets & replicate_offsets)
        const override;
    void updateHashWithValue(size_t n, SipHash & hash, const TiDB::TiDBCollatorPtr &, String &) const override;
    void updateHashWithValues(IColumn::HashValues & hash_values, const TiDB::TiDBCollatorPtr &, String &)
        const override;
    void updateWeakHash32(WeakHash32 & hash, const TiDB::TiDBCollatorPtr &, String &) const override;
    void updateWeakHash32(WeakHash32 & hash, const TiDB::TiDBCollatorPtr &, String &, const BlockSelective & selective)
        const override;
    void getExtremes(Field & min, Field & max) const override;

    MutableColumns scatter(ColumnIndex num_columns, const Selector & selector) const override
    {
        return scatterImpl<ColumnNullable>(num_columns, selector);
    }

    MutableColumns scatter(ColumnIndex num_columns, const Selector & selector, const BlockSelective & selective)
        const override
    {
        return scatterImpl<ColumnNullable>(num_columns, selector, selective);
    }

    void scatterTo(ScatterColumns & columns, const Selector & selector) const override
    {
        scatterToImpl<ColumnNullable>(columns, selector);
    }

    void scatterTo(ScatterColumns & columns, const Selector & selector, const BlockSelective & selective) const override
    {
        scatterToImpl<ColumnNullable>(columns, selector, selective);
    }

    void gather(ColumnGathererStream & gatherer_stream) override;

    void forEachSubcolumn(ColumnCallback callback) override
    {
        callback(nested_column);
        callback(null_map);
    }

    bool isColumnNullable() const override { return true; }
    bool isFixedAndContiguous() const override { return false; }
    bool valuesHaveFixedSize() const override { return nested_column->valuesHaveFixedSize(); }
    size_t sizeOfValueIfFixed() const override
    {
        return null_map->sizeOfValueIfFixed() + nested_column->sizeOfValueIfFixed();
    }
    bool onlyNull() const override { return nested_column->isDummy(); }


    /// Return the column that represents values.
    IColumn & getNestedColumn() { return nested_column->assumeMutableRef(); }
    const IColumn & getNestedColumn() const { return *nested_column; }

    const ColumnPtr & getNestedColumnPtr() const { return nested_column; }

    /// Return the column that represents the byte map.
    //ColumnPtr & getNullMapColumnPtr() { return null_map; }
    const ColumnPtr & getNullMapColumnPtr() const { return null_map; }

    ColumnUInt8 & getNullMapColumn() { return static_cast<ColumnUInt8 &>(null_map->assumeMutableRef()); }
    const ColumnUInt8 & getNullMapColumn() const { return static_cast<const ColumnUInt8 &>(*null_map); }

    NullMap & getNullMapData() { return getNullMapColumn().getData(); }
    const NullMap & getNullMapData() const { return getNullMapColumn().getData(); }

    /// Apply the null byte map of a specified nullable column onto the
    /// null byte map of the current column by performing an element-wise OR
    /// between both byte maps. This method is used to determine the null byte
    /// map of the result column of a function taking one or more nullable
    /// columns.
    void applyNullMap(const ColumnNullable & other);
    void applyNullMap(const ColumnUInt8 & map);
    void applyNegatedNullMap(const ColumnUInt8 & map);

    /// Check that size of null map equals to size of nested column.
    void checkConsistency() const;

private:
    ColumnPtr nested_column;
    ColumnPtr null_map;

    template <bool negative>
    void applyNullMapImpl(const ColumnUInt8 & map);

    template <bool selective_block>
    void updateWeakHash32Impl(
        WeakHash32 & hash,
        const TiDB::TiDBCollatorPtr & collator,
        String & sort_key_container,
        const BlockSelective & selective) const;
};


ColumnPtr makeNullable(const ColumnPtr & column);
std::tuple<const IColumn *, const NullMap *> removeNullable(const IColumn * column);

} // namespace DB
