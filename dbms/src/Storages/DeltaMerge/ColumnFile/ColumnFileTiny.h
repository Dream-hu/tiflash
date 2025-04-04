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

#include <IO/FileProvider/FileProvider_fwd.h>
#include <Storages/DeltaMerge/ColumnFile/ColumnFilePersisted.h>
#include <Storages/DeltaMerge/ColumnFile/ColumnFileSchema.h>
#include <Storages/DeltaMerge/DMContext_fwd.h>
#include <Storages/DeltaMerge/Remote/Serializer_fwd.h>
#include <Storages/DeltaMerge/dtpb/column_file.pb.h>
#include <Storages/Page/PageStorage_fwd.h>

namespace DB::DM
{

class ColumnFileTiny;
using ColumnFileTinyPtr = std::shared_ptr<ColumnFileTiny>;

/// A column file which data is stored in PageStorage.
/// It may be created in two ways:
///   1. created directly when writing to storage if the data is large enough
///   2. created when flushed `ColumnFileInMemory` to disk
class ColumnFileTiny : public ColumnFilePersisted
{
public:
    friend class ColumnFileTinyReader;
    friend class ColumnFileTinyLocalIndexWriter;
    friend class InvertedIndexReaderFromColumnFileTiny;
    friend struct Remote::Serializer;

    using IndexInfos = std::vector<dtpb::ColumnFileIndexInfo>;
    using IndexInfosPtr = std::shared_ptr<const IndexInfos>;

private:
    ColumnFileSchemaPtr schema;

    UInt64 rows = 0;
    UInt64 bytes = 0;

    /// The id of data page which stores the data of this pack.
    const PageIdU64 data_page_id;

    /// HACK: Currently this field is only available when ColumnFileTiny is restored from remote proto.
    /// It is not available when ColumnFileTiny is constructed or restored locally.
    /// Maybe we should just drop this field, and store the data_page_size in somewhere else.
    UInt64 data_page_size = 0;

    /// The index information of this file.
    const IndexInfosPtr index_infos;

    /// The id of the keyspace which this ColumnFileTiny belongs to.
    const KeyspaceID keyspace_id;
    /// The global file_provider
    const FileProviderPtr file_provider;

private:
    const DataTypePtr & getDataType(ColId column_id) const { return schema->getDataType(column_id); }

public:
    ColumnFileTiny(
        const ColumnFileSchemaPtr & schema_,
        UInt64 rows_,
        UInt64 bytes_,
        PageIdU64 data_page_id_,
        const DMContext & dm_context,
        const IndexInfosPtr & index_infos_ = nullptr);

    ColumnFileTiny(
        const ColumnFileSchemaPtr & schema_,
        UInt64 rows_,
        UInt64 bytes_,
        PageIdU64 data_page_id_,
        KeyspaceID keyspace_id_,
        const FileProviderPtr & file_provider_,
        const IndexInfosPtr & index_infos_);

    Type getType() const override { return Type::TINY_FILE; }

    size_t getRows() const override { return rows; }
    size_t getBytes() const override { return bytes; }

    IndexInfosPtr getIndexInfos() const { return index_infos; }

    bool hasIndex(Int64 index_id) const { return findIndexInfo(index_id) != nullptr; }

    const dtpb::ColumnFileIndexInfo * findIndexInfo(Int64 index_id) const
    {
        if (!index_infos)
            return nullptr;
        const auto it = std::find_if( //
            index_infos->cbegin(),
            index_infos->cend(),
            [index_id](const auto & info) { return info.index_props().index_id() == index_id; });
        if (it == index_infos->cend())
            return nullptr;
        return &*it;
    }

    ColumnFileSchemaPtr getSchema() const { return schema; }

    ColumnFileTinyPtr cloneWith(PageIdU64 new_data_page_id)
    {
        return std::make_shared<ColumnFileTiny>(
            schema,
            rows,
            bytes,
            new_data_page_id,
            keyspace_id,
            file_provider,
            index_infos);
    }

