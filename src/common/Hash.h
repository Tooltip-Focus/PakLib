// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gaëtan Dezeiraud

#pragma once

#include <xxhash.h>

#include <memory>

namespace pak::detail
{
	struct HashStateDeleter final
	{
		void operator()(XXH3_state_t *state) const noexcept
		{
			XXH3_freeState(state);
		}
	};

	using HashState = std::unique_ptr<XXH3_state_t, HashStateDeleter>;

	// Null when allocation or reset fails.
	[[nodiscard]] inline HashState CreateHashState() noexcept
	{
		HashState state {XXH3_createState()};
		if(state && XXH3_64bits_reset(state.get()) == XXH_ERROR)
		{
			state.reset();
		}
		return state;
	}
}   // namespace pak::detail
