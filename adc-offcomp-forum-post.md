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

## C. OFFCOMP + a single NOP in the DMAC EnableChannel path (just before SWTRIG)

- OFFCOMP on, no NOP: Vssa = 1070 ct, sigma = 27, r = -0.82 -> worst offset (~54 mV)
- OFFCOMP on, +NOP: Vssa = 191 ct, sigma = 1.2, r = **+0.39** -> fully healed, lowest noise of all
- SAMPLEN=16, +NOP: Vssa = 159 ct, sigma = 2.6, r = -0.26 -> lowest absolute offset

## Key findings

1. A tiny code-timing shift (the NOP only delays SWTRIG by a few cycles) heals the offset and flips the Vssa/Vref anti-correlation away (-0.82 -> +0.39).
2. The NOP's effect is ~10x larger with OFFCOMP enabled (delta 879 ct) than with it disabled (delta 92 ct) -> the dominant ~40 mV error is specific to the comparator offset-compensation phase.
3. The error decays down the sequence: Vssa (1st) -879 >> Vref (2nd) -452 >> Vin (3rd) ~0 -> signature of a **cold-started OFFCOMP/auto-zero in the first conversion of each sweep**, diluting down the sequence.
4. **SEQBUSY is irrelevant** here: by the time the DMA-completion callback re-arms the ADC, the sequencer is necessarily idle (otherwise the callback would not have fired).
5. Per the datasheet (Fig. 38-5, register map in 38.7): OFFCOMP is fused into the first STATE of every conversion ("Offset Compensation and Sampling"). **There is no OFFCOMP-ready flag and no analog-busy register** — only RESRDY, SEQBUSY and SYNCBUSY exist. The comp phase cannot be polled.

## Working hypothesis

Re-asserting SWTRIG too soon after the previous sweep corrupts the first conversion's comparator auto-zero; the NOP simply grants the analog core recovery time.

## Deterministic fix candidates (a status poll is impossible)

- Prepend one throwaway conversion to the sequence to absorb the cold-start (DMA length +1, discard the first result); or
- Insert a calibrated inter-sweep delay before SWTRIG.

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
