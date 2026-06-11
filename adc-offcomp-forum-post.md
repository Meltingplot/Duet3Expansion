# SAMC21 ADC: Vssa/Vref offset traced to the OFFCOMP (comparator auto-zero) phase

Follow-up on the intermittent ADC offset on the TOOL1LC (SAMC21G18A, rev F). I ran a full measurement matrix to pin down what is actually disturbed and "healed" by a NOP. Summary below.

## Setup

ADC0 is hardware-sequenced over 3 channels (Vssa -> Vref -> Vin), DMA-driven into 8-sample averaging filters. temp0 sits on the SDADC and serves as an independent control. All values are mean ADC counts (filter-sum / 8); 1 count = 50.4 uV (16-bit, 3.3 V FS). True Vssa is ~8 mV (~160 ct). The anti-correlation `r` is computed between the Vssa and Vref series over n = 100 samples.

## A. Hardware averaging — `AVGCTRL.SAMPLENUM` (OFFCOMP on)

Sweeping the averaging count 64 -> 128 -> 256 -> 512 -> 1024 only **dilutes** the visible thermistor error; it never removes it. At 1024x the apparent temperature approaches room temperature, but the per-sweep jitter is still there. Averaging masks the per-sample corruption, it does not fix it.

## B. Sampling time — `SAMPCTRL.SAMPLEN` (OFFCOMP must be off)

- SAMPLEN=3 (4 clk): Vssa = 1010 ct, sigma = 44, r = -0.96 -> bug, very noisy
- SAMPLEN=16 (17 clk): Vssa = 251 ct, sigma = 4.5, r = -0.65 -> much better, residual carryover remains

Longer sampling reduces the channel-to-channel carryover but does not fully remove it, and it forces OFFCOMP off (no comparator auto-zero).

## C. OFFCOMP + a single NOP in the DMAC EnableChannel path (at the DMA channel enable, around the start of the conversion)

- OFFCOMP on, no NOP: Vssa = 1070 ct, sigma = 27, r = -0.82 -> worst offset (~54 mV)
- OFFCOMP on, +NOP: Vssa = 191 ct, sigma = 1.2, r = **+0.39** -> fully healed, lowest noise of all
- SAMPLEN=16, +NOP: Vssa = 159 ct, sigma = 2.6, r = -0.26 -> lowest absolute offset

## Key findings

1. A tiny code-timing shift (the NOP only delays SWTRIG by a few cycles) heals the offset and flips the Vssa/Vref anti-correlation away (-0.82 -> +0.39).
2. The NOP's effect is ~10x larger with OFFCOMP enabled (delta 879 ct) than with it disabled (delta 92 ct) -> the dominant ~40 mV error is specific to the comparator offset-compensation phase.
3. The error decays down the sequence: Vssa (1st) -879 >> Vref (2nd) -452 >> Vin (3rd) ~0 -> the first conversion of each sweep is hit worst, diminishing down the sequence.
4. **SEQBUSY is irrelevant** here: by the time the DMA-completion callback re-arms the ADC, the sequencer is necessarily idle (otherwise the callback would not have fired).
5. Per the datasheet (Fig. 38-5, register map in 38.7): OFFCOMP is fused into the first STATE of every conversion ("Offset Compensation and Sampling"). **There is no OFFCOMP-ready flag and no analog-busy register** — only RESRDY, SEQBUSY and SYNCBUSY exist. The comp phase cannot be polled.

## Working hypothesis (revised)

It is *not* a fixed time gap, and it is not specifically about the SWTRIG write. The single NOP is only one of **many** flash-layout perturbations that heal the bug — any unrelated code change that shifts the flash layout does the same. So the determining variable is almost certainly the **NVM-cache occupancy** (which code lands in which of the 8 direct-mapped 64-bit cache lines), not a fixed instruction-count delay.

The standing suspicion is the **DMA channel and the beginning of the conversion** — the NOP sits in the DMAC `EnableChannel` path, i.e. where the DMA channel is armed just as the conversion starts. Cache hit/miss patterns there produce variable CPU/bus timing depending on the flash layout (DMA arming, descriptor access, and bus contention with the first result transfer at conversion start). What we can state from the measurements is the **observable effect**: in the bad layout the OFFCOMP stage effectively does not perform its compensation — the result carries the full uncompensated offset (~40 mV), the effect is comp-specific (879 vs 92 ct), and it decays down the sequence (first conversion worst). In a benign layout the same OFFCOMP stage produces the correct, low-noise result (Vssa 191 ct, sigma 1.2).

**What exactly makes the OFFCOMP unit stop working under a particular cache layout is internal to the silicon and can only be answered by Microchip.** From the firmware side all we can say is that it is triggered by the flash/NVM-cache layout, not by a fixed instruction-count delay — a NOP is therefore not a fix, it is luck: it nudges the cache into a benign layout for this one build.

## Deterministic fix candidates (a status poll is impossible)

A fixed NOP or a calibrated delay is **not** reliable, because the disturbance is cache-occupancy jitter, not a fixed time gap. Robust options remove the dependence on layout/timing entirely:

- **Discard the first conversion** — prepend one throwaway channel to the sequence (DMA length +1, drop the first result). Immune to cache/timing phase: the potentially corrupted first conversion is never used.
- **Make the re-arm path cache-deterministic** — run StartConversion and the callback path from RAM (`__ramfunc`) and/or with DSB barriers, so cache occupancy no longer modulates the timing (DC42 already added DSBs along these lines).
- **Avoid the stop/restart race** — keep the ADC sequencing continuously instead of re-triggering each sweep, so there is no re-arm phase to land in a bad window.

## Proposed startup self-test gate (fail-safe)

Because SAMPLEN=31 with OFFCOMP off is immune to this comp-corruption, it gives a trustworthy reference to validate the OFFCOMP path at boot:

```c
// Once at startup, after ADC init, on Vssa:
//  1) OFFCOMP=1 (4-clk + comp), average N samples         -> vssa_comp
//  2) OFFCOMP=0, SAMPLEN=31 (long settle, carryover-free) -> vssa_ref
//  3) compare:
if (abs((int)vssa_comp - (int)vssa_ref) > VSSA_GATE_THRESHOLD) {
    // healthy: ~30 counts;  comp-bug active in this build: ~900 counts
    enterFailState();   // heaters off, raise CAN fault, refuse normal operation
}
```

A threshold of ~100-150 counts separates the healthy case (~30 ct) from the bug case (~900 ct) with a huge margin. If a given build's instruction alignment re-activates the comp corruption, the gate catches it at boot and the board fails safe instead of silently reporting wrong temperatures.

## 2x2 summary (Vssa, ADC counts)

|                              | no NOP              | +NOP                 | delta(NOP) |
|------------------------------|---------------------|----------------------|-----------:|
| **OFFCOMP on**               | 1070 (s27, r -0.82) | 191 (s1.2, r +0.39)  | **-879**   |
| **OFFCOMP off (SAMPLEN=16)** | 251 (s4.5, r -0.65) | 159 (s2.6, r -0.26)  | **-92**    |
