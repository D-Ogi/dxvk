# DXVK for amdgpu-wddm

This fork of [doitsujin/dxvk](https://github.com/doitsujin/dxvk) carries the DXVK engine of
[amdgpu-wddm](https://github.com/D-Ogi/amdgpu-wddm), a Windows WDDM driver stack for the AMD BC-250. There,
DXVK is not a set of application-local DLLs: it is the engine behind the system Direct3D 10/11 user-mode
driver, so applications use the standard `d3d11.dll` and DXGI of Windows.

| Branch | What it is |
|---|---|
| `master` | upstream's `master`; no project commits land here |
| `amdgpu-wddm/ddi-engine` | upstream DXVK plus `src/ddi/` and small host-mode hooks in `src/dxvk/` and `src/d3d11/` |

`amdgpu-wddm/ddi-engine` forks from upstream commit `52fe923ca1496c8e44789b613fab8611dfcb5c4a` (2026-09-25).

- `-Denable_ddi_engine=true` (this branch only) builds the engine `amdgpu_wddm_dxvk.dll` and its offline
  positive control `amdgpu_wddm_dxvk_engine_test.exe`; commits before header r7 name them `bc250dxvk.dll` and
  `bc250dxvk_engine_test.exe`.
- The contract is `src/ddi/bc250_dxvk_engine.h`: revision r7, engine ABI 1.4 in force, minor versions only add.
- The `dxbc-spirv` submodule points at [D-Ogi/dxbc-spirv](https://github.com/D-Ogi/dxbc-spirv), pinned to
  `253c08ce`, which carries two geometry shader fixes that are not upstream yet. The head commit `ae6b8d9b`
  changes only that submodule URL; the engine code is that of `bc0d4697`.

Tested on the BC-250 (facts in the main repository's
[docs/facts.md](https://github.com/D-Ogi/amdgpu-wddm/blob/main/docs/facts.md)): earlier engine revisions alone on
RADV (M725, M726) and inside the system D3D11 runtime at FL11_1, offscreen, in a window and across resizes
(M736, M747, M752); the r7 engine `bc0d4697` in the engine pair admission and the native error controls (M755,
M756). These are bounded controls. Broad application compatibility, the performance bound and FL12_1 are open.
Scope, ABI and the validation list:
[d3d11-ddi-engine.md](https://github.com/D-Ogi/amdgpu-wddm/blob/main/docs/design/d3d11-ddi-engine.md).

The branch was rewritten on 2026-09-28 with identical trees. Older commit hashes in the main repository's
evidence map to the published ones in
[dxvk-engine-commit-map.md](https://github.com/D-Ogi/amdgpu-wddm/blob/main/docs/dxvk-engine-commit-map.md).

License: DXVK is under the zlib/libpng license in [LICENSE](https://github.com/D-Ogi/dxvk/blob/master/LICENSE)
(Copyright 2017 Philip Rebohle and others). The engine header `src/ddi/bc250_dxvk_engine.h` carries
`SPDX-License-Identifier: MIT`.
