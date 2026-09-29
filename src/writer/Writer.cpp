// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gaëtan Dezeiraud

#include "pak/Writer.h"

#include "common/ByteIO.h"
#include "common/Defer.h"
#include "common/FormatInternal.h"
#include "common/Hash.h"
#include "common/Path.h"
#include "common/Win32IO.h"
#include "writer/WriterState.h"

#include <zstd.h>
#include <omp.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace pak
{
	namespace
	{
		[[nodiscard]] Result<void> WriteAll(HANDLE file, std::span<const std::byte> bytes) noexcept
		{
			std::size_t written_total = 0;
			while(written_total < bytes.size())
			{
				const auto count   = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - written_total, static_cast<std::size_t>(std::numeric_limits<DWORD>::max())));
				DWORD      written = 0;
				if(!WriteFile(file, bytes.data() + written_total, count, &written, nullptr))
				{
					return detail::Win32Error();
				}
				if(written == 0)
				{
					return Error {ErrorCode::io_error};
				}
				written_total += written;
			}
			return {};
		}

		[[nodiscard]] Result<void> ReadAll(HANDLE file, std::span<std::byte> destination) noexcept
		{
			std::size_t read_total = 0;
			while(read_total < destination.size())
			{
				const auto count = static_cast<DWORD>(std::min<std::size_t>(destination.size() - read_total, static_cast<std::size_t>(std::numeric_limits<DWORD>::max())));
				DWORD      read  = 0;
				if(!ReadFile(file, destination.data() + read_total, count, &read, nullptr))
				{
					return detail::Win32Error();
				}
				if(read == 0)
				{
					return Error {ErrorCode::io_error};
				}
				read_total += read;
			}
			return {};
		}

		[[nodiscard]] Result<void> SetPosition(HANDLE file, std::uint64_t offset) noexcept
		{
			LARGE_INTEGER position {};
			position.QuadPart = static_cast<LONGLONG>(offset);
			if(!SetFilePointerEx(file, position, nullptr, FILE_BEGIN))
			{
				return detail::Win32Error();
			}
			return {};
		}

		[[nodiscard]] Result<void> AlignOutput(detail::WriterState &state) noexcept
		{
			const auto alignment = state.options.data_alignment;
			const auto remainder = static_cast<std::uint32_t>(state.current_offset % alignment);
			if(remainder == 0)
			{
				return {};
			}
			const auto                  padding = alignment - remainder;
			std::array<std::byte, 4096> zeros {};
			auto                        write = WriteAll(state.file.Get(), std::span<const std::byte> {zeros}.first(padding));
			if(!write)
			{
				return write.GetError();
			}
			state.current_offset += padding;
			return {};
		}

		void Rollback(detail::WriterState &state, std::uint64_t offset, std::size_t block_count) noexcept
		{
			if(SetPosition(state.file.Get(), offset))
			{
				SetEndOfFile(state.file.Get());
				state.current_offset = offset;
			}
			state.blocks.resize(block_count);
		}

		[[nodiscard]] Result<void> CheckWritable(const detail::WriterState &state) noexcept
		{
			if(state.finalized)
			{
				return Error {ErrorCode::already_finalized};
			}
			if(state.failed)
			{
				return Error {ErrorCode::io_error};
			}
			return {};
		}

		void DiscardTemporary(detail::WriterState *state) noexcept
		{
			if(state != nullptr && !state->finalized)
			{
				state->file.Reset();
				DeleteFileW(state->temporary.c_str());
			}
		}

		struct ZstdContextDeleter final
		{
			void operator()(ZSTD_CCtx *context) const noexcept
			{
				ZSTD_freeCCtx(context);
			}
		};

		using ZstdContext = std::unique_ptr<ZSTD_CCtx, ZstdContextDeleter>;

		[[nodiscard]] std::size_t ChunkCapacity(const WriterOptions &options, CompressionPolicy policy) noexcept
		{
			return policy == CompressionPolicy::none ? 1024u * 1024u : options.block_size;
		}

		[[nodiscard]] float SavingThreshold(const WriterOptions &options, CompressionPolicy policy) noexcept
		{
			return policy == CompressionPolicy::automatic ? options.minimum_saving : 0.0f;
		}

		// Compresses one block into `output`, sized with ZSTD_compressBound. Returns
		// the compressed size, or zero when the block saves less than `threshold`
		// and must be stored raw. The same context always yields the same bytes as
		// a fresh one, so reusing it keeps archives deterministic.
		[[nodiscard]] Result<std::size_t> CompressBlock(ZSTD_CCtx *context, std::span<const std::byte> input, std::span<std::byte> output, int level, float threshold) noexcept
		{
			if(context == nullptr)
			{
				return Error {ErrorCode::io_error};
			}
			const auto compressed_size = ZSTD_compressCCtx(context, output.data(), output.size(), input.data(), input.size(), level);
			if(ZSTD_isError(compressed_size))
			{
				return Error {ErrorCode::io_error};
			}
			const auto required_size = static_cast<std::size_t>(static_cast<double>(input.size()) * (1.0 - threshold));
			return compressed_size < input.size() && compressed_size <= required_size ? compressed_size : std::size_t {0};
		}

		// Reads `size` bytes in chunks of at most `capacity`, hashes them and hands
		// each chunk with its offset to `consume`. Returns the content hash.
		template<typename ReadSource, typename Consume>
		[[nodiscard]] Result<std::uint64_t> ReadChunks(std::uint64_t size, std::size_t capacity, ReadSource &&read_source, Consume &&consume) noexcept
		{
			const auto hash = detail::CreateHashState();
			if(!hash)
			{
				return Error {ErrorCode::io_error};
			}
			std::vector<std::byte> input(static_cast<std::size_t>(std::min<std::uint64_t>(capacity, size)));
			for(std::uint64_t offset = 0; offset < size;)
			{
				const auto chunk = std::span<std::byte> {input}.first(static_cast<std::size_t>(std::min<std::uint64_t>(input.size(), size - offset)));
				if(auto read = read_source(offset, chunk); !read)
				{
					return read.GetError();
				}
				if(XXH3_64bits_update(hash.get(), chunk.data(), chunk.size()) == XXH_ERROR)
				{
					return Error {ErrorCode::io_error};
				}
				if(auto consumed = consume(offset, std::span<const std::byte> {chunk}); !consumed)
				{
					return consumed.GetError();
				}
				offset += chunk.size();
			}
			return XXH3_64bits_digest(hash.get());
		}

		// Appends one entry at the end of the archive. `write_blocks` receives an
		// `append_block(stored, uncompressed_offset, uncompressed_size, compressed)`
		// callback, writes every block in order and returns the content hash. The
		// path, data and block records are rolled back unless the entry completes.
		template<typename WriteBlocks>
		[[nodiscard]] Result<void> AppendEntry(detail::WriterState &state, std::string path, std::uint64_t size, CompressionPolicy policy, WriteBlocks &&write_blocks) noexcept
		{
			auto [path_iterator, inserted] = state.paths.emplace(std::move(path));
			if(!inserted)
			{
				return Error {ErrorCode::already_exists};
			}
			detail::Defer rollback_path {[&state, path_iterator]() noexcept { state.paths.erase(path_iterator); }};

			const auto    rollback_offset = state.current_offset;
			const auto    rollback_blocks = state.blocks.size();
			detail::Defer rollback {[&state, rollback_offset, rollback_blocks]() noexcept { Rollback(state, rollback_offset, rollback_blocks); }};

			if(auto alignment = AlignOutput(state); !alignment)
			{
				return alignment;
			}

			detail::BuildEntry build_entry;
			build_entry.path        = *path_iterator;
			auto &entry             = build_entry.record;
			entry.path_hash         = XXH3_64bits(build_entry.path.data(), build_entry.path.size());
			entry.data_offset       = state.current_offset;
			entry.uncompressed_size = size;
			if(state.options.checksum_entries)
			{
				entry.flags |= detail::entry_has_checksum;
			}

			bool any_compressed = false;
			auto append_block   = [&](std::span<const std::byte> stored, std::uint64_t uncompressed_offset, std::size_t uncompressed_size, bool compressed) noexcept -> Result<void>
			{
				if(policy != CompressionPolicy::none)
				{
					detail::BlockRecord block;
					block.data_offset         = state.current_offset;
					block.stored_size         = static_cast<std::uint32_t>(stored.size());
					block.uncompressed_size   = static_cast<std::uint32_t>(uncompressed_size);
					block.uncompressed_offset = uncompressed_offset;
					state.blocks.push_back(block);
				}
				if(auto write = WriteAll(state.file.Get(), stored); !write)
				{
					return write;
				}
				state.current_offset += stored.size();
				entry.stored_size += stored.size();
				any_compressed |= compressed;
				return {};
			};
			auto content_hash = write_blocks(append_block);
			if(!content_hash)
			{
				return content_hash.GetError();
			}
			entry.content_hash = content_hash.Value();

			// An entry whose blocks all stayed raw is stored as one contiguous file.
			if(any_compressed)
			{
				entry.compression       = Compression::zstd_blocks;
				entry.block_size        = state.options.block_size;
				entry.block_table_index = rollback_blocks;
				entry.block_count       = static_cast<std::uint32_t>(state.blocks.size() - rollback_blocks);
			}
			else
			{
				state.blocks.resize(rollback_blocks);
			}

			state.entries.push_back(build_entry);
			rollback_path.Release();
			rollback.Release();
			return {};
		}

		template<typename ReadSource>
		[[nodiscard]] Result<void> AddContent(detail::WriterState &state, std::string_view archive_path, std::uint64_t size, FileOptions file_options, ReadSource &&read_source) noexcept
		{
			auto normalized = detail::NormalizePath(archive_path);
			if(!normalized)
			{
				return normalized.GetError();
			}
			if(auto writable = CheckWritable(state); !writable)
			{
				return writable;
			}

			const auto             policy = file_options.compression;
			ZstdContext            context;
			std::vector<std::byte> compressed;
			if(policy != CompressionPolicy::none)
			{
				context.reset(ZSTD_createCCtx());
				compressed.resize(ZSTD_compressBound(static_cast<std::size_t>(std::min<std::uint64_t>(state.options.block_size, size))));
			}
			const auto level     = state.options.zstd_level;
			const auto threshold = SavingThreshold(state.options, policy);

			return AppendEntry(state, std::move(normalized.Value()), size, policy,
			                   [&](auto &append_block) noexcept
			                   {
				                   return ReadChunks(size, ChunkCapacity(state.options, policy), read_source,
				                                     [&](std::uint64_t offset, std::span<const std::byte> chunk) noexcept -> Result<void>
				                                     {
					                                     if(policy == CompressionPolicy::none)
					                                     {
						                                     return append_block(chunk, offset, chunk.size(), false);
					                                     }
					                                     auto compressed_size = CompressBlock(context.get(), chunk, compressed, level, threshold);
					                                     if(!compressed_size)
					                                     {
						                                     return compressed_size.GetError();
					                                     }
					                                     if(compressed_size.Value() == 0)
					                                     {
						                                     return append_block(chunk, offset, chunk.size(), false);
					                                     }
					                                     return append_block(std::span<const std::byte> {compressed}.first(compressed_size.Value()), offset, chunk.size(), true);
				                                     });
			                   });
		}

		struct PreparedBlock final
		{
			std::vector<std::byte> bytes;
			std::uint64_t          uncompressed_offset = 0;
			std::size_t            uncompressed_size   = 0;
			bool                   compressed          = false;
		};

		struct PreparedEntry final
		{
			std::uint64_t              size         = 0;
			std::uint64_t              content_hash = 0;
			std::vector<PreparedBlock> blocks;
		};

		[[nodiscard]] Error CompressPreparedBlock(PreparedBlock &block, ZSTD_CCtx *context, int level, float threshold) noexcept
		{
			std::vector<std::byte> compressed(ZSTD_compressBound(block.bytes.size()));
			auto                   compressed_size = CompressBlock(context, block.bytes, compressed, level, threshold);
			if(!compressed_size)
			{
				return compressed_size.GetError();
			}
			if(compressed_size.Value() != 0)
			{
				compressed.resize(compressed_size.Value());
				block.bytes      = std::move(compressed);
				block.compressed = true;
			}
			return {};
		}

		// Reads one file into memory, then compresses its blocks as tasks shared by
		// the whole OpenMP team so a single large file does not serialize on the
		// worker that read it. Each task uses the context of the thread running it.
		[[nodiscard]] Result<PreparedEntry> PrepareFile(const SourceFile &source, const WriterOptions &options, ZSTD_CCtx *const *contexts) noexcept
		{
			auto source_file = detail::OpenReadFile(source.source, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN);
			if(!source_file)
			{
				return source_file.GetError();
			}
			auto source_size = detail::FileSize(source_file.Value().Get());
			if(!source_size)
			{
				return source_size.GetError();
			}

			const auto    policy = source.options.compression;
			PreparedEntry prepared;
			prepared.size     = source_size.Value();
			auto content_hash = ReadChunks(
			    prepared.size, ChunkCapacity(options, policy),
			    [handle = source_file.Value().Get()](std::uint64_t, std::span<std::byte> destination) noexcept { return ReadAll(handle, destination); },
			    [&prepared](std::uint64_t offset, std::span<const std::byte> chunk) noexcept -> Result<void>
			    {
				    prepared.blocks.push_back(PreparedBlock {std::vector<std::byte>(chunk.begin(), chunk.end()), offset, chunk.size()});
				    return {};
			    });
			if(!content_hash)
			{
				return content_hash.GetError();
			}
			prepared.content_hash = content_hash.Value();
			if(policy == CompressionPolicy::none)
			{
				return prepared;
			}

			// Tasks only capture plain values and pointers: MSVC's /openmp:llvm hands
			// tasks garbage for reference parameters, which once turned the configured
			// Zstd level into 144 (clamped to 22) for every block.
			const int      level     = options.zstd_level;
			const float    threshold = SavingThreshold(options, policy);
			PreparedBlock *blocks    = prepared.blocks.data();
			std::vector<Error> errors(prepared.blocks.size());
			Error             *block_errors = errors.data();
			for(std::size_t index = 0; index < prepared.blocks.size(); ++index)
			{
				#pragma omp task default(none) firstprivate(index, blocks, block_errors, contexts, level, threshold)
				block_errors[index] = CompressPreparedBlock(blocks[index], contexts[omp_get_thread_num()], level, threshold);
			}
			#pragma omp taskwait
			for(const auto &error : errors)
			{
				if(error)
				{
					return error;
				}
			}
			return prepared;
		}

		[[nodiscard]] Result<void> AppendPrepared(detail::WriterState &state, const SourceFile &source, const PreparedEntry &prepared) noexcept
		{
			return AppendEntry(state, source.archive_path, prepared.size, source.options.compression,
			                   [&prepared](auto &append_block) noexcept -> Result<std::uint64_t>
			                   {
				                   for(const auto &block : prepared.blocks)
				                   {
					                   if(auto appended = append_block(block.bytes, block.uncompressed_offset, block.uncompressed_size, block.compressed); !appended)
					                   {
						                   return appended.GetError();
					                   }
				                   }
				                   return prepared.content_hash;
			                   });
		}

		void SerializeHeader(std::vector<std::byte> &output, const detail::ArchiveHeader &header)
		{
			detail::AppendBytes(output, detail::archive_magic);
			detail::Append(output, header.version);
			detail::Append(output, header.header_size);
			detail::Append(output, header.flags);
			detail::Append(output, header.file_count);
			detail::Append(output, header.data_offset);
			detail::Append(output, header.index_offset);
			detail::Append(output, header.index_size);
			detail::Append(output, header.index_hash);
		}

		void SerializeEntry(std::vector<std::byte> &output, const detail::EntryRecord &entry)
		{
			detail::Append(output, entry.path_hash);
			detail::Append(output, entry.content_hash);
			detail::Append(output, entry.path_offset);
			detail::Append(output, entry.data_offset);
			detail::Append(output, entry.uncompressed_size);
			detail::Append(output, entry.stored_size);
			detail::Append(output, entry.block_table_index);
			detail::Append(output, entry.path_size);
			detail::Append(output, entry.block_count);
			detail::Append(output, entry.block_size);
			detail::Append(output, static_cast<std::uint8_t>(entry.compression));
			detail::Append(output, entry.flags);
			detail::Append(output, std::uint16_t {0});
		}

		void SerializeBlock(std::vector<std::byte> &output, const detail::BlockRecord &block)
		{
			detail::Append(output, block.data_offset);
			detail::Append(output, block.stored_size);
			detail::Append(output, block.uncompressed_size);
			detail::Append(output, block.uncompressed_offset);
		}

		// Writes the index, footer and header, then moves the temporary file
		// over the destination.
		[[nodiscard]] Result<void> Commit(detail::WriterState &state) noexcept
		{
			std::sort(state.entries.begin(), state.entries.end(),
			          [](const detail::BuildEntry &left, const detail::BuildEntry &right) noexcept
			          {
				          if(left.record.path_hash != right.record.path_hash)
				          {
					          return left.record.path_hash < right.record.path_hash;
				          }
				          return left.path < right.path;
			          });

			std::string paths;
			for(auto &entry : state.entries)
			{
				entry.record.path_offset = paths.size();
				entry.record.path_size   = static_cast<std::uint32_t>(entry.path.size());
				paths.append(entry.path);
			}

			std::vector<std::byte> index;
			index.reserve(static_cast<std::size_t>(detail::serialized_index_header_size + state.entries.size() * detail::serialized_entry_size + state.blocks.size() * detail::serialized_block_size + paths.size()));
			detail::AppendBytes(index, detail::index_magic);
			detail::Append(index, format_version);
			detail::Append(index, detail::serialized_entry_size);
			detail::Append(index, static_cast<std::uint64_t>(state.entries.size()));
			detail::Append(index, static_cast<std::uint64_t>(paths.size()));
			detail::Append(index, static_cast<std::uint64_t>(state.blocks.size()));
			detail::Append(index, std::uint64_t {0});
			for(const auto &entry : state.entries)
			{
				SerializeEntry(index, entry.record);
			}
			for(const auto &block : state.blocks)
			{
				SerializeBlock(index, block);
			}
			detail::AppendBytes(index, std::as_bytes(std::span {paths}));

			detail::ArchiveHeader header;
			header.file_count   = state.entries.size();
			header.index_offset = state.current_offset;
			header.index_size   = index.size();
			header.index_hash   = XXH3_64bits(index.data(), index.size());

			std::vector<std::byte> footer;
			footer.reserve(detail::serialized_footer_size);
			detail::AppendBytes(footer, detail::footer_magic);
			detail::Append(footer, format_version);
			detail::Append(footer, std::uint32_t {0});
			detail::Append(footer, header.index_offset);
			detail::Append(footer, header.index_size);
			detail::Append(footer, header.index_hash);

			std::vector<std::byte> serialized_header;
			serialized_header.reserve(detail::serialized_header_size);
			SerializeHeader(serialized_header, header);

			const HANDLE file = state.file.Get();
			if(auto write = WriteAll(file, index); !write)
			{
				return write;
			}
			if(auto write = WriteAll(file, footer); !write)
			{
				return write;
			}
			if(auto position = SetPosition(file, 0); !position)
			{
				return position;
			}
			if(auto write = WriteAll(file, serialized_header); !write)
			{
				return write;
			}
			if(!FlushFileBuffers(file))
			{
				return detail::Win32Error();
			}
			state.file.Reset();

			if(!MoveFileExW(state.temporary.c_str(), state.destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
			{
				return detail::Win32Error();
			}
			return {};
		}
	}   // namespace

	ArchiveWriter::ArchiveWriter(std::unique_ptr<detail::WriterState> state) noexcept
	    : m_state(std::move(state))
	{
	}

	ArchiveWriter::ArchiveWriter(ArchiveWriter &&) noexcept = default;

	ArchiveWriter &ArchiveWriter::operator=(ArchiveWriter &&other) noexcept
	{
		if(this != &other)
		{
			DiscardTemporary(m_state.get());
			m_state = std::move(other.m_state);
		}
		return *this;
	}

	ArchiveWriter::~ArchiveWriter() noexcept
	{
		DiscardTemporary(m_state.get());
	}

	Result<ArchiveWriter> ArchiveWriter::Create(const std::filesystem::path &output, WriterOptions options) noexcept
	{
		if(output.empty() || options.block_size == 0 || options.block_size > detail::maximum_block_size || options.data_alignment == 0 || options.data_alignment > 4096 || !std::isfinite(options.minimum_saving) ||
		   options.minimum_saving < 0.0f || options.minimum_saving >= 1.0f)
		{
			return Error {ErrorCode::invalid_argument};
		}

		auto state         = std::make_unique<detail::WriterState>();
		state->destination = output;
		state->options     = options;

		const auto unique = std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetTickCount64());
		state->temporary  = output;
		state->temporary += L".tmp." + unique;

		state->file.Reset(CreateFileW(state->temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
		if(!state->file)
		{
			return detail::Win32Error();
		}

		std::array<std::byte, detail::archive_header_size> empty_header {};
		auto                                               write = WriteAll(state->file.Get(), empty_header);
		if(!write)
		{
			return write.GetError();
		}
		state->current_offset = detail::archive_header_size;
		return ArchiveWriter {std::move(state)};
	}

	Result<void> ArchiveWriter::AddFile(const std::filesystem::path &source, std::string_view archive_path, FileOptions options) noexcept
	{
		if(!m_state)
		{
			return Error {ErrorCode::invalid_argument};
		}
		auto source_file = detail::OpenReadFile(source, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN);
		if(!source_file)
		{
			return source_file.GetError();
		}
		auto source_size = detail::FileSize(source_file.Value().Get());
		if(!source_size)
		{
			return source_size.GetError();
		}
		auto read_source = [handle = source_file.Value().Get()](std::uint64_t, std::span<std::byte> destination) noexcept { return ReadAll(handle, destination); };
		return AddContent(*m_state, archive_path, source_size.Value(), options, read_source);
	}

	Result<void> ArchiveWriter::AddFilesParallel(std::span<const SourceFile> files, std::uint32_t worker_count) noexcept
	{
		if(!m_state)
		{
			return Error {ErrorCode::invalid_argument};
		}
		if(auto writable = CheckWritable(*m_state); !writable)
		{
			return writable;
		}
		if(files.empty())
		{
			return {};
		}

		// Validate the whole batch before writing anything, so a rejected batch
		// leaves the writer untouched.
		std::vector<SourceFile> normalized_files;
		normalized_files.reserve(files.size());
		std::unordered_set<std::string> batch_paths;
		batch_paths.reserve(files.size());
		for(const auto &file : files)
		{
			if(file.source.empty())
			{
				return Error {ErrorCode::invalid_argument};
			}
			auto normalized = detail::NormalizePath(file.archive_path);
			if(!normalized)
			{
				return normalized.GetError();
			}
			if(m_state->paths.contains(normalized.Value()) || !batch_paths.emplace(normalized.Value()).second)
			{
				return Error {ErrorCode::already_exists};
			}
			normalized_files.push_back(SourceFile {file.source, std::move(normalized.Value()), file.options});
		}

		// Blocks of one file are shared across the team, so even a small batch
		// can use every requested worker.
		const auto available = static_cast<std::uint32_t>(std::max(1, omp_get_num_procs()));
		const auto workers   = static_cast<int>(worker_count == 0 ? available : std::min(worker_count, available));
		// Files are compressed in waves and appended in input order after each
		// wave, which bounds the memory held by prepared files. Eight files per
		// worker give dynamic scheduling room to absorb uneven file sizes.
		const auto                                wave_capacity = std::min(normalized_files.size(), static_cast<std::size_t>(workers) * 8u);
		std::vector<std::optional<PreparedEntry>> prepared(wave_capacity);
		std::vector<Error>                        errors(wave_capacity);
		std::vector<ZstdContext>                  contexts(static_cast<std::size_t>(workers));
		std::vector<ZSTD_CCtx *>                  context_pointers(static_cast<std::size_t>(workers), nullptr);
		Error                                     first_error;

		// The team must have exactly `workers` threads: one context per thread.
		const int     previous_dynamic = omp_get_dynamic();
		detail::Defer restore_dynamic {[previous_dynamic]() noexcept { omp_set_dynamic(previous_dynamic); }};
		omp_set_dynamic(0);
		#pragma omp parallel num_threads(workers)
		{
			const auto thread_index = static_cast<std::size_t>(omp_get_thread_num());
			contexts[thread_index].reset(ZSTD_createCCtx());
			context_pointers[thread_index] = contexts[thread_index].get();
			#pragma omp barrier
			for(std::size_t wave_begin = 0; wave_begin < normalized_files.size(); wave_begin += wave_capacity)
			{
				const auto wave_size = std::min(wave_capacity, normalized_files.size() - wave_begin);
				#pragma omp for schedule(dynamic, 1)
				for(std::int64_t slot = 0; slot < static_cast<std::int64_t>(wave_size); ++slot)
				{
					auto result = PrepareFile(normalized_files[wave_begin + static_cast<std::size_t>(slot)], m_state->options, context_pointers.data());
					if(result)
					{
						prepared[static_cast<std::size_t>(slot)].emplace(std::move(result.Value()));
					}
					else
					{
						errors[static_cast<std::size_t>(slot)] = result.GetError();
					}
				}

				#pragma omp single
				{
					for(std::size_t slot = 0; slot < wave_size && !first_error; ++slot)
					{
						if(errors[slot])
						{
							first_error = errors[slot];
						}
						else if(auto appended = AppendPrepared(*m_state, normalized_files[wave_begin + slot], *prepared[slot]); !appended)
						{
							first_error = appended.GetError();
						}
					}
					std::fill(prepared.begin(), prepared.end(), std::nullopt);
					std::fill(errors.begin(), errors.end(), Error {});
				}
				// The implicit barrier of `single` publishes first_error to the team.
				if(first_error)
				{
					break;
				}
			}
		}

		if(first_error)
		{
			// Earlier waves are already in the archive: the batch cannot be undone.
			m_state->failed = true;
			return first_error;
		}
		return {};
	}

	Result<void> ArchiveWriter::AddBytes(std::string_view archive_path, std::span<const std::byte> bytes, FileOptions options) noexcept
	{
		if(!m_state)
		{
			return Error {ErrorCode::invalid_argument};
		}
		auto read_source = [bytes](std::uint64_t offset, std::span<std::byte> destination) noexcept -> Result<void>
		{
			std::memcpy(destination.data(), bytes.data() + offset, destination.size());
			return {};
		};
		return AddContent(*m_state, archive_path, bytes.size(), options, read_source);
	}

	Result<void> ArchiveWriter::Finalize() noexcept
	{
		if(!m_state)
		{
			return Error {ErrorCode::invalid_argument};
		}
		if(auto writable = CheckWritable(*m_state); !writable)
		{
			return writable;
		}
		auto committed     = Commit(*m_state);
		m_state->finalized = static_cast<bool>(committed);
		m_state->failed    = !committed;
		return committed;
	}

	Result<void> ArchiveWriter::Abort() noexcept
	{
		if(!m_state)
		{
			return Error {ErrorCode::invalid_argument};
		}
		if(m_state->finalized)
		{
			return Error {ErrorCode::already_finalized};
		}
		m_state->file.Reset();
		if(!DeleteFileW(m_state->temporary.c_str()))
		{
			const auto native = GetLastError();
			if(native != ERROR_FILE_NOT_FOUND)
			{
				return Error {ErrorCode::io_error, static_cast<std::uint32_t>(native)};
			}
		}
		m_state->finalized = true;
		return {};
	}
}   // namespace pak
