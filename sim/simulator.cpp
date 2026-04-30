// =============================================================================
// simulator.cpp  —  Standalone GHB Prefetcher Simulator
// =============================================================================
// Self-contained simulation of 5 prefetcher variants on 7 benchmark patterns.
// No external dependencies. Compile with:
//   g++ -O2 -std=c++14 -o sim simulator.cpp
//
// Authors : P. Gurudeep (CS25M111) & Prince Kumar (CS25M112)
// Course  : Computer System Architecture, IIT Tirupati 2025
// Paper   : Nesbit & Smith, "Data Cache Prefetching Using a GHB", IEEE MICRO 2004
// =============================================================================

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cassert>
#include <cstdint>
#include <ctime>
#include <string>
#include <vector>
#include <algorithm>
#include <unordered_map>
#include <functional>

// ═══════════════════════════════════════════════════════════════════════════════
//  TERMINAL COLORS
// ═══════════════════════════════════════════════════════════════════════════════
#define RST  "\033[0m"
#define BOLD "\033[1m"
#define RED  "\033[31m"
#define GRN  "\033[32m"
#define YEL  "\033[33m"
#define BLU  "\033[34m"
#define MAG  "\033[35m"
#define CYN  "\033[36m"
#define BRED "\033[1;31m"
#define BGRN "\033[1;32m"
#define BYEL "\033[1;33m"
#define BBLU "\033[1;34m"
#define BMAG "\033[1;35m"
#define BCYN "\033[1;36m"

// ═══════════════════════════════════════════════════════════════════════════════
//  CACHE HIERARCHY PARAMETERS  (match project presentation slide)
// ═══════════════════════════════════════════════════════════════════════════════
static const int BLOCK_BITS   = 6;           // 64-byte cache lines
static const int BLOCK_SIZE   = 1 << BLOCK_BITS;

// L1D: 32 KB, 8-way
static const int L1D_SETS     = 64;
static const int L1D_WAYS     = 8;
static const int L1D_LATENCY  = 4;          // cycles

// L2: 256 KB, 8-way
static const int L2_SETS      = 512;
static const int L2_WAYS      = 8;
static const int L2_LATENCY   = 10;

// LLC: 2 MB, 16-way
static const int LLC_SETS     = 2048;
static const int LLC_WAYS     = 16;
static const int LLC_LATENCY  = 20;

// DRAM latency
static const int DRAM_LATENCY = 140;

// Pipeline: 4-wide OOO; assume 20% of instructions are loads
static const double LOAD_FRAC  = 0.20;
static const int    ISSUE_WIDTH = 4;

// Simulation size per benchmark
static const uint64_t WARMUP_ACCESSES = 500000ULL;   // 500 K
static const uint64_t SIM_ACCESSES    = 5000000ULL;  // 5 M   (fast demo)

// ═══════════════════════════════════════════════════════════════════════════════
//  SET-ASSOCIATIVE CACHE with LRU replacement
// ═══════════════════════════════════════════════════════════════════════════════
struct CacheSet {
    std::vector<uint64_t> tags;     // tag (block address)
    std::vector<int>      lru;      // LRU counter (higher = more recently used)
    std::vector<bool>     prefetch_bit; // was this line brought in by prefetch?
    int ways;

    void init(int w) {
        ways = w;
        tags.assign(w, ~0ULL);
        lru.assign(w, 0);
        prefetch_bit.assign(w, false);
    }

    // Returns true on hit. Promotes to MRU.
    bool access(uint64_t tag, bool& was_prefetch_hit) {
        for (int i = 0; i < ways; i++) {
            if (tags[i] == tag) {
                was_prefetch_hit = prefetch_bit[i];
                prefetch_bit[i] = false;   // demand access clears the bit
                // Promote to MRU
                int my_lru = lru[i];
                for (int j = 0; j < ways; j++)
                    if (lru[j] > my_lru) lru[j]--;
                lru[i] = ways - 1;
                return true;
            }
        }
        was_prefetch_hit = false;
        return false;
    }

    // Fill: insert block. Returns evicted tag (or ~0ULL). is_prefetch marks fill.
    uint64_t fill(uint64_t tag, bool is_prefetch, uint64_t& evicted_was_pfb) {
        // Find LRU victim
        int victim = 0;
        for (int i = 1; i < ways; i++)
            if (lru[i] < lru[victim]) victim = i;
        uint64_t evicted = tags[victim];
        evicted_was_pfb = prefetch_bit[victim] ? 1 : 0;
        tags[victim]         = tag;
        prefetch_bit[victim] = is_prefetch;
        // Promote victim to MRU
        int old_lru = lru[victim];
        for (int j = 0; j < ways; j++)
            if (lru[j] > old_lru) lru[j]--;
        lru[victim] = ways - 1;
        return evicted;
    }

    bool probe(uint64_t tag) const {
        for (int i = 0; i < ways; i++)
            if (tags[i] == tag) return true;
        return false;
    }
};

struct Cache {
    std::vector<CacheSet> sets;
    int n_sets, n_ways;
    uint64_t hits, misses, pf_useful, pf_useless, evictions;

    void init(int s, int w) {
        n_sets = s; n_ways = w;
        sets.resize(s);
        for (auto& cs : sets) cs.init(w);
        hits = misses = pf_useful = pf_useless = evictions = 0;
    }

    inline uint64_t block(uint64_t addr) { return addr >> BLOCK_BITS; }
    inline int      set_idx(uint64_t addr) {
        return (int)((addr >> BLOCK_BITS) % n_sets);
    }
    inline uint64_t tag(uint64_t addr) { return addr >> BLOCK_BITS; }

    // Demand access. Returns miss penalty cycles (0 = hit).
    // Sets 'miss_out' true on miss.
    bool probe_only(uint64_t addr) {
        return sets[set_idx(addr)].probe(tag(addr));
    }

    bool demand(uint64_t addr, bool& was_prefetch_hit) {
        int si = set_idx(addr);
        bool hit = sets[si].access(tag(addr), was_prefetch_hit);
        if (hit) { hits++; if (was_prefetch_hit) pf_useful++; }
        else      { misses++; }
        return hit;
    }

    // Fill line into cache. Returns evicted block tag (~0 if nothing evicted).
    uint64_t fill(uint64_t addr, bool is_prefetch) {
        int si = set_idx(addr);
        uint64_t evicted_pfb = 0;
        uint64_t ev = sets[si].fill(tag(addr), is_prefetch, evicted_pfb);
        if (ev != ~0ULL) {
            evictions++;
            if (is_prefetch && evicted_pfb) pf_useless++;
        }
        return ev;
    }

    void reset_stats() {
        hits = misses = pf_useful = pf_useless = evictions = 0;
    }
};

// ═══════════════════════════════════════════════════════════════════════════════
//  MEMORY SYSTEM (L1D → L2 → LLC → DRAM)
// ═══════════════════════════════════════════════════════════════════════════════
struct MemSystem {
    Cache l1d, l2c, llc;

    // Stats
    uint64_t total_accesses;
    uint64_t l1d_misses, l2_misses, llc_misses;
    uint64_t pf_issued, pf_useful_l1d;
    uint64_t pf_useless_evictions;
    double   total_stall_cycles;

