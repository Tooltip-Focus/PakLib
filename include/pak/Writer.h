// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gaëtan Dezeiraud

#pragma once

#include "pak/Format.h"
#include "pak/Result.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace pak
{
	enum class CompressionPolicy : std::uint8_t
	{
		none,
		zstd,
		automatic
	};

	struct WriterOptions final
	{
		std::uint32_t block_size       = 256u * 1024u;
		int           zstd_level       = 3;
		float         minimum_saving   = 0.02f;
		std::uint32_t data_alignment   = 16;
		bool          checksum_entries = true;
	};

	struct FileOptions final
	{
		CompressionPolicy compression = CompressionPolicy::automatic;
	};

	struct SourceFile final
	{
		std::filesystem::path source;
		std::string archive_path;
		FileOptions options = {};
	};

	namespace detail
	{
		struct WriterState;
	}

	/**
	 * @brief Builds one archive transactionally.
	 *
	 * An unfinished writer removes its temporary file on destruction. A writer
	 * is not safe for concurrent calls.
	 */
	class ArchiveWriter final
	{
	public:
		ArchiveWriter() noexcept = default;
		ArchiveWriter(ArchiveWriter &&) noexcept;
		ArchiveWriter &operator=(ArchiveWriter &&) noexcept;
		ArchiveWriter(const ArchiveWriter &)            = delete;
		ArchiveWriter &operator=(const ArchiveWriter &) = delete;
		~ArchiveWriter() noexcept;

		[[nodiscard]] static Result<ArchiveWriter> Create(const std::filesystem::path &output, WriterOptions options = {}) noexcept;

		[[nodiscard]] Result<void> AddFile(const std::filesystem::path &source, std::string_view archive_path, FileOptions options = {}) noexcept;

		// Compresses independent files and their blocks concurrently, then appends
		// them in input order: the archive is byte-identical to AddFile calls in
		// the same order. A worker count of zero uses every available processor.
		// A rejected batch (invalid or duplicate path) writes nothing; any later
		// failure leaves the writer failed, to be aborted.
		[[nodiscard]] Result<void> AddFilesParallel(std::span<const SourceFile> files, std::uint32_t worker_count = 0) noexcept;

		[[nodiscard]] Result<void> AddBytes(std::string_view archive_path, std::span<const std::byte> bytes, FileOptions options = {}) noexcept;

		[[nodiscard]] Result<void> Finalize() noexcept;
		[[nodiscard]] Result<void> Abort() noexcept;

	private:
		explicit ArchiveWriter(std::unique_ptr<detail::WriterState> state) noexcept;

		std::unique_ptr<detail::WriterState> m_state;
	};
}   // namespace pak
