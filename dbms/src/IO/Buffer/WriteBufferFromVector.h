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

#include <IO/Buffer/WriteBuffer.h>

namespace DB
{
namespace ErrorCodes
{
extern const int CANNOT_WRITE_AFTER_END_OF_BUFFER;
}

/** Writes data to existing std::vector or similar type. When not enough space, it doubles vector size.
  *
  * In destructor, vector is cut to the size of written data.
  * You can call 'finalize' to resize earlier.
  *
  * The vector should live until this object is destroyed or until the 'finish' method is called.
  */
template <typename VectorType, size_t initial_size = 32>
class WriteBufferFromVector : public WriteBuffer
{
    static_assert(sizeof(typename VectorType::value_type) == sizeof(char));

private:
    VectorType & vector;
    bool is_finished = false;

    static constexpr size_t size_multiplier = 2;

    void nextImpl() override
    {
        if (is_finished)
            throw Exception("WriteBufferFromVector is finished", ErrorCodes::CANNOT_WRITE_AFTER_END_OF_BUFFER);

        size_t old_size = vector.size();
        /// pos may not be equal to vector.data() + old_size, because WriteBuffer::next() can be used to flush data
        size_t pos_offset = pos - reinterpret_cast<Position>(vector.data());
        vector.resize(old_size * size_multiplier);
        internal_buffer = Buffer(
            reinterpret_cast<Position>(vector.data() + pos_offset),
            reinterpret_cast<Position>(vector.data() + vector.size()));
        working_buffer = internal_buffer;
    }

public:
    explicit WriteBufferFromVector(VectorType & vector_)
        : WriteBuffer(reinterpret_cast<Position>(vector_.data()), vector_.size())
        , vector(vector_)
    {
        if (vector.empty())
        {
            vector.resize(initial_size);
            set(reinterpret_cast<Position>(vector.data()), vector.size());
        }
    }

    /// Append to vector instead of rewrite.
    struct AppendModeTag
    {
    };
    WriteBufferFromVector(VectorType & vector_, AppendModeTag)
        : WriteBuffer(nullptr, 0)
        , vector(vector_)
    {
        size_t old_size = vector.size();
        size_t size = (old_size < initial_size)
            ? initial_size
            : ((old_size < vector.capacity()) ? vector.capacity() : vector.capacity() * size_multiplier);
        vector.resize(size);
        set(reinterpret_cast<Position>(vector.data() + old_size),
            (size - old_size) * sizeof(typename VectorType::value_type));
    }

    void finalize()
    {
        if (is_finished)
            return;
        is_finished = true;
        vector.resize(
            ((position() - reinterpret_cast<Position>(vector.data())) + sizeof(typename VectorType::value_type)
             - 1) /// Align up.
            / sizeof(typename VectorType::value_type));
        bytes += offset();
        /// Prevent further writes.
        set(nullptr, 0);
    }

    bool isFinished() const { return is_finished; }

    void restart()
    {
        if (vector.empty())
            vector.resize(initial_size);
        bytes = 0;
        set(reinterpret_cast<Position>(vector.data()), vector.size());
        is_finished = false;
    }

    ~WriteBufferFromVector() override { finalize(); }
};

} // namespace DB
