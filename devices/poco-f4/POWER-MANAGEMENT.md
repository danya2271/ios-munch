# Munch CPU frequency and idle states

## CPU frequency

The Munch EPSS CPU-frequency driver uses the three Kona domains at
`0x18591000`, `0x18592000`, and `0x18593000`. It accepts only frequencies from
the Munch OPP lists represented in the vendor device tree, and refuses to
touch the block unless the root device-tree model is exactly
`Qualcomm Technologies, Inc. xiaomi munch`.

The supplied Android sysfs output confirms policies for CPUs `0-3`, `4-6`, and
`7`, with available-frequency lists matching the three tables used here. This
confirms the domain grouping and advertised OPP values, but does not validate
EPSS register writes or the governor under XNU.

- `cpufreq=1` is an explicit maximum-frequency diagnostic mode.
- `cpufreq=2` is the normal schedutil-inspired mode. It samples XNU per-core
  idle-time counters every 20 ms, uses the busiest sampled core in each domain,
  maps utilization with 25% headroom, increases frequency immediately, and
  requires three lower-load samples before a downshift.
- Display blanking applies caps of 1171 MHz (little), 1382 MHz (big), and
  1401 MHz (prime). The governor target is retained separately, so unblank does
  not lose the requested performance state.

If the governor thread cannot start, it leaves the selected supported midrange
rows in place instead of unexpectedly switching to maximum frequency. A domain
that rejects a later request is removed from further governor updates.

The governor still uses idle-time sampling rather than XNU scheduler PELT
utilization, so it is an approximation of Linux `schedutil`, not an equivalent
implementation. It has no thermal cooling input or user-space policy interface.
`cpuload=MS` remains available as telemetry and does not control frequency.

## Idle states

The DT parser now imports the Munch `qcom,lpm-levels` timing values. With
`lpm=1`, a CPU whose predicted idle interval exceeds its DT entry/exit latency
plus minimum residency requests PSCI `CPU_SUSPEND` for `rail-pc`; resume uses
XNU's existing `resume_idle_cpu` reset handler and returns through
`cpu_idle_exit(TRUE)`. The little and big timings are taken independently from
the two `qcom,pm-cpu` nodes.

The parsed `llcc-off` level is reported but remains gated behind the unfinished
RPMh/PDC broadcast-timer handoff. `lpm_llcc=1` currently reports that gate and
does not enter the cluster state. This is deliberate: enabling `notify-rpm`
without programming the Kona wake source can leave the phone asleep until a
non-timer interrupt.
