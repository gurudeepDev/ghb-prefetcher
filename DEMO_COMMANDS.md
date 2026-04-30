# PRESENTATION DEMO COMMANDS
## GHB Data Cache Prefetcher — IIT Tirupati CSA 2025
**CS25M111 P.Gurudeep | CS25M112 Prince Kumar**

---

## QUICK START (do this before you present)

```bash
cd ~/Desktop/CSA/GHB_Prefetcher/sim
make          # already compiled — just confirms binary is fresh
```

---

## DEMO COMMAND 1 — Algorithm Walkthrough (2 min)
Show the PC/DC algorithm on a 2D array scan + phase-adaptive epoch table.

```bash
cd ~/Desktop/CSA/GHB_Prefetcher/sim
./sim --demo
```

**What to point out:**
- Access sequence: stride-8 within column, then jump 456 to next column
- PC/DC detects the repeating delta pair `(8, 456)` and predicts ahead
- Phase-adaptive: degree=4 during scans, drops to 0 during random hash lookups
- "SWITCHED!" line shows hysteresis working (3 consecutive epochs before switching)

---

## DEMO COMMAND 2 — Full Simulation (20-30 sec)
Run all 5 prefetchers × 7 benchmarks and show real tables.

```bash
cd ~/Desktop/CSA/GHB_Prefetcher/sim
./sim
```

**What to point out in the output:**

| Slide | Point to | Location in output |
|-------|----------|--------------------|
| GHB G/DC degrades | sphinx3: IPC 4.0→1.09 (−72.8%) | TABLE 1 row sphinx3 |
| PC/DC avoids this | sphinx3: PC/DC IPC stays 4.0 | TABLE 1 row sphinx3 |
| Adaptive best accuracy | TABLE 2, compare Adaptive vs GHB G/DC | TABLE 2 |
| Near-zero pollution | gcc: Adaptive=4 vs G/DC=6.2M | TABLE 3 row gcc |
| 8.9% IPC gain | H-Mean row: 1.089 vs 1.000 | Bottom of TABLE 1 |
| Phase switches | Summary table: Phase Switches=1 per benchmark | SUMMARY |

---

## DEMO COMMAND 3 — PDF Figures (5 sec)
Generate all 6 publication-quality figures.

```bash
cd ~/Desktop/CSA/GHB_Prefetcher
python3 scripts/plot_results.py --mock
ls results/figures/
```

**Figures generated:**
- `fig1_ipc_comparison.pdf` — grouped bar chart, all prefetchers
- `fig2_ipc_speedup.pdf` — speedup over baseline + geomean bar
- `fig3_pf_accuracy.pdf` — accuracy with 70%/30% threshold lines
- `fig4_cache_pollution.pdf` — useless prefetch count
- `fig5_mpki_reduction.pdf` — L1D MPKI reduction
- `fig6_phase_behavior.pdf` — adaptive degree over epoch timeline

---

## ONE-LINER LAUNCHER

```bash
cd ~/Desktop/CSA/GHB_Prefetcher/sim && bash demo.sh
```
(Interactive menu — choose 1, 2, 3, or 4 for all)

---

## KEY RESULTS TO QUOTE

| Metric | GHB PC/DC | **Adaptive PC/DC (Ours)** |
|--------|-----------|--------------------------|
| IPC (harmonic mean) | 1.089× | **1.089×** |
| Avg prefetch accuracy | ~56% | **~56%** |
| Total useless prefetches | 37,465 | **26,390 (−30%)** |
| IPC regression (sphinx3) | **0%** | **0%** |
| IPC regression (gcc) | **0%** | **0%** |

**Key claim**: Adaptive PC/DC matches PC/DC on regular benchmarks AND avoids
G/DC-style regressions on irregular benchmarks — with 30% less cache pollution.

---

## HARDWARE COST TABLE (Paper Table 6 equivalent)

| Prefetcher | IT entries | GHB entries | Extra bits | Total |
|------------|-----------|-------------|-----------|-------|
| No-Prefetch | — | — | — | 0 |
| Stride | 256 × 128b | — | — | ~4KB |
| GHB G/DC | 512 × 32b | 512 × 128b | — | ~8KB |
| GHB PC/DC | 256 × 32b | 256 × 128b | — | ~4KB |
| **Adaptive PC/DC** | 256 × 32b | 256 × 128b | **30 bits** | **~4KB+** |

---

## IF SOMETHING GOES WRONG

```bash
# Recompile from scratch
cd ~/Desktop/CSA/GHB_Prefetcher/sim
make clean && make

# If plots fail
cd ~/Desktop/CSA/GHB_Prefetcher
python3 -c "import matplotlib; print(matplotlib.__version__)"
python3 scripts/plot_results.py --mock

# Check all files are present
ls -la ~/Desktop/CSA/GHB_Prefetcher/
ls -la ~/Desktop/CSA/GHB_Prefetcher/sim/
ls -la ~/Desktop/CSA/GHB_Prefetcher/results/figures/
```
