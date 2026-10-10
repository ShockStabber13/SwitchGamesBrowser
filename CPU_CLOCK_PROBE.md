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
