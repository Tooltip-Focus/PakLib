// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gaëtan Dezeiraud

#include "TestSupport.h"

#include "pak/Reader.h"
#include "pak/Writer.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <span>
#include <vector>

namespace pak::tests
{
	TEST(Reader, RejectsInvalidAndTruncatedArchives)
	{
		TemporaryDirectory temporary;
		const auto         invalid_path = temporary.Path() / "Invalid.pak";
		WriteBytes(invalid_path, MakeData(128));

		auto invalid = Archive::Open(invalid_path);
		ASSERT_FALSE(invalid);
		EXPECT_EQ(invalid.GetError().code, ErrorCode::invalid_format);

		const auto valid_path    = temporary.Path() / "Valid.pak";
		auto       writer_result = ArchiveWriter::Create(valid_path);
		ASSERT_TRUE(writer_result);
		auto       writer = std::move(writer_result.Value());
		const auto data   = MakeData(1024);
		ASSERT_TRUE(writer.AddBytes("Data.bin", data, FileOptions {CompressionPolicy::none}));
		ASSERT_TRUE(writer.Finalize());

		auto bytes = ReadBytes(valid_path);
		bytes.resize(bytes.size() - 17);
		const auto truncated_path = temporary.Path() / "Truncated.pak";
		WriteBytes(truncated_path, bytes);
		auto truncated = Archive::Open(truncated_path);
		EXPECT_FALSE(truncated);
	}

	TEST(Reader, EnforcesPathAndRangeRules)
	{
		TemporaryDirectory temporary;
		const auto         archive_path  = temporary.Path() / "Paths.pak";
		auto               writer_result = ArchiveWriter::Create(archive_path);
		ASSERT_TRUE(writer_result);
		auto       writer = std::move(writer_result.Value());
		const auto data   = MakeData(256);
		ASSERT_FALSE(writer.AddBytes("../Escape.bin", data));
		ASSERT_TRUE(writer.AddBytes("Folder//./File.bin", data, FileOptions {CompressionPolicy::none}));
		ASSERT_TRUE(writer.Finalize());

		auto archive_result = Archive::Open(archive_path);
		ASSERT_TRUE(archive_result);
		auto archive     = std::move(archive_result.Value());
		auto file_result = archive.Find("Folder/File.bin");
		ASSERT_TRUE(file_result);
		auto file = std::move(file_result.Value());

		std::array<std::byte, 8> output {};
		auto                     beyond = file.ReadAt(file.Size() + 1, output);
		ASSERT_FALSE(beyond);
		EXPECT_EQ(beyond.GetError().code, ErrorCode::out_of_range);
		auto tail = file.ReadAt(file.Size() - 3, output);
		ASSERT_TRUE(tail);
		EXPECT_EQ(tail.Value(), 3u);
	}

	TEST(Reader, DetectsContentCorruption)
	{
		TemporaryDirectory temporary;
		const auto         archive_path  = temporary.Path() / "Corrupted.pak";
		auto               writer_result = ArchiveWriter::Create(archive_path);
		ASSERT_TRUE(writer_result);
		auto       writer = std::move(writer_result.Value());
		const auto data   = MakeData(4096);
		ASSERT_TRUE(writer.AddBytes("Data.bin", data, FileOptions {CompressionPolicy::none}));
		ASSERT_TRUE(writer.Finalize());

		std::fstream archive_file(archive_path, std::ios::binary | std::ios::in | std::ios::out);
		archive_file.seekp(4096 + 31);
		const char changed = '\x7F';
		archive_file.write(&changed, 1);
		archive_file.close();

		auto archive_result = Archive::Open(archive_path);
		ASSERT_TRUE(archive_result);
		auto archive = std::move(archive_result.Value());
		auto file    = archive.Find("Data.bin");
		ASSERT_TRUE(file);
		auto verified = file.Value().Verify();
		ASSERT_FALSE(verified);
		EXPECT_EQ(verified.GetError().code, ErrorCode::corrupted_data);
	}

	TEST(Reader, OpensArchiveFromCallerOwnedMemory)
	{
		TemporaryDirectory temporary;
		const auto         archive_path  = temporary.Path() / "Memory.pak";
		auto               writer_result = ArchiveWriter::Create(archive_path, WriterOptions {1024, 3, 0.02f, 16, true});
		ASSERT_TRUE(writer_result);
		auto       writer = std::move(writer_result.Value());
		const auto data   = MakeData(8192);
		ASSERT_TRUE(writer.AddBytes("Folder/Data.bin", data, FileOptions {CompressionPolicy::zstd}));
		ASSERT_TRUE(writer.Finalize());

		const auto bytes          = ReadBytes(archive_path);
		auto       archive_result = Archive::OpenMemory(bytes);
		ASSERT_TRUE(archive_result);
		auto archive     = std::move(archive_result.Value());
		auto file_result = archive.Find("Folder/Data.bin");
		ASSERT_TRUE(file_result);
		auto                   file = std::move(file_result.Value());
		std::vector<std::byte> output(data.size());
		auto                   read = file.ReadAt(0, output);
		ASSERT_TRUE(read);
		EXPECT_EQ(read.Value(), data.size());
		EXPECT_EQ(output, data);
		EXPECT_TRUE(file.Verify());
	}