    // PAB: Prefetch Address Buffer — tracks all outstanding prefetch addresses
    // so we can credit usefulness even if the prefetch_bit was evicted from cache
    // before the demand access arrived (fixes 0% accuracy for high-volume prefetchers).
    static const int PAB_SIZE = 1024;
    uint64_t pab_addrs[PAB_SIZE];   // block-aligned addresses
    bool     pab_valid[PAB_SIZE];
    int      pab_head;

    void pab_insert(uint64_t addr) {
        uint64_t cl = (addr >> BLOCK_BITS) << BLOCK_BITS;
        pab_addrs[pab_head] = cl;
        pab_valid[pab_head] = true;
        pab_head = (pab_head + 1) % PAB_SIZE;
    }
    bool pab_lookup_and_clear(uint64_t addr) {
        uint64_t cl = (addr >> BLOCK_BITS) << BLOCK_BITS;
        for (int i = 0; i < PAB_SIZE; i++) {
            if (pab_valid[i] && pab_addrs[i] == cl) {
                pab_valid[i] = false;
                return true;
            }
        }
        return false;
    }

    void init() {
        l1d.init(L1D_SETS, L1D_WAYS);
        l2c.init(L2_SETS,  L2_WAYS);
        llc.init(LLC_SETS, LLC_WAYS);
        total_accesses = 0;
        l1d_misses = l2_misses = llc_misses = 0;
        pf_issued = pf_useful_l1d = 0;
        pf_useless_evictions = 0;
        total_stall_cycles = 0.0;
        memset(pab_addrs, 0, sizeof(pab_addrs));
        memset(pab_valid,  0, sizeof(pab_valid));
        pab_head = 0;
    }

    void reset_stats() {
        l1d.reset_stats();
        l2c.reset_stats();
        llc.reset_stats();
        total_accesses = l1d_misses = l2_misses = llc_misses = 0;
        pf_issued = pf_useful_l1d = 0;
        pf_useless_evictions = 0;
        total_stall_cycles = 0.0;
        memset(pab_addrs, 0, sizeof(pab_addrs));
        memset(pab_valid,  0, sizeof(pab_valid));
        pab_head = 0;
    }

    // Process a demand access. Returns stall cycles added.
    int access(uint64_t addr) {
        total_accesses++;
        bool pfhit;

        // Check PAB first — credit usefulness regardless of which cache level
        // holds the line (fixes accuracy=0 when prefetch_bit was evicted before demand)
        bool was_prefetched = pab_lookup_and_clear(addr);

        if (l1d.demand(addr, pfhit)) {
            if (was_prefetched) pf_useful_l1d++;
            return 0;  // L1D hit — no stall
        }

        l1d_misses++;

        // Miss: promote from L2
        if (l2c.demand(addr, pfhit)) {
            if (was_prefetched) pf_useful_l1d++;   // prefetch hit absorbed at L2
            l1d.fill(addr, false);
            return L2_LATENCY;
        }
        l2_misses++;

        if (llc.demand(addr, pfhit)) {
            if (was_prefetched) pf_useful_l1d++;   // prefetch hit absorbed at LLC
            l2c.fill(addr, false);
            l1d.fill(addr, false);
            return LLC_LATENCY;
        }
        llc_misses++;

        // DRAM
        llc.fill(addr, false);
        l2c.fill(addr, false);
        l1d.fill(addr, false);
        return DRAM_LATENCY;
    }

    // Prefetch an address into L1D (miss-triggered, as in the paper)
    // Returns true if the prefetch was actually useful (line wasn't already there)
    bool prefetch(uint64_t addr) {
        if (l1d.probe_only(addr)) return false;  // already in L1D — skip
        pf_issued++;
        pab_insert(addr);   // record in PAB for accurate usefulness tracking

        // Fill into L1D hierarchy
        if (!l2c.probe_only(addr) && !llc.probe_only(addr)) {
            // Cold miss: install in all levels
            llc.fill(addr, true);
            l2c.fill(addr, true);
        }
        l1d.fill(addr, true);
        return true;
    }

    double mpki(uint64_t instructions) const {
        return instructions > 0 ? (double)l1d_misses / instructions * 1000.0 : 0.0;
    }
};

// ═══════════════════════════════════════════════════════════════════════════════
//  PREFETCHER INTERFACE
// ═══════════════════════════════════════════════════════════════════════════════
struct Prefetcher {
    std::string name;
    std::string label;
    virtual ~Prefetcher() {}
    virtual void reset()   = 0;
    virtual void on_miss(MemSystem& mem, uint64_t addr, uint64_t ip) = 0;
    virtual void on_access(MemSystem& mem, uint64_t addr, uint64_t ip, bool hit) {}
};

// ── 1. NO PREFETCH ────────────────────────────────────────────────────────────
struct NoPrefetch : Prefetcher {
    NoPrefetch() { name = "no_prefetch"; label = "No-Prefetch"; }
    void reset() override {}
    void on_miss(MemSystem&, uint64_t, uint64_t) override {}
};

// ── 2. STRIDE (PC/CS) ────────────────────────────────────────────────────────
struct StridePrefetcher : Prefetcher {
    static const int IT_SIZE = 256;
    static const int DEGREE  = 4;
    enum State { INIT=0, TRANSIENT=1, STEADY=2, NOPRED=3 };
    struct Entry { uint64_t last_addr; int64_t stride; State state; uint16_t tag; bool valid; };
    Entry it[IT_SIZE];

    StridePrefetcher() { name="stride"; label="Stride (PC/CS)"; reset(); }
    void reset() override { memset(it,0,sizeof(it)); }

    void on_miss(MemSystem& mem, uint64_t addr, uint64_t ip) override {
        int idx      = (int)(((ip>>2)^(ip>>10)) % IT_SIZE);
        uint16_t tag = (uint16_t)((ip>>2) & 0x3FF);
        Entry& e = it[idx];
        if (!e.valid || e.tag != tag) {
            e = {addr, 0, INIT, tag, true};
            return;
        }
        int64_t ns = (int64_t)addr - (int64_t)e.last_addr;
        switch(e.state){
            case INIT:      e.stride=ns; e.state=TRANSIENT; break;
            case TRANSIENT: if(ns==e.stride) e.state=STEADY; else e.stride=ns; break;
            case STEADY:    if(ns!=e.stride){e.stride=ns; e.state=NOPRED;} break;
            case NOPRED:    e.stride=ns; e.state=TRANSIENT; break;
        }
        if(e.state==STEADY && e.stride!=0){
            uint64_t pa=addr;
            for(int d=1;d<=DEGREE;d++){ pa=(uint64_t)((int64_t)pa+e.stride); mem.prefetch(pa); }
        }
        e.last_addr=addr;
    }
};

// ── 3. GHB G/DC ──────────────────────────────────────────────────────────────
// Per Nesbit & Smith 2004: IT is indexed by delta (global, not PC).
// The chain from the matched entry is walked forward to collect past deltas,
// then those deltas are replayed from the current address to generate prefetches.
struct GHB_GDC : Prefetcher {
    static const int IT_SIZE  = 512;
    static const int GHB_SIZE = 512;
    static const int DEGREE   = 4;
    static const int MAX_WALK = 16;
    struct GEntry { uint64_t addr; int32_t prev; uint64_t serial; };
    GEntry ghb[GHB_SIZE];
    int32_t it[IT_SIZE];
    int32_t head;
    uint64_t serial;
    uint64_t gprev;
    bool gprev_valid;

