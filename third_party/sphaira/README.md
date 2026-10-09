# Sphaira YATI direct installer port — source staging

Upstream: https://github.com/NaGaa95/sphaira
Pinned upstream source commit: 338348e74b6d278bc570a9be5373d25197bf6c8d
Code location: `sphaira/include/yati/**` and `sphaira/source/yati/**`
License: GPLv3 — see `LICENSE`.

This folder contains the original 39-file YATI installer source set and is
intentionally *unmodified*. This is **not a completed integration** yet:
SwitchGamesBrowser continues to use its existing installer (and nothing under
`third_party/sphaira/` is linked or compiled). This is a reference-quality
starting point for an actual YATI port, not another behavioral reimplementation.

What is still needed before a usable YATI backend exists:
- Bring over/adapt Sphaira-specific app services (App, ui::ProgressBox,
  language/logging, fs, utils, threads and crypto) to SwitchGamesBrowser.
- Reuse YATI's own NSP/XCI containers, NCZ decode, NCA writes, ticket and
  content registration, without mixing with the previous custom install loop.
- Bring across the actual asynchronous HTTP producer used by
  `yati/source/http.cpp` (PushThreadData under devoptab_common), its
  lifetime, cancellation, and verified ranged read behavior.
- Build with upstream C++ features and required libraries, handling
  collisions with existing SwitchGamesBrowser code.
- Add an opt-in adapter behind Install Manager so original stays default,
  then compile and validate on console before changing any defaults.

**Important:** Simply copying these files does not improve speed. A real
integration must compile and call YATI. Download speed also depends on the
upstream service; a source limiting traffic to 1 MB/s will remain slow.

Redistribution of binaries containing Sphaira GPLv3 code requires GPLv3
compliance for the combined work. Retain upstream notices and provide
corresponding source to recipients. Other included third-party copyright
notices and obligations must also be honored.
