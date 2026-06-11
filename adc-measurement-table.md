# SAMC21 ADC — full measurement table (Vssa / Vref / Vin)

All runs n = 100. Values are mean ADC counts (averaging-filter sum / 8); 1 count = 50.4 uV (16-bit, 3.3 V FS). Vssa mV is shown for intuition. `r` is the correlation between the Vssa and Vref series.

| # | Sampling            | OFFCOMP | NOP | Vssa ct (sigma) | Vssa mV | Vref ct (sigma) | Vin ct (sigma) | r(Vssa,Vref) |
|---|---------------------|:-------:|:---:|----------------:|--------:|----------------:|---------------:|-------------:|
| 1 | 4 clk               | on      | no  | 1070 (27.2)     | 53.9    | 41637 (16.4)    | 22836 (11.8)   | -0.82        |
| 2 | 4 clk (SAMPLEN=3)   | off     | no  | 1010 (44.4)     | 50.9    | 41620 (24.1)    | 22800 (12.0)   | -0.96        |
| 3 | 17 clk (SAMPLEN=16) | off     | no  | 251 (4.5)       | 12.6    | 42020 (3.6)     | 22772 (8.3)    | -0.65        |
| 4 | 4 clk               | on      | yes | 191 (1.2)       | 9.6     | 42089 (6.1)     | 22839 (8.8)    | +0.39        |
| 5 | 17 clk (SAMPLEN=16) | off     | yes | 160 (2.6)       | 8.0     | 42050 (2.7)     | 22763 (8.3)    | -0.26        |

Notes:
- Vssa is the indicator: 1070 -> 160 ct (54 -> 8 mV) from the worst to the best state. Bug states (1, 2) sit at ~1000+ ct; healed states (4, 5) at ~160-190 ct.
- Vref mirrors inversely: low (~41620) in the bug, high (~42050-42090) when healed.
- Vin stays essentially stable across all runs (~22760-22840, sigma ~8-12) — the third channel in the sequence is barely affected, consistent with the error diminishing down the sequence.
- r(Vssa,Vref): strongly negative in the bug (-0.82 / -0.96, carryover coupling), near zero / positive when healed (+0.39 / -0.26).

Hardware-averaging note (AVGCTRL.SAMPLENUM, OFFCOMP on): sweeping 64 -> 128 -> 256 -> 512 -> 1024 only dilutes the visible error; it never removes it (jitter persists even at 1024x). Averaging masks the per-sample corruption, it does not fix it.