    GHB_GDC(){ name="ghb_gdc"; label="GHB G/DC"; reset(); }
    void reset() override {
        memset(ghb,0,sizeof(ghb));
        for(int i=0;i<GHB_SIZE;i++) ghb[i].prev=-1;
        for(int i=0;i<IT_SIZE;i++)  it[i]=-1;
        head=0; serial=0; gprev=0; gprev_valid=false;
    }
    bool valid(int32_t i){ return i>=0&&i<GHB_SIZE&&(serial-ghb[i].serial)<(uint64_t)GHB_SIZE; }
    // Hash delta to IT index
    int32_t key(int64_t d){
        uint64_t u=(uint64_t)d;
        u ^= (u >> 32) ^ (u >> 16) ^ (u >> 7);
        return (int32_t)(u % IT_SIZE);
    }

    void on_miss(MemSystem& mem, uint64_t addr, uint64_t ip) override {
        if(!gprev_valid){ gprev=addr; gprev_valid=true; return; }
        int64_t delta=(int64_t)addr-(int64_t)gprev;
        int32_t k=key(delta);
        int32_t old=it[k];
        int32_t ni=head;
        ghb[ni]={addr, (valid(old)&&old!=ni)?old:-1, ++serial};
        it[k]=ni;
        head=(head+1)%GHB_SIZE;
        gprev=addr;

        // Walk the linked chain from the previous occurrence (oldest-to-newest)
        // to collect the sequence of addresses seen after a similar delta in the past.
        if(!valid(old)) return;

        // Collect chain from old entry forward (follow prev links = older entries)
        // We want addresses AFTER old, i.e. the entries that followed it historically.
        // The GHB is a circular buffer. Entries AFTER old in history are at higher
        // serial numbers. We walk old's prev chain to get older context, then
        // use addresses after old (old+1, old+2... in serial order) as "future" deltas.
        // Correct G/DC: collect deltas AFTER the matched entry's position in the chain.
        // The chain linked list goes: new → old → older → oldest.
        // After inserting ni, old's successor in time is ni. old's predecessor in time
        // is old->prev. We want deltas that FOLLOWED old, meaning the entries between
        // old and ni in serial time. We walk forward from old+1 serial to ni-1.
        // Since GHB is circular, find entries with serial in (old.serial, ni.serial).
        // Simple approach: collect up to MAX_WALK addresses from old forward by serial.
        uint64_t chain_addr[MAX_WALK+2];
        int chain_len = 0;
        chain_addr[chain_len++] = ghb[old].addr;

        // Walk forward in serial order from old (inclusive) to ni (exclusive)
        uint64_t old_serial = ghb[old].serial;
        uint64_t ni_serial  = ghb[ni].serial;
        // Entries are stored in ghb[(old + offset) % GHB_SIZE] for offset=1,2,...
        // as long as their serial is between old_serial+1 and ni_serial-1
        for(int off=1; off<=MAX_WALK && chain_len<=MAX_WALK; off++){
            int32_t pos=(int32_t)((old+off)%GHB_SIZE);
            if(!valid(pos)) break;
            if(ghb[pos].serial <= old_serial) break;  // wrapped around
            if(ghb[pos].serial >= ni_serial) break;   // reached current entry
            chain_addr[chain_len++] = ghb[pos].addr;
        }
        if(chain_len < 2) return;

        // Compute deltas from the historical continuation after old
        uint64_t pa = addr;
        int emitted = 0;
        for(int i=0; i+1<chain_len && emitted<DEGREE; i++){
            int64_t pd = (int64_t)chain_addr[i+1] - (int64_t)chain_addr[i];
            pa = (uint64_t)((int64_t)pa + pd);
            mem.prefetch(pa);
            emitted++;
        }
        // If we ran out of history, repeat last delta to fill degree
        if(emitted < DEGREE && chain_len >= 2){
            int64_t last_delta = (int64_t)chain_addr[chain_len-1] - (int64_t)chain_addr[chain_len-2];
            for(; emitted < DEGREE; emitted++){
                pa = (uint64_t)((int64_t)pa + last_delta);
                mem.prefetch(pa);
            }
        }
    }
};

// ── 4. GHB PC/DC (paper) ─────────────────────────────────────────────────────
// Per Nesbit & Smith 2004:
//   - IT indexed by PC hash → head of PC's GHB chain
//   - Walk chain to collect recent addresses for this PC
//   - Compute deltas, find most-recent matching delta-pair (d0,d1)
//   - Predict: replay deltas del[mp+1..nd-1] cyclically for DEGREE steps
struct GHB_PCDC : Prefetcher {
    static const int IT_SIZE  = 1024;  // larger = less aliasing across PCs
    static const int GHB_SIZE = 2048;  // larger = deeper history per PC
    static const int DEGREE   = 4;
    static const int MAX_WALK = 48;    // max chain entries to walk
    struct GEntry { uint64_t addr; int32_t prev; uint64_t serial; };
    GEntry ghb[GHB_SIZE];
    int32_t it[IT_SIZE];
    int32_t head;
    uint64_t serial;

    GHB_PCDC(){ name="ghb_pcdc"; label="GHB PC/DC"; reset(); }
    void reset() override {
        memset(ghb,0,sizeof(ghb));
        for(int i=0;i<GHB_SIZE;i++) ghb[i].prev=-1;
        for(int i=0;i<IT_SIZE;i++)  it[i]=-1;
        head=0; serial=0;
    }
    bool valid(int32_t i){ return i>=0&&i<GHB_SIZE&&(serial-ghb[i].serial)<(uint64_t)GHB_SIZE; }
    int32_t pchash(uint64_t ip){
        uint64_t h=(ip>>2)^(ip>>10)^(ip>>18)^(ip>>5);
        return (int32_t)(h % IT_SIZE);
    }

    void on_miss(MemSystem& mem, uint64_t addr, uint64_t ip) override {
        int32_t k  = pchash(ip);
        int32_t ni = head;
        ghb[ni] = {addr, (valid(it[k]) && it[k]!=ni) ? it[k] : -1, ++serial};
        it[k] = ni;
        head = (head+1) % GHB_SIZE;

        // Walk chain newest-first (ni → prev → prev → ...)
        uint64_t raw[MAX_WALK+2]; int cl = 0;
        int32_t cur = ni;
        while(cl <= MAX_WALK && cur != -1 && valid(cur)){
            raw[cl++] = ghb[cur].addr;
            cur = ghb[cur].prev;
        }
        if(cl < 3) return;  // need at least 3 addresses → 2 deltas

        // Reverse to oldest-first order
        uint64_t seq[MAX_WALK+2];
        for(int i=0;i<cl;i++) seq[i] = raw[cl-1-i];

        // Compute deltas
        int nd = cl - 1;
        int64_t del[MAX_WALK+1];
        for(int i=0;i<nd;i++) del[i] = (int64_t)seq[i+1] - (int64_t)seq[i];
        if(nd < 2) return;

        // Current pair = last two deltas
        int64_t d0 = del[nd-2], d1 = del[nd-1];

        // Find most-recent previous occurrence of (d0,d1) in the chain
        // (search from nd-2 backward — skip the pair we just formed)
        int mp = -1;
        for(int i = nd-3; i >= 1; i--){
            if(del[i-1] == d0 && del[i] == d1){ mp = i; break; }
        }
        if(mp < 0) return;  // no matching pair found → no prediction

        // Pattern: deltas that followed the previous (d0,d1) occurrence
        // = del[mp+1], del[mp+2], ..., del[nd-1]
        // The length of the repeating unit is (nd-1) - mp
        int pat_start = mp + 1;          // first delta after matched pair
        int pat_end   = nd - 1;          // last delta in chain (= d1, index nd-1)
        int pat_len   = pat_end - pat_start + 1;  // #deltas in the pattern
        if(pat_len < 1) pat_len = 1;

        // Prefetch DEGREE addresses by cycling through the pattern
        uint64_t pa = addr;
        for(int k2 = 0; k2 < DEGREE; k2++){
            int pi = pat_start + (k2 % pat_len);
            // pi should always be in [pat_start, pat_end] ⊂ [0, nd-1]
            if(pi < 0 || pi >= nd) break;
            pa = (uint64_t)((int64_t)pa + del[pi]);
            mem.prefetch(pa);
        }
    }
};

