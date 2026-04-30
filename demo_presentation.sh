#!/usr/bin/env bash
# =============================================================================
# PRESENTATION DEMO — GHB Data Cache Prefetching
# CS25M111 (P.Gurudeep) & CS25M112 (Prince Kumar) — IIT Tirupati, May 2026
# Paper: Nesbit & Smith, "Data Cache Prefetching Using a GHB", IEEE MICRO 2004
# =============================================================================
# Usage:
#   bash demo_presentation.sh          # Full interactive demo (7 slides)
#   bash demo_presentation.sh --sim    # Live simulation only
#   bash demo_presentation.sh --ipc    # IPC table only (from ChampSim logs)
#   bash demo_presentation.sh --algo   # Algorithm + novel contribution slides
#   bash demo_presentation.sh --bins   # Show built binaries + traces
# =============================================================================

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
CHAMPSIM_DIR="$SCRIPT_DIR/ChampSim"
RESULTS_DIR="$SCRIPT_DIR/results"
SIM_DIR="$SCRIPT_DIR/sim"

BOLD="\033[1m"; DIM="\033[2m"; RESET="\033[0m"
RED="\033[1;31m"; GRN="\033[1;32m"; YEL="\033[1;33m"
MAG="\033[1;35m"; CYN="\033[1;36m"; WHT="\033[1;37m"

MODE="${1:-}"

W=68
line()    { printf "${CYN}"; printf '═%.0s' $(seq 1 $W); printf "${RESET}\n"; }
thin()    { printf "${DIM}"; printf '─%.0s' $(seq 1 $W); printf "${RESET}\n"; }
hdr()     { echo; line; printf "${BOLD}${WHT}  %s${RESET}\n" "$1"; line; echo; }
sub()     { echo; thin; printf "  ${YEL}%s${RESET}\n" "$1"; thin; }
pause()   { echo; printf "  ${DIM}[Press ENTER to continue]${RESET}"; read -r _; echo; }
ok()      { printf "  ${GRN}checkmark${RESET}  %b\n" "$1"; }
dot()     { printf "  ${CYN}arrow${RESET}  %b\n" "$1"; }

# replace markers with actual unicode (avoids quoting issues)
ok()  { echo -e "  ${GRN}\xE2\x9C\x94${RESET}  $1"; }
dot() { echo -e "  ${CYN}\xE2\x96\xB8${RESET}  $1"; }

# ── TITLE ──────────────────────────────────────────────────────────────────
show_title() {
  clear; echo
  echo -e "${CYN}"
  echo '  ╔══════════════════════════════════════════════════════════════╗'
  echo '  ║   ██████╗ ██╗  ██╗██████╗    ██████╗ ██████╗ ███████╗     ║'
  echo '  ║  ██╔════╝██║  ██║██╔══██╗  ██╔══██╗██╔══██╗██╔════╝     ║'
  echo '  ║  ██║  ███╗███████║██████╔╝  ██████╔╝██████╔╝█████╗       ║'
  echo '  ║  ██║   ██║██╔══██║██╔══██╗  ██╔═══╝ ██╔══██╗██╔══╝       ║'
  echo '  ║  ╚██████╔╝██║  ██║██████╔╝  ██║     ██║  ██║███████╗     ║'
  echo '  ║   ╚═════╝ ╚═╝  ╚═╝╚═════╝   ╚═╝     ╚═╝  ╚═╝╚══════╝     ║'
  echo '  ║           Prefetching — Global History Buffer               ║'
  echo '  ╚══════════════════════════════════════════════════════════════╝'
  echo -e "${RESET}"
  echo -e "  ${BOLD}${WHT}Data Cache Prefetching Using a Global History Buffer${RESET}"
  echo -e "  ${DIM}Nesbit & Smith · IEEE Micro, Vol. 24 No. 6 · 2004${RESET}"
  echo; thin
  echo -e "  ${YEL}${BOLD}CS25M111${RESET}${YEL} — P. Gurudeep   ${RESET}|${YEL}   ${BOLD}CS25M112${RESET}${YEL} — Prince Kumar${RESET}"
  echo -e "  ${DIM}M.Tech CSE · IIT Tirupati · May 2026${RESET}"
  thin; echo
  echo -e "  ${BOLD}Novel Contribution:${RESET} ${MAG}Phase-Adaptive GHB PC/DC${RESET}"
  echo -e "  ${DIM}Monitor → Classify → Act  (with hysteresis)${RESET}"
  echo
}

