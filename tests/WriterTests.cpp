// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gaëtan Dezeiraud

#include "TestSupport.h"

#include "pak/Reader.h"
#include "pak/Writer.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace pak::tests
{
	TEST(Writer, RawRoundTripCursorAndMapping)
	{
		TemporaryDirectory temporary;
		const auto         archive_path = temporary.Path() / "Raw.pak";
		const auto         source       = MakeData(128u * 1024u + 37u);

		auto writer_result = ArchiveWriter::Create(archive_path);
		ASSERT_TRUE(writer_result);
		auto writer = std::move(writer_result.Value());
		ASSERT_TRUE(writer.AddBytes("Models\\Test.bin", source, FileOptions {CompressionPolicy::none}));
		ASSERT_TRUE(writer.AddBytes("Empty.bin", {}, FileOptions {CompressionPolicy::none}));
		ASSERT_TRUE(writer.Finalize());

		auto archive_result = Archive::Open(archive_path);
		ASSERT_TRUE(archive_result);
		auto archive = std::move(archive_result.Value());
		EXPECT_EQ(archive.FileCount(), 2u);
		EXPECT_TRUE(archive.Contains("Models/Test.bin"));

		auto file_result = archive.Find("Models/Test.bin");
		ASSERT_TRUE(file_result);
		auto file = std::move(file_result.Value());
		EXPECT_EQ(file.Size(), source.size());
		EXPECT_EQ(file.GetCompression(), Compression::none);

		std::vector<std::byte> output(source.size());
		auto                   read = file.ReadAt(0, output);
		ASSERT_TRUE(read);
		EXPECT_EQ(read.Value(), output.size());
		EXPECT_EQ(output, source);
		EXPECT_TRUE(file.Verify());

		auto view_result = file.Map(3, 8193);
		ASSERT_TRUE(view_result);
		const auto view = view_result.Value().Bytes();
		EXPECT_TRUE(std::equal(view.begin(), view.end(), source.begin() + 3));

		auto cursor = file.CreateCursor();
		ASSERT_TRUE(cursor.Seek(4095));
		std::array<std::byte, 257> cursor_output {};
		auto                       cursor_read = cursor.Read(cursor_output);
		ASSERT_TRUE(cursor_read);
		EXPECT_EQ(cursor.Tell(), 4095u + cursor_output.size());
		EXPECT_TRUE(std::equal(cursor_output.begin(), cursor_output.end(), source.begin() + 4095));
	}

	TEST(Writer, ZstdRoundTripAcrossBlockBoundaries)
	{
		TemporaryDirectory     temporary;
		const auto             archive_path = temporary.Path() / "Compressed.pak";
		std::vector<std::byte> source(std::size_t {600} * 1024);
		for(std::size_t index = 0; index < source.size(); ++index)
		{
			source[index] = static_cast<std::byte>((index / 97u) & 0x0Fu);
		}

		WriterOptions writer_options;
		writer_options.block_size = 64u * 1024u;
		auto writer_result        = ArchiveWriter::Create(archive_path, writer_options);
		ASSERT_TRUE(writer_result);
		auto writer = std::move(writer_result.Value());
		ASSERT_TRUE(writer.AddBytes("Data.bin", source, FileOptions {CompressionPolicy::automatic}));
		ASSERT_TRUE(writer.Finalize());

		auto archive_result = Archive::Open(archive_path);
		ASSERT_TRUE(archive_result);
		auto archive     = std::move(archive_result.Value());
		auto file_result = archive.Find("Data.bin");
		ASSERT_TRUE(file_result);
		auto file = std::move(file_result.Value());
		EXPECT_EQ(file.GetCompression(), Compression::zstd_blocks);
		EXPECT_LT(file.StoredSize(), file.Size());

		constexpr std::uint64_t offset = 64u * 1024u - 127u;
		std::vector<std::byte>  output(64u * 1024u + 521u);
		auto                    read = file.ReadAt(offset, output);
		ASSERT_TRUE(read);
		EXPECT_EQ(read.Value(), output.size());
		EXPECT_TRUE(std::equal(output.begin(), output.end(), source.begin() + offset));
		EXPECT_TRUE(file.Verify());

		auto view = file.Map(0, 1);
		ASSERT_FALSE(view);
		EXPECT_EQ(view.GetError().code, ErrorCode::not_mappable);
	}

	TEST(Writer, OutputIsDeterministic)
	{
		TemporaryDirectory temporary;
		const auto         first_path  = temporary.Path() / "First.pak";
		const auto         second_path = temporary.Path() / "Second.pak";
		const auto         first_data  = MakeData(32768, 1);
		const auto         second_data = MakeData(16384, 2);

		for(const auto &path : {first_path, second_path})
		{
			auto result = ArchiveWriter::Create(path);
			ASSERT_TRUE(result);
			auto writer = std::move(result.Value());
			ASSERT_TRUE(writer.AddBytes("B.bin", second_data, FileOptions {CompressionPolicy::automatic}));
			ASSERT_TRUE(writer.AddBytes("A.bin", first_data, FileOptions {CompressionPolicy::automatic}));
			ASSERT_TRUE(writer.Finalize());
		}
		EXPECT_EQ(ReadBytes(first_path), ReadBytes(second_path));
	}

	TEST(Writer, RejectsNonFiniteMinimumSaving)
	{
		TemporaryDirectory temporary;
		WriterOptions      options;
		options.minimum_saving = std::numeric_limits<float>::quiet_NaN();

		auto writer = ArchiveWriter::Create(temporary.Path() / "Invalid.pak", options);
		ASSERT_FALSE(writer);
		EXPECT_EQ(writer.GetError().code, ErrorCode::invalid_argument);
	}

	TEST(Writer, RejectsDuplicateNormalizedPath)
	{
		TemporaryDirectory temporary;
		const auto         archive_path  = temporary.Path() / "Duplicate.pak";
		auto               writer_result = ArchiveWriter::Create(archive_path);
		ASSERT_TRUE(writer_result);
		auto       writer = std::move(writer_result.Value());
		const auto data   = MakeData(128);

		ASSERT_TRUE(writer.AddBytes("Folder\\Data.bin", data));
		auto duplicate = writer.AddBytes("Folder/Data.bin", data);
		ASSERT_FALSE(duplicate);
		EXPECT_EQ(duplicate.GetError().code, ErrorCode::already_exists);
		ASSERT_TRUE(writer.Finalize());

		auto archive = Archive::Open(archive_path);
		ASSERT_TRUE(archive);
		EXPECT_EQ(archive.Value().FileCount(), 1u);
	}

	TEST(Writer, ParallelFilesMatchSequentialArchive)
	{
		TemporaryDirectory temporary;
		// Compressible, varied content: every Zstd level yields different bytes,
		// so the comparison also proves the configured level reaches each block.
		std::vector<std::byte> text(std::size_t {700} * 1024);
		auto                   value = 0x9E3779B9u;
		for(std::size_t index = 0; index < text.size(); ++index)
		{
			value       = value * 1664525u + 1013904223u;
			text[index] = static_cast<std::byte>('a' + (value >> 28u) + (index / 4096u) % 7u);
		}

		const std::array<std::pair<const char *, std::vector<std::byte>>, 5> contents {{
		    {"Text.txt", text},
		    {"Random.bin", MakeData(300000, 11)},
		    {"Empty.bin", {}},
		    {"Stored.txt", text},
		    {"Forced.txt", std::vector<std::byte>(text.begin(), text.begin() + 200000)},
		}};
		const std::array<CompressionPolicy, 5> policies {CompressionPolicy::automatic, CompressionPolicy::automatic, CompressionPolicy::automatic, CompressionPolicy::none, CompressionPolicy::zstd};
		std::vector<SourceFile> files;
		for(std::size_t index = 0; index < contents.size(); ++index)
		{
			const auto source = temporary.Path() / contents[index].first;
			WriteBytes(source, contents[index].second);
			files.push_back(SourceFile {source, std::string {"Data\\"} + contents[index].first, FileOptions {policies[index]}});
		}
		const WriterOptions options {64u * 1024u, 9, 0.02f, 16, true};

		const auto serial_path   = temporary.Path() / "Serial.pak";
		auto       serial_result = ArchiveWriter::Create(serial_path, options);
		ASSERT_TRUE(serial_result);
		auto serial = std::move(serial_result.Value());
		for(const auto &file : files)
		{
			ASSERT_TRUE(serial.AddFile(file.source, file.archive_path, file.options));
		}
		ASSERT_TRUE(serial.Finalize());
		const auto expected = ReadBytes(serial_path);

		auto archive_result = Archive::Open(serial_path);
		ASSERT_TRUE(archive_result);
		auto text_file = archive_result.Value().Find("Data/Text.txt");
		ASSERT_TRUE(text_file);
		EXPECT_EQ(text_file.Value().GetCompression(), Compression::zstd_blocks);
		EXPECT_LT(text_file.Value().StoredSize(), text_file.Value().Size());

		for(const std::uint32_t workers : {0u, 1u, 3u})
		{
			const auto parallel_path   = temporary.Path() / ("Parallel" + std::to_string(workers) + ".pak");
			auto       parallel_result = ArchiveWriter::Create(parallel_path, options);
			ASSERT_TRUE(parallel_result);
			auto parallel = std::move(parallel_result.Value());
			ASSERT_TRUE(parallel.AddFilesParallel(files, workers));
			ASSERT_TRUE(parallel.Finalize());
			EXPECT_TRUE(ReadBytes(parallel_path) == expected) << workers << " worker(s)";
		}
	}

	TEST(Writer, ParallelRejectsDuplicateAndMissingFiles)
	{
		TemporaryDirectory temporary;
		const auto         source = temporary.Path() / "Data.bin";
		WriteBytes(source, MakeData(1024));

		auto writer_result = ArchiveWriter::Create(temporary.Path() / "Duplicate.pak");
		ASSERT_TRUE(writer_result);
		auto                          writer = std::move(writer_result.Value());
		const std::vector<SourceFile> duplicates {{source, "Folder\\Data.bin"}, {source, "Folder/Data.bin"}};
		auto                          duplicate = writer.AddFilesParallel(duplicates);
		ASSERT_FALSE(duplicate);
		EXPECT_EQ(duplicate.GetError().code, ErrorCode::already_exists);

		const std::vector<SourceFile> missing {{source, "Data.bin"}, {temporary.Path() / "Missing.bin", "Missing.bin"}};
		EXPECT_FALSE(writer.AddFilesParallel(missing));
		// Earlier files may already be written: a failed batch poisons the writer.
		EXPECT_FALSE(writer.Finalize());
	}
}   // namespace pak::tests
