// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gaëtan Dezeiraud

#include "TestSupport.h"

#include "pak/Reader.h"
#include "pak/Writer.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace pak::tests
{
	namespace
	{
		[[nodiscard]] std::string ToArchivePath(const std::filesystem::path &path)
		{
			const auto utf8 = path.generic_u8string();
			return {reinterpret_cast<const char *>(utf8.data()), utf8.size()};
		}
	}   // namespace

	TEST(Assets, PackAndReadConfiguredDirectory)
	{
		const wchar_t *configured = _wgetenv(L"PAKLIB_TEST_ASSET_DIR");
		if(configured == nullptr || *configured == L'\0')
		{
			GTEST_SKIP() << "PAKLIB_TEST_ASSET_DIR is not configured";
		}

		const std::filesystem::path        root {configured};
		std::error_code                    error;
		std::vector<std::filesystem::path> sources;
		for(std::filesystem::recursive_directory_iterator iterator {root, std::filesystem::directory_options::skip_permission_denied, error}, end; !error && iterator != end; iterator.increment(error))
		{
			if(iterator->is_regular_file(error) && !error)
			{
				sources.push_back(iterator->path());
			}
		}
		ASSERT_FALSE(error);
		ASSERT_FALSE(sources.empty());
		std::sort(sources.begin(), sources.end());

		TemporaryDirectory temporary;
		const auto         archive_path  = temporary.Path() / "Assets.pak";
		auto               writer_result = ArchiveWriter::Create(archive_path);
		ASSERT_TRUE(writer_result);
		auto writer = std::move(writer_result.Value());
		for(const auto &source : sources)
		{
			const auto relative = std::filesystem::relative(source, root, error);
			ASSERT_FALSE(error);
			ASSERT_TRUE(writer.AddFile(source, ToArchivePath(relative), FileOptions {CompressionPolicy::automatic}));
		}
		ASSERT_TRUE(writer.Finalize());

		auto archive_result = Archive::Open(archive_path);
		ASSERT_TRUE(archive_result);
		auto archive = std::move(archive_result.Value());
		ASSERT_EQ(archive.FileCount(), sources.size());
		for(const auto &source : sources)
		{
			const auto relative = std::filesystem::relative(source, root, error);
			ASSERT_FALSE(error);
			auto file_result = archive.Find(ToArchivePath(relative));
			ASSERT_TRUE(file_result);
			auto                  &file     = file_result.Value();
			const auto             expected = ReadBytes(source);
			std::vector<std::byte> actual(expected.size());
			auto                   read = file.ReadAt(0, actual);
			ASSERT_TRUE(read);
			EXPECT_EQ(actual, expected);
			EXPECT_TRUE(file.Verify());
		}
	}
}   // namespace pak::tests
