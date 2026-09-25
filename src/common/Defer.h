// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gaëtan Dezeiraud

#pragma once

#include <type_traits>
#include <utility>

namespace pak::detail
{
	template<typename Lambda>
	class Defer final
	{
	public:
		explicit Defer(Lambda lambda) noexcept
		    : m_lambda(std::move(lambda))
		{
			static_assert(std::is_nothrow_invocable_v<Lambda &>, "Defer callback must be noexcept");
		}

		Defer(const Defer &)            = delete;
		Defer &operator=(const Defer &) = delete;
		Defer(Defer &&)                 = delete;
		Defer &operator=(Defer &&)      = delete;

		~Defer() noexcept
		{
			if(m_active)
			{
				m_lambda();
			}
		}

		void Release() noexcept
		{
			m_active = false;
		}

	private:
		Lambda m_lambda;
		bool   m_active = true;
	};

	template<typename Lambda>
	Defer(Lambda) -> Defer<Lambda>;
}   // namespace pak::detail
