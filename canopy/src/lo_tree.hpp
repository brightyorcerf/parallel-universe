#pragma once
// Concurrent BST / AVL / Treap built on "logical ordering"
// (Drachsler, Vechev, Yahav, PPoPP'14).
//
// Every node sits in two structures:
//  - a sorted doubly linked list (pred/succ) that alone decides membership,
//    guarded by per-node succLocks;
//  - the physical tree (left/right/parent), guarded by per-node treeLocks,
//    used only as a fast index into the list.
// contains() takes no locks: it walks the tree, then fixes its position by
// walking pred/succ, so rotations and relocations can never make it miss a key.
//
// Locking rules (they make the code below deadlock-free):
//  - succLocks before treeLocks; succLocks in key order (pred then node).
//  - A thread blocks on a treeLock only while holding no other treeLock;
//    every further treeLock is try-locked, and on failure all are released.
//  - x.left/right/height change only under x.treeLock; x.parent changes only
//    while holding the treeLocks of both the old and the new parent.
#include <algorithm>
#include <climits>
#include <cstdlib>
#include <cstddef>
#include "common.hpp"

enum class Balance { None, Avl, Treap };

template <Balance B>
class LoTree {
  struct Node {
    const int key;
    const uint32_t prio;  // treap heap priority
    std::atomic<int> height{1};
    std::atomic<bool> mark{false};
    SpinLock succLock, treeLock;
    std::atomic<Node*> left{nullptr}, right{nullptr}, parent{nullptr}, pred{nullptr}, succ{nullptr};
    Node(int k, uint32_t p) : key(k), prio(p) {}
  };

  static Node* ld(const std::atomic<Node*>& a) { return a.load(std::memory_order_acquire); }
  static void st(std::atomic<Node*>& a, Node* v) { a.store(v, std::memory_order_release); }
  static bool marked(Node* n) { return n->mark.load(std::memory_order_acquire); }
  static int h(Node* n) { return n ? n->height.load(std::memory_order_relaxed) : 0; }
  static void backoff() { std::this_thread::yield(); }
  static std::atomic<Node*>& slot_of(Node* parent, Node* child) {
    return ld(parent->left) == child ? parent->left : parent->right;
  }

  struct LockSet {
    Node* held[8];
    int n = 0;
    void lock(Node* x) { x->treeLock.lock(); held[n++] = x; }
    bool try_lock(Node* x) {
      for (int i = 0; i < n; ++i)
        if (held[i] == x) return true;
      if (!x->treeLock.try_lock()) return false;
      held[n++] = x;
      return true;
    }
    void release() { while (n) held[--n]->treeLock.unlock(); }
    ~LockSet() { release(); }
  };

  // Tree root is tail(+inf) whose left child is head(-inf); real keys live in head->right.
  Node* head_;
  Node* tail_;
  Ebr ebr_{[](void* p) { delete static_cast<Node*>(p); }};

  Node* search(int k) const {
    Node* n = tail_;
    for (;;) {
      if (k == n->key) return n;
      Node* c = ld(k < n->key ? n->left : n->right);
      if (!c) return n;
      n = c;
    }
  }

  // Some node with key < k whose successor (at the time of reading) has key >= k.
  Node* pred_of(int k) const {
    Node* n = search(k);
    while (n->key >= k) n = ld(n->pred);
    for (Node* s; (s = ld(n->succ))->key < k;) n = s;
    return n;
  }

  // p, s adjacent in the list (p.succLock held): one of p.right / s.left is free.
  // Returns the chosen parent with its treeLock held.
  Node* choose_parent(Node* p, Node* s) {
    for (Node* c = p;; c = (c == p ? s : p)) {
      c->treeLock.lock();
      if (!ld(c == p ? p->right : s->left)) return c;
      c->treeLock.unlock();
      cpu_relax();  // a removed node between p and s is still being unlinked
    }
  }

  // Lift y (a child of x) above x; x is a child of p. Caller holds p, x, y.
  // Store order never creates a cycle for lock-free readers.
  static void rotate_up(Node* p, Node* x, Node* y) {
    if (ld(x->left) == y) {
      Node* m = ld(y->right);
      st(x->left, m);
      if (m) st(m->parent, x);
      st(y->right, x);
    } else {
      Node* m = ld(y->left);
      st(x->right, m);
      if (m) st(m->parent, x);
      st(y->left, x);
    }
    st(x->parent, y);
    st(y->parent, p);
    st(slot_of(p, x), y);
  }

  static void fix_height(Node* n) {
    n->height.store(1 + std::max(h(ld(n->left)), h(ld(n->right))), std::memory_order_relaxed);
  }

