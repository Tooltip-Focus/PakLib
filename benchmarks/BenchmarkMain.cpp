// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gaëtan Dezeiraud

#include "pak/Reader.h"
#include "pak/Writer.h"

#include <benchmark/benchmark.h>
#include <zip.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace
{
	struct Dataset final
	{
		std::vector<std::filesystem::path> systemPaths;
		std::vector<std::uint64_t>         systemSizes;
		std::vector<pak::File>             rawFiles;
		std::vector<pak::File>             compressedFiles;
		pak::Archive                       rawArchive;
		pak::Archive                       compressedArchive;
		std::uint64_t                      totalBytes         = 0;
		std::uint64_t                      rawPakBytes        = 0;
		std::uint64_t                      compressedPakBytes = 0;
		std::uint64_t                      zipStoreBytes      = 0;
		std::uint64_t                      zipDeflateBytes    = 0;
		std::size_t                        maximumFileSize    = 0;
		std::filesystem::path              zipStorePath;
		std::filesystem::path              zipDeflatePath;
		bool                               skipZip = false;
	};

	Dataset g_dataset;

	[[nodiscard]] std::string ToArchivePath(const std::filesystem::path &path)
	{
		const auto utf8 = path.generic_u8string();
		return {reinterpret_cast<const char *>(utf8.data()), utf8.size()};
	}

	[[nodiscard]] bool BuildArchive(const std::filesystem::path &output, const std::filesystem::path &root, const std::vector<std::filesystem::path> &files, pak::CompressionPolicy compression)
	{
		auto writer_result = pak::ArchiveWriter::Create(output);
		if(!writer_result)
		{
			return false;
		}
		auto            writer = std::move(writer_result.Value());
		std::error_code error;
		for(const auto &file : files)
		{
			const auto relative = std::filesystem::relative(file, root, error);
			if(error || !writer.AddFile(file, ToArchivePath(relative), pak::FileOptions {compression}))
			{
				return false;
			}
		}
		return static_cast<bool>(writer.Finalize());
	}

	[[nodiscard]] bool BuildZip(const std::filesystem::path &output, const std::filesystem::path &root, const std::vector<std::filesystem::path> &files, zip_int32_t compression)
	{
		int        zip_error   = ZIP_ER_OK;
		const auto output_path = ToArchivePath(output);
		zip_t     *archive     = zip_open(output_path.c_str(), ZIP_CREATE | ZIP_TRUNCATE, &zip_error);
		if(archive == nullptr)
		{
			return false;
		}

		std::error_code error;
		for(const auto &file : files)
		{
			const auto relative = std::filesystem::relative(file, root, error);
			if(error)
			{
				zip_discard(archive);
				return false;
			}

			zip_source_t *source = zip_source_win32w(archive, file.c_str(), 0, 0);
			if(source == nullptr)
			{
				zip_discard(archive);
				return false;
			}

			const auto        entry_path = ToArchivePath(relative);
			const zip_int64_t entry      = zip_file_add(archive, entry_path.c_str(), source, ZIP_FL_ENC_UTF_8 | ZIP_FL_OVERWRITE);
			if(entry < 0)
			{
				zip_source_free(source);
				zip_discard(archive);
				return false;
			}
			if(zip_set_file_compression(archive, static_cast<zip_uint64_t>(entry), compression, 0) < 0)
			{
				zip_discard(archive);
				return false;
			}
		}

		if(zip_close(archive) < 0)
		{
			zip_discard(archive);
			return false;
		}
		return true;
	}

	[[nodiscard]] bool ReadZipEntry(zip_t *archive, zip_uint64_t index, std::span<std::byte> destination)
	{
		zip_file_t *file = zip_fopen_index(archive, index, 0);
		if(file == nullptr)
		{
			return false;
		}

		std::size_t offset = 0;
		while(offset < destination.size())
		{
			const zip_int64_t read = zip_fread(file, destination.data() + offset, destination.size() - offset);
			if(read <= 0)
			{
				zip_fclose(file);
				return false;
			}
			offset += static_cast<std::size_t>(read);
		}
		return zip_fclose(file) == 0;
	}

	[[nodiscard]] bool PrepareDataset()
	{
		const wchar_t *configured = _wgetenv(L"PAKLIB_BENCH_ASSET_DIR");
		if(configured == nullptr || *configured == L'\0')
		{
			std::cerr << "Set PAKLIB_BENCH_ASSET_DIR to the 01_01 asset directory.\n";
			return false;
		}
		const std::filesystem::path root {configured};
		std::error_code             error;
		for(std::filesystem::recursive_directory_iterator iterator {root, std::filesystem::directory_options::skip_permission_denied, error}, end; !error && iterator != end; iterator.increment(error))
		{
			if(iterator->is_regular_file(error) && !error)
			{
				g_dataset.systemPaths.push_back(iterator->path());
			}
		}
		if(error || g_dataset.systemPaths.empty())
		{
			return false;
		}
		std::sort(g_dataset.systemPaths.begin(), g_dataset.systemPaths.end());
		for(const auto &path : g_dataset.systemPaths)
		{
			const auto size = std::filesystem::file_size(path, error);
			if(error)
				return false;
			g_dataset.systemSizes.push_back(size);
			g_dataset.totalBytes += size;
			g_dataset.maximumFileSize = std::max(g_dataset.maximumFileSize, static_cast<std::size_t>(size));
		}

		const auto benchmark_directory = std::filesystem::current_path(error) / L"PakBenchData";
		if(error)
		{
			return false;
		}
		std::filesystem::create_directories(benchmark_directory, error);
		if(error)
			return false;
		const auto raw_path        = benchmark_directory / L"AssetsRaw.pak";
		const auto compressed_path = benchmark_directory / L"AssetsZstd.pak";
		g_dataset.zipStorePath     = benchmark_directory / L"AssetsStore.zip";
		g_dataset.zipDeflatePath   = benchmark_directory / L"AssetsDeflate.zip";
		g_dataset.skipZip          = _wgetenv(L"PAKLIB_BENCH_SKIP_ZIP") != nullptr;
		if(!BuildArchive(raw_path, root, g_dataset.systemPaths, pak::CompressionPolicy::none) || !BuildArchive(compressed_path, root, g_dataset.systemPaths, pak::CompressionPolicy::automatic) ||
		   (!g_dataset.skipZip && (!BuildZip(g_dataset.zipStorePath, root, g_dataset.systemPaths, ZIP_CM_STORE) || !BuildZip(g_dataset.zipDeflatePath, root, g_dataset.systemPaths, ZIP_CM_DEFLATE))))
		{
			return false;
		}
		g_dataset.rawPakBytes = std::filesystem::file_size(raw_path, error);
		if(error)
			return false;
		g_dataset.compressedPakBytes = std::filesystem::file_size(compressed_path, error);
		if(error)
			return false;
		if(!g_dataset.skipZip)
		{
			g_dataset.zipStoreBytes = std::filesystem::file_size(g_dataset.zipStorePath, error);
			if(error)
				return false;
			g_dataset.zipDeflateBytes = std::filesystem::file_size(g_dataset.zipDeflatePath, error);
			if(error)
				return false;
		}

		auto raw        = pak::Archive::Open(raw_path);
		auto compressed = pak::Archive::Open(compressed_path);
		if(!raw || !compressed)
		{
			return false;
		}
		g_dataset.rawArchive        = std::move(raw.Value());
		g_dataset.compressedArchive = std::move(compressed.Value());
		for(std::size_t index = 0; index < g_dataset.rawArchive.FileCount(); ++index)
		{
			auto info = g_dataset.rawArchive.Entry(index);
			if(!info)
				return false;
			auto raw_file        = g_dataset.rawArchive.Find(info.Value().path);
			auto compressed_file = g_dataset.compressedArchive.Find(info.Value().path);
			if(!raw_file || !compressed_file)
				return false;
			g_dataset.rawFiles.push_back(std::move(raw_file.Value()));
			g_dataset.compressedFiles.push_back(std::move(compressed_file.Value()));
		}
		return true;
	}

	void SetThroughput(benchmark::State &state, std::uint64_t files, std::uint64_t bytes)
	{
		state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * files));
		state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations() * bytes));
	}

	void Touch(std::span<const std::byte> bytes)
	{
		for(std::size_t offset = 0; offset < bytes.size(); offset += 4096)
		{
			benchmark::DoNotOptimize(bytes[offset]);
		}
		if(!bytes.empty())
			benchmark::DoNotOptimize(bytes.back());
	}

	void Scan(std::span<const std::byte> bytes)
	{
		std::uint64_t checksum = 0;
		for(const auto value : bytes)
		{
			checksum = checksum * 33u + std::to_integer<std::uint8_t>(value);
		}
		benchmark::DoNotOptimize(checksum);
	}

	[[nodiscard]] std::uint64_t PartialBytes() noexcept
	{
		std::uint64_t result = 0;
		for(const auto &file : g_dataset.rawFiles)
		{
			result += std::min<std::uint64_t>(4096, file.Size());
		}
		return result;
	}

	void BM_SystemOpenReadClose(benchmark::State &state)
	{
		std::vector<std::byte> buffer(g_dataset.maximumFileSize);
		for(auto _ : state)
		{
			for(const auto &path : g_dataset.systemPaths)
			{
				HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
				if(file == INVALID_HANDLE_VALUE)
				{
					state.SkipWithError("CreateFileW failed");
					return;
				}
				LARGE_INTEGER size {};
				GetFileSizeEx(file, &size);
				DWORD      bytes_read = 0;
				const auto requested  = static_cast<DWORD>(size.QuadPart);
				const BOOL success    = ReadFile(file, buffer.data(), requested, &bytes_read, nullptr);
				CloseHandle(file);
				if(!success || bytes_read != requested)
				{
					state.SkipWithError("ReadFile failed");
					return;
				}
				benchmark::DoNotOptimize(buffer.data());
			}
		}
		SetThroughput(state, g_dataset.systemPaths.size(), g_dataset.totalBytes);
	}

	void BM_PakReadAtRaw(benchmark::State &state)
	{
		std::vector<std::byte> buffer(g_dataset.maximumFileSize);
		for(auto _ : state)
		{
			for(const auto &file : g_dataset.rawFiles)
			{
				auto read = file.ReadAt(0, std::span<std::byte> {buffer}.first(static_cast<std::size_t>(file.Size())));
				if(!read)
				{
					state.SkipWithError("PAK ReadAt failed");
					return;
				}
				benchmark::DoNotOptimize(buffer.data());
			}
		}
		SetThroughput(state, g_dataset.rawFiles.size(), g_dataset.totalBytes);
	}

	void BM_PakMapRaw(benchmark::State &state)
	{
		for(auto _ : state)
		{
			for(const auto &file : g_dataset.rawFiles)
			{
				auto view = file.Map(0, static_cast<std::size_t>(file.Size()));
				if(!view)
				{
					state.SkipWithError("PAK Map failed");
					return;
				}
				Touch(view.Value().Bytes());
			}
		}
		SetThroughput(state, g_dataset.rawFiles.size(), g_dataset.totalBytes);
	}

	void BM_PakMapAndScanRaw(benchmark::State &state)
	{
		for(auto _ : state)
		{
			for(const auto &file : g_dataset.rawFiles)
			{
				auto view = file.Map(0, static_cast<std::size_t>(file.Size()));
				if(!view)
				{
					state.SkipWithError("PAK Map failed");
					return;
				}
				Scan(view.Value().Bytes());
			}
		}
		SetThroughput(state, g_dataset.rawFiles.size(), g_dataset.totalBytes);
	}

	void BM_SystemMapAndCopy(benchmark::State &state)
	{
		std::vector<std::byte> staging_buffer(g_dataset.maximumFileSize);
		for(auto _ : state)
		{
			for(const auto &path : g_dataset.systemPaths)
			{
				HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
				if(file == INVALID_HANDLE_VALUE)
				{
					state.SkipWithError("CreateFileW failed");
					return;
				}

				LARGE_INTEGER size {};
				if(!GetFileSizeEx(file, &size))
				{
					CloseHandle(file);
					state.SkipWithError("GetFileSizeEx failed");
					return;
				}
				if(size.QuadPart == 0)
				{
					CloseHandle(file);
					continue;
				}

				HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
				if(mapping == nullptr)
				{
					CloseHandle(file);
					state.SkipWithError("CreateFileMappingW failed");
					return;
				}
				const void *source = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
				if(source == nullptr)
				{
					CloseHandle(mapping);
					CloseHandle(file);
					state.SkipWithError("MapViewOfFile failed");
					return;
				}

				std::memcpy(staging_buffer.data(), source, static_cast<std::size_t>(size.QuadPart));
				benchmark::ClobberMemory();
				UnmapViewOfFile(source);
				CloseHandle(mapping);
				CloseHandle(file);
			}
		}
		SetThroughput(state, g_dataset.systemPaths.size(), g_dataset.totalBytes);
	}

	void BM_PakMapAndCopyRaw(benchmark::State &state)
	{
		std::vector<std::byte> staging_buffer(g_dataset.maximumFileSize);
		for(auto _ : state)
		{
			for(const auto &file : g_dataset.rawFiles)
			{
				auto view = file.Map(0, static_cast<std::size_t>(file.Size()));
				if(!view)
				{
					state.SkipWithError("PAK Map failed");
					return;
				}
				const auto bytes = view.Value().Bytes();
				std::memcpy(staging_buffer.data(), bytes.data(), bytes.size());
				benchmark::ClobberMemory();
			}
		}
		SetThroughput(state, g_dataset.rawFiles.size(), g_dataset.totalBytes);
	}

	void BM_SystemPartial4K(benchmark::State &state)
	{
		std::vector<std::byte> buffer(4096);
		for(auto _ : state)
		{
			for(const auto &path : g_dataset.systemPaths)
			{
				HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
				if(file == INVALID_HANDLE_VALUE)
				{
					state.SkipWithError("CreateFileW failed");
					return;
				}
				DWORD      bytes_read = 0;
				const BOOL success    = ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &bytes_read, nullptr);
				CloseHandle(file);
				if(!success)
				{
					state.SkipWithError("ReadFile failed");
					return;
				}
				benchmark::DoNotOptimize(buffer.data());
			}
		}
		SetThroughput(state, g_dataset.systemPaths.size(), PartialBytes());
	}

	void BM_PakReadAtPartial4K(benchmark::State &state)
	{
		std::vector<std::byte> buffer(4096);
		for(auto _ : state)
		{
			for(const auto &file : g_dataset.rawFiles)
			{
				const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), file.Size()));
				auto       read  = file.ReadAt(0, std::span<std::byte> {buffer}.first(count));
				if(!read)
				{
					state.SkipWithError("PAK partial ReadAt failed");
					return;
				}
				benchmark::DoNotOptimize(buffer.data());
			}
		}
		SetThroughput(state, g_dataset.rawFiles.size(), PartialBytes());
	}

	void BM_PakMapAndCopyPartial4K(benchmark::State &state)
	{
		std::vector<std::byte> staging_buffer(4096);
		for(auto _ : state)
		{
			for(const auto &file : g_dataset.rawFiles)
			{
				const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(4096, file.Size()));
				auto       view  = file.Map(0, count);
				if(!view)
				{
					state.SkipWithError("PAK partial Map failed");
					return;
				}
				const auto bytes = view.Value().Bytes();
				std::memcpy(staging_buffer.data(), bytes.data(), bytes.size());
				benchmark::ClobberMemory();
			}
		}
		SetThroughput(state, g_dataset.rawFiles.size(), PartialBytes());
	}

	void BM_PakReadAtZstd(benchmark::State &state)
	{
		std::vector<std::byte> buffer(g_dataset.maximumFileSize);
		for(auto _ : state)
		{
			for(const auto &file : g_dataset.compressedFiles)
			{
				auto read = file.ReadAt(0, std::span<std::byte> {buffer}.first(static_cast<std::size_t>(file.Size())));
				if(!read)
				{
					state.SkipWithError("compressed PAK ReadAt failed");
					return;
				}
				benchmark::DoNotOptimize(buffer.data());
			}
		}
		SetThroughput(state, g_dataset.compressedFiles.size(), g_dataset.totalBytes);
	}

	void BM_PakReadAtZstdPartial4K(benchmark::State &state)
	{
		std::vector<std::byte> buffer(4096);
		for(auto _ : state)
		{
			for(const auto &file : g_dataset.compressedFiles)
			{
				const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), file.Size()));
				auto       read  = file.ReadAt(0, std::span<std::byte> {buffer}.first(count));
				if(!read)
				{
					state.SkipWithError("compressed PAK partial ReadAt failed");
					return;
				}
				benchmark::ClobberMemory();
			}
		}
		SetThroughput(state, g_dataset.compressedFiles.size(), PartialBytes());
	}

	void BM_PakReadAtRawThreads(benchmark::State &state)
	{
		std::vector<std::byte> buffer(g_dataset.maximumFileSize);
		std::uint64_t          local_files = 0;
		std::uint64_t          local_bytes = 0;
		for(std::size_t index = state.thread_index(); index < g_dataset.rawFiles.size(); index += state.threads())
		{
			++local_files;
			local_bytes += g_dataset.rawFiles[index].Size();
		}
		for(auto _ : state)
		{
			for(std::size_t index = state.thread_index(); index < g_dataset.rawFiles.size(); index += state.threads())
			{
				const auto &file = g_dataset.rawFiles[index];
				auto        read = file.ReadAt(0, std::span<std::byte> {buffer}.first(static_cast<std::size_t>(file.Size())));
				if(!read)
				{
					state.SkipWithError("parallel PAK ReadAt failed");
					return;
				}
				benchmark::DoNotOptimize(buffer.data());
			}
		}
		SetThroughput(state, local_files, local_bytes);
	}

	void BM_PakMapAndCopyRawThreads(benchmark::State &state)
	{
		std::vector<std::byte> staging_buffer(g_dataset.maximumFileSize);
		std::uint64_t          local_files = 0;
		std::uint64_t          local_bytes = 0;
		for(std::size_t index = state.thread_index(); index < g_dataset.rawFiles.size(); index += state.threads())
		{
			++local_files;
			local_bytes += g_dataset.rawFiles[index].Size();
		}
		for(auto _ : state)
		{
			for(std::size_t index = state.thread_index(); index < g_dataset.rawFiles.size(); index += state.threads())
			{
				const auto &file = g_dataset.rawFiles[index];
				auto        view = file.Map(0, static_cast<std::size_t>(file.Size()));
				if(!view)
				{
					state.SkipWithError("parallel PAK Map failed");
					return;
				}
				const auto bytes = view.Value().Bytes();
				std::memcpy(staging_buffer.data(), bytes.data(), bytes.size());
				benchmark::ClobberMemory();
			}
		}
		SetThroughput(state, local_files, local_bytes);
	}

	void BM_SystemOpenReadCloseThreads(benchmark::State &state)
	{
		std::vector<std::byte> staging_buffer(g_dataset.maximumFileSize);
		std::uint64_t          local_files = 0;
		std::uint64_t          local_bytes = 0;
		for(std::size_t index = state.thread_index(); index < g_dataset.systemPaths.size(); index += state.threads())
		{
			++local_files;
			std::error_code error;
			local_bytes += std::filesystem::file_size(g_dataset.systemPaths[index], error);
			if(error)
			{
				state.SkipWithError("file_size failed");
				return;
			}
		}
		for(auto _ : state)
		{
			for(std::size_t index = state.thread_index(); index < g_dataset.systemPaths.size(); index += state.threads())
			{
				const auto &path = g_dataset.systemPaths[index];
				HANDLE      file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
				if(file == INVALID_HANDLE_VALUE)
				{
					state.SkipWithError("CreateFileW failed");
					return;
				}
				LARGE_INTEGER size {};
				DWORD         bytes_read   = 0;
				const BOOL    size_success = GetFileSizeEx(file, &size);
				const BOOL    read_success = size_success && ReadFile(file, staging_buffer.data(), static_cast<DWORD>(size.QuadPart), &bytes_read, nullptr);
				CloseHandle(file);
				if(!read_success || bytes_read != static_cast<DWORD>(size.QuadPart))
				{
					state.SkipWithError("ReadFile failed");
					return;
				}
				benchmark::ClobberMemory();
			}
		}
		SetThroughput(state, local_files, local_bytes);
	}

	void BM_PakReadAtZstdThreads(benchmark::State &state)
	{
		std::vector<std::byte> staging_buffer(g_dataset.maximumFileSize);
		std::uint64_t          local_files = 0;
		std::uint64_t          local_bytes = 0;
		for(std::size_t index = state.thread_index(); index < g_dataset.compressedFiles.size(); index += state.threads())
		{
			++local_files;
			local_bytes += g_dataset.compressedFiles[index].Size();
		}
		for(auto _ : state)
		{
			for(std::size_t index = state.thread_index(); index < g_dataset.compressedFiles.size(); index += state.threads())
			{
				const auto &file = g_dataset.compressedFiles[index];
				auto        read = file.ReadAt(0, std::span<std::byte> {staging_buffer}.first(static_cast<std::size_t>(file.Size())));
				if(!read)
				{
					state.SkipWithError("parallel compressed PAK ReadAt failed");
					return;
				}
				benchmark::ClobberMemory();
			}
		}
		SetThroughput(state, local_files, local_bytes);
	}

	void BM_ZipThreads(benchmark::State &state, const std::filesystem::path &path)
	{
		if(g_dataset.skipZip)
		{
			state.SkipWithMessage("ZIP benchmarks disabled by PAKLIB_BENCH_SKIP_ZIP");
			return;
		}
		const auto archive_path = ToArchivePath(path);
		int        zip_error    = ZIP_ER_OK;
		zip_t     *archive      = zip_open(archive_path.c_str(), ZIP_RDONLY, &zip_error);
		if(archive == nullptr)
		{
			state.SkipWithError("zip_open failed");
			return;
		}

		std::vector<std::byte> staging_buffer(g_dataset.maximumFileSize);
		std::uint64_t          local_files = 0;
		std::uint64_t          local_bytes = 0;
		for(std::size_t index = state.thread_index(); index < g_dataset.systemSizes.size(); index += state.threads())
		{
			++local_files;
			local_bytes += g_dataset.systemSizes[index];
		}

		for(auto _ : state)
		{
			for(std::size_t index = state.thread_index(); index < g_dataset.systemSizes.size(); index += state.threads())
			{
				const auto size = static_cast<std::size_t>(g_dataset.systemSizes[index]);
				if(!ReadZipEntry(archive, static_cast<zip_uint64_t>(index), std::span<std::byte> {staging_buffer}.first(size)))
				{
					zip_discard(archive);
					state.SkipWithError("ZIP entry read failed");
					return;
				}
				benchmark::ClobberMemory();
			}
		}

		zip_discard(archive);
		SetThroughput(state, local_files, local_bytes);
	}

	void BM_ZipStoreThreads(benchmark::State &state)
	{
		BM_ZipThreads(state, g_dataset.zipStorePath);
	}

	void BM_ZipDeflateThreads(benchmark::State &state)
	{
		BM_ZipThreads(state, g_dataset.zipDeflatePath);
	}
}   // namespace

