#pragma once

#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <vector>

#include <Core/Names.h>
#include <Storages/Transaction/RegionDataRead.h>
#include <Storages/Transaction/RegionException.h>
#include <Storages/Transaction/TiKVHandle.h>
#include <common/logger_useful.h>

namespace TiDB
{
struct TableInfo;
};

namespace DB
{

class Region;
using RegionPtr = std::shared_ptr<Region>;
struct ColumnsDescription;
class Context;
class IStorage;
using StoragePtr = std::shared_ptr<IStorage>;
class TMTContext;
class IBlockInputStream;
using BlockInputStreamPtr = std::shared_ptr<IBlockInputStream>;
class Block;
// for debug
struct MockTiDBTable;
class RegionRangeKeys;

class RegionTable : private boost::noncopyable
{
public:
    struct InternalRegion
    {
        InternalRegion(const RegionID region_id_, const HandleRange<HandleID> & range_in_table_ = {0, 0})
            : region_id(region_id_), range_in_table(range_in_table_)
        {}

        RegionID region_id;
        HandleRange<HandleID> range_in_table;
        bool pause_flush = false;
        Int64 cache_bytes = 0;
        Timepoint last_flush_time = Clock::now();
    };

    using InternalRegions = std::unordered_map<RegionID, InternalRegion>;

    struct Table : boost::noncopyable
    {
        Table(const TableID table_id_) : table_id(table_id_) {}
        TableID table_id;
        InternalRegions regions;
    };

    using TableMap = std::unordered_map<TableID, Table>;
    using RegionInfoMap = std::unordered_map<RegionID, TableID>;

    struct TableOptimizeChecker
    {
        std::mutex mutex;
        bool is_checking = false;
        double threshold = 1.0;
        Timepoint last_check_time = Clock::now();
    };

    using DirtyRegions = std::unordered_set<RegionID>;
    using TableToOptimize = std::unordered_set<TableID>;

    struct FlushThresholds
    {
        using FlushThresholdsData = std::vector<std::pair<Int64, Seconds>>;

        FlushThresholds(FlushThresholdsData && data_) : data(std::make_shared<FlushThresholdsData>(std::move(data_))) {}

        void setFlushThresholds(const FlushThresholdsData & flush_thresholds_)
        {
            auto flush_thresholds = std::make_shared<FlushThresholdsData>(flush_thresholds_);
            {
                std::lock_guard<std::mutex> lock(mutex);
                data = std::move(flush_thresholds);
            }
        }

        auto getData() const
        {
            std::lock_guard<std::mutex> lock(mutex);
            return data;
        }

    private:
        std::shared_ptr<const FlushThresholdsData> data;
        mutable std::mutex mutex;
    };

    RegionTable(Context & context_);
    void restore();

    void setFlushThresholds(const FlushThresholds::FlushThresholdsData & flush_thresholds_);

    void updateRegion(const Region & region);

    /// This functional only shrink the table range of this region_id
    void shrinkRegionRange(const Region & region);

    void removeRegion(const RegionID region_id);

    TableID popOneTableToOptimize();

    bool tryFlushRegions();
    RegionDataReadInfoList tryFlushRegion(RegionID region_id, bool try_persist = false);
    RegionDataReadInfoList tryFlushRegion(const RegionPtr & region, bool try_persist);

    void waitTillRegionFlushed(RegionID region_id);

    void handleInternalRegionsByTable(const TableID table_id, std::function<void(const InternalRegions &)> && callback) const;
    std::vector<std::pair<RegionID, RegionPtr>> getRegionsByTable(const TableID table_id) const;

    /// Write the data of the given region into the table with the given table ID, fill the data list for outer to remove.
    /// Will trigger schema sync on read error for only once,
    /// assuming that newer schema can always apply to older data by setting force_decode to true in readRegionBlock.
    /// Note that table schema must be keep unchanged throughout the process of read then write, we take good care of the lock.
    static void writeBlockByRegion(Context & context, RegionPtr region, RegionDataReadInfoList & data_list_to_remove, Logger * log);

    /// Read the data of the given region into block, take good care of learner read and locks.
    /// Assuming that the schema has been properly synced by outer, i.e. being new enough to decode data before start_ts,
    /// we directly ask readRegionBlock to perform a read with the given start_ts and force_decode being true.
    static std::tuple<Block, RegionException::RegionReadStatus> readBlockByRegion(const TiDB::TableInfo & table_info,
        const ColumnsDescription & columns,
        const Names & column_names_to_read,
        const RegionPtr & region,
        RegionVersion region_version,
        RegionVersion conf_version,
        bool resolve_locks,
        Timestamp start_ts,
        DB::HandleRange<HandleID> & handle_range);

    void checkTableOptimize();
    void checkTableOptimize(TableID, const double);
    void setTableCheckerThreshold(double);

    /// extend range for possible InternalRegion or add one.
    void extendRegionRange(const RegionID region_id, const RegionRangeKeys & region_range_keys);

private:
    friend class MockTiDB;
    friend class StorageMergeTree;
    friend class StorageDeltaMerge;
    friend class StorageDebugging;

    Table & getOrCreateTable(const TableID table_id);
    void removeTable(TableID table_id);
    InternalRegion & insertRegion(Table & table, const Region & region);
    InternalRegion & getOrInsertRegion(const Region & region);
    InternalRegion & insertRegion(Table & table, const RegionRangeKeys & region_range_keys, const RegionID region_id);
    InternalRegion & doGetInternalRegion(TableID table_id, RegionID region_id);

    bool shouldFlush(const InternalRegion & region) const;
    RegionID pickRegionToFlush();
    RegionDataReadInfoList flushRegion(const RegionPtr & region, bool try_persist) const;

    void incrDirtyFlag(RegionID region_id);
    void clearDirtyFlag(RegionID region_id);
    DirtyRegions::iterator clearDirtyFlag(const RegionTable::DirtyRegions::iterator & region_iter, std::lock_guard<std::mutex> &);

private:
    TableMap tables;
    RegionInfoMap regions;
    DirtyRegions dirty_regions;
    TableToOptimize table_to_optimize;

    std::mutex dirty_regions_mutex;
    std::condition_variable dirty_regions_cv;

    FlushThresholds flush_thresholds;

    Context * const context;

    mutable std::mutex mutex;

    mutable TableOptimizeChecker table_checker;

    Logger * log;
};

using RegionPartitionPtr = std::shared_ptr<RegionTable>;

} // namespace DB