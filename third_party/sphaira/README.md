# Sphaira YATI direct installer port — source staging

Upstream: https://github.com/NaGaa95/sphaira
Pinned upstream source commit: 338348e74b6d278bc570a9be5373d25197bf6c8d
Code location: `sphaira/include/yati/**` and `sphaira/source/yati/**`
License: GPLv3 — see `LICENSE`.

This folder contains the original 39-file YATI installer source set, plus
12 original Sphaira support files and headers (including the actual libcurl
push-thread source), intentionally *unmodified*. This is **not a completed integration** yet:
SwitchGamesBrowser continues to use its existing installer (and nothing under
`third_party/sphaira/` is linked or compiled). This is a reference-quality
starting point for an actual YATI port, not another behavioral reimplementation.

An integration seam now exists in `switch/include/installer_backend.hpp` and
`switch/source/installer_backend.cpp`. The Install Manager routes through it,
but **only the original backend is available**. The YATI path intentionally
returns "not linked" if selected programmatically until its real implementation
is compiled; there is no misleading UI switch yet.

What is still needed before a usable YATI backend exists:
- Finish porting Sphaira-specific app services (App, ui::ProgressBox,
  language/logging, fs, utils, threads and crypto) to SwitchGamesBrowser.
  Upstream interface headers and the original HTTP queue module are staged;
  these dependencies are NOT yet linked.
- Reuse YATI's own NSP/XCI containers, NCZ decode, NCA writes, ticket and
  content registration, without mixing with the previous custom install loop.
- Compile and link the original asynchronous HTTP producer under
  `source/utils/devoptab_common.cpp` (PushThreadData), then wire its
  lifetime, cancellation and HTTP ranged reads via the YATI adapter.
- Build with upstream C++ features and required libraries, handling
  collisions with existing SwitchGamesBrowser code.
- Add an opt-in adapter behind Install Manager so original stays default,
  then compile and validate on console before changing any defaults.

**Important:** Simply copying these files or adding the backend dispatcher does not improve speed. A real
integration must compile and call YATI. Download speed also depends on the
upstream service; a source limiting traffic to 1 MB/s will remain slow.

Redistribution of binaries containing Sphaira GPLv3 code requires GPLv3
compliance for the combined work. Retain upstream notices and provide
corresponding source to recipients. Other included third-party copyright
notices and obligations must also be honored.