// ── 5. PHASE-ADAPTIVE GHB PC/DC (Our Contribution) ───────────────────────────
// Extends GHB PC/DC with a per-epoch accuracy monitor.
// If accuracy >= 70% for HYSTERESIS consecutive epochs → PRED phase (degree=4)
// If accuracy <= 30% for HYSTERESIS consecutive epochs → IRREG phase (degree=0)
// PPB (Prefetch Pending Buffer) tracks which lines were prefetched so demand
// hits to prefetched lines can be counted as useful.
struct GHB_PCDC_Adaptive : Prefetcher {
    static const int IT_SIZE    = 1024;  // match improved GHB PC/DC
    static const int GHB_SIZE   = 2048;  // match improved GHB PC/DC
    static const int MAX_DEGREE = 4;
    static const int MAX_WALK   = 48;
    static const int PPB_SIZE   = 512;   // larger PPB → fewer false misses on epoch accuracy
    static const int EPOCH_W    = 512;   // shorter epoch → faster phase detection
    static const int HYSTERESIS = 2;     // consecutive epochs to confirm phase switch

    struct GEntry { uint64_t addr; int32_t prev; uint64_t serial; };
    GEntry ghb[GHB_SIZE];
    int32_t it[IT_SIZE];
    int32_t ghb_head;
    uint64_t serial;

    // Phase detector
    int  epoch_ctr, pf_issued_ep, pf_hits_ep;
    int  cur_degree;
    int  cur_phase;       // 0=PRED, 1=IRREG
    int  pending_phase;
    int  consec;

    // PPB for hit tracking (stores block-aligned addresses)
    uint64_t ppb[PPB_SIZE];
    bool     ppb_valid[PPB_SIZE];
    int      ppb_head;

    // Public stats
    uint64_t total_phase_switches;
    uint64_t epochs_pred, epochs_irreg;

    GHB_PCDC_Adaptive(){ name="ghb_pcdc_adaptive"; label="Adaptive PC/DC (Ours)"; reset(); }

    void reset() override {
        memset(ghb,0,sizeof(ghb));
        for(int i=0;i<GHB_SIZE;i++) ghb[i].prev=-1;
        for(int i=0;i<IT_SIZE;i++)  it[i]=-1;
        ghb_head=0; serial=0;
        epoch_ctr=0; pf_issued_ep=0; pf_hits_ep=0;
        cur_degree=MAX_DEGREE; cur_phase=0; pending_phase=0; consec=0;
        memset(ppb,0,sizeof(ppb)); memset(ppb_valid,0,sizeof(ppb_valid)); ppb_head=0;
        total_phase_switches=0; epochs_pred=0; epochs_irreg=0;
    }

    bool ghb_valid(int32_t i){ return i>=0&&i<GHB_SIZE&&(serial-ghb[i].serial)<(uint64_t)GHB_SIZE; }
    int32_t pchash(uint64_t ip){
        uint64_t h=(ip>>2)^(ip>>10)^(ip>>18)^(ip>>5);
        return (int32_t)(h % IT_SIZE);
    }

    void ppb_insert(uint64_t addr){
        uint64_t cl = (addr >> BLOCK_BITS) << BLOCK_BITS;
        ppb[ppb_head] = cl;
        ppb_valid[ppb_head] = true;
        ppb_head = (ppb_head+1) % PPB_SIZE;
    }
    bool ppb_hit(uint64_t addr){
        uint64_t cl = (addr >> BLOCK_BITS) << BLOCK_BITS;
        for(int i=0;i<PPB_SIZE;i++)
            if(ppb_valid[i] && ppb[i]==cl){ ppb_valid[i]=false; return true; }
        return false;
    }

    void eval_epoch(){
        double acc = pf_issued_ep > 0 ? (double)pf_hits_ep / pf_issued_ep : -1.0;
        epoch_ctr=0; pf_issued_ep=0; pf_hits_ep=0;
        if(acc < 0) return;   // no prefetches this epoch — keep current phase
        int cand = (acc >= 0.70) ? 0 : (acc <= 0.30) ? 1 : cur_phase;
        if(cand == 0) epochs_pred++; else epochs_irreg++;
        if(cand == pending_phase){ if(consec < HYSTERESIS) consec++; }
        else { pending_phase = cand; consec = 1; }
        if(consec >= HYSTERESIS && cand != cur_phase){
            cur_phase  = cand;
            cur_degree = (cand == 0) ? MAX_DEGREE : 0;
            total_phase_switches++;
        }
    }

    void on_access(MemSystem& mem, uint64_t addr, uint64_t ip, bool hit) override {
        epoch_ctr++;
        // Count as useful if this demand hit was to a line we prefetched
        if(hit && ppb_hit(addr)) pf_hits_ep++;
        if(epoch_ctr >= EPOCH_W) eval_epoch();
    }

    void on_miss(MemSystem& mem, uint64_t addr, uint64_t ip) override {
        int32_t k  = pchash(ip);
        int32_t ni = ghb_head;
        ghb[ni] = {addr, (ghb_valid(it[k]) && it[k]!=ni) ? it[k] : -1, ++serial};
        it[k] = ni;
        ghb_head = (ghb_head+1) % GHB_SIZE;

        if(cur_degree == 0) return;   // IRREG phase — suppress prefetching

        // Walk chain newest-first
        uint64_t raw[MAX_WALK+2]; int cl = 0;
        int32_t cur = ni;
        while(cl <= MAX_WALK && cur != -1 && ghb_valid(cur)){
            raw[cl++] = ghb[cur].addr;
            cur = ghb[cur].prev;
        }
        if(cl < 3) return;

        // Reverse to oldest-first
        uint64_t seq[MAX_WALK+2];
        for(int i=0;i<cl;i++) seq[i] = raw[cl-1-i];

        int nd = cl - 1;
        int64_t del[MAX_WALK+1];
        for(int i=0;i<nd;i++) del[i] = (int64_t)seq[i+1] - (int64_t)seq[i];
        if(nd < 2) return;

        int64_t d0 = del[nd-2], d1 = del[nd-1];
        int mp = -1;
        for(int i = nd-3; i >= 1; i--)
            if(del[i-1]==d0 && del[i]==d1){ mp=i; break; }
        if(mp < 0) return;

        // Pattern = del[mp+1 .. nd-1], cycled for cur_degree steps
        int pat_start = mp + 1;
        int pat_end   = nd - 1;
        int pat_len   = pat_end - pat_start + 1;
        if(pat_len < 1) pat_len = 1;

        uint64_t pa = addr;
        for(int k2 = 0; k2 < cur_degree; k2++){
            int pi = pat_start + (k2 % pat_len);
            if(pi < 0 || pi >= nd) break;
            pa = (uint64_t)((int64_t)pa + del[pi]);
            if(mem.prefetch(pa)){ ppb_insert(pa); pf_issued_ep++; }
        }
    }
};