BENCHMARK(BM_SystemOpenReadClose)->UseRealTime();
BENCHMARK(BM_PakReadAtRaw)->UseRealTime();
BENCHMARK(BM_PakMapRaw)->UseRealTime();
BENCHMARK(BM_PakMapAndScanRaw)->UseRealTime();
BENCHMARK(BM_SystemMapAndCopy)->UseRealTime();
BENCHMARK(BM_PakMapAndCopyRaw)->UseRealTime();
BENCHMARK(BM_PakReadAtZstd)->UseRealTime();
BENCHMARK(BM_PakReadAtZstdPartial4K)->UseRealTime();
BENCHMARK(BM_SystemPartial4K)->UseRealTime();
BENCHMARK(BM_PakReadAtPartial4K)->UseRealTime();
BENCHMARK(BM_PakMapAndCopyPartial4K)->UseRealTime();
BENCHMARK(BM_PakReadAtRawThreads)->ThreadRange(1, 16)->UseRealTime();
BENCHMARK(BM_PakMapAndCopyRawThreads)->ThreadRange(1, 16)->UseRealTime();
BENCHMARK(BM_SystemOpenReadCloseThreads)->ThreadRange(1, 16)->UseRealTime();
BENCHMARK(BM_PakReadAtZstdThreads)->ThreadRange(1, 16)->UseRealTime();
BENCHMARK(BM_ZipStoreThreads)->ThreadRange(1, 16)->UseRealTime();
BENCHMARK(BM_ZipDeflateThreads)->ThreadRange(1, 16)->UseRealTime();

