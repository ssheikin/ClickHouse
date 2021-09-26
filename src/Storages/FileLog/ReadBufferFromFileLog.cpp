#include <Interpreters/Context.h>
#include <Storages/FileLog/ReadBufferFromFileLog.h>
#include <Common/Stopwatch.h>

#include <common/logger_useful.h>

#include <algorithm>
#include <filesystem>
#include <boost/algorithm/string/join.hpp>

namespace DB
{
namespace ErrorCodes
{
    extern const int CANNOT_READ_FROM_ISTREAM;
}

ReadBufferFromFileLog::ReadBufferFromFileLog(
    StorageFileLog & storage_,
    size_t max_batch_size,
    size_t poll_timeout_,
    ContextPtr context_,
    size_t stream_number_,
    size_t max_streams_number_)
    : ReadBuffer(nullptr, 0)
    , log(&Poco::Logger::get("ReadBufferFromFileLog " + toString(stream_number)))
    , storage(storage_)
    , batch_size(max_batch_size)
    , poll_timeout(poll_timeout_)
    , context(context_)
    , stream_number(stream_number_)
    , max_streams_number(max_streams_number_)
{
    cleanUnprocessed();
    allowed = false;
}

void ReadBufferFromFileLog::cleanUnprocessed()
{
    records.clear();
    current = records.begin();
    BufferBase::set(nullptr, 0, 0);
}

bool ReadBufferFromFileLog::poll()
{
    if (hasMorePolledRecords())
    {
        allowed = true;
        return true;
    }

    buffer_status = BufferStatus::NO_RECORD_RETURNED;

    auto new_records = pollBatch(batch_size);
    if (new_records.empty())
    {
        LOG_TRACE(log, "No records returned");
        return false;
    }
    else
    {
        records = std::move(new_records);
        current = records.begin();

        LOG_TRACE(log, "Polled batch of {} records. ", records.size());

        buffer_status = BufferStatus::POLLED_OK;
        allowed = true;
        return true;
    }
}

ReadBufferFromFileLog::Records ReadBufferFromFileLog::pollBatch(size_t batch_size_)
{
    Records new_records;
    new_records.reserve(batch_size_);

    readNewRecords(new_records, batch_size);
    if (new_records.size() == batch_size_ || stream_out)
        return new_records;

    Stopwatch watch;
    while (watch.elapsedMilliseconds() < poll_timeout && new_records.size() != batch_size_)
    {
        readNewRecords(new_records, batch_size);
        /// All ifstrem reach end, no need to wait for timeout,
        /// since file status can not be updated during a streamToViews
        if (stream_out)
            break;
    }

    return new_records;
}

void ReadBufferFromFileLog::readNewRecords(ReadBufferFromFileLog::Records & new_records, size_t batch_size_)
{
    size_t need_records_size = batch_size_ - new_records.size();
    size_t read_records_size = 0;

    auto & file_infos = storage.getFileInfos();

    size_t files_per_stream = file_infos.file_names.size() / max_streams_number;
    size_t start = stream_number * files_per_stream;
    size_t end = stream_number == max_streams_number - 1 ? file_infos.file_names.size() : (stream_number + 1) * files_per_stream;

    for (size_t i = start; i < end; ++i)
    {
        auto file_name = file_infos.file_names[i];
        auto & file_ctx = file_infos.context_by_name.at(file_name);
        if (file_ctx.status == StorageFileLog::FileStatus::NO_CHANGE)
            continue;

        auto & file_meta = file_infos.meta_by_inode.at(file_infos.inode_by_name.at(file_name));

        Record record;
        while (read_records_size < need_records_size && static_cast<UInt64>(file_ctx.reader.tellg()) < file_meta.last_open_end)
        {
            if (!file_ctx.reader.good())
            {
                throw Exception("Can not read from file " + file_name + ", stream broken.", ErrorCodes::CANNOT_READ_FROM_ISTREAM);
            }
            UInt64 start_offset = file_ctx.reader.tellg();
            std::getline(file_ctx.reader, record.data);
            record.file_name = file_name;
            record.offset = start_offset;
            new_records.emplace_back(record);
            ++read_records_size;
        }

        UInt64 current_position = file_ctx.reader.tellg();
        if (!file_ctx.reader.good())
        {
            throw Exception("Can not read from file " + file_name + ", stream broken.", ErrorCodes::CANNOT_READ_FROM_ISTREAM);
        }

        file_meta.last_writen_position = current_position;

        /// stream reach to end
        if (current_position == file_meta.last_open_end)
        {
            file_ctx.status = StorageFileLog::FileStatus::NO_CHANGE;
        }

        /// All ifstream reach end
        if (i == end - 1 && (file_ctx.status == StorageFileLog::FileStatus::NO_CHANGE))
        {
            stream_out = true;
        }

        if (read_records_size == need_records_size)
        {
            break;
        }
    }
}

bool ReadBufferFromFileLog::nextImpl()
{
    if (!allowed || !hasMorePolledRecords())
        return false;

    auto * new_position = const_cast<char *>(current->data.data());
    BufferBase::set(new_position, current->data.size(), 0);
    allowed = false;

    current_file = current->file_name;
    current_offset = current->offset;

    ++current;

    return true;
}

}
