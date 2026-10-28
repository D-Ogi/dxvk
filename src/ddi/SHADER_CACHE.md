# D3D11 shader IR cache

The hosted engine enables its persistent IR cache by default in release and lab builds.
It caches conversion results before pipeline-specific SPIR-V lowering. RADV's pipeline
cache serves a different stage and does not replace this cache.

PROVENANCE: D-Ogi/dxvk (MIT), including the cache policy in 95bcef23, the truncate helper
in 98d19075, and the subsequent inline-cache implementation.

## Execution and lifetime

BD-165 already permits CPU-only shader translation on pipeline workers. Each converting
thread appends the result after conversion; the cache starts no writer thread. This keeps
the existing worker shutdown and avoids another queue or shutdown dependency. Vulkan
calls retain the E2 calling-thread contract. The ABI stays at 1.4.

A shader holds its cache sink until conversion or destruction. The cache holds serialized
records, not shader references. Factory acquisition and final release use the same lock.
Cache failures fall back to conversion and must not fail device or shader creation.

## Profile and flavour policy

The engine obtains LocalAppData through `SHGetKnownFolderPath` with an explicit process
token, then uses `amdgpu-wddm\dxvk` below it. It does not trust environment variables to
identify the user profile. Only ordinary local, domain or Entra user accounts at medium
or high integrity qualify. Service logons, session zero, DWM and UMFD identities,
AppContainer, restricting SIDs, unreadable token properties and thread impersonation
disable access. Admission is checked again before cache operations, including writes
from translation workers.

Release builds contain no lookup of `DXVK_SHADER_CACHE` or `DXVK_SHADER_CACHE_PATH`.
Lab builds keep these diagnostic overrides after the process-policy check. Neither
flavour enables cache access for an ineligible identity through an override.

## Format and sharing

The file name includes hashes of the executable's full path and the module build identity.
The header verifies the full identity: DXVK version, format version, flavour, and the
linker's CodeView GUID and age. A module without this identity disables the cache.
The optimized release link includes a private PDB to provide that identity; NDEBUG and
the release optimization flags remain enabled.

Format 2 frames each lookup record with a bounded length and checksum. A second checksum
covers both serialized IR and its metadata. Readers check bounds and checksums before
they interpret records. Invalid records count as rejections and cause fresh conversion.
An incomplete tail is removed only by a writer under the exclusive file lock.

The lookup file's byte-range lock covers both files. Reads hold a shared lock through
payload loading; append and reset hold an exclusive lock. Acquisition is bounded to
250 ms. A new random generation in the header makes other processes discard old offsets
after a reset. Before an append, writers merge complete records written by other processes.

Each executable/build pair is limited to 384 MiB of binary data and 64 MiB of lookup data.
The in-memory index is limited to 16,384 entries and a conservative 64 MiB budget. Each
IR payload is limited to 16 MiB, metadata to 1 MiB, and a lookup record to 64 KiB. Files
from older builds of the same executable can be removed when no process holds them open.

All cache files in the directory together are limited to 2 GiB (`src/dxvk/dxvk_shader_cache_quota.h`).
At initialization, after the files of older builds are removed, the cache sets the last write
time of its own look-up table to now and adds the sizes of all files with the exact cache name
pattern (16 hex digits, `-`, 16 hex digits, `.dxvk.lut` or `.dxvk.bin`). When the sum is above
the limit, it removes the files of other executables, all files of one executable at a time,
the executable with the oldest newest file first, until the sum is at the limit or below. The
files of the running executable are never selected. Other names are not counted and not
removed. One initialization removes at most 64 files and visits at most 4096 directory
entries. The cache files are opened without delete sharing, so the files of a process that
runs now cannot be removed; the sum is then checked again at the next start.

## Verification

`amdgpu_wddm_dxvk_engine_test` requires `--cache-root <absolute directory>`. It tests the
exact engine DLL, cold writes, warm reads, unchanged rendered output, and recovery after
binary corruption. A process-local test stub redirects only the engine's LocalAppData
query so test files can stay on a dedicated volume. Token checks and cache code remain
unchanged. This verifies the engine path, not a real installation's profile mapping.

`amdgpu_wddm_dxvk_shader_cache_test --dumps <DXBC directory> --cache <test directory>`
tests byte-identical SPIR-V, reopening, corruption, bounded growth, concurrent processes,
unavailable storage and token-policy transitions. `--limit N` selects a smaller corpus.
`amdgpu_wddm_dxvk_cache_quota_test` (no arguments) tests the selection of the directory limit
as a pure function of a file list: no file system and no GPU.

Run the programs in x64/x86 and release/lab configurations. The source flavour check,
the shipped-byte forbidden-name scan and the release live-assert scan are separate gates.
