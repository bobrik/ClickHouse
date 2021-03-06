#include <DataStreams/MergeSortingBlockInputStream.h>
#include <DataStreams/MergingSortedBlockInputStream.h>
#include <DataStreams/NativeBlockOutputStream.h>
#include <DataStreams/copyData.h>
#include <IO/WriteBufferFromFile.h>
#include <IO/CompressedWriteBuffer.h>


namespace ProfileEvents
{
    extern const Event ExternalSortWritePart;
    extern const Event ExternalSortMerge;
}

namespace DB
{


/** Remove constant columns from block.
  */
static void removeConstantsFromBlock(Block & block)
{
    size_t columns = block.columns();
    size_t i = 0;
    while (i < columns)
    {
        if (block.getByPosition(i).column->isConst())
        {
            block.erase(i);
            --columns;
        }
        else
            ++i;
    }
}

static void removeConstantsFromSortDescription(const Block & sample_block, SortDescription & description)
{
    description.erase(std::remove_if(description.begin(), description.end(),
        [&](const SortColumnDescription & elem)
        {
            if (!elem.column_name.empty())
                return sample_block.getByName(elem.column_name).column->isConst();
            else
                return sample_block.safeGetByPosition(elem.column_number).column->isConst();
        }), description.end());
}

/** Add into block, whose constant columns was removed by previous function,
  *  constant columns from sample_block (which must have structure as before removal of constants from block).
  */
static void enrichBlockWithConstants(Block & block, const Block & sample_block)
{
    size_t rows = block.rows();
    size_t columns = sample_block.columns();

    for (size_t i = 0; i < columns; ++i)
    {
        const auto & col_type_name = sample_block.getByPosition(i);
        if (col_type_name.column->isConst())
            block.insert(i, {col_type_name.column->cloneResized(rows), col_type_name.type, col_type_name.name});
    }
}


Block MergeSortingBlockInputStream::readImpl()
{
    /** Algorithm:
      * - read to memory blocks from source stream;
      * - if too much of them and if external sorting is enabled,
      *   - merge all blocks to sorted stream and write it to temporary file;
      * - at the end, merge all sorted streams from temporary files and also from rest of blocks in memory.
      */

    /// If has not read source blocks.
    if (!impl)
    {
        while (Block block = children.back()->read())
        {
            if (!sample_block)
            {
                sample_block = block.cloneEmpty();
                removeConstantsFromSortDescription(sample_block, description);
            }

            removeConstantsFromBlock(block);

            blocks.push_back(block);
            sum_bytes_in_blocks += block.bytes();

            /** If too much of them and if external sorting is enabled,
              *  will merge blocks that we have in memory at this moment and write merged stream to temporary (compressed) file.
              * NOTE. It's possible to check free space in filesystem.
              */
            if (max_bytes_before_external_sort && sum_bytes_in_blocks > max_bytes_before_external_sort)
            {
                temporary_files.emplace_back(new Poco::TemporaryFile(tmp_path));
                const std::string & path = temporary_files.back()->path();
                WriteBufferFromFile file_buf(path);
                CompressedWriteBuffer compressed_buf(file_buf);
                NativeBlockOutputStream block_out(compressed_buf);
                MergeSortingBlocksBlockInputStream block_in(blocks, description, max_merged_block_size, limit);

                LOG_INFO(log, "Sorting and writing part of data into temporary file " + path);
                ProfileEvents::increment(ProfileEvents::ExternalSortWritePart);
                copyData(block_in, block_out, &is_cancelled);    /// NOTE. Possibly limit disk usage.
                LOG_INFO(log, "Done writing part of data into temporary file " + path);

                blocks.clear();
                sum_bytes_in_blocks = 0;
            }
        }

        if ((blocks.empty() && temporary_files.empty()) || isCancelled())
            return Block();

        if (temporary_files.empty())
        {
            impl = std::make_unique<MergeSortingBlocksBlockInputStream>(blocks, description, max_merged_block_size, limit);
        }
        else
        {
            /// If there was temporary files.
            ProfileEvents::increment(ProfileEvents::ExternalSortMerge);

            LOG_INFO(log, "There are " << temporary_files.size() << " temporary sorted parts to merge.");

            /// Create sorted streams to merge.
            for (const auto & file : temporary_files)
            {
                temporary_inputs.emplace_back(std::make_unique<TemporaryFileStream>(file->path()));
                inputs_to_merge.emplace_back(temporary_inputs.back()->block_in);
            }

            /// Rest of blocks in memory.
            if (!blocks.empty())
                inputs_to_merge.emplace_back(std::make_shared<MergeSortingBlocksBlockInputStream>(blocks, description, max_merged_block_size, limit));

            /// Will merge that sorted streams.
            impl = std::make_unique<MergingSortedBlockInputStream>(inputs_to_merge, description, max_merged_block_size, limit);
        }
    }

    Block res = impl->read();
    if (res)
        enrichBlockWithConstants(res, sample_block);
    return res;
}


MergeSortingBlocksBlockInputStream::MergeSortingBlocksBlockInputStream(
    Blocks & blocks_, SortDescription & description_, size_t max_merged_block_size_, size_t limit_)
    : blocks(blocks_), description(description_), max_merged_block_size(max_merged_block_size_), limit(limit_)
{
    Blocks nonempty_blocks;
    for (const auto & block : blocks)
    {
        if (block.rows() == 0)
            continue;

        nonempty_blocks.push_back(block);
        cursors.emplace_back(block, description);
        has_collation |= cursors.back().has_collation;
    }

    blocks.swap(nonempty_blocks);

    if (!has_collation)
    {
        for (size_t i = 0; i < cursors.size(); ++i)
            queue.push(SortCursor(&cursors[i]));
    }
    else
    {
        for (size_t i = 0; i < cursors.size(); ++i)
            queue_with_collation.push(SortCursorWithCollation(&cursors[i]));
    }
}


Block MergeSortingBlocksBlockInputStream::readImpl()
{
    if (blocks.empty())
        return Block();

    if (blocks.size() == 1)
    {
        Block res = blocks[0];
        blocks.clear();
        return res;
    }

    return !has_collation
        ? mergeImpl<SortCursor>(queue)
        : mergeImpl<SortCursorWithCollation>(queue_with_collation);
}


template <typename TSortCursor>
Block MergeSortingBlocksBlockInputStream::mergeImpl(std::priority_queue<TSortCursor> & queue)
{
    Block merged = blocks[0].cloneEmpty();
    size_t num_columns = blocks[0].columns();

    ColumnPlainPtrs merged_columns;
    for (size_t i = 0; i < num_columns; ++i)    /// TODO: reserve
        merged_columns.push_back(merged.safeGetByPosition(i).column.get());

    /// Take rows from queue in right order and push to 'merged'.
    size_t merged_rows = 0;
    while (!queue.empty())
    {
        TSortCursor current = queue.top();
        queue.pop();

        for (size_t i = 0; i < num_columns; ++i)
            merged_columns[i]->insertFrom(*current->all_columns[i], current->pos);

        if (!current->isLast())
        {
            current->next();
            queue.push(current);
        }

        ++total_merged_rows;
        if (limit && total_merged_rows == limit)
        {
            blocks.clear();
            return merged;
        }

        ++merged_rows;
        if (merged_rows == max_merged_block_size)
            return merged;
    }

    if (merged_rows == 0)
        merged.clear();

    return merged;
}


}
