// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gaëtan Dezeiraud

#pragma once

#include "pak/Error.h"

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace pak::detail
{
	inline void AppendBytes(std::vector<std::byte> &output, std::span<const std::byte> bytes)
	{
		output.insert(output.end(), bytes.begin(), bytes.end());
	}

	// Little-endian, whatever the host byte order.
	template<std::unsigned_integral T>
	void Append(std::vector<std::byte> &output, T value)
	{
		for(unsigned shift = 0; shift < sizeof(T) * 8u; shift += 8u)
		{
			output.push_back(static_cast<std::byte>(value >> shift));
		}
	}

	// Once a read runs past the end, every later read fails too and returns
	// zeroes, so callers can read a whole record and check Failed() once.
	class ByteReader final
	{
	public:
		explicit ByteReader(std::span<const std::byte> bytes) noexcept
		    : m_bytes(bytes)
		{
		}

		[[nodiscard]] std::span<const std::byte> ReadBytes(std::size_t count) noexcept
		{
			if(m_failed || count > m_bytes.size() - m_position)
			{
				m_failed = true;
				return {};
			}
			auto result = m_bytes.subspan(m_position, count);
			m_position += count;
			return result;
		}

		void Skip(std::size_t count) noexcept
		{
			static_cast<void>(ReadBytes(count));
		}

		[[nodiscard]] bool ReadMagic(std::span<const std::byte> magic) noexcept
		{
			const auto bytes = ReadBytes(magic.size());
			return !m_failed && std::equal(bytes.begin(), bytes.end(), magic.begin());
		}

		template<std::unsigned_integral T>
		[[nodiscard]] T Read() noexcept
		{
			const auto bytes = ReadBytes(sizeof(T));
			T          value = 0;
			for(std::size_t index = 0; index < bytes.size(); ++index)
			{
				value = static_cast<T>(value | (static_cast<T>(std::to_integer<std::uint8_t>(bytes[index])) << (index * 8u)));
			}
			return value;
		}

		[[nodiscard]] bool Failed() const noexcept
		{
			return m_failed;
		}

		[[nodiscard]] Error GetError() const noexcept
		{
			return Error {ErrorCode::invalid_format, 0, static_cast<std::uint64_t>(m_position)};
		}

	private:
		std::span<const std::byte> m_bytes;
		std::size_t                m_position = 0;
		bool                       m_failed   = false;
	};
}   // namespace pak::detail
