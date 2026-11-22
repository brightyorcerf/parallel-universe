// Throughput harness + correctness stress test.
//
//   bench <bst|avl|treap|skiplist> <threads> <contains%> <insert%> [seconds=10] [runs=5]
//         [size=1000000] [range=2000000]
//   bench test
//
// remove% = 100 - contains% - insert%. Keys are uniform in [1, range]; each run
// builds a fresh structure prefilled with `size` distinct random keys.
//
// perf integration: if PERF_CTL_FIFO / PERF_ACK_FIFO are set (see
// scripts/perf_cache.sh), counting is enabled only around the timed phase, so
// prefill and teardown are excluded from the cache-miss numbers.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include "lo_tree.hpp"
#include "skiplist.hpp"

using Clock = std::chrono::steady_clock;

struct PerfCtl {
  FILE *ctl = nullptr, *ack = nullptr;
  PerfCtl() {
    const char *c = std::getenv("PERF_CTL_FIFO"), *a = std::getenv("PERF_ACK_FIFO");
    if (c && a) {
      ctl = std::fopen(c, "w");
      ack = std::fopen(a, "r");
    }
  }
  void send(const char* cmd) {
    if (!ctl) return;
    std::fputs(cmd, ctl);
    std::fflush(ctl);
    char buf[16];
    std::fgets(buf, sizeof buf, ack);  // wait for perf's "ack"
  }
};

struct alignas(64) Counter {
  long long ops = 0;
};

template <class DS>
void prefill(DS& ds, int size, int range) {
  int nt = std::clamp<int>(std::thread::hardware_concurrency(), 1, kMaxThreads);  // one EBR slot per thread
  std::vector<std::thread> ts;
  for (int t = 0; t < nt; ++t)
    ts.emplace_back([&, t] {
      tid = t;
      Rng rng(0xC0FFEE + t);
      for (int done = 0, quota = size / nt + (t < size % nt); done < quota;)
        done += ds.insert(1 + static_cast<int>(rng.next() % range));
    });
  for (auto& t : ts) t.join();
}

template <class DS>
double run_once(int threads, int pc, int pi, double secs, int size, int range, PerfCtl& perf, long long& ops) {
  auto ds = std::make_unique<DS>();
  prefill(*ds, size, range);
  std::atomic<int> ready{0};
  std::atomic<bool> go{false}, stop{false};
  std::vector<Counter> cnt(threads);
  std::vector<std::thread> ts;
  for (int t = 0; t < threads; ++t)
    ts.emplace_back([&, t] {
      tid = t;
      Rng rng(Clock::now().time_since_epoch().count() * 31 + t);
      long long n = 0;
      ready.fetch_add(1);
      while (!go.load(std::memory_order_acquire)) cpu_relax();
      while (!stop.load(std::memory_order_relaxed)) {
        uint64_t r = rng.next();
        int key = 1 + static_cast<int>((r >> 7) % range), op = static_cast<int>(r % 100);
        if (op < pc) ds->contains(key);
        else if (op < pc + pi) ds->insert(key);
        else ds->remove(key);
        ++n;
      }
      cnt[t].ops = n;
    });
  while (ready.load() < threads) std::this_thread::yield();
  perf.send("enable\n");
  auto t0 = Clock::now();
  go.store(true, std::memory_order_release);
  std::this_thread::sleep_for(std::chrono::duration<double>(secs));
  stop.store(true);
  for (auto& t : ts) t.join();
  double elapsed = std::chrono::duration<double>(Clock::now() - t0).count();
  perf.send("disable\n");
  ops = 0;
  for (auto& c : cnt) ops += c.ops;
  return ops / elapsed / 1e6;
}

template <class DS>
int bench(const char* name, int threads, int pc, int pi, double secs, int runs, int size, int range) {
  PerfCtl perf;
  std::vector<double> mops;
  long long total = 0;
  for (int r = 0; r < runs; ++r) {
    long long ops;
    mops.push_back(run_once<DS>(threads, pc, pi, secs, size, range, perf, ops));
    total += ops;
    std::fprintf(stderr, "  run %d: %.3f Mops/s\n", r + 1, mops.back());
  }
  double mean = 0, var = 0;
  for (double m : mops) mean += m / runs;
  for (double m : mops) var += (m - mean) * (m - mean) / std::max(1, runs - 1);
  std::printf("RESULT,%s,%dC-%dI-%dD,%d,%.4f,%.4f,%lld", name, pc, pi, 100 - pc - pi, threads, mean,
              std::sqrt(var), total);
  for (double m : mops) std::printf(",%.4f", m);
  std::printf("\n");
  return 0;
}

