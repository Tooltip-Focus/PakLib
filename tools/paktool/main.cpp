// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gaëtan Dezeiraud

#include "pak/Error.h"
#include "pak/Reader.h"
#include "pak/Writer.h"

#include <CLI/CLI.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace
{
	[[nodiscard]] std::string ToArchivePath(const std::filesystem::path &path)
	{
		const auto utf8 = path.generic_u8string();
		return std::string {reinterpret_cast<const char *>(utf8.data()), utf8.size()};
	}

	[[nodiscard]] bool IsAlreadyCompressed(const std::filesystem::path &path)
	{
		constexpr std::array<std::string_view, 6> extensions {".dds", ".ogg", ".png", ".jpg", ".jpeg", ".ktx2"};
		auto                                      extension = path.extension().string();
		std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
		return std::find(extensions.begin(), extensions.end(), extension) != extensions.end();
	}

	int PrintError(std::string_view operation, pak::Error error)
	{
		std::cerr << operation << ": " << pak::ErrorMessage(error.code);
		if(error.native_code != 0)
		{
			std::cerr << " (Win32 " << error.native_code << ')';
		}
		if(error.file_offset != 0)
		{
			std::cerr << " at offset " << error.file_offset;
		}
		std::cerr << '\n';
		return EXIT_FAILURE;
	}

	int Verify(const std::filesystem::path &path)
	{
		auto archive = pak::Archive::Open(path);
		if(!archive)
		{
			return PrintError("open generated archive", archive.GetError());
		}
		for(std::size_t index = 0; index < archive.Value().FileCount(); ++index)
		{
			auto info = archive.Value().Entry(index);
			if(!info)
			{
				return PrintError("read archive index", info.GetError());
			}
			auto file = archive.Value().Find(info.Value().path);
			if(!file)
			{
				return PrintError("find generated entry", file.GetError());
			}
			if(auto verified = file.Value().Verify(); !verified)
			{
				return PrintError("verify generated entry", verified.GetError());
			}
		}
		return EXIT_SUCCESS;
	}
}   // namespace

int wmain(int argc, wchar_t **argv)
{
	CLI::App app {"Build a PakLib archive"};
	auto    *pack = app.add_subcommand("pack", "Pack a directory into an archive");
	app.require_subcommand(1);

	std::string            input;
	std::string            output;
	pak::CompressionPolicy compression = pak::CompressionPolicy::automatic;
	pak::WriterOptions     writer_options;
	unsigned int           minimum_saving_percent = 2;
	bool                   verify                 = false;

	const std::map<std::string, pak::CompressionPolicy> compression_modes {
	    {"none", pak::CompressionPolicy::none},
	    {"zstd", pak::CompressionPolicy::zstd},
	    {"auto", pak::CompressionPolicy::automatic},
	};
	pack->add_option("--input", input, "Source directory")->required()->check(CLI::ExistingDirectory);
	pack->add_option("--output", output, "Output .pak file")->required();
	pack->add_option("--compression", compression, "Compression mode")->transform(CLI::CheckedTransformer(compression_modes, CLI::ignore_case));
	pack->add_option("--zstd-level", writer_options.zstd_level, "Zstandard compression level")->check(CLI::Range(1, 22));
	pack->add_option("--block-size", writer_options.block_size, "Block size in bytes, K, or M")->transform(CLI::AsSizeValue(false))->check(CLI::Range(1u, 64u * 1024u * 1024u));
	pack->add_option("--min-saving-percent", minimum_saving_percent, "Minimum saving required by automatic compression")->check(CLI::Range(0u, 99u));
	pack->add_flag("--verify", verify, "Verify every entry after writing");

	CLI11_PARSE(app, argc, argv);

	const auto input_path         = CLI::to_path(input);
	const auto output_path        = CLI::to_path(output);
	writer_options.minimum_saving = static_cast<float>(minimum_saving_percent) / 100.0f;

	std::vector<std::filesystem::path> files;
	try
	{
		for(const auto &entry : std::filesystem::recursive_directory_iterator {input_path, std::filesystem::directory_options::skip_permission_denied})
		{
			if(entry.is_regular_file())
			{
				files.push_back(entry.path());
			}
		}
	}
	catch(const std::filesystem::filesystem_error &error)
	{
		std::cerr << "Cannot enumerate input directory: " << error.what() << '\n';
		return EXIT_FAILURE;
	}
	std::sort(files.begin(), files.end());

	auto writer = pak::ArchiveWriter::Create(output_path, writer_options);
	if(!writer)
	{
		return PrintError("create archive", writer.GetError());
	}
	for(const auto &source : files)
	{
		pak::FileOptions file_options;
		file_options.compression = IsAlreadyCompressed(source) ? pak::CompressionPolicy::none : compression;
		if(auto added = writer.Value().AddFile(source, ToArchivePath(source.lexically_relative(input_path)), file_options); !added)
		{
			return PrintError("add file", added.GetError());
		}
	}
	if(auto finalized = writer.Value().Finalize(); !finalized)
	{
		return PrintError("finalize archive", finalized.GetError());
	}

	if(verify)
	{
		if(const int result = Verify(output_path); result != EXIT_SUCCESS)
		{
			return result;
		}
	}

	std::wcout << L"Packed " << files.size() << L" files into " << output_path << L'\n';
	return EXIT_SUCCESS;
}
