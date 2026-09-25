// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gaëtan Dezeiraud

#pragma once

#include <cstdint>

namespace pak
{
	inline constexpr std::uint32_t format_version = 1;

	enum class Compression : std::uint8_t
	{
		none        = 0,
		zstd_blocks = 1
	};
}   // namespace pak
