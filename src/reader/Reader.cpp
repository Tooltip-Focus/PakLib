// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gaëtan Dezeiraud

#include "pak/Reader.h"

#include "common/ByteIO.h"
#include "common/FormatInternal.h"
#include "common/Hash.h"
#include "common/Path.h"
#include "common/Win32IO.h"
#include "reader/ReaderState.h"

#include <zstd.h>

#include <algorithm>
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
		[[nodiscard]] bool RangeIsValid(std::uint64_t total, std::uint64_t offset, std::uint64_t size) noexcept
		{
			return offset <= total && size <= total - offset;
		}

		[[nodiscard]] std::string_view EntryPath(const detail::ArchiveState &state, const detail::EntryRecord &entry) noexcept
		{
			return std::string_view {state.paths.data() + entry.path_offset, entry.path_size};
		}

		[[nodiscard]] Result<detail::ArchiveHeader> ParseHeader(std::span<const std::byte> bytes) noexcept
		{
			detail::ByteReader reader {bytes};
			if(!reader.ReadMagic(detail::archive_magic))
			{
				return Error {ErrorCode::invalid_format};
			}

			detail::ArchiveHeader header;
			header.version      = reader.Read<std::uint32_t>();
			header.header_size  = reader.Read<std::uint32_t>();
			header.flags        = reader.Read<std::uint64_t>();
			header.file_count   = reader.Read<std::uint64_t>();
			header.data_offset  = reader.Read<std::uint64_t>();
			header.index_offset = reader.Read<std::uint64_t>();
			header.index_size   = reader.Read<std::uint64_t>();
			header.index_hash   = reader.Read<std::uint64_t>();
			if(reader.Failed())
			{
				return reader.GetError();
			}
			return header;
		}

		[[nodiscard]] Result<void> ValidateFooter(std::span<const std::byte> archive, const detail::ArchiveHeader &header) noexcept
		{
			const auto footer_offset = static_cast<std::uint64_t>(archive.size()) - detail::serialized_footer_size;

			detail::ByteReader reader {archive.last(detail::serialized_footer_size)};
			const bool         magic   = reader.ReadMagic(detail::footer_magic);
			const auto         version = reader.Read<std::uint32_t>();
			reader.Skip(sizeof(std::uint32_t));   // reserved
			const auto index_offset = reader.Read<std::uint64_t>();
			const auto index_size   = reader.Read<std::uint64_t>();
			const auto index_hash   = reader.Read<std::uint64_t>();
			if(!magic || reader.Failed() || version != format_version || index_offset != header.index_offset || index_size != header.index_size || index_hash != header.index_hash ||
			   header.index_offset + header.index_size != footer_offset)
			{
				return Error {ErrorCode::invalid_format, 0, footer_offset};
			}
			return {};
		}

		[[nodiscard]] Result<void> ParseIndex(detail::ArchiveState &state, const detail::ArchiveHeader &archive_header, std::span<const std::byte> bytes) noexcept
		{
			detail::ByteReader reader {bytes};
			const bool         magic           = reader.ReadMagic(detail::index_magic);
			const auto         version         = reader.Read<std::uint32_t>();
			const auto         entry_size      = reader.Read<std::uint32_t>();
			const auto         entry_count     = reader.Read<std::uint64_t>();
			const auto         path_table_size = reader.Read<std::uint64_t>();
			const auto         block_count     = reader.Read<std::uint64_t>();
			reader.Skip(sizeof(std::uint64_t));   // reserved
			if(!magic || reader.Failed())
			{
				return Error {ErrorCode::invalid_format};
			}
			if(version != format_version)
			{
				return Error {ErrorCode::unsupported_version};
			}
			if(entry_size != detail::serialized_entry_size || entry_count != archive_header.file_count)
			{
				return Error {ErrorCode::invalid_format};
			}

			const auto max_size      = static_cast<std::uint64_t>(bytes.size());
			const auto entries_bytes = entry_count * detail::serialized_entry_size;
			const auto blocks_bytes  = block_count * detail::serialized_block_size;
			if(entry_count > max_size / detail::serialized_entry_size || block_count > max_size / detail::serialized_block_size || detail::serialized_index_header_size > max_size ||
			   entries_bytes > max_size - detail::serialized_index_header_size || blocks_bytes > max_size - detail::serialized_index_header_size - entries_bytes ||
			   path_table_size != max_size - detail::serialized_index_header_size - entries_bytes - blocks_bytes || entry_count > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()) ||
			   block_count > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
			{
				return Error {ErrorCode::invalid_format};
			}

			state.entries.resize(static_cast<std::size_t>(entry_count));
			for(auto &entry : state.entries)
			{
				entry.path_hash         = reader.Read<std::uint64_t>();
				entry.content_hash      = reader.Read<std::uint64_t>();
				entry.path_offset       = reader.Read<std::uint64_t>();
				entry.data_offset       = reader.Read<std::uint64_t>();
				entry.uncompressed_size = reader.Read<std::uint64_t>();
				entry.stored_size       = reader.Read<std::uint64_t>();
				entry.block_table_index = reader.Read<std::uint64_t>();
				entry.path_size         = reader.Read<std::uint32_t>();
				entry.block_count       = reader.Read<std::uint32_t>();
				entry.block_size        = reader.Read<std::uint32_t>();
				entry.compression       = static_cast<Compression>(reader.Read<std::uint8_t>());
				entry.flags             = reader.Read<std::uint8_t>();
				reader.Skip(sizeof(std::uint16_t));   // reserved
			}

			state.blocks.resize(static_cast<std::size_t>(block_count));
			for(auto &block : state.blocks)
			{
				block.data_offset         = reader.Read<std::uint64_t>();
				block.stored_size         = reader.Read<std::uint32_t>();
				block.uncompressed_size   = reader.Read<std::uint32_t>();
				block.uncompressed_offset = reader.Read<std::uint64_t>();
			}

			const auto paths = reader.ReadBytes(static_cast<std::size_t>(path_table_size));
			if(reader.Failed())
			{
				return reader.GetError();
			}
			state.paths.assign(reinterpret_cast<const char *>(paths.data()), paths.size());

			for(std::size_t index = 0; index < state.entries.size(); ++index)
			{
				const auto &entry = state.entries[index];
				if(!RangeIsValid(state.paths.size(), entry.path_offset, entry.path_size))
				{
					return Error {ErrorCode::invalid_format};
				}
				const auto path       = EntryPath(state, entry);
				auto       normalized = detail::NormalizePath(path);
				if(!normalized || normalized.Value() != path || XXH3_64bits(path.data(), path.size()) != entry.path_hash)
				{
					return Error {ErrorCode::invalid_format};
				}

				if(entry.compression == Compression::none)
				{
					if(entry.block_count != 0 || entry.stored_size != entry.uncompressed_size || !RangeIsValid(archive_header.index_offset, entry.data_offset, entry.stored_size))
					{
						return Error {ErrorCode::invalid_format};
					}
				}
				else if(entry.compression == Compression::zstd_blocks)
				{
					if(entry.block_size == 0 || entry.block_size > detail::maximum_block_size || entry.block_count == 0 || !RangeIsValid(state.blocks.size(), entry.block_table_index, entry.block_count))
					{
						return Error {ErrorCode::invalid_format};
					}

					const auto expected_block_count = entry.uncompressed_size / entry.block_size + (entry.uncompressed_size % entry.block_size != 0 ? 1u : 0u);
					if(entry.block_count != expected_block_count)
					{
						return Error {ErrorCode::invalid_format};
					}
					std::uint64_t expected_offset = 0;
					std::uint64_t stored_total    = 0;
					for(std::uint32_t block_index = 0; block_index < entry.block_count; ++block_index)
					{
						const auto &block         = state.blocks[static_cast<std::size_t>(entry.block_table_index + block_index)];
						const auto  expected_size = static_cast<std::uint32_t>(std::min<std::uint64_t>(entry.block_size, entry.uncompressed_size - expected_offset));
						if(block.uncompressed_offset != expected_offset || block.uncompressed_size != expected_size || block.stored_size == 0 || !RangeIsValid(archive_header.index_offset, block.data_offset, block.stored_size))
						{
							return Error {ErrorCode::invalid_format};
						}
						expected_offset += block.uncompressed_size;
						stored_total += block.stored_size;
					}
					if(expected_offset != entry.uncompressed_size || stored_total != entry.stored_size)
					{
						return Error {ErrorCode::invalid_format};
					}
				}
				else
				{
					return Error {ErrorCode::unsupported_compression};
				}

				if(index > 0)
				{
					const auto &previous      = state.entries[index - 1];
					const auto  previous_path = EntryPath(state, previous);
					if(previous.path_hash > entry.path_hash || (previous.path_hash == entry.path_hash && previous_path >= path))
					{
						return Error {ErrorCode::invalid_format};
					}
				}
			}
			return {};
		}

		[[nodiscard]] Result<void> ParseArchive(detail::ArchiveState &state, std::span<const std::byte> archive, ReaderOptions options) noexcept
		{
			if(archive.size() < detail::minimum_archive_size)
			{
				return Error {ErrorCode::invalid_format};
			}

			auto parsed_header = ParseHeader(archive.first(detail::serialized_header_size));
			if(!parsed_header)
			{
				return parsed_header.GetError();
			}
			const auto &header = parsed_header.Value();
			if(header.version != format_version)
			{
				return Error {ErrorCode::unsupported_version};
			}
			if(header.header_size != detail::archive_header_size || header.data_offset != detail::archive_header_size || !RangeIsValid(archive.size(), header.index_offset, header.index_size) ||
			   header.index_size < detail::serialized_index_header_size)
			{
				return Error {ErrorCode::invalid_format};
			}

			auto footer = ValidateFooter(archive, header);
			if(!footer)
			{
				return footer.GetError();
			}

			const auto index_bytes = archive.subspan(static_cast<std::size_t>(header.index_offset), static_cast<std::size_t>(header.index_size));
			if(options.verify_index && XXH3_64bits(index_bytes.data(), index_bytes.size()) != header.index_hash)
			{
				return Error {ErrorCode::corrupted_data, 0, header.index_offset};
			}
			return ParseIndex(state, header, index_bytes);
		}
	}   // namespace

	Archive::Archive(std::shared_ptr<detail::ArchiveState> state) noexcept
	    : m_state(std::move(state))
	{
	}

	Result<Archive> Archive::Open(const std::filesystem::path &path, ReaderOptions options) noexcept
	{
		auto opened = detail::OpenReadFile(path);
		if(!opened)
		{
			return opened.GetError();
		}
		auto size = detail::FileSize(opened.Value().Get());
		if(!size)
		{
			return size.GetError();
		}
		if(size.Value() < detail::minimum_archive_size)
		{
			return Error {ErrorCode::invalid_format};
		}
		if(size.Value() > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
		{
			return Error {ErrorCode::out_of_range};
		}

		auto state       = std::make_shared<detail::ArchiveState>();
		state->file      = std::move(opened.Value());
		state->file_size = size.Value();
		state->mapping.Reset(CreateFileMappingW(state->file.Get(), nullptr, PAGE_READONLY, 0, 0, nullptr));
		if(!state->mapping)
		{
			return detail::Win32Error();
		}
		state->mapped_data = static_cast<const std::byte *>(MapViewOfFile(state->mapping.Get(), FILE_MAP_READ, 0, 0, 0));
		if(state->mapped_data == nullptr)
		{
			return detail::Win32Error();
		}
		const std::span archive {state->mapped_data, static_cast<std::size_t>(state->file_size)};
		auto            parsed = ParseArchive(*state, archive, options);
		if(!parsed)
		{
			return parsed.GetError();
		}
		return Archive {std::move(state)};
	}

	Result<Archive> Archive::OpenMemory(std::span<const std::byte> archive, ReaderOptions options) noexcept
	{
		auto state          = std::make_shared<detail::ArchiveState>();
		state->mapped_data  = archive.data();
		state->owns_mapping = false;
		state->file_size    = archive.size();

		auto parsed = ParseArchive(*state, archive, options);
		if(!parsed)
		{
			return parsed.GetError();
		}
		return Archive {std::move(state)};
	}

	Result<File> Archive::Find(std::string_view path) const noexcept
	{
		if(!m_state)
		{
			return Error {ErrorCode::invalid_argument};
		}
		auto normalized = detail::NormalizePath(path);
		if(!normalized)
		{
			return normalized.GetError();
		}
		const auto hash     = XXH3_64bits(normalized.Value().data(), normalized.Value().size());
		const auto iterator = std::lower_bound(m_state->entries.begin(), m_state->entries.end(), hash, [](const detail::EntryRecord &entry, std::uint64_t value) noexcept { return entry.path_hash < value; });
		for(auto current = iterator; current != m_state->entries.end() && current->path_hash == hash; ++current)
		{
			if(EntryPath(*m_state, *current) == normalized.Value())
			{
				return File {m_state, static_cast<std::size_t>(std::distance(m_state->entries.begin(), current))};
			}
		}
		return Error {ErrorCode::not_found};
	}

	bool Archive::Contains(std::string_view path) const noexcept
	{
		return static_cast<bool>(Find(path));
	}

	std::size_t Archive::FileCount() const noexcept
	{
		return m_state ? m_state->entries.size() : 0;
	}

	Result<EntryInfo> Archive::Entry(std::size_t index) const noexcept
	{
		if(!m_state || index >= m_state->entries.size())
		{
			return Error {ErrorCode::out_of_range};
		}
		const auto &entry = m_state->entries[index];
		return EntryInfo {EntryPath(*m_state, entry), entry.uncompressed_size, entry.stored_size, entry.compression};
	}

	File::File(std::shared_ptr<const detail::ArchiveState> state, std::size_t entry_index) noexcept
	    : m_state(std::move(state))
	    , m_entryIndex(entry_index)
	{
	}

	std::string_view File::Path() const noexcept
	{
		return m_state ? EntryPath(*m_state, m_state->entries[m_entryIndex]) : std::string_view {};
	}

	std::uint64_t File::Size() const noexcept
	{
		return m_state ? m_state->entries[m_entryIndex].uncompressed_size : 0;
	}

	std::uint64_t File::StoredSize() const noexcept
	{
		return m_state ? m_state->entries[m_entryIndex].stored_size : 0;
	}

	Compression File::GetCompression() const noexcept
	{
		return m_state ? m_state->entries[m_entryIndex].compression : Compression::none;
	}

	Result<std::size_t> File::ReadAt(std::uint64_t offset, std::span<std::byte> destination) const noexcept
	{
		if(!m_state)
		{
			return Error {ErrorCode::invalid_argument};
		}
		const auto &entry = m_state->entries[m_entryIndex];
		if(offset > entry.uncompressed_size)
		{
			return Error {ErrorCode::out_of_range};
		}
		const auto available = entry.uncompressed_size - offset;
		const auto requested = static_cast<std::size_t>(std::min<std::uint64_t>(available, destination.size()));
		if(requested == 0)
		{
			return std::size_t {0};
		}

		if(entry.compression == Compression::none)
		{
			// ParseIndex keeps every entry inside the mapped archive, so no overflow here.
			const auto *source = m_state->mapped_data + static_cast<std::size_t>(entry.data_offset + offset);
			std::memcpy(destination.data(), source, requested);
			return requested;
		}
		if(entry.compression != Compression::zstd_blocks)
		{
			return Error {ErrorCode::unsupported_compression};
		}

		// Sequential reads smaller than a block would otherwise decode the same
		// block once per call, so each thread keeps its last decoded block.
		struct DecodeCache final
		{
			~DecodeCache() noexcept
			{
				ZSTD_freeDCtx(context);
			}

			ZSTD_DCtx             *context = ZSTD_createDCtx();
			std::vector<std::byte> decoded;
			std::uint64_t          archive_id = 0;
			std::uint64_t          block      = 0;
		};
		thread_local DecodeCache cache;
		if(cache.context == nullptr)
		{
			return Error {ErrorCode::io_error};
		}
		const auto *mapped_data = m_state->mapped_data;

		const auto    end         = offset + requested;
		std::size_t   copied      = 0;
		std::uint32_t block_index = static_cast<std::uint32_t>(offset / entry.block_size);
		while(block_index < entry.block_count && offset + copied < end)
		{
			const auto &block       = m_state->blocks[static_cast<std::size_t>(entry.block_table_index + block_index)];
			const auto  block_begin = block.uncompressed_offset;
			const auto  block_end   = block_begin + block.uncompressed_size;
			const auto  copy_begin  = std::max(offset + copied, block_begin);
			const auto  copy_end    = std::min(end, block_end);
			if(copy_begin >= copy_end)
			{
				++block_index;
				continue;
			}
			const auto local_offset = static_cast<std::size_t>(copy_begin - block_begin);
			const auto copy_size    = static_cast<std::size_t>(copy_end - copy_begin);

			if(block.stored_size == block.uncompressed_size)
			{
				const auto *source = mapped_data + static_cast<std::size_t>(block.data_offset) + local_offset;
				std::memcpy(destination.data() + copied, source, copy_size);
			}
			else
			{
				const auto  block_key = entry.block_table_index + block_index;
				const auto *stored    = mapped_data + static_cast<std::size_t>(block.data_offset);
				const bool  cached    = cache.archive_id == m_state->id && cache.block == block_key;
				if(!cached && copy_size == block.uncompressed_size)
				{
					// Whole block requested: decode in place, skip the copy.
					const auto result = ZSTD_decompressDCtx(cache.context, destination.data() + copied, copy_size, stored, block.stored_size);
					if(ZSTD_isError(result) || result != copy_size)
					{
						return Error {ErrorCode::corrupted_data, 0, block.data_offset};
					}
				}
				else
				{
					if(!cached)
					{
						cache.archive_id = 0;
						cache.decoded.resize(block.uncompressed_size);
						const auto result = ZSTD_decompressDCtx(cache.context, cache.decoded.data(), cache.decoded.size(), stored, block.stored_size);
						if(ZSTD_isError(result) || result != cache.decoded.size())
						{
							return Error {ErrorCode::corrupted_data, 0, block.data_offset};
						}
						cache.archive_id = m_state->id;
						cache.block      = block_key;
					}
					std::memcpy(destination.data() + copied, cache.decoded.data() + local_offset, copy_size);
				}
			}
			copied += copy_size;
			++block_index;
		}
		return copied;
	}

	Result<MappedView> File::Map(std::uint64_t offset, std::size_t length) const noexcept
	{
		if(!m_state)
		{
			return Error {ErrorCode::invalid_argument};
		}
		const auto &entry = m_state->entries[m_entryIndex];
		if(entry.compression != Compression::none)
		{
			return Error {ErrorCode::not_mappable};
		}
		if(offset > entry.uncompressed_size || length > entry.uncompressed_size - offset)
		{
			return Error {ErrorCode::out_of_range};
		}
		if(length == 0)
		{
			return MappedView {};
		}

		const auto *data = m_state->mapped_data + static_cast<std::size_t>(entry.data_offset + offset);
		return MappedView {m_state, data, length};
	}

	Result<void> File::Verify() const noexcept
	{
		if(!m_state)
		{
			return Error {ErrorCode::invalid_argument};
		}
		const auto &entry = m_state->entries[m_entryIndex];
		if((entry.flags & detail::entry_has_checksum) == 0)
		{
			return {};
		}

		const auto hash = detail::CreateHashState();
		if(!hash)
		{
			return Error {ErrorCode::io_error};
		}

		std::vector<std::byte> buffer(std::size_t {1024} * 1024);
		std::uint64_t          offset = 0;
		while(offset < entry.uncompressed_size)
		{
			const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), entry.uncompressed_size - offset));
			auto       read  = ReadAt(offset, std::span<std::byte> {buffer}.first(count));
			if(!read)
			{
				return read.GetError();
			}
			if(XXH3_64bits_update(hash.get(), buffer.data(), read.Value()) == XXH_ERROR)
			{
				return Error {ErrorCode::io_error};
			}
			offset += read.Value();
		}
		if(XXH3_64bits_digest(hash.get()) != entry.content_hash)
		{
			return Error {ErrorCode::corrupted_data, 0, entry.data_offset};
		}
		return {};
	}

	Cursor File::CreateCursor() const noexcept
	{
		return Cursor {*this};
	}

	Cursor::Cursor(File file) noexcept
	    : m_file(std::move(file))
	{
	}

	Result<std::size_t> Cursor::Read(std::span<std::byte> destination) noexcept
	{
		auto result = m_file.ReadAt(m_position, destination);
		if(result)
		{
			m_position += result.Value();
		}
		return result;
	}

	Result<void> Cursor::Seek(std::uint64_t offset) noexcept
	{
		if(offset > m_file.Size())
		{
			return Error {ErrorCode::out_of_range};
		}
		m_position = offset;
		return {};
	}

	std::uint64_t Cursor::Tell() const noexcept
	{
		return m_position;
	}

	std::uint64_t Cursor::Remaining() const noexcept
	{
		return m_file.Size() - m_position;
	}
}   // namespace pak
