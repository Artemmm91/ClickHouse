#include <Storages/MergeTree/MergeTreeReaderStream.h>
#include <Compression/CachedCompressedReadBuffer.h>

#include <base/getThreadId.h>
#include <utility>


namespace DB
{

namespace ErrorCodes
{
    extern const int ARGUMENT_OUT_OF_BOUND;
}

MergeTreeReaderStream::MergeTreeReaderStream(
        DiskPtr disk_,
        const String & path_prefix_, const String & data_file_extension_, size_t marks_count_,
        const MarkRanges & all_mark_ranges,
        const MergeTreeReaderSettings & settings,
        MarkCache * mark_cache_,
        UncompressedCache * uncompressed_cache, size_t file_size_,
        const MergeTreeIndexGranularityInfo * index_granularity_info_,
        const ReadBufferFromFileBase::ProfileCallback & profile_callback, clockid_t clock_type)
    : disk(std::move(disk_))
    , path_prefix(path_prefix_)
    , data_file_extension(data_file_extension_)
    , marks_count(marks_count_)
    , file_size(file_size_)
    , mark_cache(mark_cache_)
    , save_marks_in_cache(settings.save_marks_in_cache)
    , index_granularity_info(index_granularity_info_)
    , marks_loader(disk, mark_cache, index_granularity_info->getMarksFilePath(path_prefix),
        marks_count, *index_granularity_info, save_marks_in_cache)
{
    /// Compute the size of the buffer.
    size_t max_mark_range_bytes = 0;
    size_t sum_mark_range_bytes = 0;

    for (const auto & mark_range : all_mark_ranges)
    {
        size_t left_mark = mark_range.begin;
        size_t right_mark = mark_range.end;
        auto [right_offset, mark_range_bytes] = getRightOffsetAndBytesRange(left_mark, right_mark);

        max_mark_range_bytes = std::max(max_mark_range_bytes, mark_range_bytes);
        sum_mark_range_bytes += mark_range_bytes;
    }

    /// Avoid empty buffer. May happen while reading dictionary for DataTypeLowCardinality.
    /// For example: part has single dictionary and all marks point to the same position.
    ReadSettings read_settings = settings.read_settings;
    if (max_mark_range_bytes != 0)
        read_settings = read_settings.adjustBufferSize(max_mark_range_bytes);

    /// Initialize the objects that shall be used to perform read operations.
    if (uncompressed_cache)
    {
        auto buffer = std::make_unique<CachedCompressedReadBuffer>(
            fullPath(disk, path_prefix + data_file_extension),
            [this, sum_mark_range_bytes, read_settings]()
            {
                return disk->readFile(
                    path_prefix + data_file_extension,
                    read_settings,
                    sum_mark_range_bytes);
            },
            uncompressed_cache);

        if (profile_callback)
            buffer->setProfileCallback(profile_callback, clock_type);

        if (!settings.checksum_on_read)
            buffer->disableChecksumming();

        cached_buffer = std::move(buffer);
        data_buffer = cached_buffer.get();
    }
    else
    {
        auto buffer = std::make_unique<CompressedReadBufferFromFile>(
            disk->readFile(
                path_prefix + data_file_extension,
                read_settings,
                sum_mark_range_bytes));

        if (profile_callback)
            buffer->setProfileCallback(profile_callback, clock_type);

        if (!settings.checksum_on_read)
            buffer->disableChecksumming();

        non_cached_buffer = std::move(buffer);
        data_buffer = non_cached_buffer.get();
    }
}


std::pair<size_t, size_t> MergeTreeReaderStream::getRightOffsetAndBytesRange(size_t left_mark, size_t right_mark)
{
    /// NOTE: if we are reading the whole file, then right_mark == marks_count
    /// and we will use max_read_buffer_size for buffer size, thus avoiding the need to load marks.

    /// If the end of range is inside the block, we will need to read it too.
    size_t result_right_mark = right_mark;
    if (right_mark < marks_count && marks_loader.getMark(right_mark).offset_in_decompressed_block > 0)
    {
        auto indices = collections::range(right_mark, marks_count);
        auto it = std::upper_bound(indices.begin(), indices.end(), right_mark, [this](size_t i, size_t j)
        {
            return marks_loader.getMark(i).offset_in_compressed_file < marks_loader.getMark(j).offset_in_compressed_file;
        });

        result_right_mark = (it == indices.end() ? marks_count : *it);
    }

    size_t right_offset;
    size_t mark_range_bytes;

    /// If there are no marks after the end of range, just use file size
    if (result_right_mark >= marks_count
        || (result_right_mark + 1 == marks_count
            && marks_loader.getMark(result_right_mark).offset_in_compressed_file == marks_loader.getMark(right_mark).offset_in_compressed_file))
    {
        right_offset = file_size;
        mark_range_bytes = right_offset - (left_mark < marks_count ? marks_loader.getMark(left_mark).offset_in_compressed_file : 0);
    }
    else
    {
        right_offset = marks_loader.getMark(result_right_mark).offset_in_compressed_file;
        mark_range_bytes = right_offset - marks_loader.getMark(left_mark).offset_in_compressed_file;
    }

    return std::make_pair(right_offset, mark_range_bytes);
}


void MergeTreeReaderStream::seekToMark(size_t index)
{
    MarkInCompressedFile mark = marks_loader.getMark(index);

    try
    {
        if (cached_buffer)
            cached_buffer->seek(mark.offset_in_compressed_file, mark.offset_in_decompressed_block);
        if (non_cached_buffer)
            non_cached_buffer->seek(mark.offset_in_compressed_file, mark.offset_in_decompressed_block);
    }
    catch (Exception & e)
    {
        /// Better diagnostics.
        if (e.code() == ErrorCodes::ARGUMENT_OUT_OF_BOUND)
            e.addMessage("(while seeking to mark " + toString(index)
                         + " of column " + path_prefix + "; offsets are: "
                         + toString(mark.offset_in_compressed_file) + " "
                         + toString(mark.offset_in_decompressed_block) + ")");

        throw;
    }
}


void MergeTreeReaderStream::seekToStart()
{
    try
    {
        if (cached_buffer)
            cached_buffer->seek(0, 0);
        if (non_cached_buffer)
            non_cached_buffer->seek(0, 0);
    }
    catch (Exception & e)
    {
        /// Better diagnostics.
        if (e.code() == ErrorCodes::ARGUMENT_OUT_OF_BOUND)
            e.addMessage("(while seeking to start of column " + path_prefix + ")");

        throw;
    }
}


void MergeTreeReaderStream::adjustForRange(size_t left_mark, size_t right_mark)
{
    auto [right_offset, mark_range_bytes] = getRightOffsetAndBytesRange(left_mark, right_mark);
    if (right_offset > last_right_offset)
    {
        last_right_offset = right_offset;
        if (cached_buffer)
            cached_buffer->setReadUntilPosition(last_right_offset);
        if (non_cached_buffer)
            non_cached_buffer->setReadUntilPosition(last_right_offset);
    }
}

}
