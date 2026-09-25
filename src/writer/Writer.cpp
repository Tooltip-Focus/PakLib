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

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
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
			auto [path_iterator, inserted] = state.paths.emplace(std::move(normalized.Value()));
			if(!inserted)
			{
				return Error {ErrorCode::already_exists};
			}
			detail::Defer rollback_path {[&state, path_iterator]() noexcept { state.paths.erase(path_iterator); }};

			const auto    rollback_offset = state.current_offset;
			const auto    rollback_blocks = state.blocks.size();
			detail::Defer rollback {[&state, rollback_offset, rollback_blocks]() noexcept { Rollback(state, rollback_offset, rollback_blocks); }};

			auto alignment = AlignOutput(state);
			if(!alignment)
			{
				return alignment.GetError();
			}

			detail::BuildEntry build_entry;
			build_entry.path        = *path_iterator;
			auto &entry             = build_entry.record;
			entry.path_hash         = XXH3_64bits(build_entry.path.data(), build_entry.path.size());
			entry.data_offset       = state.current_offset;
			entry.uncompressed_size = size;
			entry.block_size        = state.options.block_size;
			entry.block_table_index = state.blocks.size();
			if(state.options.checksum_entries)
			{
				entry.flags |= detail::entry_has_checksum;
			}

			const auto hash = detail::CreateHashState();
			if(!hash)
			{
				return Error {ErrorCode::io_error};
			}

			const auto             chunk_capacity = file_options.compression == CompressionPolicy::none ? 1024u * 1024u : state.options.block_size;
			std::vector<std::byte> input(chunk_capacity);
			std::vector<std::byte> compressed;
			if(file_options.compression != CompressionPolicy::none)
			{
				compressed.resize(ZSTD_compressBound(state.options.block_size));
			}

			std::uint64_t source_offset  = 0;
			bool          any_compressed = false;
			while(source_offset < size)
			{
				const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(input.size(), size - source_offset));
				auto       read  = read_source(source_offset, std::span<std::byte> {input}.first(count));
				if(!read)
				{
					return read.GetError();
				}
				if(XXH3_64bits_update(hash.get(), input.data(), count) == XXH_ERROR)
				{
					return Error {ErrorCode::io_error};
				}

				if(file_options.compression == CompressionPolicy::none)
				{
					auto write = WriteAll(state.file.Get(), std::span<const std::byte> {input}.first(count));
					if(!write)
					{
						return write.GetError();
					}
					state.current_offset += count;
					entry.stored_size += count;
				}
				else
				{
					const auto compressed_size = ZSTD_compress(compressed.data(), compressed.size(), input.data(), count, state.options.zstd_level);
					if(ZSTD_isError(compressed_size))
					{
						return Error {ErrorCode::io_error};
					}

					const auto threshold      = file_options.compression == CompressionPolicy::automatic ? state.options.minimum_saving : 0.0f;
					const auto required_size  = static_cast<std::size_t>(static_cast<double>(count) * (1.0 - threshold));
					const bool use_compressed = compressed_size < count && compressed_size <= required_size;
					const auto stored_size    = use_compressed ? compressed_size : count;
					const auto stored_bytes   = use_compressed ? std::span<const std::byte> {compressed}.first(compressed_size) : std::span<const std::byte> {input}.first(count);

					detail::BlockRecord block;
					block.data_offset         = state.current_offset;
					block.stored_size         = static_cast<std::uint32_t>(stored_size);
					block.uncompressed_size   = static_cast<std::uint32_t>(count);
					block.uncompressed_offset = source_offset;
					state.blocks.push_back(block);

					auto write = WriteAll(state.file.Get(), stored_bytes);
					if(!write)
					{
						return write.GetError();
					}
					any_compressed |= use_compressed;
					state.current_offset += stored_size;
					entry.stored_size += stored_size;
				}
				source_offset += count;
			}

			entry.content_hash = XXH3_64bits_digest(hash.get());
			if(file_options.compression != CompressionPolicy::none && any_compressed)
			{
				entry.compression = Compression::zstd_blocks;
				entry.block_count = static_cast<std::uint32_t>(state.blocks.size() - rollback_blocks);
			}
			else
			{
				entry.compression       = Compression::none;
				entry.block_count       = 0;
				entry.block_table_index = 0;
				entry.block_size        = 0;
				state.blocks.resize(rollback_blocks);
				entry.stored_size = entry.uncompressed_size;
			}

			state.entries.push_back(build_entry);
			rollback_path.Release();
			rollback.Release();
			return {};
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
