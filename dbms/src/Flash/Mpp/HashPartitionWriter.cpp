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

#include <Common/TiFlashException.h>
#include <Flash/Coprocessor/CHBlockChunkCodec.h>
#include <Flash/Coprocessor/CHBlockChunkCodecV1.h>
#include <Flash/Coprocessor/DAGContext.h>
#include <Flash/Mpp/HashBaseWriterHelper.h>
#include <Flash/Mpp/HashPartitionWriter.h>
#include <Flash/Mpp/MPPTunnelSetWriter.h>
#include <TiDB/Decode/TypeMapping.h>

namespace DB
{
constexpr ssize_t MAX_BATCH_SEND_MIN_LIMIT_MEM_SIZE
    = 1024 * 1024 * 64; // 64MB: 8192 Rows * 256 Byte/row * 32 partitions
const char * HashPartitionWriterLabels[] = {"HashPartitionWriter", "HashPartitionWriter-V1"};

template <class ExchangeWriterPtr>
HashPartitionWriter<ExchangeWriterPtr>::HashPartitionWriter(
    ExchangeWriterPtr writer_,
    std::vector<Int64> partition_col_ids_,
    TiDB::TiDBCollators collators_,
    Int64 batch_send_min_limit_,
    DAGContext & dag_context_,
    MPPDataPacketVersion data_codec_version_,
    tipb::CompressionMode compression_mode_)
    : DAGResponseWriter(/*records_per_chunk=*/-1, dag_context_)
    , batch_send_min_limit(batch_send_min_limit_)
    , writer(writer_)
    , partition_col_ids(std::move(partition_col_ids_))
    , collators(std::move(collators_))
    , data_codec_version(data_codec_version_)
    , compression_method(ToInternalCompressionMethod(compression_mode_))
{
    rows_in_blocks = 0;
    partition_num = writer_->getPartitionNum();
    RUNTIME_CHECK(partition_num > 0);
    RUNTIME_CHECK(dag_context.encode_type == tipb::EncodeType::TypeCHBlock);

    switch (data_codec_version)
    {
    case MPPDataPacketV0:
        if (batch_send_min_limit <= 0)
            batch_send_min_limit = 1;
        break;
    case MPPDataPacketV1:
    default:
    {
        // make `batch_send_min_limit` always GT 0
        if (batch_send_min_limit <= 0)
        {
            // set upper limit if not specified
            batch_send_min_limit = 8 * 1024 * partition_num /* 8K * partition-num */;
        }
        for (const auto & field_type : dag_context.result_field_types)
        {
            expected_types.emplace_back(getDataTypeByFieldTypeForComputingLayer(field_type));
        }
        break;
    }
    }
}

template <class ExchangeWriterPtr>
WriteResult HashPartitionWriter<ExchangeWriterPtr>::flush()
{
    has_pending_flush = false;
    if (0 == rows_in_blocks)
        return WriteResult::Done;

    auto wait_res = waitForWritable();
    if (wait_res == WaitResult::Ready)
    {
        switch (data_codec_version)
        {
        case MPPDataPacketV0:
        {
            partitionAndWriteBlocks();
            break;
        }
        case MPPDataPacketV1:
        default:
        {
            partitionAndWriteBlocksV1();
            break;
        }
        }
        return WriteResult::Done;
    }
    // set has_pending_flush to true since current flush is not done
    has_pending_flush = true;
    return wait_res == WaitResult::WaitForPolling ? WriteResult::NeedWaitForPolling : WriteResult::NeedWaitForNotify;
}

template <class ExchangeWriterPtr>
WaitResult HashPartitionWriter<ExchangeWriterPtr>::waitForWritable() const
{
    return writer->waitForWritable();
}

template <class ExchangeWriterPtr>
WriteResult HashPartitionWriter<ExchangeWriterPtr>::write(const Block & block)
{
    assert(has_pending_flush == false);
    RUNTIME_CHECK_MSG(
        block.columns() == dag_context.result_field_types.size(),
        "Output column size mismatch with field type size");

    size_t rows = 0;
    if (block.info.selective)
        rows = block.info.selective->size();
    else
        rows = block.rows();

    if (rows > 0)
    {
        rows_in_blocks += rows;
        if (data_codec_version >= MPPDataPacketV1)
            mem_size_in_blocks += block.bytes();
        blocks.push_back(block);
    }

    auto row_count_exceed = static_cast<Int64>(rows_in_blocks) >= batch_send_min_limit;
    auto row_bytes_exceed
        = data_codec_version >= MPPDataPacketV1 && mem_size_in_blocks >= MAX_BATCH_SEND_MIN_LIMIT_MEM_SIZE;

    if (row_count_exceed || row_bytes_exceed)
        return flush();
    return WriteResult::Done;
}

template <class ExchangeWriterPtr>
void HashPartitionWriter<ExchangeWriterPtr>::partitionAndWriteBlocksV1()
{
    assert(rows_in_blocks > 0);
    assert(mem_size_in_blocks > 0);
    assert(!blocks.empty());

    HashBaseWriterHelper::materializeBlocks(blocks);
    // All blocks are same, use one block's meta info as header
    Block dest_block_header = blocks.back().cloneEmpty();
    std::vector<String> partition_key_containers(collators.size());
    std::vector<std::vector<MutableColumns>> dest_columns(partition_num);
    size_t total_rows = 0;

    for (auto & block : blocks)
    {
        {
            // check schema
            assertBlockSchema(expected_types, block, HashPartitionWriterLabels[MPPDataPacketV1]);
        }
        auto && dest_tbl_cols = HashBaseWriterHelper::createDestColumns(block, partition_num);
        if (block.info.selective)
            HashBaseWriterHelper::scatterColumnsSelectiveBlock(
                block,
                partition_col_ids,
                collators,
                partition_key_containers,
                partition_num,
                dest_tbl_cols);
        else
            HashBaseWriterHelper::scatterColumns(
                block,
                partition_col_ids,
                collators,
                partition_key_containers,
                partition_num,
                dest_tbl_cols);
        block.clear();

        for (size_t part_id = 0; part_id < partition_num; ++part_id)
        {
            auto & columns = dest_tbl_cols[part_id];
            if unlikely (!columns.front())
                continue;
            size_t expect_size = columns.front()->size();
            total_rows += expect_size;
            dest_columns[part_id].emplace_back(std::move(columns));
        }
    }
    blocks.clear();
    RUNTIME_CHECK(rows_in_blocks, total_rows);

    for (size_t part_id = 0; part_id < partition_num; ++part_id)
    {
        writer->partitionWrite(
            dest_block_header,
            std::move(dest_columns[part_id]),
            part_id,
            data_codec_version,
            compression_method);
    }

    assert(blocks.empty());
    rows_in_blocks = 0;
    mem_size_in_blocks = 0;
}

template <class ExchangeWriterPtr>
void HashPartitionWriter<ExchangeWriterPtr>::partitionAndWriteBlocks()
{
    assert(!blocks.empty());

    std::vector<Blocks> partition_blocks;
    partition_blocks.resize(partition_num);
    {
        HashBaseWriterHelper::materializeBlocks(blocks);
        std::vector<String> partition_key_containers(collators.size());

        Block header = blocks[0].cloneEmpty();
        while (!blocks.empty())
        {
            const auto & block = blocks.back();
            auto dest_tbl_cols = HashBaseWriterHelper::createDestColumns(block, partition_num);
            if (block.info.selective)
                HashBaseWriterHelper::scatterColumnsSelectiveBlock(
                    block,
                    partition_col_ids,
                    collators,
                    partition_key_containers,
                    partition_num,
                    dest_tbl_cols);
            else
                HashBaseWriterHelper::scatterColumns(
                    block,
                    partition_col_ids,
                    collators,
                    partition_key_containers,
                    partition_num,
                    dest_tbl_cols);

            blocks.pop_back();

            for (size_t part_id = 0; part_id < partition_num; ++part_id)
            {
                Block dest_block = header.cloneEmpty();
                dest_block.setColumns(std::move(dest_tbl_cols[part_id]));
                if (dest_block.rows() > 0)
                    partition_blocks[part_id].push_back(std::move(dest_block));
            }
        }
        assert(blocks.empty());
        rows_in_blocks = 0;
    }

    for (size_t part_id = 0; part_id < partition_num; ++part_id)
    {
        auto & blocks = partition_blocks[part_id];
        writer->partitionWrite(blocks, part_id);
        blocks.clear();
    }
}

template class HashPartitionWriter<SyncMPPTunnelSetWriterPtr>;
template class HashPartitionWriter<AsyncMPPTunnelSetWriterPtr>;


} // namespace DB
