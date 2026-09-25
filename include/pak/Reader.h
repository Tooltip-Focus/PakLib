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
#include <string_view>

namespace pak
{
	namespace detail
	{
		struct ArchiveState;
	}

	struct ReaderOptions final
	{
		bool verify_index = true;
	};

	struct EntryInfo final
	{
		std::string_view path;
		std::uint64_t    size        = 0;
		std::uint64_t    stored_size = 0;
		Compression      compression = Compression::none;
	};

	class MappedView final
	{
	public:
		MappedView() noexcept = default;
		MappedView(MappedView &&other) noexcept;
		MappedView &operator=(MappedView &&other) noexcept;
		MappedView(const MappedView &)            = delete;
		MappedView &operator=(const MappedView &) = delete;
		~MappedView() noexcept;

		[[nodiscard]] std::span<const std::byte> Bytes() const noexcept;
		[[nodiscard]] explicit                   operator bool() const noexcept;

	private:
		friend class File;

		MappedView(std::shared_ptr<const detail::ArchiveState> state, const std::byte *data, std::size_t size) noexcept;

		std::shared_ptr<const detail::ArchiveState> m_state;
		const std::byte                            *m_data = nullptr;
		std::size_t                                 m_size = 0;
	};

	class Cursor;

	class File final
	{
	public:
		File() noexcept = default;

		[[nodiscard]] std::string_view Path() const noexcept;
		[[nodiscard]] std::uint64_t    Size() const noexcept;
		[[nodiscard]] std::uint64_t    StoredSize() const noexcept;
		[[nodiscard]] Compression      GetCompression() const noexcept;

		[[nodiscard]] Result<std::size_t> ReadAt(std::uint64_t offset, std::span<std::byte> destination) const noexcept;

		[[nodiscard]] Result<MappedView> Map(std::uint64_t offset, std::size_t length) const noexcept;

		[[nodiscard]] Result<void> Verify() const noexcept;
		[[nodiscard]] Cursor       CreateCursor() const noexcept;

	private:
		friend class Archive;

		File(std::shared_ptr<const detail::ArchiveState> state, std::size_t entry_index) noexcept;

		std::shared_ptr<const detail::ArchiveState> m_state;
		std::size_t                                 m_entryIndex = 0;
	};

	class Cursor final
	{
	public:
		Cursor() noexcept = default;

		[[nodiscard]] Result<std::size_t> Read(std::span<std::byte> destination) noexcept;

		[[nodiscard]] Result<void> Seek(std::uint64_t offset) noexcept;

		[[nodiscard]] std::uint64_t Tell() const noexcept;
		[[nodiscard]] std::uint64_t Remaining() const noexcept;

	private:
		friend class File;

		explicit Cursor(File file) noexcept;

		File          m_file;
		std::uint64_t m_position = 0;
	};

	class Archive final
	{
	public:
		Archive() noexcept = default;

		[[nodiscard]] static Result<Archive> Open(const std::filesystem::path &path, ReaderOptions options = {}) noexcept;
		/**
		 * @brief Opens an archive without copying caller-owned memory.
		 * @param bytes Immutable archive bytes. The storage must remain valid
		 * and unchanged until the archive and every File, Cursor, and
		 * MappedView obtained from it have been destroyed.
		 * @param options Reader validation options.
		 */
		[[nodiscard]] static Result<Archive> OpenMemory(std::span<const std::byte> bytes, ReaderOptions options = {}) noexcept;

		[[nodiscard]] Result<File>      Find(std::string_view path) const noexcept;
		[[nodiscard]] bool              Contains(std::string_view path) const noexcept;
		[[nodiscard]] std::size_t       FileCount() const noexcept;
		[[nodiscard]] Result<EntryInfo> Entry(std::size_t index) const noexcept;

	private:
		explicit Archive(std::shared_ptr<detail::ArchiveState> state) noexcept;

		std::shared_ptr<detail::ArchiveState> m_state;
	};
}   // namespace pak
