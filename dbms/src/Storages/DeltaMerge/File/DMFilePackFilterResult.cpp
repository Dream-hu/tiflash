// Copyright 2024 PingCAP, Inc.
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

#include <Common/Stopwatch.h>
#include <Storages/DeltaMerge/File/DMFilePackFilter.h>
#include <Storages/DeltaMerge/File/DMFilePackFilterResult.h>

namespace DB::DM
{

UInt64 DMFilePackFilterResult::countUsePack() const
{
    return std::count_if(pack_res.begin(), pack_res.end(), [](RSResult res) { return res.isUse(); });
}

std::tuple<UInt64, UInt64, UInt64, UInt64> DMFilePackFilterResult::countPackRes() const
{
    UInt64 none_count = 0;
    UInt64 some_count = 0;
    UInt64 all_count = 0;
    UInt64 all_null_count = 0;
    for (auto res : pack_res)
    {
        if (res == RSResult::None || res == RSResult::NoneNull)
            ++none_count;
        else if (res == RSResult::Some || res == RSResult::SomeNull)
            ++some_count;
        else if (res == RSResult::All)
            ++all_count;
        else if (res == RSResult::AllNull)
            ++all_null_count;
    }
    return {none_count, some_count, all_count, all_null_count};
}

size_t DMFilePackFilterResult::modify(const DMFilePtr & dmfile, const BitmapFilterPtr & bitmap_filter, size_t offset)
{
    size_t skipped_pack = 0;
    const auto & pack_stats = dmfile->getPackStats();
    for (size_t pack_id = 0; pack_id < pack_stats.size(); ++pack_id)
    {
        if (pack_res[pack_id].isUse() && bitmap_filter->isAllNotMatch(offset, pack_stats[pack_id].rows))
        {
            pack_res[pack_id] = RSResult::None;
            ++skipped_pack;
        }
        offset += pack_stats[pack_id].rows;
    }
    return skipped_pack;
}

void DMFilePackFilterResult::tryLoadIndex(
    const DMFilePtr & dmfile,
    ColId col_id,
    const FileProviderPtr & file_provider,
    const ScanContextPtr & scan_context) const
{
    if (param.indexes.count(col_id))
        return;

    if (!dmfile->isColIndexExist(col_id))
        return;

    Stopwatch watch;
    DMFilePackFilter::loadIndex(
        param.indexes,
        dmfile,
        file_provider,
        index_cache,
        /*set_cache_if_miss=*/true,
        col_id,
        read_limiter,
        scan_context);
}

} // namespace DB::DM
