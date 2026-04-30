#!/bin/bash
# =============================================================================
# demo.sh — Presentation Live Demo Script
# Run this during the presentation to show live simulation results.
#
# Authors: P. Gurudeep (CS25M111) & Prince Kumar (CS25M112)
# IIT Tirupati, CSA 2025
# =============================================================================

SIM="./sim"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

# Colors
BOLD='\033[1m'
CYN='\033[36m'
GRN='\033[32m'
YEL='\033[33m'
RST='\033[0m'

echo ""
echo -e "${BOLD}${CYN}╔══════════════════════════════════════════════════════════╗"
echo "║         GHB PREFETCHER — LIVE DEMO LAUNCHER             ║"
echo -e "╚══════════════════════════════════════════════════════════╝${RST}"
echo ""
echo "  Choose what to run:"
echo "    1) Step-by-step algorithm walkthrough  (./sim --demo)"
echo "    2) Full simulation — all benchmarks   (./sim)"
echo "    3) Generate PDF plots                 (python3 ../scripts/plot_results.py --mock)"
echo "    4) Run all three in sequence"
echo ""
read -p "  Enter choice [1-4]: " choice

case "$choice" in
  1)
    echo -e "\n${BOLD}${GRN}▶  Running --demo mode...${RST}\n"
    "$SIM" --demo
    ;;
  2)
    echo -e "\n${BOLD}${GRN}▶  Running full simulation...${RST}\n"
    "$SIM"
    ;;
  3)
    echo -e "\n${BOLD}${GRN}▶  Generating PDF plots...${RST}\n"
    python3 ../scripts/plot_results.py --mock
    echo ""
    echo -e "${GRN}  Plots saved to ../results/figures/*.pdf${RST}"
    ls -lh ../results/figures/*.pdf 2>/dev/null | awk '{print "    " $5 "  " $9}'
    ;;
  4)
    echo -e "\n${BOLD}${YEL}▶  Step 1/3 — Algorithm walkthrough${RST}\n"
    "$SIM" --demo
    echo ""
    echo -e "${BOLD}${YEL}  Press Enter to continue to full simulation...${RST}"
    read -r
    echo -e "\n${BOLD}${YEL}▶  Step 2/3 — Full simulation${RST}\n"
    "$SIM"
    echo ""
    echo -e "${BOLD}${YEL}  Press Enter to generate plots...${RST}"
    read -r
    echo -e "\n${BOLD}${YEL}▶  Step 3/3 — Generating PDF plots${RST}\n"
    python3 ../scripts/plot_results.py --mock
    echo -e "${GRN}\n  All done! Open ../results/figures/ for PDF figures.${RST}"
    ;;
  *)
    echo "  Running default: full simulation"
    "$SIM"
    ;;
esac
