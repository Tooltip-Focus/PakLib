// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gaëtan Dezeiraud

#include "TestSupport.h"

#include "pak/Reader.h"
#include "pak/Writer.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <random>
#include <span>
#include <thread>
#include <vector>

namespace pak::tests
{
	TEST(Concurrency, IndependentReadersAndCursors)
	{
		TemporaryDirectory temporary;
		const auto         archive_path = temporary.Path() / "Concurrent.pak";
		const auto         source       = MakeData(2u * 1024u * 1024u + 333u);

		WriterOptions options;
		options.block_size = 128u * 1024u;
		auto writer_result = ArchiveWriter::Create(archive_path, options);
		ASSERT_TRUE(writer_result);
		auto writer = std::move(writer_result.Value());
		ASSERT_TRUE(writer.AddBytes("Large.bin", source, FileOptions {CompressionPolicy::zstd}));
		ASSERT_TRUE(writer.Finalize());

		auto archive_result = Archive::Open(archive_path);
		ASSERT_TRUE(archive_result);
		auto archive     = std::move(archive_result.Value());
		auto file_result = archive.Find("Large.bin");
		ASSERT_TRUE(file_result);
		const auto file = file_result.Value();

		std::atomic_bool         failed = false;
		std::vector<std::thread> threads;
		threads.reserve(12);
		for(std::uint32_t thread_index = 0; thread_index < 12; ++thread_index)
		{
			threads.emplace_back(
			    [&, thread_index]
			    {
				    std::mt19937           generator {0xBADC0DEu + thread_index};
				    std::vector<std::byte> output(1u + thread_index * 997u);
				    for(std::size_t iteration = 0; iteration < 500; ++iteration)
				    {
					    const auto max_offset = source.size() - output.size();
					    const auto offset     = static_cast<std::size_t>(generator()) % max_offset;
					    auto       read       = file.ReadAt(offset, output);
					    if(!read || read.Value() != output.size() || !std::equal(output.begin(), output.end(), source.begin() + offset))
					    {
						    failed.store(true, std::memory_order_relaxed);
						    return;
					    }
				    }
			    });
		}
		for(auto &thread : threads)
		{
			thread.join();
		}
		EXPECT_FALSE(failed.load(std::memory_order_relaxed));
	}
}   // namespace pak::tests