  // BST/AVL: lock s, its parent and, for two children, its successor and the
  // successor's parent. Called with the succLocks of s and its pred held, so
  // s->succ is stable. Sets succ = nullptr when s has at most one child.
  void lock_for_removal(Node* s, LockSet& ls, Node*& P, Node*& succ, Node*& SP) {
    for (;; ls.release(), backoff()) {
      ls.lock(s);
      P = ld(s->parent);
      if (!ls.try_lock(P) || ld(s->parent) != P) continue;
      succ = nullptr;
      if (!ld(s->left) || !ld(s->right)) return;
      succ = ld(s->succ);
      if (!ls.try_lock(succ) || ld(succ->left)) continue;
      SP = ld(succ->parent);
      if (!ls.try_lock(SP) || ld(succ->parent) != SP) continue;
      if (ld(SP == s ? SP->right : SP->left) != succ) continue;
      return;
    }
  }

  // Physical BST removal; returns where rebalancing should start.
  Node* unlink_from_tree(Node* s, Node* P, Node* succ, Node* SP) {
    std::atomic<Node*>& slot = slot_of(P, s);
    if (!succ) {
      Node* c = ld(s->left) ? ld(s->left) : ld(s->right);
      if (c) st(c->parent, P);
      st(slot, c);
      return P;
    }
    // Two children: the successor (leftmost in s->right, no left child) takes s's place.
    Node* from = succ;
    if (SP != s) {
      Node* sr = ld(succ->right);
      st(SP->left, sr);
      if (sr) st(sr->parent, SP);
      Node* r = ld(s->right);
      st(succ->right, r);
      st(r->parent, succ);
      from = SP;
    }
    Node* l = ld(s->left);
    st(succ->left, l);
    st(l->parent, succ);
    succ->height.store(h(s), std::memory_order_relaxed);
    st(succ->parent, P);
    st(slot, succ);
    return from;
  }

  // Relaxed AVL: walk up fixing heights, rotating where |balance| > 1.
  void rebalance(Node* n) {
    while (n != head_ && n != tail_) {
      LockSet ls;
      ls.lock(n);
      if (marked(n)) return;  // n already unlinked by its remover
      Node* P = ld(n->parent);
      if (!ls.try_lock(P) || ld(n->parent) != P) { ls.release(); backoff(); continue; }
      Node *L = ld(n->left), *R = ld(n->right);
      int lh = h(L), rh = h(R), bf = lh - rh;
      if (bf >= -1 && bf <= 1) {
        int nh = 1 + std::max(lh, rh);
        if (nh == h(n)) return;
        n->height.store(nh, std::memory_order_relaxed);
        n = P;
        continue;
      }
      Node* c = bf > 1 ? L : R;
      if (!ls.try_lock(c)) { ls.release(); backoff(); continue; }
      Node* inner = ld(bf > 1 ? c->right : c->left);
      Node* outer = ld(bf > 1 ? c->left : c->right);
      if (h(inner) > h(outer)) {  // double rotation
        if (!ls.try_lock(inner)) { ls.release(); backoff(); continue; }
        rotate_up(n, c, inner);
        rotate_up(P, n, inner);
        fix_height(c);
        fix_height(n);
        fix_height(inner);
      } else {
        rotate_up(P, n, c);
        fix_height(n);
        fix_height(c);
      }
      n = P;
    }
  }

  // Treap insert: rotate x up while its priority beats its parent's.
  void bubble_up(Node* x) {
    for (;;) {
      LockSet ls;
      ls.lock(x);
      if (marked(x)) return;  // its remover will rotate it down
      Node* P = ld(x->parent);
      if (!ls.try_lock(P) || ld(x->parent) != P) { ls.release(); backoff(); continue; }
      if (P == head_ || P->prio >= x->prio) return;
      Node* G = ld(P->parent);
      if (!ls.try_lock(G) || ld(P->parent) != G) { ls.release(); backoff(); continue; }
      rotate_up(G, P, x);
    }
  }

  // Treap remove: rotate s down below its higher-priority child until it has
  // at most one child, then splice it out.
  void rotate_down_and_splice(Node* s) {
    for (;;) {
      LockSet ls;
      ls.lock(s);
      Node* P = ld(s->parent);
      if (!ls.try_lock(P) || ld(s->parent) != P) { ls.release(); backoff(); continue; }
      Node *L = ld(s->left), *R = ld(s->right);
      if (!L || !R) {
        Node* c = L ? L : R;
        if (c) st(c->parent, P);
        st(slot_of(P, s), c);
        return;
      }
      Node* c = L->prio > R->prio ? L : R;
      if (!ls.try_lock(c)) { ls.release(); backoff(); continue; }
      rotate_up(P, s, c);
    }
  }