    ColumnFileTinyPtr cloneWith(PageIdU64 new_data_page_id, const IndexInfosPtr & new_index_infos) const
    {
        return std::make_shared<ColumnFileTiny>(
            schema,
            rows,
            bytes,
            new_data_page_id,
            keyspace_id,
            file_provider,
            new_index_infos);
    }

    ColumnFileReaderPtr getReader(
        const DMContext &,
        const IColumnFileDataProviderPtr & data_provider,
        const ColumnDefinesPtr & col_defs,
        ReadTag) const override;

    void removeData(WriteBatches & wbs) const override;

    void serializeMetadata(WriteBuffer & buf, bool save_schema) const override;

    void serializeMetadata(dtpb::ColumnFilePersisted * cf_pb, bool save_schema) const override;

    PageIdU64 getDataPageId() const { return data_page_id; }

    /// WARNING: DO NOT USE THIS MEMBER FUNCTION UNLESS YOU KNOW WHAT YOU ARE DOING.
    /// This function will be refined and dropped soon.
    UInt64 getDataPageSize() const { return data_page_size; }

    Block readBlockForMinorCompaction(const PageReader & page_reader) const;

    static std::shared_ptr<ColumnFileSchema> getSchema(
        const DMContext & dm_context,
        BlockPtr schema_block,
        ColumnFileSchemaPtr & last_schema);

    static ColumnFileTinyPtr writeColumnFile(
        const DMContext & dm_context,
        const Block & block,
        size_t offset,
        size_t limit,
        WriteBatches & wbs);

    static PageIdU64 writeColumnFileData(
        const DMContext & dm_context,
        const Block & block,
        size_t offset,
        size_t limit,
        WriteBatches & wbs);

    static ColumnFilePersistedPtr deserializeMetadata(
        const DMContext & dm_context,
        ReadBuffer & buf,
        ColumnFileSchemaPtr & last_schema);

    static ColumnFilePersistedPtr deserializeMetadata(
        const DMContext & dm_context,
        const dtpb::ColumnFileTiny & cf_pb,
        ColumnFileSchemaPtr & last_schema);

    static ColumnFilePersistedPtr restoreFromCheckpoint(
        const LoggerPtr & parent_log,
        const DMContext & dm_context,
        UniversalPageStoragePtr temp_ps,
        WriteBatches & wbs,
        BlockPtr schema,
        PageIdU64 data_page_id,
        size_t rows,
        size_t bytes,
        IndexInfosPtr index_infos);
    static std::tuple<ColumnFilePersistedPtr, BlockPtr> createFromCheckpoint(
        const LoggerPtr & parent_log,
        const DMContext & dm_context,
        ReadBuffer & buf,
        UniversalPageStoragePtr temp_ps,
        const BlockPtr & last_schema,
        WriteBatches & wbs);
    static std::tuple<ColumnFilePersistedPtr, BlockPtr> createFromCheckpoint(
        const LoggerPtr & parent_log,
        const DMContext & dm_context,
        const dtpb::ColumnFileTiny & cf_pb,
        UniversalPageStoragePtr temp_ps,
        const BlockPtr & last_schema,
        WriteBatches & wbs);

    bool mayBeFlushedFrom(ColumnFile * from_file) const override
    {
        // The current ColumnFileTiny may come from a ColumnFileInMemory (which contains data in memory)
        // or ColumnFileTiny (which contains data in PageStorage).

        if (const auto * other_tiny = from_file->tryToTinyFile(); other_tiny)
            return data_page_id == other_tiny->data_page_id;
        else if (const auto * other_in_memory = from_file->tryToInMemoryFile(); other_in_memory)
            // For ColumnFileInMemory, we just do a rough check, instead of checking byte by byte, which
            // is too expensive.
            return bytes == from_file->getBytes() && rows == from_file->getRows();
        else
            return false;
    }

    String toString() const override
    {
        return fmt::format(
            "{{tiny_file,rows:{},bytes:{},data_page_id:{},schema:{}}}",
            rows,
            bytes,
            data_page_id,
            (schema ? schema->toString() : "none"));
    }
};

} // namespace DB::DM
