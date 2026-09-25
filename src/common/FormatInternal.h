// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gaëtan Dezeiraud

#pragma once

#include "pak/Format.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace pak::detail
{
	inline constexpr std::array<std::byte, 8> archive_magic {std::byte {'P'}, std::byte {'A'}, std::byte {'K'}, std::byte {'L'}, std::byte {'I'}, std::byte {'B'}, std::byte {'1'}, std::byte {0}};

	inline constexpr std::array<std::byte, 8> index_magic {std::byte {'P'}, std::byte {'A'}, std::byte {'K'}, std::byte {'I'}, std::byte {'D'}, std::byte {'X'}, std::byte {'1'}, std::byte {0}};

	inline constexpr std::array<std::byte, 8> footer_magic {std::byte {'P'}, std::byte {'A'}, std::byte {'K'}, std::byte {'E'}, std::byte {'N'}, std::byte {'D'}, std::byte {'1'}, std::byte {0}};

	inline constexpr std::uint32_t archive_header_size          = 4096;
	inline constexpr std::uint32_t serialized_header_size       = 64;
	inline constexpr std::uint32_t serialized_index_header_size = 48;
	inline constexpr std::uint32_t serialized_entry_size        = 72;
	inline constexpr std::uint32_t serialized_block_size        = 24;
	inline constexpr std::uint32_t serialized_footer_size       = 40;
	inline constexpr std::uint32_t minimum_archive_size         = archive_header_size + serialized_index_header_size + serialized_footer_size;
	inline constexpr std::uint32_t maximum_block_size           = 64u * 1024u * 1024u;
	inline constexpr std::uint8_t  entry_has_checksum           = 1u;

	struct ArchiveHeader final
	{
		std::uint32_t version      = format_version;
		std::uint32_t header_size  = archive_header_size;
		std::uint64_t flags        = 0;
		std::uint64_t file_count   = 0;
		std::uint64_t data_offset  = archive_header_size;
		std::uint64_t index_offset = 0;
		std::uint64_t index_size   = 0;
		std::uint64_t index_hash   = 0;
	};

	struct EntryRecord final
	{
		std::uint64_t path_hash         = 0;
		std::uint64_t content_hash      = 0;
		std::uint64_t path_offset       = 0;
		std::uint64_t data_offset       = 0;
		std::uint64_t uncompressed_size = 0;
		std::uint64_t stored_size       = 0;
		std::uint64_t block_table_index = 0;
		std::uint32_t path_size         = 0;
		std::uint32_t block_count       = 0;
		std::uint32_t block_size        = 0;
		Compression   compression       = Compression::none;
		std::uint8_t  flags             = 0;
	};

	struct BlockRecord final
	{
		std::uint64_t data_offset         = 0;
		std::uint32_t stored_size         = 0;
		std::uint32_t uncompressed_size   = 0;
		std::uint64_t uncompressed_offset = 0;
	};
}   // namespace pak::detail