 public:
  LoTree() {
    head_ = new Node(INT_MIN, UINT32_MAX);
    tail_ = new Node(INT_MAX, UINT32_MAX);
    head_->succ = tail_;
    tail_->pred = head_;
    tail_->left = head_;
    head_->parent = tail_;
  }
  ~LoTree() {
    for (Node* n = head_; n;) {
      Node* nx = n->succ.load();
      delete n;
      n = nx;
    }
  }
  LoTree(const LoTree&) = delete;
  LoTree& operator=(const LoTree&) = delete;

  bool contains(int k) {
    Ebr::Guard g(ebr_);
    Node* n = search(k);
    while (n->key > k) n = ld(n->pred);
    while (n->key < k) n = ld(n->succ);
    return n->key == k && !marked(n);
  }

  bool insert(int k) {
    Ebr::Guard g(ebr_);
    for (;;) {
      Node* p = pred_of(k);
      p->succLock.lock();
      Node* s = ld(p->succ);
      if (marked(p) || s->key < k) { p->succLock.unlock(); continue; }
      if (s->key == k) { p->succLock.unlock(); return false; }
      Node* x = new Node(k, B == Balance::Treap ? static_cast<uint32_t>(tl_rng.next()) : 0);
      Node* parent = choose_parent(p, s);
      x->pred.store(p, std::memory_order_relaxed);
      x->succ.store(s, std::memory_order_relaxed);
      x->parent.store(parent, std::memory_order_relaxed);
      st(s->pred, x);
      st(p->succ, x);  // linearization point
      p->succLock.unlock();
      st(parent == p ? p->right : s->left, x);
      parent->treeLock.unlock();
      if constexpr (B == Balance::Avl) rebalance(parent);
      if constexpr (B == Balance::Treap) bubble_up(x);
      return true;
    }
  }

  bool remove(int k) {
    Ebr::Guard g(ebr_);
    for (;;) {
      Node* p = pred_of(k);
      p->succLock.lock();
      Node* s = ld(p->succ);
      if (marked(p) || s->key < k) { p->succLock.unlock(); continue; }
      if (s->key > k) { p->succLock.unlock(); return false; }
      s->succLock.lock();
      LockSet ls;
      Node *P = nullptr, *succ = nullptr, *SP = nullptr;
      if constexpr (B != Balance::Treap) lock_for_removal(s, ls, P, succ, SP);
      s->mark.store(true, std::memory_order_release);
      Node* ss = ld(s->succ);
      st(ss->pred, p);
      st(p->succ, ss);  // linearization point
      s->succLock.unlock();
      p->succLock.unlock();
      if constexpr (B == Balance::Treap) {
        rotate_down_and_splice(s);
      } else {
        Node* from = unlink_from_tree(s, P, succ, SP);
        ls.release();
        if constexpr (B == Balance::Avl) rebalance(from);
      }
      ebr_.retire(s);
      return true;
    }
  }

  // Quiescent self-check: tree in-order == list, links consistent, heap/balance stats.
  struct Check {
    bool ok;
    size_t size, depth, violations;  // violations: heap order (treap) or |balance| > 1 (AVL)
  };
  Check validate() const {
    Check r{true, 0, 0, 0};
    std::vector<std::pair<Node*, size_t>> stack;
    Node* list = ld(head_->succ);
    Node* cur = ld(head_->right);
    size_t d = 1;
    while (cur || !stack.empty()) {
      while (cur) { stack.push_back({cur, d++}); cur = ld(cur->left); }
      auto [n, nd] = stack.back();
      stack.pop_back();
      r.depth = std::max(r.depth, nd);
      if (n != list || marked(n) || ld(list->pred)->succ != list) r.ok = false;
      for (Node* c : {ld(n->left), ld(n->right)}) {
        if (c && ld(c->parent) != n) r.ok = false;
        if constexpr (B == Balance::Treap)
          if (c && c->prio > n->prio) ++r.violations;
      }
      if constexpr (B == Balance::Avl)
        if (std::abs(h(ld(n->left)) - h(ld(n->right))) > 1) ++r.violations;
      list = ld(list->succ);
      ++r.size;
      cur = ld(n->right);
      d = nd + 1;
    }
    if (list != tail_) r.ok = false;
    return r;
  }
};

using ConcurrentBst = LoTree<Balance::None>;
using ConcurrentAvl = LoTree<Balance::Avl>;
using ConcurrentTreap = LoTree<Balance::Treap>;
