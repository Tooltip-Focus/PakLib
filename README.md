# PakLib

PakLib is a Windows-only C++20 library for building and reading immutable PAK
archives. It is designed for game assets and other read-heavy workloads:

- raw entries can be exposed as zero-copy memory-mapped views;
- compressed entries use independently readable Zstandard blocks;
- the index and optional file checksums use XXH3;
- archives are written transactionally and deterministically;
- production libraries do not use C++ exceptions: failures are returned as
  `pak::Result<T>`.

## Targets

| CMake target | Purpose |
|---|---|
| `pak::reader` | Archive lookup, mapped views, positional reads, cursors, and verification |
| `pak::writer` | Deterministic archive creation; links the reader transitively |
| `pak::c_api` | Combined PakLib static C ABI for FFI consumers |
| `paktool` | Command-line directory packer |
| `pak_tests` | Unit, corruption, concurrency, C ABI, and optional asset tests |
| `pak_bench` | Google Benchmark comparison of filesystem, PAK, and ZIP access |

The library currently supports Windows only.

## Build

Requirements:

- CMake 3.25 or newer;
- Visual Studio with a C++20 toolchain;
- vcpkg.

The manifest provides Zstandard and xxHash. The validation preset also enables
GoogleTest, Google Benchmark, and libzip.

```powershell
$env:VCPKG_ROOT = "C:\Path\To\vcpkg"

cmake --preset windows-validation
cmake --build --preset validation
ctest --preset validation
```

For a normal optimized build:

```powershell
cmake --preset windows-release
cmake --build --preset release
```

Install the libraries, headers, and CMake package with:

```powershell
cmake --install build/windows-release --config Release --prefix out/install
```

A static-runtime preset intended for Rust/FFI consumers is also available:

```powershell
cmake --preset windows-static
cmake --build --preset windows-static
```

## Command-line packer

```powershell
paktool pack `
  --input "C:\Assets" `
  --output "Assets.pak" `
  --compression auto `
  --zstd-level 3 `
  --block-size 256K `
  --min-saving-percent 2 `
  --verify
```

Compression modes:

- `none`: store every file raw;
- `zstd`: try to compress every block, retaining raw blocks when compression
  would make them larger;
- `auto`: compress only when the configured minimum saving is reached.

With `auto`, the CLI keeps common already-compressed runtime formats such as
DDS, OGG, PNG, JPEG, and KTX2 raw.

## Reader API

`Archive::Open` maps the whole archive read-only once. Every read is served
from that mapping; there is no per-file `CreateFile`, `ReadFile`, or extra
mapping. `File`, `Cursor`, and `MappedView` keep the archive alive.

```cpp
#include <pak/Reader.h>

auto opened = pak::Archive::Open("Assets.pak");
if (!opened)
{
    return opened.GetError();
}
auto archive = std::move(opened.Value());

auto found = archive.Find("Textures/Wall.dds");
if (!found)
{
    return found.GetError();
}
auto file = std::move(found.Value());
```

There are two ways to get the bytes:

| | `File::Map` | `File::ReadAt` |
|---|---|---|
| Copy | None: a view into the archive mapping | Into a caller-owned buffer |
| Entries | Raw (`Compression::none`) only, else `not_mappable` | Raw and compressed |
| Lifetime | Valid while the `MappedView` lives | Buffer is yours |

### `Map`: zero copy, raw entries only

```cpp
auto mapped = file.Map(0, static_cast<std::size_t>(file.Size()));
if (!mapped)
{
    return mapped.GetError();   // not_mappable if the entry is compressed
}
std::span<const std::byte> bytes = mapped.Value().Bytes();
UploadOrConsume(bytes);   // read directly from the archive pages
```

Pages are faulted in on first access. Destroying a `MappedView` does not call
`UnmapViewOfFile`; the archive owns the single mapping.

### `ReadAt`: copy into your buffer, any entry

```cpp
std::vector<std::byte> buffer(static_cast<std::size_t>(file.Size()));
auto read = file.ReadAt(0, buffer);
if (!read)
{
    return read.GetError();
}
// Do your stuff with buffer (read.Value() bytes were written)
```

For a raw entry, `ReadAt` is a `memcpy` from the mapping. For a compressed
entry, it decompresses the blocks covering the range: whole blocks are decoded
straight into the destination, partial blocks go through a per-thread cache
that keeps the last decoded block, so small sequential reads do not decode the
same block twice. `ReadAt` is positional and thread-safe.

`ReadAt` writes into whatever memory you give it. If the data is going
somewhere anyway, such as a GPU upload buffer, read straight into it rather
than into a temporary `std::vector` first: that saves one copy.

### `Cursor`: read in chunks

When the whole entry should not be in memory at once, a `Cursor` reads it
piece by piece and remembers where it stopped:

```cpp
auto cursor = file.CreateCursor();
std::array<std::byte, 64 * 1024> chunk;
while (cursor.Remaining() > 0)
{
    auto read = cursor.Read(chunk);
    if (!read)
    {
        return read.GetError();
    }
    // Do your stuff with the first read.Value() bytes of chunk
}
```

`Seek` and `Tell` move and query the position. Each thread should use its own
`Cursor`; several cursors can read the same file at the same time.

### Choosing

```cpp
if (file.GetCompression() == pak::Compression::none)
{
    auto mapped = file.Map(0, static_cast<std::size_t>(file.Size()));
    UploadOrConsume(mapped.Value().Bytes());
}
else
{
    std::vector<std::byte> buffer(static_cast<std::size_t>(file.Size()));
    auto read = file.ReadAt(0, buffer);
    // Do your stuff with buffer
}
```

| Entry | What you do with the data | Use |
|---|---|---|
| Raw | Consume it in place (upload, parse, hash...) | `Map`: no copy at all |
| Raw | Keep or modify your own copy | `ReadAt` into your buffer |
| Compressed | Anything | `ReadAt`: the only option, `Map` returns `not_mappable` |
| Any | Too large to hold at once | `Cursor`, chunk by chunk |

`Archive::OpenMemory` works the same way over caller-owned bytes and performs
no copy. Its input must remain valid and unchanged until the archive and every
object obtained from it have been destroyed.

## Writer API

```cpp
#include <pak/Writer.h>