// Stress test: hammer a small key range, then check structure invariants and
// that every key's membership equals (successful inserts - successful removes).
template <class DS>
bool stress(const char* name, int threads, int range, double secs) {
  DS ds;
  std::vector<std::vector<int>> net(threads, std::vector<int>(range + 1));
  std::atomic<bool> stop{false};
  std::vector<std::thread> ts;
  for (int t = 0; t < threads; ++t)
    ts.emplace_back([&, t] {
      tid = t;
      Rng rng(t * 7919 + 1);
      while (!stop.load(std::memory_order_relaxed)) {
        uint64_t r = rng.next();
        int key = 1 + static_cast<int>((r >> 7) % range), op = static_cast<int>(r % 100);
        if (op < 20) ds.contains(key);
        else if (op < 60) net[t][key] += ds.insert(key);
        else net[t][key] -= ds.remove(key);
      }
    });
  std::this_thread::sleep_for(std::chrono::duration<double>(secs));
  stop.store(true);
  for (auto& t : ts) t.join();
  bool ok = true;
  size_t expect = 0;
  for (int k = 1; k <= range; ++k) {
    int s = 0;
    for (auto& v : net) s += v[k];
    if (s != 0 && s != 1) ok = false;
    if (ds.contains(k) != (s == 1)) ok = false;
    expect += s == 1;
  }
  auto c = ds.validate();
  bool members_ok = ok;
  ok = ok && c.ok && c.size == expect;
  std::printf("%-9s threads=%-2d range=%-8d size=%-8zu depth=%-4zu violations=%-6zu %s\n", name, threads, range,
              c.size, c.depth, c.violations, ok ? "PASS" : "FAIL");
  if (!ok)
    std::printf("          expected size=%zu  structure=%s  membership=%s\n", expect, c.ok ? "ok" : "BROKEN",
                members_ok ? "ok" : "WRONG");
  return ok;
}

// Regression: a removed node still in the tree ("zombie") must not hide a
// re-inserted copy of its key. Thread A is frozen inside remove(512) between the
// logical and the physical unlink; thread B re-inserts 512; contains(512) must
// then see it. Keys are inserted in level order, so in the BST/AVL 512 is the
// root and has two children, which puts the new copy below the zombie.
inline std::atomic<int> g_zombie_stage{0};
template <class DS>
bool zombie_test(const char* name) {
  constexpr int kVictim = 512;
  DS ds;
  for (int step = 512; step >= 1; step /= 2)
    for (int k = step; k < 1024; k += 2 * step) ds.insert(k);
  g_zombie_stage = 0;
  DS::after_unlink_hook = [](int k) {
    if (k != kVictim) return;
    g_zombie_stage = 1;
    while (g_zombie_stage.load() != 2) std::this_thread::yield();
  };
  std::thread a([&] { tid = 1; ds.remove(kVictim); });
  while (g_zombie_stage.load() != 1) std::this_thread::yield();
  std::thread b([&] { tid = 2; ds.insert(kVictim); });
  bool seen = false;
  for (auto end = Clock::now() + std::chrono::milliseconds(500); !seen && Clock::now() < end;)
    seen = ds.contains(kVictim);
  g_zombie_stage = 2;
  a.join();
  b.join();
  DS::after_unlink_hook = nullptr;
  bool ok = seen && ds.contains(kVictim) && ds.validate().ok;
  std::printf("%-9s zombie re-insert visible to contains                       %s\n", name, ok ? "PASS" : "FAIL");
  return ok;
}

template <class DS>
bool stress_all(const char* name) {
  bool ok = true;
  for (int range : {64, 4096, 1 << 20}) ok &= stress<DS>(name, 16, range, 2.0);
  return ok;
}

int main(int argc, char** argv) {
  if (argc >= 2 && !std::strcmp(argv[1], "test")) {
    const char* only = argc > 2 ? argv[2] : "";  // optional: test one structure
    auto want = [&](const char* n) { return !*only || !std::strcmp(only, n); };
    bool ok = true;
    if (want("bst")) ok &= stress_all<ConcurrentBst>("bst") & zombie_test<ConcurrentBst>("bst");
    if (want("avl")) ok &= stress_all<ConcurrentAvl>("avl") & zombie_test<ConcurrentAvl>("avl");
    if (want("treap")) ok &= stress_all<ConcurrentTreap>("treap") & zombie_test<ConcurrentTreap>("treap");
    if (want("skiplist")) ok &= stress_all<ConcurrentSkiplist>("skiplist");
    std::puts(ok ? "ALL PASS" : "FAILURES");
    return ok ? 0 : 1;
  }
  if (argc < 5) {
    std::fprintf(stderr,
                 "usage: %s <bst|avl|treap|skiplist> <threads> <contains%%> <insert%%> [seconds=10] [runs=5] "
                 "[size=1000000] [range=2000000]\n       %s test\n",
                 argv[0], argv[0]);
    return 2;
  }
  std::string ds = argv[1];
  int threads = std::atoi(argv[2]), pc = std::atoi(argv[3]), pi = std::atoi(argv[4]);
  double secs = argc > 5 ? std::atof(argv[5]) : 10;
  int runs = argc > 6 ? std::atoi(argv[6]) : 5;
  int size = argc > 7 ? std::atoi(argv[7]) : 1000000;
  int range = argc > 8 ? std::atoi(argv[8]) : 2000000;
  if (threads < 1 || threads > kMaxThreads || pc < 0 || pi < 0 || pc + pi > 100 || secs <= 0 || runs < 1 ||
      size < 0 || range < 1 || size > range) {
    std::fprintf(stderr, "invalid arguments\n");
    return 2;
  }
  if (ds == "bst") return bench<ConcurrentBst>("bst", threads, pc, pi, secs, runs, size, range);
  if (ds == "avl") return bench<ConcurrentAvl>("avl", threads, pc, pi, secs, runs, size, range);
  if (ds == "treap") return bench<ConcurrentTreap>("treap", threads, pc, pi, secs, runs, size, range);
  if (ds == "skiplist") return bench<ConcurrentSkiplist>("skiplist", threads, pc, pi, secs, runs, size, range);
  std::fprintf(stderr, "unknown data structure: %s\n", ds.c_str());
  return 2;
}