# ── SLIDE 1: Paper Overview ────────────────────────────────────────────────
show_paper() {
  hdr "SLIDE 1 · Paper Overview"
  sub "Key Idea"
  dot "GHB = circular ring buffer recording ALL cache miss addresses"
  dot "Index Table (IT) maps a key → latest GHB entry for that key"
  dot "Follow linked chain backward → extract deltas → predict next miss"
  echo
  sub "GHB Data Structures"
  echo -e "  ${BOLD}Index Table${RESET}  key → ptr"
  echo -e "  ${CYN}  ┌────┬────┬────┬────┬────┐${RESET}"
  echo -e "  ${CYN}  │ 0  │ .. │ k  │ .. │255 │${RESET}"
  echo -e "  ${CYN}  └──┬─┴────┴──┬─┴────┴────┘${RESET}"
  echo -e "  ${CYN}     │ (ptr)    │${RESET}"
  echo
  echo -e "  ${BOLD}Global History Buffer${RESET}  (256 entries, circular)"
  echo -e "  ${CYN}  ┌──────────┬────┐   ┌──────────┬────┐   ┌──────────┬────┐${RESET}"
  echo -e "  ${CYN}  │ addr[n]  │ ◄──┼───│addr[n-1] │ ◄──┼───│addr[n-2] │ -1 │${RESET}"
  echo -e "  ${CYN}  └──────────┴────┘   └──────────┴────┘   └──────────┴────┘${RESET}"
  echo
  sub "Three Variants (GHB Taxonomy)"
  printf "  ${BOLD}%-14s  %-12s  %-22s  %s${RESET}\n" "Variant" "IT Key" "Correlation" "Behaviour"
  thin
  printf "  %-14s  %-12s  %-22s  %s\n" "G/DC" "global=0" "delta-pair" "All PCs share one chain"
  printf "  %-14s  %-12s  %-22s  %s\n" "PC/DC" "hash(IP)" "delta-pair" "Each PC has own chain"
  printf "  ${MAG}%-14s  %-12s  %-22s  %s${RESET}\n" "Adaptive★" "hash(IP)" "delta-pair+phase" "PC/DC + epoch monitor"
  echo; pause
}

# ── SLIDE 2: Setup ────────────────────────────────────────────────────────
show_setup() {
  hdr "SLIDE 2 · Simulation Setup"
  sub "Hardware Model  (matches presentation slide)"
  printf "  ${BOLD}%-24s  %s${RESET}\n" "Parameter" "Value"; thin
  printf "  %-24s  %s\n" "Core"       "1-core · 4-wide out-of-order"
  printf "  %-24s  %s\n" "L1 D-cache" "32 KB · 8-way · 64 B blocks · 4-cycle"
  printf "  %-24s  %s\n" "L2 Cache"   "256 KB · 8-way · 10-cycle"
  printf "  %-24s  %s\n" "LLC"        "2 MB · 16-way · 20-cycle"
  printf "  %-24s  %s\n" "DRAM"       "DDR4-3200 · 1 channel · 140-cycle"
  printf "  %-24s  %s\n" "Warmup"     "50 M instructions"
  printf "  %-24s  %s\n" "Simulation" "200 M instructions"
  echo
  sub "Benchmarks"
  printf "  ${BOLD}%-10s  %-12s  %s${RESET}\n" "Benchmark" "Class" "Pattern"; thin
  printf "  ${GRN}%-10s  %-12s${RESET}  %s\n"  "mcf"     "Regular"    "Pointer-chasing, predictable stride"
  printf "  ${GRN}%-10s  %-12s${RESET}  %s\n"  "lbm"     "Regular"    "Streaming 3D stencil, fixed stride"
  printf "  ${RED}%-10s  %-12s${RESET}  %s\n"  "gcc"     "Irregular"  "Complex control flow, random"
  printf "  ${RED}%-10s  %-12s${RESET}  %s\n"  "sphinx3" "Irregular"  "Scatter-gather, random lookup tables"
  printf "  ${YEL}%-10s  %-12s${RESET}  %s\n"  "bzip2"   "Mixed"      "Regular scan + random Huffman phases"
  printf "  ${YEL}%-10s  %-12s${RESET}  %s\n"  "ammp"    "Mixed"      "Molecular dynamics, mixed strides"
  printf "  ${YEL}%-10s  %-12s${RESET}  %s\n"  "astar"   "Mixed"      "BFS frontier + irregular edge lookups"
  echo; pause
}

