// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gaëtan Dezeiraud

#include "pak/CAbi.h"

#include "pak/Error.h"
#include "pak/Reader.h"
#include "pak/Writer.h"

#include <cstddef>
#include <filesystem>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

struct pak_archive_handle
{
	pak::Archive value;
};

struct pak_file_handle
{
	pak::File value;
};

struct pak_writer_handle
{
	pak::ArchiveWriter value;
};

static_assert(sizeof(pak_error) == 16);
static_assert(sizeof(pak_writer_options) == 20);
static_assert(sizeof(pak_source_file) == 32);

namespace
{
	uint16_t Fail(pak::Error error, pak_error *output) noexcept
	{
		if(output != nullptr)
		{
			output->code        = static_cast<uint16_t>(error.code);
			output->reserved    = 0;
			output->native_code = error.native_code;
			output->file_offset = error.file_offset;
		}
		return static_cast<uint16_t>(error.code);
	}

	uint16_t Invalid(pak_error *output) noexcept
	{
		return Fail(pak::Error {pak::ErrorCode::invalid_argument}, output);
	}

	uint16_t Succeed(pak_error *output) noexcept
	{
		if(output != nullptr)
		{
			*output = {};
		}
		return 0;
	}

	uint16_t Complete(const pak::Result<void> &result, pak_error *output) noexcept
	{
		return result ? Succeed(output) : Fail(result.GetError(), output);
	}

	// Moves a successful result into a new C handle.
	template<typename Handle, typename Value>
	uint16_t Publish(pak::Result<Value> &result, Handle **out_handle, pak_error *output) noexcept
	{
		if(!result)
		{
			return Fail(result.GetError(), output);
		}
		auto *handle = new(std::nothrow) Handle {std::move(result.Value())};
		if(handle == nullptr)
		{
			return Fail(pak::Error {pak::ErrorCode::io_error}, output);
		}
		*out_handle = handle;
		return Succeed(output);
	}

	[[nodiscard]] bool IsValidSource(const wchar_t *source_path, const char *archive_path_utf8, uint8_t compression_policy) noexcept
	{
		return source_path != nullptr && archive_path_utf8 != nullptr && compression_policy <= PAK_COMPRESSION_AUTOMATIC;
	}

	[[nodiscard]] pak::FileOptions ToFileOptions(uint8_t compression_policy) noexcept
	{
		return pak::FileOptions {static_cast<pak::CompressionPolicy>(compression_policy)};
	}
}   // namespace

extern "C" uint16_t pak_archive_open_memory(const void *data, size_t size, pak_archive_handle **out_archive, pak_error *out_error)
{
	if(out_archive != nullptr)
	{
		*out_archive = nullptr;
	}
	if(data == nullptr || out_archive == nullptr)
	{
		return Invalid(out_error);
	}
	auto opened = pak::Archive::OpenMemory({static_cast<const std::byte *>(data), size});
	return Publish(opened, out_archive, out_error);
}

extern "C" void pak_archive_destroy(pak_archive_handle *archive)
{
	delete archive;
}

extern "C" uint16_t pak_archive_open_file(pak_archive_handle *archive, const char *path_utf8, size_t path_size, pak_file_handle **out_file, uint64_t *out_size, pak_error *out_error)
{
	if(out_file != nullptr)
	{
		*out_file = nullptr;
	}
	if(out_size != nullptr)
	{
		*out_size = 0;
	}
	if(archive == nullptr || path_utf8 == nullptr || out_file == nullptr)
	{
		return Invalid(out_error);
	}
	auto       found  = archive->value.Find({path_utf8, path_size});
	const auto result = Publish(found, out_file, out_error);
	if(result == 0 && out_size != nullptr)
	{
		*out_size = (*out_file)->value.Size();
	}
	return result;
}

extern "C" void pak_file_destroy(pak_file_handle *file)
{
	delete file;
}

extern "C" uint16_t pak_file_read(pak_file_handle *file, uint64_t offset, void *destination, size_t destination_size, size_t *out_read, pak_error *out_error)
{
	if(file == nullptr || out_read == nullptr || (destination == nullptr && destination_size != 0))
	{
		return Invalid(out_error);
	}
	*out_read = 0;
	auto read = file->value.ReadAt(offset, {static_cast<std::byte *>(destination), destination_size});
	if(!read)
	{
		return Fail(read.GetError(), out_error);
	}
	*out_read = read.Value();
	return Succeed(out_error);
}

extern "C" uint16_t pak_writer_create(const wchar_t *output_path, const pak_writer_options *options, pak_writer_handle **out_writer, pak_error *out_error)
{
	if(out_writer != nullptr)
	{
		*out_writer = nullptr;
	}
	if(output_path == nullptr || out_writer == nullptr)
	{
		return Invalid(out_error);
	}
	pak::WriterOptions cpp_options;
	if(options != nullptr)
	{
		cpp_options.block_size       = options->block_size;
		cpp_options.zstd_level       = options->zstd_level;
		cpp_options.minimum_saving   = options->minimum_saving;
		cpp_options.data_alignment   = options->data_alignment;
		cpp_options.checksum_entries = options->checksum_entries != 0;
	}
	auto created = pak::ArchiveWriter::Create(std::filesystem::path {output_path}, cpp_options);
	return Publish(created, out_writer, out_error);
}

extern "C" uint16_t pak_writer_add_file(pak_writer_handle *writer, const wchar_t *source_path, const char *archive_path_utf8, size_t archive_path_size, uint8_t compression_policy, pak_error *out_error)
{
	if(writer == nullptr || !IsValidSource(source_path, archive_path_utf8, compression_policy))
	{
		return Invalid(out_error);
	}
	return Complete(writer->value.AddFile(std::filesystem::path {source_path}, std::string_view {archive_path_utf8, archive_path_size}, ToFileOptions(compression_policy)),
	                out_error);
}

extern "C" uint16_t pak_writer_add_files_parallel(pak_writer_handle *writer, const pak_source_file *files, size_t file_count, uint32_t worker_count, pak_error *out_error)
{
	if(writer == nullptr || (files == nullptr && file_count != 0))
	{
		return Invalid(out_error);
	}
	std::vector<pak::SourceFile> sources;
	sources.reserve(file_count);
	for(size_t index = 0; index < file_count; ++index)
	{
		const auto &file = files[index];
		if(!IsValidSource(file.source_path, file.archive_path_utf8, file.compression_policy))
		{
			return Invalid(out_error);
		}
		sources.push_back(pak::SourceFile {
			std::filesystem::path {file.source_path},
			std::string {file.archive_path_utf8, file.archive_path_size},
			ToFileOptions(file.compression_policy)});
	}
	return Complete(writer->value.AddFilesParallel(sources, worker_count), out_error);
}

extern "C" uint16_t pak_writer_finalize(pak_writer_handle *writer, pak_error *out_error)
{
	if(writer == nullptr)
	{
		return Invalid(out_error);
	}
	return Complete(writer->value.Finalize(), out_error);
}

extern "C" void pak_writer_destroy(pak_writer_handle *writer)
{
	delete writer;
}

extern "C" const char *pak_error_message(uint16_t code)
{
	return pak::ErrorMessage(static_cast<pak::ErrorCode>(code));
}