auto created = pak::ArchiveWriter::Create("Assets.pak");
if (!created)
{
    return created.GetError();
}
auto writer = std::move(created.Value());

auto added = writer.AddFile(
    "Boat.glb",
    "Models/Boat.glb",
    pak::FileOptions{pak::CompressionPolicy::none});
if (!added)
{
    return added.GetError();
}

return writer.Finalize();
```

The writer creates a temporary file beside the destination, writes and flushes
the complete archive, then replaces the destination. Destroying or explicitly
aborting an unfinished writer removes its temporary file. One writer must not
be called concurrently.

Writer limits include a 64 MiB maximum block size, a maximum 4096-byte data
alignment, and a minimum-saving ratio in the range `[0, 1)`.

## C ABI

Include `<pak/CAbi.h>` and link `pak::c_api`. CMake propagates the required
Zstandard and xxHash libraries; direct linker or Rust integrations must link
those dependencies too. Handles are opaque and have matching destroy
functions. Passing `NULL` for `pak_writer_create` options uses the C++ defaults.

`pak_archive_open_memory` is non-owning. The backing bytes must remain valid
and immutable until both the archive handle and every file handle opened from
it have been destroyed. Operations return zero on success or a numeric
`pak::ErrorCode`; an optional `pak_error` provides the native Win32 code and
file offset.

## Archive format

All integers are little-endian. Version 1 is laid out as:

```text
4 KiB header
aligned data region
  raw entry bytes
  or independently stored/Zstandard-compressed blocks
index
  fixed header
  sorted 72-byte entry records
  24-byte block records
  UTF-8 path table
40-byte redundant footer
```

The reader validates physical ranges, normalized paths, sort order, block
layout, header/footer agreement, and the index hash before exposing entries.
Per-entry content verification is available through `File::Verify`.

## Performance guidance

Prefer raw mapped entries for runtime-ready data such as DDS textures and GLB
files that can be consumed directly. Mapping reserves virtual address space;
it does not load the entire archive into physical memory.

Use block compression for assets where disk savings justify decompression.
Partial sequential reads reuse the last decoded block per thread, while
full-block reads decompress directly into the caller's destination.

Measure with your production corpus: storage, antivirus, cache state, file
sizes, and thread count can dominate small benchmark differences.

## Tests

The regular suite needs no external assets:

```powershell
ctest --preset validation
```

To include the asset round-trip integration test:

```powershell
$env:PAKLIB_TEST_ASSET_DIR = "C:\Assets\Models\01_01"
ctest --preset validation
```

## Benchmarks

```powershell
$env:PAKLIB_BENCH_ASSET_DIR = "C:\Assets\Models\01_01"
build\windows-validation\Release\pak_bench.exe `
  --benchmark_repetitions=3 `
  --benchmark_report_aggregates_only=true
```

Set `PAKLIB_BENCH_SKIP_ZIP=1` when only filesystem and PAK measurements are
needed. The benchmark creates temporary comparison archives under
`PakBenchData/`. Benchmark JSON reports may be written to `PakBenchRuns/`.
Both directories are generated, machine-specific output and are intentionally
ignored by Git.

Use `tools/compression-sweep.ps1 -Source <directory>` to compare block sizes,
Zstandard levels, and minimum-saving thresholds on a real asset corpus.
