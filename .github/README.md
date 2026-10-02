# DXVK for amdgpu-wddm

This fork of [doitsujin/dxvk](https://github.com/doitsujin/dxvk) carries the DXVK engine of
[amdgpu-wddm](https://github.com/D-Ogi/amdgpu-wddm), a Windows WDDM driver stack for the AMD BC-250. There,
DXVK is not a set of application-local DLLs: it is the engine behind the system Direct3D 10/11 user-mode
driver, so applications use the standard `d3d11.dll` and DXGI of Windows.

## Registered on unit A (2026-10-03)

| Binary | SHA-256 | Branch | Commit | Registered since |
|---|---|---|---|---|
| `amdgpu_wddm_dxvk.dll`, the D3D11 engine | 8E9B3187 | `amdgpu-wddm/ddi-engine-fl12` | `0c187731` | 2026-10-01 |

That engine sits in the project's Direct3D 11 user-mode driver, selected per application by an allowlist on the
lab machine. Through the system D3D11 runtime it reports feature level 12_1 and renders the project's own
clients exactly, with checksums equal to a reference GPU and to WARP, single-threaded and across four and eight
threads. Direct3D 12 is the default route for games there, so the D3D11 route is a named variant; its game
coverage and its performance bound are open and recorded in the main repository.

The `dxbc-spirv` submodule points at [D-Ogi/dxbc-spirv](https://github.com/D-Ogi/dxbc-spirv), pinned on the
registered commit to `f241996e` of its `amdgpu-wddm/ddi-engine` branch.

## Branches

| Branch | What it is |
|---|---|
| `master` | upstream's `master`; no project commits land here |
| `amdgpu-wddm/ddi-engine` | upstream DXVK plus `src/ddi/` and small host-mode hooks in `src/dxvk/` and `src/d3d11/` |
| `amdgpu-wddm/ddi-engine-fl12` | `GetAdapterInfo` reports 12_0 and 12_1; the registered engine |
| `amdgpu-wddm/ddi-engine-inline-throttle` | submissions retired inside the initializer's throttle wait; a trial root, not registered |
| `amdgpu-wddm/ddi-engine-async-translate` | shader creation off the calling thread, with a shader cache; a benchmark branch |

`-Denable_ddi_engine=true` (the project branches only) builds the engine `amdgpu_wddm_dxvk.dll` and its offline
positive control `amdgpu_wddm_dxvk_engine_test.exe`; commits before header r7 name them `bc250dxvk.dll` and
`bc250dxvk_engine_test.exe`. The contract is `src/ddi/bc250_dxvk_engine.h`.

The branch was rewritten on 2026-09-28 with identical trees. Older commit hashes in the main repository's
evidence map to the published ones in
[dxvk-engine-commit-map.md](https://github.com/D-Ogi/amdgpu-wddm/blob/main/docs/dxvk-engine-commit-map.md).
Scope, ABI and the validation list:
[d3d11-ddi-engine.md](https://github.com/D-Ogi/amdgpu-wddm/blob/main/docs/design/d3d11-ddi-engine.md); facts and
evidence: [docs/facts.md](https://github.com/D-Ogi/amdgpu-wddm/blob/main/docs/facts.md).

License: DXVK is under the zlib/libpng license in [LICENSE](https://github.com/D-Ogi/dxvk/blob/master/LICENSE)
(Copyright 2017 Philip Rebohle and others). The engine header `src/ddi/bc250_dxvk_engine.h` carries
`SPDX-License-Identifier: MIT`.