	TEST(Reader, SmallReadsReuseOnlyTheirOwnDecodedBlock)
	{
		// Two archives with an identical layout but different bytes: the
		// per-thread decoded block must never leak from one into the other.
		TemporaryDirectory temporary;
		// Compressible, so every block really goes through the decoder.
		std::vector<std::byte> first(16384);
		for(std::size_t index = 0; index < first.size(); ++index)
		{
			first[index] = static_cast<std::byte>('a' + (index / 7) % 26);
		}
		std::vector<std::byte> second = first;
		for(auto &value : second)
		{
			value = static_cast<std::byte>(~std::to_integer<unsigned>(value));
		}

		std::vector<std::vector<std::byte>> images;
		for(const auto *data : {&first, &second})
		{
			const auto path          = temporary.Path() / (data == &first ? "First.pak" : "Second.pak");
			auto       writer_result = ArchiveWriter::Create(path, WriterOptions {4096, 3, 0.0f, 16, true});
			ASSERT_TRUE(writer_result);
			auto writer = std::move(writer_result.Value());
			ASSERT_TRUE(writer.AddBytes("Data.bin", *data, FileOptions {CompressionPolicy::zstd}));
			ASSERT_TRUE(writer.Finalize());
			images.push_back(ReadBytes(path));
		}

		auto first_archive  = Archive::OpenMemory(images[0]);
		auto second_archive = Archive::OpenMemory(images[1]);
		ASSERT_TRUE(first_archive);
		ASSERT_TRUE(second_archive);
		auto first_file  = first_archive.Value().Find("Data.bin");
		auto second_file = second_archive.Value().Find("Data.bin");
		ASSERT_TRUE(first_file);
		ASSERT_TRUE(second_file);

		std::vector<std::byte> first_output(first.size());
		std::vector<std::byte> second_output(second.size());
		constexpr std::size_t  chunk = 1000;
		for(std::size_t offset = 0; offset < first.size(); offset += chunk)
		{
			const auto count = std::min(chunk, first.size() - offset);
			auto       a     = first_file.Value().ReadAt(offset, std::span<std::byte> {first_output}.subspan(offset, count));
			auto       b     = second_file.Value().ReadAt(offset, std::span<std::byte> {second_output}.subspan(offset, count));
			ASSERT_TRUE(a);
			ASSERT_TRUE(b);
			ASSERT_EQ(a.Value(), count);
			ASSERT_EQ(b.Value(), count);
		}
		EXPECT_EQ(first_output, first);
		EXPECT_EQ(second_output, second);
	}

	TEST(Reader, RejectsInconsistentCompressedBlockLayout)
	{
		TemporaryDirectory temporary;
		const auto         archive_path  = temporary.Path() / "MalformedBlocks.pak";
		auto               writer_result = ArchiveWriter::Create(archive_path, WriterOptions {4096, 3, 0.0f, 16, true});
		ASSERT_TRUE(writer_result);
		auto                   writer = std::move(writer_result.Value());
		std::vector<std::byte> data(8192, std::byte {'A'});
		ASSERT_TRUE(writer.AddBytes("Data.bin", data, FileOptions {CompressionPolicy::zstd}));
		ASSERT_TRUE(writer.Finalize());

		auto       bytes    = ReadBytes(archive_path);
		const auto read_u64 = [&bytes](std::size_t offset)
		{
			std::uint64_t value = 0;
			for(unsigned shift = 0; shift < 64; shift += 8)
			{
				value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(bytes[offset + shift / 8])) << shift;
			}
			return value;
		};
		const auto index_offset      = static_cast<std::size_t>(read_u64(40));
		const auto block_size_offset = index_offset + 48 + 64;
		ASSERT_LE(block_size_offset + 4, bytes.size());
		bytes[block_size_offset]     = std::byte {0x00};
		bytes[block_size_offset + 1] = std::byte {0x08};
		bytes[block_size_offset + 2] = std::byte {0x00};
		bytes[block_size_offset + 3] = std::byte {0x00};

		auto opened = Archive::OpenMemory(bytes, ReaderOptions {false});
		ASSERT_FALSE(opened);
		EXPECT_EQ(opened.GetError().code, ErrorCode::invalid_format);
	}
}   // namespace pak::tests