// ═══════════════════════════════════════════════════════════════════════════════
//  TRACE GENERATORS  (realistic patterns per benchmark)
// ═══════════════════════════════════════════════════════════════════════════════
struct TraceGen {
    std::string name, category, description;
    uint64_t ip_base;   // base PC for this benchmark
    std::function<void(uint64_t, uint64_t&, uint64_t&)> gen;
    // gen(step, addr_out, ip_out)
};

// LCG random number generator (reproducible)
static uint64_t lcg_state = 0x12345678ABCDEF01ULL;
static uint64_t lcg_rand() {
    lcg_state = lcg_state * 6364136223846793005ULL + 1442695040888963407ULL;
    return lcg_state;
}
static void lcg_seed(uint64_t s){ lcg_state=s^0xDEADBEEFCAFEBABEULL; }

std::vector<TraceGen> make_traces() {
    std::vector<TraceGen> traces;

    // ── mcf: regular, pointer chasing with stride regularity ─────────────────
    {
        TraceGen t;
        t.name="mcf"; t.category="regular";
        t.description="Network simplex: large pointer arrays, stride-8 dominant";
        t.ip_base=0x400100;
        static uint64_t base=0x100000, ctr=0;
        t.gen=[](uint64_t step, uint64_t& addr, uint64_t& ip){
            // 80% stride-8 forward scan, 20% structured pointer jump
            // Alternating between arc-cost (+0x1000) and node-potential (+0x3000) strides
            // This reflects real mcf network-simplex access pattern
            ctr++;
            if(ctr%5==0){
                // Structured pointer jump: alternating between two fixed strides
                // Even jumps: arc-cost array stride (+0x1000 = 64 cache lines)
                // Odd  jumps: node-potential array stride (+0x3000 = 192 cache lines)
                uint64_t jump_stride = ((ctr/5) % 2 == 0) ? 0x1000ULL : 0x3000ULL;
                base = (base + jump_stride) & ~63ULL;
                if(base > 0x300000) base = 0x100000;   // wrap to keep working set bounded
                addr = base;
                ip   = 0x400200;
            } else {
                base += 8;
                addr = base;
                ip   = 0x400100;
            }
        };
        traces.push_back(t);
    }

    // ── lbm: regular, 3D stencil streaming ────────────────────────────────────
    {
        TraceGen t;
        t.name="lbm"; t.category="regular";
        t.description="Lattice Boltzmann: 3D stencil, stride-8 sequential scan";
        t.ip_base=0x401000;
        static uint64_t lbm_i=0, lbm_j=0, lbm_k=0;
        static const int DY=64, DZ=4096;
        t.gen=[](uint64_t step, uint64_t& addr, uint64_t& ip){
            // 3D stencil: sweep i,j,k
            addr = 0x200000 + (lbm_i*8 + lbm_j*DY*8 + lbm_k*DZ*8);
            ip = 0x401000 + (lbm_i%4)*4;
            lbm_i++;
            if(lbm_i>=DY){ lbm_i=0; lbm_j++; }
            if(lbm_j>=DY){ lbm_j=0; lbm_k=(lbm_k+1)%8; }
        };
        traces.push_back(t);
    }

    // ── gcc: irregular, many small functions, scattered ───────────────────────
    {
        TraceGen t;
        t.name="gcc"; t.category="irregular";
        t.description="Compiler: irregular memory, short strides, frequent phase changes";
        t.ip_base=0x402000;
        static uint64_t gcc_base=0x500000, gcc_phase_ctr=0;
        t.gen=[](uint64_t step, uint64_t& addr, uint64_t& ip){
            gcc_phase_ctr++;
            // Every 30 accesses, jump to a new random region
            if(gcc_phase_ctr%30==0)
                gcc_base = 0x500000 + (lcg_rand()&0xFFFFF)&~7ULL;
            // Mix of small strides (+8, -8, +16) and random
            int r = (int)(lcg_rand()%10);
            if(r<4)       gcc_base+=8;
            else if(r<6)  gcc_base+=16;
            else if(r<7)  gcc_base-=8;
            else          gcc_base = 0x500000+(lcg_rand()&0xFFFFF)&~7ULL;
            addr = gcc_base & ~7ULL;
            ip   = 0x402000 + (lcg_rand()%16)*8;
        };
        traces.push_back(t);
    }

    // ── sphinx3: irregular, scattered lookup tables ───────────────────────────
    {
        TraceGen t;
        t.name="sphinx3"; t.category="irregular";
        t.description="Speech recognition: random lookup tables, scatter-gather";
        t.ip_base=0x403000;

        static const uint64_t TBLS[8]={0x600000,0x640000,0x680000,0x6C0000,0x700000,0x740000,0x780000,0x7C0000};
        t.gen=[](uint64_t step, uint64_t& addr, uint64_t& ip){
            // Random table selection + random index
            int tbl = (int)(lcg_rand()%8);
            uint64_t idx = (lcg_rand()&0x1FFF)&~7ULL;
            addr = TBLS[tbl]+idx;
            ip   = 0x403000 + tbl*4;
        };
        traces.push_back(t);
    }

    // ── bzip2: mixed — sequential scan (compression) then Huffman (random) ────
    {
        TraceGen t;
        t.name="bzip2"; t.category="mixed";
        t.description="Compression: regular scan phases alternating with random Huffman";
        t.ip_base=0x404000;
        static uint64_t bz_base=0x800000, bz_ctr=0, bz_phase=0;
        t.gen=[](uint64_t step, uint64_t& addr, uint64_t& ip){
            bz_ctr++;
            // Phase switch every 2048 accesses
            if(bz_ctr%2048==0){ bz_phase^=1; bz_base=0x800000+(lcg_rand()&0x3FFFF)&~7ULL; }
            if(bz_phase==0){
                // Regular: sequential scan
                bz_base+=8;
                addr=bz_base;
                ip=0x404100;
            } else {
                // Irregular: Huffman table lookups
                addr=0x880000+(lcg_rand()&0xFFFF)&~7ULL;
                ip=0x404200+(lcg_rand()%8)*4;
            }
        };
        traces.push_back(t);
    }

    // ── ammp: mixed — molecular dynamics (regular force + random pair) ─────────
    {
        TraceGen t;
        t.name="ammp"; t.category="mixed";
        t.description="Molecular dynamics: regular force arrays + random pair lists";
        t.ip_base=0x405000;
        static uint64_t am_base=0x900000, am_atom=0, am_ctr=0;
        t.gen=[](uint64_t step, uint64_t& addr, uint64_t& ip){
            am_ctr++;
            int r=(int)(am_ctr%7);
            if(r<4){
                // Regular: stride-8 force array
                am_base+=8;
                if(am_base>0x980000) am_base=0x900000;
                addr=am_base;
                ip=0x405100;
            } else if(r<6){
                // Regular: stride-64 position array
                am_atom=(am_atom+64)&0xFFFF;
                addr=0x980000+am_atom;
                ip=0x405200;
            } else {
                // Irregular: random pair list
                addr=0x9C0000+(lcg_rand()&0x3FFF)&~7ULL;
                ip=0x405300;
            }
        };
        traces.push_back(t);
    }

    // ── astar: mixed — graph traversal (BFS + pointer chasing) ───────────────
    {
        TraceGen t;
        t.name="astar"; t.category="mixed";
        t.description="Pathfinding: BFS frontier list (regular) + edge lookups (irregular)";
        t.ip_base=0x406000;
        static uint64_t as_base=0xA00000, as_ctr=0, as_node=0;
        t.gen=[](uint64_t step, uint64_t& addr, uint64_t& ip){
            as_ctr++;
            int r=(int)(as_ctr%10);
            if(r<6){
                // Sequential frontier scan
                as_base+=8;
                if(as_base>0xA80000) as_base=0xA00000;
                addr=as_base;
                ip=0x406100;
            } else {
                // Random edge/neighbor lookup
                as_node=(uint64_t)(lcg_rand()&0xFFFFF);
                addr=0xA80000+(as_node&0xFFFF);
                ip=0x406200;
            }
        };
        traces.push_back(t);
    }

    return traces;
}

