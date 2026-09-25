// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gaëtan Dezeiraud

#pragma once

#include "pak/Result.h"

#include <string>
#include <string_view>

namespace pak::detail
{
	[[nodiscard]] Result<std::string> NormalizePath(std::string_view path) noexcept;
}
