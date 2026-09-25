// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gaëtan Dezeiraud

#include "common/Win32IO.h"

namespace pak::detail
{
	Error Win32Error(ErrorCode code, std::uint64_t offset) noexcept
	{
		return Error {code, static_cast<std::uint32_t>(GetLastError()), offset};
	}

	Result<UniqueHandle> OpenReadFile(const std::filesystem::path &path, DWORD flags) noexcept
	{
		UniqueHandle file {CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, flags, nullptr)};
		if(!file)
		{
			const auto native = GetLastError();
			return Error {native == ERROR_FILE_NOT_FOUND || native == ERROR_PATH_NOT_FOUND ? ErrorCode::not_found : ErrorCode::io_error, static_cast<std::uint32_t>(native)};
		}
		return file;
	}

	Result<std::uint64_t> FileSize(HANDLE file) noexcept
	{
		LARGE_INTEGER size {};
		if(!GetFileSizeEx(file, &size))
		{
			return Win32Error();
		}
		if(size.QuadPart < 0)
		{
			return Error {ErrorCode::invalid_format};
		}
		return static_cast<std::uint64_t>(size.QuadPart);
	}
}   // namespace pak::detail
