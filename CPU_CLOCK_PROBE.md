# Native CPU clock probe (experimental)

Branch: `experiment/native-cpu-clock-probe`

This experiment does not modify clock speeds. It tests whether a SwitchGamesBrowser
homebrew process can access libnx's `clkrst` service.

## Usage

1. Build the Switch application (or download the artifact from this branch's GitHub Actions build).
2. Launch SwitchGamesBrowser in application mode (hold R while launching a game).
3. Press Y for Settings, choose **CPU Clock Diagnostic (read-only)**, press A.
4. Record the displayed `clkrstInitialize`, `clkrstOpenSession`,
   `clkrstGetClockRate`, and possible-clock-list results.
5. Press A to run the probe again, or B to return to Settings.

Any failed service call is intentionally shown as a `0x...` libnx Result.
Inability to read the service does not prove the Switch cannot boost its CPU.

**Existing behavior:** SwitchGamesBrowser already requests
`appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad)` while installing and
benchmarking, then requests Normal afterward. The new diagnostic does not
change that behavior.

Do not interpret a successful read as proof that clock **writes** are allowed.
The service may restrict writes. This branch never calls `clkrstSetClockRate`,
and it does not change RAM/GPU frequencies or voltage.

## 1224 MHz CPU write test (10 seconds)

On the `experiment/native-cpu-clock-boost-10s` branch, open
Settings > CPU Clock Diagnostic. Press **X** to request 1224 MHz.

- A successful read-only diagnostic **does not** imply a clock write is authorized.
- The app checks for 1224 MHz in the CPU's enumerated discrete clock list.
- The original clock is read before writing; the requested clock is read back.
- The app attempts restoration after 10 seconds, when pressing B, or on
  normal app exit (+ included).
- If service denies a write, the rejection code appears on the diagnostic page.
- A restoration request and readback are shown; never assume restoration
  succeeded if the readback differs or fails.
- This test touches neither RAM, GPU nor voltages.
- An app crash or hard power failure can prevent timed restoration, so this is
  an experiment, not a fail-safe overclock manager.

## Saved CPU clock mode (experiment)

In **Settings > CPU Clock Settings**, use **Left/Right** to select
**Off / 1224 / 1326 / 1428 / 1581 MHz** and press **X** to apply/save it.
Press **A** for the read-only diagnostic, **Y** for the original 10-second
1224 MHz experiment (requires saved mode to be Off), **B** to return.

The selection is saved at
`sdmc:/switch/SwitchGamesBrowser/cpu-clock.json`, and is reapplied
when SwitchGamesBrowser starts again. It is only saved if the CPU
accepts and confirms the requested frequency. The available discrete
frequency list is checked before applying a preset. If a clock is
unavailable or a request fails, the existing saved preference is retained.

The request remains active while SwitchGamesBrowser is running
(including when leaving Settings), **without a 10-second timeout**.
The app restores the original CPU clock on normal exit. There is **no
background sysmodule**, so the CPU cannot be forced to retain this
setting after the app exits, and crash/power-off restoration cannot
be guaranteed.

The existing automatic FastLoad installation/benchmark boost is
skipped while a saved clock is held, to avoid conflicting requests.
RAM, GPU and voltage control are intentionally excluded. The highest
offered user preset is 1581 MHz.

## AllDebrid Sphaira-style single-connection buffering experiment

On branch `experiment/alldebrid-sphaira-buffering` the AllDebrid
installation pipeline collects **512 KiB** queue blocks (rather than
4 MiB), and buffers up to eight queue blocks. The curl transfer remains
a **single continuous HTTP/1.1 ranged request per NCA/NCZ** and
continues overlapping download, content decoding and SD writing.

Sphaira's HTTP VFS uses a producer/consumer queue, so smaller blocks
allow the installer to start consuming data earlier and reduce stalls
between download and content processing. This is a hypothesis to
benchmark, **not a guarantee of Sphaira's throughput**. TorBox and shop
installation queues remain unchanged.

To distinguish CDN throughput from SD or installation bottlenecks,
open **Install Manager**, highlight an **AllDebrid** item with at
least 64 MiB and press **ZL**. This runs a *network-only*
single-connection speed test on the same authorized AllDebrid download
URL without installing or writing to SD. Wait for a result resembling
`AllDebrid HTTP-only 1x: X.XX MiB/s`. Compare with the normal
installation network rate and Sphaira's displayed speed on the same
connection and content. This test does not start four connections
on AllDebrid.

AllDebrid URLs, debrid credentials, and torrent identifiers are not
displayed by the benchmark.

The build also uses Sphaira's `SocketInitConfig` for
**application mode** (64 KiB initial TCP send/receive buffers and
4 MiB maxima, 3 BSD sessions), falling back to libnx defaults when
that allocation is unavailable. AllDebrid requests a 1 MiB socket
receive window; other providers retain their previous 256 KiB request.

This is a measured A/B experiment, not a claim of identical throughput
to Sphaira. Keep the same network and an equivalent link when comparing.

## Optional 1785 MHz CPU preset

The `experiment/cpu-1785-preset` branch adds `1785 MHz` as an optional
CPU-only setting alongside Off, 1224, 1326, 1428 and 1581 MHz.
It retains the AllDebrid Sphaira-style buffering experiment.

The app refuses to apply 1785 MHz unless the active firmware's
`clkrstGetPossibleClockRates` reports that exact rate as a **discrete
allowed CPU frequency**. A successful `clkrstSetClockRate` must then
be corroborated by `clkrstGetClockRate`; unverified changes are
rolled back, and failed choices are not saved. The clock is requested
only while SwitchGamesBrowser runs, and the original rate is restored
on normal exit. **There is no crash-proof background restore.**

**1785 MHz draws more power and generates more heat.** Use adequate
cooling and monitor temperatures; stop if the Switch becomes unusually
hot or unstable. This change never modifies GPU, RAM or voltages.