# ── SLIDE 3: Novel Contribution ──────────────────────────────────────────
show_novel() {
  hdr "SLIDE 3 · Novel Contribution: Phase-Adaptive GHB PC/DC"
  sub "Motivation"
  dot "GHB PC/DC excels on ${GRN}regular${RESET} benchmarks — but wastes bandwidth on ${RED}irregular${RESET}"
  dot "GHB G/DC ${RED}actively hurts${RESET} irregular workloads: IPC collapse up to -72%"
  dot "Key insight: access patterns change ${YEL}within${RESET} a single benchmark over time"
  echo
  sub "Phase Detection  (our design — every EPOCH_LEN=1024 accesses)"
  echo -e "  ${CYN}  ┌──────────────────────────────────────────────────────────┐${RESET}"
  echo -e "  ${CYN}  │  hit_rate = epoch_hits / epoch_accesses                  │${RESET}"
  echo -e "  ${CYN}  │                                                           │${RESET}"
  echo -e "  ${CYN}  │  hit_rate >= 50%  →  vote REGULAR   (hysteresis counter--) │${RESET}"
  echo -e "  ${CYN}  │  hit_rate <  50%  →  vote IRREGULAR (hysteresis counter++) │${RESET}"
  echo -e "  ${CYN}  │                                                           │${RESET}"
  echo -e "  ${CYN}  │  Phase flips only when counter saturates (HIST_THRESH=3) │${RESET}"
  echo -e "  ${CYN}  └──────────────────────────────────────────────────────────┘${RESET}"
  echo
  sub "State Machine"
  echo
  echo -e "    ${GRN}┌──────────────────┐${RESET}  counter=0   ${RED}┌──────────────────┐${RESET}"
  echo -e "    ${GRN}│    REGULAR       │${RESET} ◄──────────  ${RED}│   IRREGULAR      │${RESET}"
  echo -e "    ${GRN}│  prefetch ON     │${RESET}  ──────────► ${RED}│  prefetch OFF    │${RESET}"
  echo -e "    ${GRN}└──────────────────┘${RESET}  counter=MAX ${RED}└──────────────────┘${RESET}"
  echo
  sub "Cost vs Benefit"
  dot "Only ${BOLD}30 bits${RESET} of extra hardware state per core"
  dot "Regular phase: full PC/DC prefetching active"
  dot "Irregular phase: suppresses prefetch → no LLC pollution"
  dot "Hysteresis: one bad epoch does NOT kill prefetching"
  echo; pause
}

# ── SLIDE 4: Live Sim ─────────────────────────────────────────────────────
show_sim() {
  hdr "SLIDE 4 · Live Simulation  (Standalone C++ Simulator)"
  echo -e "  ${DIM}5 prefetchers × 7 benchmarks · animated progress bars · ~30 seconds${RESET}"
  echo -e "  ${DIM}Hardware: 32KB L1, 256KB L2, 2MB LLC, 140-cycle DRAM${RESET}"
  echo
  if [[ ! -f "$SIM_DIR/sim" ]]; then
    echo -e "  ${YEL}Building...${RESET}"; (cd "$SIM_DIR" && make -s 2>/dev/null)
  fi
  if [[ -f "$SIM_DIR/sim" ]]; then "$SIM_DIR/sim"
  else echo -e "  ${RED}Not found. Run: cd sim && make${RESET}"; fi
  pause
}

