// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gaëtan Dezeiraud

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

	typedef struct pak_archive_handle pak_archive_handle;
	typedef struct pak_file_handle    pak_file_handle;
	typedef struct pak_writer_handle  pak_writer_handle;

	typedef struct pak_error
	{
		uint16_t code;
		uint16_t reserved;
		uint32_t native_code;
		uint64_t file_offset;
	} pak_error;

	typedef struct pak_writer_options
	{
		uint32_t block_size;
		int32_t  zstd_level;
		float    minimum_saving;
		uint32_t data_alignment;
		uint8_t  checksum_entries;
		uint8_t  reserved[3];
	} pak_writer_options;

	enum pak_compression_policy
	{
		PAK_COMPRESSION_NONE      = 0,
		PAK_COMPRESSION_ZSTD      = 1,
		PAK_COMPRESSION_AUTOMATIC = 2
	};

	/**
	 * @brief Opens an archive backed by caller-owned immutable memory.
	 * @param data Pointer to @p size archive bytes.
	 * @param size Archive size in bytes.
	 * @param out_archive Receives a handle destroyed with pak_archive_destroy.
	 * @param out_error Optional detailed error output.
	 *
	 * The memory must remain valid and unchanged until the archive handle and all
	 * file handles opened from it have been destroyed.
	 */
	uint16_t pak_archive_open_memory(const void *data, size_t size, pak_archive_handle **out_archive, pak_error *out_error);

	void pak_archive_destroy(pak_archive_handle *archive);

	/** @brief Opens a file handle that remains valid after the archive is destroyed. */
	uint16_t pak_archive_open_file(pak_archive_handle *archive, const char *path_utf8, size_t path_size, pak_file_handle **out_file, uint64_t *out_size, pak_error *out_error);

	void pak_file_destroy(pak_file_handle *file);

	uint16_t pak_file_read(pak_file_handle *file, uint64_t offset, void *destination, size_t destination_size, size_t *out_read, pak_error *out_error);

	/**
	 * @brief Creates a transactional archive writer.
	 * @param options Optional settings; pass NULL to use the C++ defaults.
	 */
	uint16_t pak_writer_create(const wchar_t *output_path, const pak_writer_options *options, pak_writer_handle **out_writer, pak_error *out_error);

	uint16_t pak_writer_add_file(pak_writer_handle *writer, const wchar_t *source_path, const char *archive_path_utf8, size_t archive_path_size, uint8_t compression_policy, pak_error *out_error);

	uint16_t pak_writer_finalize(pak_writer_handle *writer, pak_error *out_error);

	/** @brief Destroys the writer and aborts an unfinished transaction. */
	void pak_writer_destroy(pak_writer_handle *writer);

	const char *pak_error_message(uint16_t code);

#ifdef __cplusplus
}
#endif
