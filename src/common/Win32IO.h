// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gaëtan Dezeiraud

#pragma once

#include "common/UniqueHandle.h"
#include "pak/Result.h"

#include <cstdint>
#include <filesystem>

namespace pak::detail
{
	[[nodiscard]] Error Win32Error(ErrorCode code = ErrorCode::io_error, std::uint64_t offset = 0) noexcept;

	[[nodiscard]] Result<UniqueHandle> OpenReadFile(const std::filesystem::path &path, DWORD flags = FILE_ATTRIBUTE_NORMAL) noexcept;

	[[nodiscard]] Result<std::uint64_t> FileSize(HANDLE file) noexcept;
}   // namespace pak::detail
