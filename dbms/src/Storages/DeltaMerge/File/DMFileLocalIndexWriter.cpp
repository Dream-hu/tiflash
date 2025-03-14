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

#include <Common/Exception.h>
#include <Common/FailPoint.h>
#include <Interpreters/SharedContexts/Disagg.h>
#include <Storages/DeltaMerge/DeltaMergeDefines.h>
#include <Storages/DeltaMerge/File/DMFile.h>
#include <Storages/DeltaMerge/File/DMFileBlockInputStream.h>
#include <Storages/DeltaMerge/File/DMFileLocalIndexWriter.h>
#include <Storages/DeltaMerge/File/DMFileV3IncrementWriter.h>
#include <Storages/DeltaMerge/Index/LocalIndexInfo.h>
#include <Storages/DeltaMerge/Index/LocalIndexWriter.h>
#include <Storages/DeltaMerge/ScanContext.h>
#include <Storages/PathPool.h>

#include <unordered_map>


namespace DB::ErrorCodes
{
extern const int ABORTED;
}

namespace DB::FailPoints
{
extern const char exception_build_local_index_for_file[];
} // namespace DB::FailPoints

namespace DB::DM
{

LocalIndexBuildInfo DMFileLocalIndexWriter::getLocalIndexBuildInfo(
    const LocalIndexInfosSnapshot & index_infos,
    const DMFiles & dm_files)
{
    assert(index_infos != nullptr);
    static constexpr double VECTOR_INDEX_SIZE_FACTOR = 1.2;

    // TODO(vector-index): Now we only generate the build info when new index is added.
    //    The built indexes will be dropped (lazily) after the segment instance is updated.
    //    We can support dropping the vector index more quickly later.
    LocalIndexBuildInfo build;
    build.indexes_to_build = std::make_shared<LocalIndexInfos>();
    build.dm_files.reserve(dm_files.size());
    for (const auto & dmfile : dm_files)
    {
        bool any_new_index_build = false;
        for (const auto & index : *index_infos)
        {
            auto col_id = index.column_id;
            const auto [state, data_bytes] = dmfile->getLocalIndexState(col_id, index.index_id);
            switch (state)
            {
            case DMFileMeta::LocalIndexState::NoNeed:
            case DMFileMeta::LocalIndexState::IndexBuilt:
                // The dmfile may be built before col_id is added, or has been built. Skip build indexes for it
                break;
            case DMFileMeta::LocalIndexState::IndexPending:
            {
                any_new_index_build = true;

                build.indexes_to_build->emplace_back(index);
                build.estimated_memory_bytes += data_bytes * VECTOR_INDEX_SIZE_FACTOR;
                break;
            }
            }
        }

        if (any_new_index_build)
            build.dm_files.emplace_back(dmfile);
    }

    build.dm_files.shrink_to_fit();
    return build;
}

size_t DMFileLocalIndexWriter::buildIndexForFile(const DMFilePtr & dm_file_mutable, ProceedCheckFn should_proceed) const
{
    DMFileV3IncrementWriter::Options iw_options{
        .dm_file = dm_file_mutable,
        .file_provider = options.dm_context.global_context.getFileProvider(),
        .write_limiter = options.dm_context.global_context.getWriteLimiter(),
        .path_pool = options.path_pool,
        .disagg_ctx = options.dm_context.global_context.getSharedContextDisagg(),
    };
    auto iw = DMFileV3IncrementWriter::create(iw_options);

    const auto column_defines = dm_file_mutable->getColumnDefines();
    const auto del_cd_iter = std::find_if(column_defines.cbegin(), column_defines.cend(), [](const ColumnDefine & cd) {
        return cd.id == MutSup::delmark_col_id;
    });
    RUNTIME_CHECK_MSG(
        del_cd_iter != column_defines.cend(),
        "Cannot find del_mark column, file={}",
        dm_file_mutable->path());

    // read_columns are: DEL_MARK, COL_A, COL_B, ...
    // index_builders are: COL_A -> {idx_M, idx_N}, COL_B -> {idx_O}, ...

    ColumnDefines read_columns{*del_cd_iter};
    read_columns.reserve(options.index_infos->size() + 1);

    struct IndexToBuild
    {
        LocalIndexInfo info;
        String index_file_path; // For write out
        String index_file_name; // For meta include
        LocalIndexWriterOnDiskPtr index_writer;
    };

    std::unordered_map<ColId, std::vector<IndexToBuild>> index_builders;

    for (const auto & index_info : *options.index_infos)
    {
        index_builders[index_info.column_id].emplace_back(IndexToBuild{
            .info = index_info,
            .index_file_path = "",
            .index_file_name = "",
            .index_writer = {},
        });
    }

    for (auto & [col_id, indexes] : index_builders)
    {
        const auto cd_iter
            = std::find_if(column_defines.cbegin(), column_defines.cend(), [col_id = col_id](const auto & cd) {
                  return cd.id == col_id;
              });
        RUNTIME_CHECK_MSG(
            cd_iter != column_defines.cend(),
            "Cannot find column_id={} in file={}",
            col_id,
            dm_file_mutable->path());

        for (auto & index : indexes)
        {
            const IndexID index_id = index.info.index_id;
            index.index_file_name = index_id > 0 ? dm_file_mutable->localIndexFileName(index_id, index.info.kind)
                                                 : colIndexFileName(DMFile::getFileNameBase(col_id));
            index.index_file_path = iw->localPath() + "/" + index.index_file_name;

            // Index already built. We don't allow. The caller should filter away,
            RUNTIME_CHECK(
                !dm_file_mutable->isLocalIndexExist(index.info.column_id, index.info.index_id),
                index.info.column_id,
                index.info.index_id);

            index.index_writer = LocalIndexWriter::createOnDisk(index.index_file_path, index.info);
        }
        read_columns.push_back(*cd_iter);
    }

    if (read_columns.size() == 1 || index_builders.empty())
    {
        // No index to build.
        return 0;
    }

    DMFileBlockInputStreamBuilder read_stream_builder(options.dm_context.global_context);
    auto scan_context = std::make_shared<ScanContext>();

    // Note: We use range::newAll to build index for all data in dmfile, because the index is file-level.
    auto read_stream = read_stream_builder.build(
        dm_file_mutable,
        read_columns,
        {RowKeyRange::newAll(options.dm_context.is_common_handle, options.dm_context.rowkey_column_size)},
        scan_context);

    // Read all blocks and build index
    const size_t num_cols = read_columns.size();
    while (true)
    {
        if (!should_proceed())
            throw Exception(ErrorCodes::ABORTED, "Index build is interrupted");

        auto block = read_stream->read();
        if (!block)
            break;

        RUNTIME_CHECK(block.columns() == num_cols);
        RUNTIME_CHECK(block.getByPosition(0).column_id == MutSup::delmark_col_id);

        auto del_mark_col = block.safeGetByPosition(0).column;
        RUNTIME_CHECK(del_mark_col != nullptr);
        const auto * del_mark = static_cast<const ColumnVector<UInt8> *>(del_mark_col.get());
        RUNTIME_CHECK(del_mark != nullptr);

        for (size_t col_idx = 1; col_idx < num_cols; ++col_idx)
        {
            const auto & col_with_type_and_name = block.safeGetByPosition(col_idx);
            RUNTIME_CHECK(col_with_type_and_name.column_id == read_columns[col_idx].id);
            const auto & col = col_with_type_and_name.column;
            for (const auto & index : index_builders[read_columns[col_idx].id])
            {
                RUNTIME_CHECK(index.index_writer);
                index.index_writer->addBlock(*col, del_mark, should_proceed);
            }
        }
    }

    FAIL_POINT_TRIGGER_EXCEPTION(FailPoints::exception_build_local_index_for_file);

    // Write down the index
    size_t total_built_index_bytes = 0;
    std::unordered_map<ColId, std::vector<dtpb::DMFileIndexInfo>> new_indexes_on_cols;
    for (size_t col_idx = 1; col_idx < num_cols; ++col_idx)
    {
        const auto & cd = read_columns[col_idx];

        std::vector<dtpb::DMFileIndexInfo> new_indexes;

        for (const auto & index : index_builders[cd.id])
        {
            dtpb::DMFileIndexInfo pb_dmfile_idx;
            auto idx_info = index.index_writer->finalize();
            pb_dmfile_idx.mutable_index_props()->Swap(&idx_info);
            total_built_index_bytes += pb_dmfile_idx.index_props().file_size();
            new_indexes.emplace_back(std::move(pb_dmfile_idx));
            iw->include(index.index_file_name);
        }

        if (!new_indexes.empty())
            new_indexes_on_cols.emplace(cd.id, std::move(new_indexes));
    }

    dm_file_mutable->meta->bumpMetaVersion(DMFileMetaChangeset{new_indexes_on_cols});
    iw->finalize(); // Note: There may be S3 uploads here.
    return total_built_index_bytes;
}

DMFiles DMFileLocalIndexWriter::build(ProceedCheckFn should_proceed) const
{
    RUNTIME_CHECK(!built);
    // Create a clone of existing DMFile instances by using DMFile::restore,
    // because later we will mutate some fields and persist these mutations.
    DMFiles cloned_dm_files{};
    cloned_dm_files.reserve(options.dm_files.size());

    auto delegate = options.path_pool->getStableDiskDelegator();
    for (const auto & dm_file : options.dm_files)
    {
        if (const auto disagg_ctx = options.dm_context.global_context.getSharedContextDisagg();
            !disagg_ctx || !disagg_ctx->remote_data_store)
            RUNTIME_CHECK(dm_file->parentPath() == delegate.getDTFilePath(dm_file->fileId()));

        auto new_dmfile = DMFile::restore(
            options.dm_context.global_context.getFileProvider(),
            dm_file->fileId(),
            dm_file->pageId(),
            dm_file->parentPath(),
            DMFileMeta::ReadMode::all(),
            dm_file->metaVersion());
        cloned_dm_files.push_back(new_dmfile);
    }

    for (const auto & cloned_dmfile : cloned_dm_files)
    {
        auto index_bytes = buildIndexForFile(cloned_dmfile, should_proceed);
        if (auto data_store = options.dm_context.global_context.getSharedContextDisagg()->remote_data_store;
            !data_store)
        {
            // After building index, add the index size to the file size.
            auto res = options.path_pool->getStableDiskDelegator().updateDTFileSize(
                cloned_dmfile->fileId(),
                cloned_dmfile->getBytesOnDisk() + index_bytes);
            RUNTIME_CHECK_MSG(res, "update dt file size failed, path={}", cloned_dmfile->path());
        }
    }

    built = true;
    return cloned_dm_files;
}

} // namespace DB::DM