int RunBenchmarks(int argc, char **argv)
{
	if(!PrepareDataset())
	{
		std::cerr << "Failed to prepare benchmark dataset.\n";
		return EXIT_FAILURE;
	}
	std::cout << "Dataset: " << g_dataset.systemPaths.size() << " files, " << g_dataset.totalBytes << " bytes; raw PAK " << g_dataset.rawPakBytes << " bytes; compressed PAK " << g_dataset.compressedPakBytes
	          << " bytes; ZIP Store " << g_dataset.zipStoreBytes << " bytes; ZIP Deflate " << g_dataset.zipDeflateBytes << " bytes.\n";
	benchmark::Initialize(&argc, argv);
	if(benchmark::ReportUnrecognizedArguments(argc, argv))
	{
		return EXIT_FAILURE;
	}
	benchmark::RunSpecifiedBenchmarks();
	benchmark::Shutdown();
	return EXIT_SUCCESS;
}

int main(int argc, char **argv)
{
	try
	{
		return RunBenchmarks(argc, argv);
	}
	catch(const std::exception &exception)
	{
		std::cerr << "Benchmark failed: " << exception.what() << '\n';
	}
	catch(...)
	{
		std::cerr << "Benchmark failed with an unknown exception.\n";
	}

	return EXIT_FAILURE;
}
