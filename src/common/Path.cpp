// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gaëtan Dezeiraud

#include "common/Path.h"

#include <cstddef>

namespace pak::detail
{
	Result<std::string> NormalizePath(std::string_view path) noexcept
	{
		if(path.empty() || path.front() == '/' || path.front() == '\\')
		{
			return Error {ErrorCode::invalid_argument};
		}

		std::string result;
		result.reserve(path.size());

		std::size_t component_start = 0;
		for(std::size_t index = 0; index <= path.size(); ++index)
		{
			const bool end       = index == path.size();
			const char character = end ? '/' : path[index];

			if(!end && character == '\0')
			{
				return Error {ErrorCode::invalid_argument};
			}

			if(character != '/' && character != '\\')
			{
				continue;
			}

			const auto component = path.substr(component_start, index - component_start);
			component_start      = index + 1;

			if(component.empty() || component == ".")
			{
				continue;
			}
			if(component == ".." || component.find(':') != std::string_view::npos)
			{
				return Error {ErrorCode::invalid_argument};
			}

			if(!result.empty())
			{
				result.push_back('/');
			}
			result.append(component);
		}

		if(result.empty())
		{
			return Error {ErrorCode::invalid_argument};
		}
		return result;
	}
}   // namespace pak::detail
