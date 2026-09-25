// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gaëtan Dezeiraud

#pragma once

#include "pak/Error.h"

#include <cassert>
#include <new>
#include <type_traits>
#include <utility>

namespace pak
{
	template<typename T>
	class [[nodiscard]] Result final
	{
		static_assert(!std::is_reference_v<T>);
		static_assert(std::is_nothrow_destructible_v<T>);
		static_assert(std::is_nothrow_move_constructible_v<T>);

	public:
		Result(T value) noexcept
		    : m_hasValue(true)
		{
			new(&m_storage.m_value) T(std::move(value));
		}

		Result(Error error) noexcept
		    : m_hasValue(false)
		{
			new(&m_storage.m_error) Error(error);
		}

		Result(Result &&other) noexcept
		    : m_hasValue(other.m_hasValue)
		{
			if(m_hasValue)
			{
				new(&m_storage.m_value) T(std::move(other.m_storage.m_value));
			}
			else
			{
				new(&m_storage.m_error) Error(other.m_storage.m_error);
			}
		}

		Result &operator=(Result &&other) noexcept
		{
			if(this == &other)
			{
				return *this;
			}

			Destroy();
			m_hasValue = other.m_hasValue;
			if(m_hasValue)
			{
				new(&m_storage.m_value) T(std::move(other.m_storage.m_value));
			}
			else
			{
				new(&m_storage.m_error) Error(other.m_storage.m_error);
			}
			return *this;
		}

		Result(const Result &)            = delete;
		Result &operator=(const Result &) = delete;

		~Result() noexcept
		{
			Destroy();
		}

		[[nodiscard]] explicit operator bool() const noexcept
		{
			return m_hasValue;
		}

		[[nodiscard]] bool HasValue() const noexcept
		{
			return m_hasValue;
		}

		[[nodiscard]] T &Value() & noexcept
		{
			assert(m_hasValue);
			return m_storage.m_value;
		}

		[[nodiscard]] const T &Value() const & noexcept
		{
			assert(m_hasValue);
			return m_storage.m_value;
		}

		[[nodiscard]] T &&Value() && noexcept
		{
			assert(m_hasValue);
			return std::move(m_storage.m_value);
		}

		[[nodiscard]] Error GetError() const noexcept
		{
			assert(!m_hasValue);
			return m_storage.m_error;
		}

	private:
		union Storage
		{
			T     m_value;
			Error m_error;

			Storage() noexcept { }
			~Storage() noexcept { }
		} m_storage;

		bool m_hasValue;

		void Destroy() noexcept
		{
			if(m_hasValue)
			{
				m_storage.m_value.~T();
			}
			else
			{
				m_storage.m_error.~Error();
			}
		}
	};

	template<>
	class [[nodiscard]] Result<void> final
	{
	public:
		Result() noexcept = default;

		Result(Error error) noexcept
		    : m_error(error)
		{
		}

		[[nodiscard]] explicit operator bool() const noexcept
		{
			return m_error.code == ErrorCode::none;
		}

		[[nodiscard]] bool HasValue() const noexcept
		{
			return m_error.code == ErrorCode::none;
		}

		[[nodiscard]] Error GetError() const noexcept
		{
			assert(m_error.code != ErrorCode::none);
			return m_error;
		}

	private:
		Error m_error {};
	};
}   // namespace pak
