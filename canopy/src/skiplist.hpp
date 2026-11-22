#pragma once
// Lazy skiplist (Herlihy, Lev, Luchangco, Shavit, SIROCCO'07): lock-based
// insert/remove with optimistic validation, wait-free contains.
#include <bit>
#include <climits>
#include <cstddef>
#include <new>
#include "common.hpp"

class ConcurrentSkiplist {
  static constexpr int kLevels = 22;  // ~log2(2M keys)

  // The top + 1 next pointers live in the same allocation, right after the node,
  // as separately constructed objects (no over-indexed array).
  struct alignas(std::atomic<void*>) Node {
    const int key;
    const int top;  // highest level, 0-based
    std::atomic<bool> marked{false}, linked{false};
    SpinLock lock;
    Node(int k, int t) : key(k), top(t) {}
    std::atomic<Node*>* next() { return std::launder(reinterpret_cast<std::atomic<Node*>*>(this + 1)); }
  };

  static Node* make(int key, int top) {
    void* mem = ::operator new(sizeof(Node) + (top + 1) * sizeof(std::atomic<Node*>));
    Node* n = new (mem) Node(key, top);
    for (int i = 0; i <= top; ++i) new (reinterpret_cast<std::atomic<Node*>*>(n + 1) + i) std::atomic<Node*>(nullptr);
    return n;
  }
  static void destroy(void* p) {
    static_cast<Node*>(p)->~Node();  // std::atomic<Node*> is trivially destructible
    ::operator delete(p);
  }
  static Node* ld(const std::atomic<Node*>& a) { return a.load(std::memory_order_acquire); }
  static int random_top() {
    return std::min(std::countr_zero(tl_rng.next() | (1ull << (kLevels - 1))), kLevels - 1);
  }

  Node* head_;
  Node* tail_;
  Ebr ebr_{destroy};

  int find(int k, Node** preds, Node** succs) const {
    int found = -1;
    Node* pred = head_;
    for (int l = kLevels - 1; l >= 0; --l) {
      Node* cur = ld(pred->next()[l]);
      while (cur->key < k) {
        pred = cur;
        cur = ld(pred->next()[l]);
      }
      if (found == -1 && cur->key == k) found = l;
      preds[l] = pred;
      succs[l] = cur;
    }
    return found;
  }

  // Unlocks each distinct pred locked at levels 0..top (equal preds are adjacent).
  static void unlock_preds(Node** preds, int top) {
    for (int l = 0; l <= top; ++l)
      if (l == 0 || preds[l] != preds[l - 1]) preds[l]->lock.unlock();
  }

 public:
  ConcurrentSkiplist() {
    head_ = make(INT_MIN, kLevels - 1);
    tail_ = make(INT_MAX, kLevels - 1);
    for (int l = 0; l < kLevels; ++l) head_->next()[l] = tail_;
  }
  ~ConcurrentSkiplist() {
    for (Node* n = head_; n;) {
      Node* nx = n->next()[0].load();
      destroy(n);
      n = nx;
    }
  }
  ConcurrentSkiplist(const ConcurrentSkiplist&) = delete;
  ConcurrentSkiplist& operator=(const ConcurrentSkiplist&) = delete;

  bool contains(int k) {
    Ebr::Guard g(ebr_);
    Node* pred = head_;
    for (int l = kLevels - 1; l >= 0; --l) {
      Node* cur = ld(pred->next()[l]);
      while (cur->key < k) {
        pred = cur;
        cur = ld(pred->next()[l]);
      }
      if (cur->key == k) return cur->linked.load(std::memory_order_acquire) && !cur->marked.load(std::memory_order_acquire);
    }
    return false;
  }

  bool insert(int k) {
    Ebr::Guard g(ebr_);
    int top = random_top();
    Node *preds[kLevels], *succs[kLevels];
    for (;;) {
      int found = find(k, preds, succs);
      if (found != -1) {
        Node* n = succs[found];
        if (!n->marked.load(std::memory_order_acquire)) {
          while (!n->linked.load(std::memory_order_acquire)) cpu_relax();
          return false;
        }
        continue;  // being removed; retry once it is unlinked
      }
      int locked = -1;
      bool valid = true;
      for (int l = 0; valid && l <= top; ++l) {
        if (l == 0 || preds[l] != preds[l - 1]) preds[l]->lock.lock();
        locked = l;
        valid = !preds[l]->marked.load(std::memory_order_acquire) &&
                !succs[l]->marked.load(std::memory_order_acquire) && ld(preds[l]->next()[l]) == succs[l];
      }
      if (!valid) {
        unlock_preds(preds, locked);
        continue;
      }
      Node* x = make(k, top);
      for (int l = 0; l <= top; ++l) x->next()[l].store(succs[l], std::memory_order_relaxed);
      for (int l = 0; l <= top; ++l) preds[l]->next()[l].store(x, std::memory_order_release);
      x->linked.store(true, std::memory_order_release);  // linearization point
      unlock_preds(preds, top);
      return true;
    }
  }

  bool remove(int k) {
    Ebr::Guard g(ebr_);
    Node* victim = nullptr;
    Node *preds[kLevels], *succs[kLevels];
    for (;;) {
      int found = find(k, preds, succs);
      if (!victim) {
        if (found == -1) return false;
        Node* n = succs[found];
        if (!n->linked.load(std::memory_order_acquire) || n->top != found ||
            n->marked.load(std::memory_order_acquire))
          return false;
        n->lock.lock();
        if (n->marked.load(std::memory_order_relaxed)) {
          n->lock.unlock();
          return false;
        }
        n->marked.store(true, std::memory_order_release);  // linearization point
        victim = n;
      }
      int top = victim->top, locked = -1;
      bool valid = true;
      for (int l = 0; valid && l <= top; ++l) {
        if (l == 0 || preds[l] != preds[l - 1]) preds[l]->lock.lock();
        locked = l;
        valid = !preds[l]->marked.load(std::memory_order_acquire) && ld(preds[l]->next()[l]) == victim;
      }
      if (!valid) {
        unlock_preds(preds, locked);
        continue;
      }
      for (int l = top; l >= 0; --l) preds[l]->next()[l].store(ld(victim->next()[l]), std::memory_order_release);
      victim->lock.unlock();
      unlock_preds(preds, top);
      ebr_.retire(victim);
      return true;
    }
  }

  struct Check {
    bool ok;
    size_t size, depth, violations;
  };
  Check validate() const {
    Check r{true, 0, 0, 0};
    for (int l = 0; l < kLevels; ++l) {
      size_t n = 0;
      int prev = INT_MIN;
      for (Node* x = ld(head_->next()[l]); x != tail_; x = ld(x->next()[l]), ++n) {
        if (x->key <= prev || x->marked || !x->linked || x->top < l) r.ok = false;
        prev = x->key;
      }
      if (l == 0) r.size = n;
      if (n) r.depth = l + 1;
    }
    return r;
  }
};