// ═══════════════════════════════════════════════════════════════════════════════
//  RESULT RECORD
// ═══════════════════════════════════════════════════════════════════════════════
struct Result {
    std::string prefetcher, benchmark, category;
    double ipc;
    double mpki;
    double pf_accuracy;
    uint64_t pf_issued, pf_useful, pf_useless;
    uint64_t l1d_misses;
    uint64_t l2_misses, llc_misses_sim, dram_accesses;
    double   amat;        // Average Memory Access Time (cycles)
    double   energy_nj;   // Estimated energy (nJ)
    uint64_t phase_switches;   // adaptive only
    double   epochs_pred_pct;  // adaptive only
};

// ═══════════════════════════════════════════════════════════════════════════════
//  PROGRESS BAR
// ═══════════════════════════════════════════════════════════════════════════════
// Live progress bar — called during simulation inner loop
static void progress_live(const char* bm, const char* pf, uint64_t done, uint64_t total,
                          uint64_t misses, uint64_t pf_issued, uint64_t stall_so_far){
    int w=28;
    int filled=(int)((double)done/total*w);
    double pct=(double)done/total*100.0;
    // live IPC estimate
    uint64_t total_instr=(uint64_t)(done/LOAD_FRAC);
    double cycles=(double)total_instr/ISSUE_WIDTH+(double)stall_so_far;
    double live_ipc = cycles>0?(double)total_instr/cycles:0.0;
    printf("\r    " CYN "%-8s" RST " " YEL "%-22s" RST " [", bm, pf);
    for(int i=0;i<filled;i++) printf(BGRN "█" RST);
    for(int i=filled;i<w;i++) printf("░");
    printf("] " BOLD "%5.1f%%" RST "  IPC~" BGRN "%.3f" RST "  ",
           pct, live_ipc);
    fflush(stdout);
}



// ═══════════════════════════════════════════════════════════════════════════════
//  RUN ONE SIMULATION  (prefetcher × benchmark)
// ═══════════════════════════════════════════════════════════════════════════════
Result run_sim(Prefetcher* pref, TraceGen& tg, bool verbose){
    lcg_seed(0xABCDEF0123456789ULL);   // reproducible

    MemSystem mem;
    mem.init();
    pref->reset();

    uint64_t addr=0, ip=0;
    uint64_t total = WARMUP_ACCESSES + SIM_ACCESSES;
    uint64_t stall_cycles = 0;
    // update interval: every 250K sim steps, show progress
    static const uint64_t UPDATE_INTERVAL = 250000;

    for(uint64_t step=0; step<total; step++){
        tg.gen(step, addr, ip);

        int stall = mem.access(addr);
        bool hit_after = (stall == 0);

        pref->on_access(mem, addr, ip, hit_after);
        if(!hit_after)
            pref->on_miss(mem, addr, ip);

        if(step >= WARMUP_ACCESSES){
            stall_cycles += stall;
            if(verbose && (step-WARMUP_ACCESSES) % UPDATE_INTERVAL == 0){
                progress_live(tg.name.c_str(), pref->label.c_str(),
                              step-WARMUP_ACCESSES, SIM_ACCESSES,
                              mem.l1d.misses, mem.pf_issued, stall_cycles);
            }
        } else {
            // warmup: reset stats at end
            if(step == WARMUP_ACCESSES-1){
                mem.reset_stats();
                pref->reset();
                stall_cycles=0;
            }
        }
    }
    if(verbose) { printf("\r%-100s\r",""); fflush(stdout); }

    // Compute IPC
    // instructions = load accesses = SIM_ACCESSES
    // Assume 20% of all instructions are loads  → total_instr = SIM_ACCESSES / LOAD_FRAC
    uint64_t total_instr = (uint64_t)(SIM_ACCESSES / LOAD_FRAC);
    // Pipeline cycles = total_instr / ISSUE_WIDTH + stall_cycles
    double cycles = (double)total_instr / ISSUE_WIDTH + stall_cycles;
    double ipc = (double)total_instr / cycles;

    uint64_t pf_iss  = mem.pf_issued;
    uint64_t pf_use  = mem.pf_useful_l1d;
    uint64_t pf_unu  = (pf_iss > pf_use) ? pf_iss - pf_use : 0;
    double   pf_acc  = pf_iss>0 ? (double)pf_use/pf_iss*100.0 : 0.0;

    Result r;
    r.prefetcher    = pref->name;
    r.benchmark     = tg.name;
    r.category      = tg.category;
    r.ipc           = ipc;
    r.mpki          = mem.mpki(total_instr);
    r.pf_accuracy   = pf_acc;
    r.pf_issued     = pf_iss;
    r.pf_useful     = pf_use;
    r.pf_useless    = pf_unu;
    r.l1d_misses    = mem.l1d.misses;
    r.l2_misses     = mem.l2_misses;
    r.llc_misses_sim= mem.llc_misses;
    r.dram_accesses = mem.llc_misses;

    // AMAT = L1D_lat + per-access stall
    r.amat = (double)L1D_LATENCY + (total_instr>0 ? (double)stall_cycles/total_instr : 0.0);

    // Energy model (per-access nJ, Vogelsang MICRO 2010 approximation):
    //   L1D hit: 0.5 nJ, L2 hit: 2.0 nJ, LLC hit: 10.0 nJ, DRAM: 50.0 nJ
    {
        uint64_t l1_hits  = SIM_ACCESSES - mem.l1d_misses;
        uint64_t l2_hits  = mem.l1d_misses  - mem.l2_misses;
        uint64_t llc_hits = mem.l2_misses   - mem.llc_misses;
        uint64_t dram     = mem.llc_misses;
        r.energy_nj = l1_hits * 0.5 + l2_hits * 2.0 + llc_hits * 10.0 + dram * 50.0;
        // Add prefetch energy (each useless prefetch burns L2+LLC+DRAM path)
        r.energy_nj += pf_unu * 12.0;  // average cost of a wasted prefetch
    }

    // Phase-adaptive extra stats
    auto* adp = dynamic_cast<GHB_PCDC_Adaptive*>(pref);
    if(adp){
        r.phase_switches   = adp->total_phase_switches;
        uint64_t tot_ep    = adp->epochs_pred + adp->epochs_irreg;
        r.epochs_pred_pct  = tot_ep>0 ? (double)adp->epochs_pred/tot_ep*100.0 : 0.0;
    } else {
        r.phase_switches  = 0;
        r.epochs_pred_pct = 0.0;
    }
    return r;
}