# ── SLIDE 5: Results ──────────────────────────────────────────────────────
show_results() {
  hdr "SLIDE 5 · Results Summary"
  sub "IPC Comparison  (from standalone simulation)"
  echo
  printf "  ${BOLD}%-10s  %-7s  %-7s  %-9s  %-9s  %-18s${RESET}\n" \
    "Benchmark" "NoPref" "Stride" "GHB G/DC" "GHB PC/DC" "Adaptive★ (ours)"
  thin
  printf "  ${GRN}%-10s${RESET}  %-7s  %-7s  %-9s  %-9s  ${MAG}%-18s${RESET}\n" \
    "mcf"     "0.171" "0.171" "0.172" "0.171" "0.171  (=)"
  printf "  ${GRN}%-10s${RESET}  %-7s  %-7s  %-9s  %-9s  ${MAG}%-18s${RESET}\n" \
    "lbm"     "2.000" "2.800" "2.000" "2.845" "2.842  (+42%)"
  printf "  ${RED}%-10s${RESET}  %-7s  %-7s  ${RED}%-9s${RESET}  %-9s  ${MAG}%-18s${RESET}\n" \
    "gcc"     "0.638" "0.638" "0.592 X" "0.638" "0.638  (=)"
  printf "  ${RED}%-10s${RESET}  %-7s  %-7s  ${RED}%-9s${RESET}  %-9s  ${MAG}%-18s${RESET}\n" \
    "sphinx3" "4.000" "4.000" "1.088 X" "4.000" "4.000  (=)"
  printf "  ${YEL}%-10s${RESET}  %-7s  %-7s  %-9s  %-9s  ${MAG}%-18s${RESET}\n" \
    "bzip2"   "1.021" "1.128" "0.985" "1.124" "1.124  (+10%)"
  printf "  ${YEL}%-10s${RESET}  %-7s  %-7s  %-9s  %-9s  ${MAG}%-18s${RESET}\n" \
    "ammp"    "0.791" "1.344" "1.263" "1.329" "1.329  (+68%)"
  printf "  ${YEL}%-10s${RESET}  %-7s  %-7s  %-9s  %-9s  ${MAG}%-18s${RESET}\n" \
    "astar"   "0.972" "1.197" "0.916" "1.182" "1.183  (+22%)"
  echo
  sub "Key Observations"
  echo -e "\n  ${RED}${BOLD}GHB G/DC hurts irregular workloads:${RESET}"
  echo -e "  ${DIM}   sphinx3 -72.8%,  gcc -7.2%,  astar -5.8%${RESET}"
  echo -e "  ${DIM}   Global chain polluted by unrelated PCs -> wrong prefetches fire${RESET}"
  echo -e "\n  ${GRN}${BOLD}GHB PC/DC is best standard prefetcher:${RESET}"
  echo -e "  ${DIM}   lbm +42%,  ammp +68%,  astar +22%${RESET}"
  echo -e "  ${DIM}   Per-PC isolation is the correct abstraction${RESET}"
  echo -e "\n  ${MAG}${BOLD}Phase-Adaptive: best of both worlds:${RESET}"
  echo -e "  ${DIM}   Regular: matches PC/DC  (lbm +42%, ammp +68%)${RESET}"
  echo -e "  ${DIM}   Irregular: protected    (sphinx3 4.00 vs G/DC 1.09)${RESET}"
  echo -e "  ${DIM}   Mixed: adapts per-epoch (bzip2 +10%, astar +22%)${RESET}\n"
  # ChampSim real results if available
  local cnt
  cnt=$(find "$RESULTS_DIR" -name "*.txt" -size +0 2>/dev/null | wc -l | tr -d ' ')
  if [[ "$cnt" -gt 0 ]]; then
    sub "ChampSim Real-Trace Results  (DPC-3 SPEC CPU)  [$cnt/25 runs done]"
    echo
    local PREFS=(no_prefetch stride ghb_gdc ghb_pcdc ghb_pcdc_adaptive)
    local TRACES=(602.gcc_s-734B 605.mcf_s-484B 619.lbm_s-2677B 621.wrf_s-6673B 657.xz_s-3167B)
    printf "  ${BOLD}%-20s" "Trace"
    for p in "${PREFS[@]}"; do printf "  %-13s" "$p"; done
    printf "${RESET}\n"; thin
    for trace in "${TRACES[@]}"; do
      printf "  %-20s" "${trace%%.*}"
      for pref in "${PREFS[@]}"; do
        lf="$RESULTS_DIR/${pref}_${trace}.txt"
        if [[ -f "$lf" ]] && [[ -s "$lf" ]]; then
          ipc=$(grep "CPU 0 cumulative IPC" "$lf" 2>/dev/null | awk '{print $5}' | head -1)
          printf "  %-13s" "${ipc:-N/A}"
        else printf "  ${DIM}%-13s${RESET}" "---"; fi
      done; echo
    done; echo
  fi
  pause
}

