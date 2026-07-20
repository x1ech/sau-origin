# Mikui SA_ENGINE Step 0 RTL

This directory freezes the minimum `SA_ENGINE` hierarchy used by the
Im2Col-to-SAU integration model.

- Upstream repository: `http://git.acelab.net.cn/zbn/npu_lpnpu.git`
- Upstream branch: `mikui_16x16` (informational)
- Upstream commit: `2ca8252ef1cac43ef843998e9e08023259ac17ee`
- Upstream source directory: `hardware/src/sa_execute/`

`original/` contains byte-for-byte copies of the eight files listed in
`provenance.json`. Those files must not be edited. `integration/SA_ENGINE.sv`
is derived from the original engine using exactly
`patches/0001-fix-finish-dimension-width.patch`; all other integration files are
compiled directly from `original/`.

The patch corrects only the width of the constants used by `FINISH_ROW` and
`FINISH_COL`. Golden traces produced for this project therefore describe
`2ca8252` with the documented finish-dimension patch, not the unmodified
upstream engine.
