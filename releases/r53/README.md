# R53 development wake diagnostic

Physical R52 testing failed: cold-boot brightness controls worked, then the first
sleep/wake dimmed the screen and reversed the relationship between visible light
and the meter. The captured log retained and reapplied the selected raw value.
Cached brightness equality therefore does not establish restored panel light.

This development build identifies itself as `r53-diag` and adds observation only.
It records selected and cached
brightness, display state, bounded display-owner names, and read-only PWM
register samples around adjustment, restoration, sleep and wake. It verifies the
live device-tree panel channel, pin binding and register region before sampling.
MMIO reads run in a child process with a bounded timeout; faults and unavailable
diagnostics do not fail display restoration. The probe does not write registers.
Private device logs and game filenames are not included in this source record.

AArch64 checks cover nonfatal observer faults/timeouts, successful normal
observation, existing level/restoration behavior, and a hypothetical transfer
change that reproduces why cached-value checks can miss the reported failure.
That model does not prove the physical cause. No physical fix or successful
handheld acceptance is claimed. There is no public diagnostic binary release.

The required result remains: Menu + Volume Up raises both screen light and meter,
Menu + Volume Down lowers both, and repeated wakes restore the chosen light.
The existing R52 reconstruction instructions and toolchain apply to these source
files. Vendor components and existing third-party notices are unchanged.
