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
