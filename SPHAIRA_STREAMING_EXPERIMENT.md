# Experimental Sphaira-style HTTP/1.1 streaming

This branch keeps SwitchGamesBrowser's existing NCA/NCZ installer, including the
Install Manager, package validation, CPU boost, 4 MiB content-storage write
buffers, and its original one-range-per-content HTTP/1.1 backend.

It adds an **opt-in alternative HTTP transport** inspired by the streaming and
range-resume design in Sphaira's YATI HTTP source:
https://github.com/NaGaa95/sphaira/blob/master/sphaira/source/yati/source/http.cpp

This is **not a complete port of Sphaira's GPLv3 YATI installer**. No Sphaira
source code was copied into this branch. Porting its entire installer would
require a separate integration of Sphaira's service management, package
containers, key handling, NCA/NCZ processing, progress UI, and GPLv3 notices.

## How to select

On the Switch, open Settings -> Installer HTTP Stream and press A to select:

- **Original** (default): the previous streaming installer with 4 MiB RAM
  chunks, unchanged HTTP/1.1 download behavior.
- **Sphaira-style (experimental)**: contiguous HTTP/1.1 ranged reads,
  interruption recovery at the first missing byte, and 1 MiB RAM chunks.
  The 512 KiB libcurl receive buffer and 4 MiB asynchronous SD writes are
  preserved.

Selection is persisted in `sdmc:/switch/SwitchGamesBrowser/config.json` under
`"sphairaStyleStream": true|false`. It applies to newly started jobs, including
queued jobs, but does not alter an already-running install.

## Important limits

- Both modes still use SwitchGamesBrowser's **existing NCA/NCZ installer**.
- A single HTTP/1.1 connection transfers each NCA/NCZ. The multi-range
  experimental path remains disabled.
- Resume is attempted only after a transient connection failure or short 206
  response; the server must honor Range with HTTP 206. Invalid-range,
  authentication, content, cancellation, and content-storage failures are
  never silently retried.
- The in-flight installer cannot safely switch backends midway through
  creating a Nintendo content-storage placeholder. If experimental mode fails,
  retry that job with **Original** selected.
- Lower RAM chunk sizes and retries **are not a guaranteed speed improvement**.
  The purpose is to compare transport behavior without changing the package
  install logic. Use the existing Install Manager live network speed, SD
  write time, HTTP wait time, and buffer wait time to compare.

## Build and test

Build using the repository's normal `make -C switch -j2` workflow.

1. Install a package you are authorized to install using **Original**.
2. Note its network speed and the Install Manager timing line.
3. Switch the setting to **Sphaira-style**, retry the same source, and compare.
4. For reliability checks, interrupt and restore network mid-transfer and
   verify the package installation succeeds or fails cleanly.

For a full Sphaira/YATI installer replacement, perform a separate documented
port and ensure GPLv3 compliance before redistributing any copied code.
