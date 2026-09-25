// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gaëtan Dezeiraud

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace pak::tests
{
	class TemporaryDirectory final
	{
	public:
		TemporaryDirectory()
		{
			m_path = std::filesystem::current_path() / ("PakTests-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
			std::filesystem::create_directories(m_path);
		}

		TemporaryDirectory(const TemporaryDirectory &)            = delete;
		TemporaryDirectory &operator=(const TemporaryDirectory &) = delete;

		~TemporaryDirectory() noexcept
		{
			try
			{
				std::error_code error;
				std::filesystem::remove_all(m_path, error);
			}
			catch(...)
			{
				std::fputs("PakLib test cleanup failed.\n", stderr);
			}
		}

		[[nodiscard]] const std::filesystem::path &Path() const noexcept
		{
			return m_path;
		}

	private:
		std::filesystem::path m_path;
	};

	[[nodiscard]] inline std::vector<std::byte> MakeData(std::size_t size, std::uint32_t seed = 0x12345678u)
	{
		std::vector<std::byte> data(size);
		auto                   value = seed;
		for(auto &byte : data)
		{
			value = value * 1664525u + 1013904223u;
			byte  = static_cast<std::byte>(value >> 24u);
		}
		return data;
	}

	inline void WriteBytes(const std::filesystem::path &path, const std::vector<std::byte> &bytes)
	{
		std::ofstream output(path, std::ios::binary);
		output.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
	}

	[[nodiscard]] inline std::vector<std::byte> ReadBytes(const std::filesystem::path &path)
	{
		std::ifstream input(path, std::ios::binary | std::ios::ate);
		const auto    size = input.tellg();
		input.seekg(0);
		std::vector<std::byte> bytes(static_cast<std::size_t>(size));
		input.read(reinterpret_cast<char *>(bytes.data()), size);
		return bytes;
	}
}   // namespace pak::tests
