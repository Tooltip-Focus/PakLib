// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gaëtan Dezeiraud

#pragma once

#include <cstdint>

namespace pak
{
	enum class ErrorCode : std::uint16_t
	{
		none = 0,
		invalid_argument,
		not_found,
		already_exists,
		invalid_format,
		unsupported_version,
		unsupported_compression,
		not_mappable,
		out_of_range,
		io_error,
		corrupted_data,
		not_finalized,
		already_finalized
	};

	struct Error final
	{
		ErrorCode     code        = ErrorCode::none;
		std::uint32_t native_code = 0;
		std::uint64_t file_offset = 0;

		[[nodiscard]] constexpr explicit operator bool() const noexcept
		{
			return code != ErrorCode::none;
		}
	};

	[[nodiscard]] constexpr const char *ErrorMessage(ErrorCode code) noexcept
	{
		switch(code)
		{
			case ErrorCode::none:
				return "no error";
			case ErrorCode::invalid_argument:
				return "invalid argument";
			case ErrorCode::not_found:
				return "not found";
			case ErrorCode::already_exists:
				return "already exists";
			case ErrorCode::invalid_format:
				return "invalid PAK format";
			case ErrorCode::unsupported_version:
				return "unsupported PAK version";
			case ErrorCode::unsupported_compression:
				return "unsupported compression";
			case ErrorCode::not_mappable:
				return "entry cannot be memory mapped";
			case ErrorCode::out_of_range:
				return "offset is out of range";
			case ErrorCode::io_error:
				return "I/O error";
			case ErrorCode::corrupted_data:
				return "corrupted data";
			case ErrorCode::not_finalized:
				return "archive is not finalized";
			case ErrorCode::already_finalized:
				return "archive is already finalized";
		}
		return "unknown error";
	}
}   // namespace pak
