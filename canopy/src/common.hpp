#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>
#include <utility>
#include <vector>
#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
inline void cpu_relax() { _mm_pause(); }
#else
inline void cpu_relax() {}
#endif

constexpr int kMaxThreads = 64;

// Index of the calling thread in [0, kMaxThreads); set by the harness.
inline thread_local int tid = 0;

struct Rng {  // splitmix64
  uint64_t s;
  explicit Rng(uint64_t seed) : s(seed) {}
  uint64_t next() {
    uint64_t z = (s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }
};
inline thread_local Rng tl_rng{std::hash<std::thread::id>{}(std::this_thread::get_id())};

// Test-and-test-and-set lock; yields after a short spin because the benchmark
// oversubscribes cores (16 threads on 8 hardware threads).
struct SpinLock {
  std::atomic<bool> held{false};
  bool try_lock() {
    return !held.load(std::memory_order_relaxed) && !held.exchange(true, std::memory_order_acquire);
  }
  void lock() {
    for (int i = 0; !try_lock(); ++i) {
      if (i < 128) cpu_relax();
      else std::this_thread::yield();
    }
  }
  void unlock() { held.store(false, std::memory_order_release); }
};

// Epoch-based reclamation. A node retired while the global epoch is e is freed
// once the epoch reaches e + 3. Plain EBR uses e + 2, which assumes a retired node
// is unreachable. Here a removed tree node that is still physically linked keeps
// pred pointers to nodes removed after it, so a reader can reach a node one epoch
// later than plain EBR allows. Every such node was live after the remover entered
// (epoch >= its e_r) and the reader entered at <= e_r + 1, so e + 3 covers it.
class Ebr {
  static constexpr uint64_t kIdle = ~0ull;
  struct alignas(64) Slot {
    std::atomic<uint64_t> local{kIdle};
    std::vector<std::pair<uint64_t, void*>> limbo;
  };
  alignas(64) std::atomic<uint64_t> epoch_{0};
  Slot slots_[kMaxThreads];
  void (*free_)(void*);

  void try_advance() {
    uint64_t e = epoch_.load();
    for (auto& s : slots_) {
      uint64_t l = s.local.load();
      if (l != kIdle && l != e) return;
    }
    epoch_.compare_exchange_strong(e, e + 1);
  }
  void reclaim(Slot& s) {
    uint64_t e = epoch_.load();
    size_t i = 0;
    while (i < s.limbo.size() && s.limbo[i].first + 3 <= e) free_(s.limbo[i++].second);
    s.limbo.erase(s.limbo.begin(), s.limbo.begin() + i);
  }

 public:
  explicit Ebr(void (*f)(void*)) : free_(f) {}
  ~Ebr() {
    for (auto& s : slots_)
      for (auto& [e, p] : s.limbo) free_(p);
  }
  void enter() { slots_[tid].local.store(epoch_.load(), std::memory_order_seq_cst); }
  void exit() { slots_[tid].local.store(kIdle, std::memory_order_release); }
  void retire(void* p) {
    Slot& s = slots_[tid];
    s.limbo.emplace_back(epoch_.load(), p);
    if (s.limbo.size() % 256 == 0) {
      try_advance();
      reclaim(s);
    }
  }
  struct Guard {
    Ebr& e;
    explicit Guard(Ebr& x) : e(x) { e.enter(); }
    ~Guard() { e.exit(); }
  };
};
