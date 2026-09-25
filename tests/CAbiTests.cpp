// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gaëtan Dezeiraud

#include "TestSupport.h"

#include "pak/CAbi.h"
#include "pak/Error.h"

#include <gtest/gtest.h>

#include <memory>
#include <string_view>
#include <vector>

namespace pak::tests
{
	TEST(CAbi, WriterAndMemoryReaderRoundTrip)
	{
		TemporaryDirectory temporary;
		const auto         source_path  = temporary.Path() / "Source.bin";
		const auto         archive_path = temporary.Path() / "Archive.pak";
		const auto         source       = MakeData(8192);
		WriteBytes(source_path, source);

		pak_error          error {};
		pak_writer_handle *raw_writer = nullptr;
		ASSERT_EQ(pak_writer_create(archive_path.c_str(), nullptr, &raw_writer, &error), 0);
		std::unique_ptr<pak_writer_handle, decltype(&pak_writer_destroy)> writer {raw_writer, &pak_writer_destroy};

		constexpr std::string_view entry_path = "Data/Source.bin";
		ASSERT_EQ(pak_writer_add_file(writer.get(), source_path.c_str(), entry_path.data(), entry_path.size(), PAK_COMPRESSION_AUTOMATIC, &error), 0);
		ASSERT_EQ(pak_writer_finalize(writer.get(), &error), 0);
		writer.reset();

		const auto          archive_bytes = ReadBytes(archive_path);
		pak_archive_handle *raw_archive   = nullptr;
		ASSERT_EQ(pak_archive_open_memory(archive_bytes.data(), archive_bytes.size(), &raw_archive, &error), 0);
		std::unique_ptr<pak_archive_handle, decltype(&pak_archive_destroy)> archive {raw_archive, &pak_archive_destroy};

		pak_file_handle *raw_file  = nullptr;
		std::uint64_t    file_size = 0;
		ASSERT_EQ(pak_archive_open_file(archive.get(), entry_path.data(), entry_path.size(), &raw_file, &file_size, &error), 0);
		std::unique_ptr<pak_file_handle, decltype(&pak_file_destroy)> file {raw_file, &pak_file_destroy};
		ASSERT_EQ(file_size, source.size());

		archive.reset();
		std::vector<std::byte> output(source.size());
		std::size_t            read = 0;
		ASSERT_EQ(pak_file_read(file.get(), 0, output.data(), output.size(), &read, &error), 0);
		EXPECT_EQ(read, output.size());
		EXPECT_EQ(output, source);

		EXPECT_EQ(pak_file_read(file.get(), file_size, nullptr, 0, &read, &error), 0);
		EXPECT_EQ(read, 0u);
	}

	TEST(CAbi, ClearsOutputsOnInvalidArguments)
	{
		pak_error        error {};
		pak_file_handle *file   = reinterpret_cast<pak_file_handle *>(1);
		std::uint64_t    size   = 123;
		const auto       result = pak_archive_open_file(nullptr, "x", 1, &file, &size, &error);

		EXPECT_EQ(result, static_cast<std::uint16_t>(ErrorCode::invalid_argument));
		EXPECT_EQ(error.code, result);
		EXPECT_EQ(file, nullptr);
		EXPECT_EQ(size, 0u);
	}
}   // namespace pak::tests