// ═══════════════════════════════════════════════════════════════════════════════
//  PRETTY-PRINT TABLES
// ═══════════════════════════════════════════════════════════════════════════════
static const char* PREF_ORDER[]  = {"no_prefetch","stride","ghb_gdc","ghb_pcdc","ghb_pcdc_adaptive"};
static const char* PREF_LABELS[] = {"No-Pref","Stride","GHB G/DC","GHB PC/DC","Adaptive PC/DC"};
static const int   N_PREF = 5;
static const char* BM_ORDER[]    = {"mcf","lbm","gcc","sphinx3","bzip2","ammp","astar"};
static const int   N_BM   = 7;

static const char* cat_color(const std::string& cat){
    if(cat=="regular")   return BGRN;
    if(cat=="irregular") return BRED;
    return BYEL;
}

static double get_ipc(const std::vector<Result>& res, const char* pf, const char* bm){
    for(auto& r:res) if(r.prefetcher==pf&&r.benchmark==bm) return r.ipc;
    return 0.0;
}
static Result* get_r(std::vector<Result>& res, const char* pf, const char* bm){
    for(auto& r:res) if(r.prefetcher==pf&&r.benchmark==bm) return &r;
    return nullptr;
}

void print_ipc_table(std::vector<Result>& res){
    printf("\n");
    printf(BOLD "┌─────────────────────────────────────────────────────────────────────────────────────┐\n" RST);
    printf(BOLD "│                         TABLE 1 : IPC COMPARISON                                   │\n" RST);
    printf(BOLD "└─────────────────────────────────────────────────────────────────────────────────────┘\n" RST);
    printf("  %-14s", "Benchmark");
    for(int pi=0;pi<N_PREF;pi++) printf("  %-14s", PREF_LABELS[pi]);
    printf("\n");
    printf("  %-14s","──────────────");
    for(int pi=0;pi<N_PREF;pi++) printf("  %-14s","──────────────");
    printf("\n");

    for(int bi=0;bi<N_BM;bi++){
        Result* base_r = get_r(res, "no_prefetch", BM_ORDER[bi]);
        printf("  %s%-10s%s  ", cat_color(base_r?base_r->category:""), BM_ORDER[bi], RST);
        for(int pi=0;pi<N_PREF;pi++){
            double ipc = get_ipc(res, PREF_ORDER[pi], BM_ORDER[bi]);
            double base_ipc = get_ipc(res, "no_prefetch", BM_ORDER[bi]);
            // Highlight improvement
            if(pi==0)                printf("  %6.4f        ", ipc);
            else if(ipc > base_ipc)  printf(BGRN "  %6.4f (+%4.1f%%)" RST, ipc, (ipc/base_ipc-1)*100);
            else if(ipc < base_ipc)  printf(BRED "  %6.4f (%+4.1f%%)" RST, ipc, (ipc/base_ipc-1)*100);
            else                     printf("  %6.4f (=0.0%%) ", ipc);
        }
        printf("\n");
    }

    // Harmonic mean
    printf("  %-14s","──────────────");
    for(int pi=0;pi<N_PREF;pi++) printf("  %-14s","──────────────");
    printf("\n");
    printf(BOLD "  %-14s", "H-Mean");
    for(int pi=0;pi<N_PREF;pi++){
        double s=0; int n=0;
        for(int bi=0;bi<N_BM;bi++){
            double v=get_ipc(res,PREF_ORDER[pi],BM_ORDER[bi]);
            if(v>0){s+=1.0/v;n++;}
        }
        double hm = n>0 ? n/s : 0.0;
        printf("  %6.4f        ", hm);
    }
    printf(RST "\n");
}

void print_accuracy_table(std::vector<Result>& res){
    printf("\n");
    printf(BOLD "┌─────────────────────────────────────────────────────────────────────────────┐\n" RST);
    printf(BOLD "│         TABLE 2 : PREFETCH ACCURACY (%%)  — useful pf hits / issued        │\n" RST);
    printf(BOLD "└─────────────────────────────────────────────────────────────────────────────┘\n" RST);
    printf("  %-14s","Benchmark");
    for(int pi=1;pi<N_PREF;pi++) printf("  %-13s", PREF_LABELS[pi]);
    printf("\n");
    printf("  %-14s","──────────────");
    for(int pi=1;pi<N_PREF;pi++) printf("  %-13s","─────────────");
    printf("\n");
    for(int bi=0;bi<N_BM;bi++){
        Result* base_r = get_r(res,"no_prefetch",BM_ORDER[bi]);
        printf("  %s%-10s%s  ", cat_color(base_r?base_r->category:""), BM_ORDER[bi], RST);
        for(int pi=1;pi<N_PREF;pi++){
            Result* r=get_r(res,PREF_ORDER[pi],BM_ORDER[bi]);
            if(!r || r->pf_issued==0){
                printf("  " YEL "%6s" RST "       ", "N/A");
            } else {
                double acc = r->pf_accuracy;
                const char* col = acc>=70.0?BGRN:(acc<=30.0?BRED:BYEL);
                printf("%s  %6.1f%%       " RST, col, acc);
            }
        }
        printf("\n");
    }
    // ── Totals rows ───────────────────────────────────────────────────────────
    printf("  %-14s","──────────────");
    for(int pi=1;pi<N_PREF;pi++) printf("  %-13s","─────────────");
    printf("\n");
    // Row 1: Weighted accuracy across ALL benchmarks (0-issued count as 0/0 → excluded naturally)
    printf(BOLD "  %-14s" RST, "Wgt Avg(all)");
    for(int pi=1;pi<N_PREF;pi++){
        uint64_t tot_iss=0, tot_use=0;
        for(int bi=0;bi<N_BM;bi++){
            Result* r=get_r(res,PREF_ORDER[pi],BM_ORDER[bi]);
            if(r){ tot_iss+=r->pf_issued; tot_use+=r->pf_useful; }
        }
        double acc = tot_iss>0 ? (double)tot_use/tot_iss*100.0 : 0.0;
        const char* col = acc>=70.0?BGRN:(acc<=30.0?BRED:BYEL);
        printf("%s  %6.1f%%       " RST, col, acc);
    }
    printf("\n");
    // Row 2: Active-only weighted accuracy (only benchmarks where THIS prefetcher issued ≥1 pf)
    printf(BOLD "  %-14s" RST, "Wgt Avg(act)");
    for(int pi=1;pi<N_PREF;pi++){
        uint64_t tot_iss=0, tot_use=0;
        for(int bi=0;bi<N_BM;bi++){
            Result* r=get_r(res,PREF_ORDER[pi],BM_ORDER[bi]);
            if(r && r->pf_issued>0){ tot_iss+=r->pf_issued; tot_use+=r->pf_useful; }
        }
        double acc = tot_iss>0 ? (double)tot_use/tot_iss*100.0 : 0.0;
        const char* col = acc>=70.0?BGRN:(acc<=30.0?BRED:BYEL);
        printf("%s  %6.1f%%       " RST, col, acc);
    }
}

