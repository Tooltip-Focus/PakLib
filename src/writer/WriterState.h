// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gaëtan Dezeiraud

#pragma once

#include "common/FormatInternal.h"
#include "common/UniqueHandle.h"
#include "pak/Writer.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace pak::detail
{
	struct BuildEntry final
	{
		EntryRecord      record;
		std::string_view path;
	};

	struct WriterState final
	{
		UniqueHandle                    file;
		std::filesystem::path           destination;
		std::filesystem::path           temporary;
		WriterOptions                   options;
		std::uint64_t                   current_offset = 0;
		std::unordered_set<std::string> paths;
		std::vector<BuildEntry>         entries;
		std::vector<BlockRecord>        blocks;
		bool                            finalized = false;
		bool                            failed    = false;
	};
}   // namespace pak::detail