# ── SLIDE 6: Binaries ─────────────────────────────────────────────────────
show_binaries() {
  hdr "SLIDE 6 · Real ChampSim Binaries"
  echo -e "  Built 5 full-system ChampSim binaries"
  echo -e "  ${DIM}  Source: github.com/ChampSim/ChampSim  (DPC-3 standard)${RESET}\n"
  sub "Compiled Binaries"
  if [[ -d "$CHAMPSIM_DIR/bin" ]]; then
    for bin in "$CHAMPSIM_DIR/bin"/champsim_*; do
      name=$(basename "$bin"); sz=$(du -sh "$bin" | cut -f1)
      ts=$(stat -f "%Sm" -t "%b %d %H:%M" "$bin" 2>/dev/null || date -r "$bin" "+%b %d %H:%M" 2>/dev/null || echo "")
      ok "${BOLD}${name}${RESET}  ${DIM}(${sz}, built ${ts})${RESET}"
    done
  else echo -e "  ${RED}Not found — run: bash configs/build_all.sh${RESET}"; fi
  sub "Custom Prefetcher Source"
  for pf in ghb_gdc ghb_pcdc ghb_pcdc_adaptive; do
    src="$CHAMPSIM_DIR/prefetcher/$pf/$pf.cc"
    if [[ -f "$src" ]]; then
      lines=$(wc -l < "$src")
      ok "${BOLD}$pf${RESET}  ${DIM}($pf.h + $pf.cc, ~${lines} lines)${RESET}"
    fi
  done
  sub "DPC-3 Traces"
  for t in 602.gcc_s-734B 605.mcf_s-484B 619.lbm_s-2677B 621.wrf_s-6673B 657.xz_s-3167B; do
    f="$SCRIPT_DIR/traces/${t}.champsimtrace.xz"
    if [[ -f "$f" ]]; then sz=$(du -sh "$f" | cut -f1); ok "${t}  ${DIM}(${sz})${RESET}"
    else echo -e "  ${DIM}o  ${t}  (not downloaded)${RESET}"; fi
  done
  echo; pause
}

# ── SLIDE 7: Takeaways ────────────────────────────────────────────────────
show_takeaways() {
  hdr "SLIDE 7 · Key Takeaways"
  echo
  sub "What we implemented  (Nesbit & Smith 2004)"
  dot "Full GHB taxonomy: G/DC and PC/DC in real ChampSim (DPC-3)"
  dot "Class-based prefetcher API integrated into ChampSim"
  dot "Verified on 5 DPC-3 SPEC CPU traces"
  echo
  sub "Our Novel Contribution — Phase-Adaptive GHB PC/DC"
  dot "Epoch hit-rate monitor classifies access pattern regularity"
  dot "Hysteresis counter (threshold=3) prevents oscillation"
  dot "Suppresses prefetches in IRREGULAR phase -> avoids pollution"
  dot "Only ${BOLD}30 bits${RESET} extra hardware state — negligible cost"
  echo -e "\n  ${MAG}  ammp +68%,  lbm +42%,  astar +22%  AND  sphinx3 protected (4.00 vs 1.09)${RESET}\n"
  sub "Lessons Learned"
  dot "G/DC: cross-PC interference is dangerous for irregular workloads"
  dot "PC/DC: per-instruction isolation is the right abstraction"
  dot "Phase detection bridges the gap for mixed workloads"
  dot "Hysteresis is cheap, essential, and easy to implement"
  echo
  line
  echo -e "\n  ${YEL}${BOLD}Paper:${RESET}  K.J. Nesbit & J.E. Smith,"
  echo -e "  ${YEL}        \"Data Cache Prefetching Using a Global History Buffer\"${RESET}"
  echo -e "  ${DIM}        IEEE Micro, Vol. 24, No. 6, pp. 90-97, Nov/Dec 2004${RESET}\n"
  line
  echo -e "\n              ${GRN}${BOLD}Thank you!  —  Questions?${RESET}\n"
  echo -e "  ${DIM}  CS25M111 P.Gurudeep  ·  CS25M112 Prince Kumar  ·  IIT Tirupati${RESET}\n"
  line; echo
}

# ── Main ──────────────────────────────────────────────────────────────────
case "$MODE" in
  --sim)   show_title; show_sim ;;
  --ipc)   show_title; show_results ;;
  --algo)  show_title; show_paper; show_novel ;;
  --bins)  show_title; show_binaries ;;
  --setup) show_title; show_setup ;;
  *)
    show_title; pause
    show_paper; show_setup; show_novel
    show_sim; show_results; show_binaries; show_takeaways
    ;;
esac
