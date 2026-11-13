# Storm Voice

A Resona module: C++ that runs on the Resona Box, and on your desktop in Resona Lab.

| File | What it is |
| --- | --- |
| `src/dsp.cpp`, `src/dsp.h` | **The algorithm.** `dsp_process()` handles the host's frame count; `dsp_param()` is told when a parameter changes. Start here. |
| `src/params.h` | The parameters: a name in the enum, a line in `kParams` (name, range, default, unit). |
| `src/ui.cpp`, `src/ui.h` | The 320 x 172 screen and the two encoders. One knob per parameter by default. |
| `src/main.cpp` | The glue to the module interface. You rarely change it. |
| `blocks/` | The blocks this project uses (**Add block** copies one in and calls it from `dsp_process()`), and your own. |
| `resona.project.json` | The name, id, version and list of source files. |

The rules the algorithm follows (no heap, no C library, no waiting) are in
`docs/resona/packs.md` (and `AGENTS.md` for AI agents). **Build** runs the checks and redraws the preview; the CLI also offers `resona-lab check`;
use **Device → + → My algorithm** to load it on a compatible Resona Box.
