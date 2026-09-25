// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gaëtan Dezeiraud

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <utility>

namespace pak::detail
{
	class UniqueHandle final
	{
	public:
		UniqueHandle() noexcept = default;

		explicit UniqueHandle(HANDLE handle) noexcept
		    : m_handle(handle)
		{
		}

		UniqueHandle(UniqueHandle &&other) noexcept
		    : m_handle(other.Release())
		{
		}

		UniqueHandle &operator=(UniqueHandle &&other) noexcept
		{
			if(this != &other)
			{
				Reset(other.Release());
			}
			return *this;
		}

		UniqueHandle(const UniqueHandle &)            = delete;
		UniqueHandle &operator=(const UniqueHandle &) = delete;

		~UniqueHandle() noexcept
		{
			Reset();
		}

		[[nodiscard]] HANDLE Get() const noexcept
		{
			return m_handle;
		}

		[[nodiscard]] explicit operator bool() const noexcept
		{
			return m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE;
		}

		[[nodiscard]] HANDLE Release() noexcept
		{
			return std::exchange(m_handle, nullptr);
		}

		void Reset(HANDLE handle = nullptr) noexcept
		{
			if(*this)
			{
				CloseHandle(m_handle);
			}
			m_handle = handle;
		}

	private:
		HANDLE m_handle = nullptr;
	};
}   // namespace pak::detail