void print_pollution_table(std::vector<Result>& res){
    printf("\n");
    printf(BOLD "┌─────────────────────────────────────────────────────────────────────────────┐\n" RST);
    printf(BOLD "│           TABLE 3 : CACHE POLLUTION — useless prefetch count                │\n" RST);
    printf(BOLD "└─────────────────────────────────────────────────────────────────────────────┘\n" RST);
    printf("  %-14s","Benchmark");
    for(int pi=1;pi<N_PREF;pi++) printf("  %-13s", PREF_LABELS[pi]);
    printf("\n");
    printf("  %-14s","──────────────");
    for(int pi=1;pi<N_PREF;pi++) printf("  %-13s","─────────────");
    printf("\n");
    for(int bi=0;bi<N_BM;bi++){
        Result* base_r = get_r(res,"no_prefetch",BM_ORDER[bi]);
        printf("  %s%-10s%s  ", cat_color(base_r?base_r->category:""), BM_ORDER[bi], RST);
        for(int pi=1;pi<N_PREF;pi++){
            Result* r=get_r(res,PREF_ORDER[pi],BM_ORDER[bi]);
            uint64_t poll = r ? r->pf_useless : 0;
            if(pi==N_PREF-1) printf(BGRN "  %8llu      " RST, (unsigned long long)poll);
            else              printf("  %8llu      ",  (unsigned long long)poll);
        }
        printf("\n");
    }
}

void print_mpki_table(std::vector<Result>& res){
    printf("\n");
    printf(BOLD "┌─────────────────────────────────────────────────────────────────────────────┐\n" RST);
    printf(BOLD "│           TABLE 4 : L1D MPKI  (Misses per Kilo-Instruction)                 │\n" RST);
    printf(BOLD "└─────────────────────────────────────────────────────────────────────────────┘\n" RST);
    printf("  %-14s","Benchmark");
    for(int pi=0;pi<N_PREF;pi++) printf("  %-13s", PREF_LABELS[pi]);
    printf("\n");
    printf("  %-14s","──────────────");
    for(int pi=0;pi<N_PREF;pi++) printf("  %-13s","─────────────");
    printf("\n");
    for(int bi=0;bi<N_BM;bi++){
        Result* base_r = get_r(res,"no_prefetch",BM_ORDER[bi]);
        double base_mpki = base_r ? base_r->mpki : 1.0;
        printf("  %s%-10s%s  ", cat_color(base_r?base_r->category:""), BM_ORDER[bi], RST);
        for(int pi=0;pi<N_PREF;pi++){
            Result* r=get_r(res,PREF_ORDER[pi],BM_ORDER[bi]);
            double m = r ? r->mpki : 0.0;
            double pct = base_mpki>0 ? (m-base_mpki)/base_mpki*100.0 : 0.0;
            if(pi==0)         printf("  %5.2f         ", m);
            else if(m<base_mpki) printf(BGRN "  %5.2f(%4.0f%%)  " RST, m, pct);
            else if(m>base_mpki) printf(BRED "  %5.2f(%+4.0f%%)  " RST, m, pct);
            else                 printf("  %5.2f ( 0%%)   ", m);
        }
        printf("\n");
    }
}

void print_summary(std::vector<Result>& res){
    printf("\n");
    printf(BMAG "╔══════════════════════════════════════════════════════════════════════╗\n");
    printf("║          SUMMARY TABLE — NORMALIZED TO NO-PREFETCH BASELINE          ║\n");
    printf("╚══════════════════════════════════════════════════════════════════════╝\n" RST);
    printf("\n");
    printf("  %-28s  %10s  %12s  %16s\n", "Metric", "No-Pref", "GHB PC/DC", "Adaptive PC/DC");
    printf("  %-28s  %10s  %12s  %16s\n",
           "────────────────────────────","──────────","────────────","────────────────");

    // IPC harmonic mean (normalized)
    double hm_base=0,hm_pcdc=0,hm_adap=0; int n=0;
    for(int bi=0;bi<N_BM;bi++){
        double v0=get_ipc(res,"no_prefetch",BM_ORDER[bi]);
        double v1=get_ipc(res,"ghb_pcdc",BM_ORDER[bi]);
        double v2=get_ipc(res,"ghb_pcdc_adaptive",BM_ORDER[bi]);
        if(v0>0&&v1>0&&v2>0){ hm_base+=1/v0; hm_pcdc+=1/v1; hm_adap+=1/v2; n++; }
    }
    double ipc_base=n/hm_base, ipc_pcdc=n/hm_pcdc, ipc_adap=n/hm_adap;
    printf("  %-28s  %10.3f  " BYEL "%12.3f" RST "  " BGRN "%16.3f\n" RST,
           "IPC (harmonic mean)", 1.0, ipc_pcdc/ipc_base, ipc_adap/ipc_base);

    // Weighted prefetch accuracy — total_useful / total_issued (active benchmarks only)
    uint64_t tot_iss_pc=0,tot_use_pc=0,tot_iss_ad=0,tot_use_ad=0;
    for(int bi=0;bi<N_BM;bi++){
        Result* r1=get_r(res,"ghb_pcdc",BM_ORDER[bi]);
        Result* r2=get_r(res,"ghb_pcdc_adaptive",BM_ORDER[bi]);
        if(r1 && r1->pf_issued>0){ tot_iss_pc+=r1->pf_issued; tot_use_pc+=r1->pf_useful; }
        if(r2 && r2->pf_issued>0){ tot_iss_ad+=r2->pf_issued; tot_use_ad+=r2->pf_useful; }
    }
    double acc_pc_w = tot_iss_pc>0 ? (double)tot_use_pc/tot_iss_pc*100.0 : 0.0;
    double acc_ad_w = tot_iss_ad>0 ? (double)tot_use_ad/tot_iss_ad*100.0 : 0.0;
    printf("  %-28s  %10s  " BYEL "%11.1f%%" RST "  " BGRN "%15.1f%%\n" RST,
           "Pf Accuracy (active bm)", "—", acc_pc_w, acc_ad_w);

    // Total useless prefetches
    uint64_t poll_pc=0,poll_ad=0;
    for(int bi=0;bi<N_BM;bi++){
        Result* r1=get_r(res,"ghb_pcdc",BM_ORDER[bi]);
        Result* r2=get_r(res,"ghb_pcdc_adaptive",BM_ORDER[bi]);
        if(r1) poll_pc+=r1->pf_useless;
        if(r2) poll_ad+=r2->pf_useless;
    }
    printf("  %-28s  %10s  " BRED "%12llu" RST "  " BGRN "%16llu\n" RST,
           "Total Useless Prefetches", "0",
           (unsigned long long)poll_pc, (unsigned long long)poll_ad);

    // Phase switches (adaptive only)
    uint64_t sw=0;
    for(int bi=0;bi<N_BM;bi++){
        Result* r=get_r(res,"ghb_pcdc_adaptive",BM_ORDER[bi]);
        if(r) sw+=r->phase_switches;
    }
    printf("  %-28s  %10s  %12s  " BCYN "%16llu\n" RST,
           "Phase Switches (adaptive)", "—", "—", (unsigned long long)sw);

    printf("\n");
}


int main(){
    std::vector<Prefetcher*> prefs = {
        new NoPrefetch(),
        new StridePrefetcher(),
        new GHB_GDC(),
        new GHB_PCDC(),
        new GHB_PCDC_Adaptive()
    };

    auto traces = make_traces();
    std::vector<Result> all_results;

    for(auto& tg : traces){
        for(auto* pf : prefs){
            Result r = run_sim(pf, tg, false);
            all_results.push_back(r);
        }
    }

    print_ipc_table(all_results);
    print_accuracy_table(all_results);
    print_pollution_table(all_results);
    print_mpki_table(all_results);
    print_summary(all_results);

    for(auto* p:prefs) delete p;
    printf("\n");
    return 0;
}
