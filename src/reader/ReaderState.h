// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gaëtan Dezeiraud

#pragma once

#include "common/FormatInternal.h"
#include "common/UniqueHandle.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace pak::detail
{
	[[nodiscard]] inline std::uint64_t NextArchiveId() noexcept
	{
		static std::atomic<std::uint64_t> next {1};
		return next.fetch_add(1, std::memory_order_relaxed);
	}

	struct ArchiveState final
	{
		~ArchiveState() noexcept
		{
			if(owns_mapping && mapped_data != nullptr)
			{
				UnmapViewOfFile(mapped_data);
			}
		}

		UniqueHandle file;
		UniqueHandle mapping;
		// Never reused, unlike the state's address: keys the per-thread cache
		// of the last decoded block so a new archive cannot hit a stale entry.
		std::uint64_t            id           = NextArchiveId();
		const std::byte         *mapped_data  = nullptr;
		bool                     owns_mapping = true;
		std::uint64_t            file_size    = 0;
		std::vector<EntryRecord> entries;
		std::vector<BlockRecord> blocks;
		std::string              paths;
	};
}   // namespace pak::detail
