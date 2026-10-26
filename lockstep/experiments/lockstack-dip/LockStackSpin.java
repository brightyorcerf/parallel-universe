import java.util.concurrent.atomic.AtomicBoolean;

/**
 * Lazy lock-based stack: read top optimistically outside the lock, prepare the update, then lock,
 * validate that top is still the observed node, and only then publish. Retry on failed validation.
 *
 * Linearization points:
 *  push           : write top = node (under lock, after validation)
 *  pop  success   : write top = observed.next (under lock, after validation)
 *  pop  empty     : volatile read of top == null
 *  peek           : volatile read of top
 */
public class LockStackSpin implements ConcurrentDS {
    static final class Node { final int value; Node next; Node(int v) { value = v; } }

    volatile Node top;
    final AtomicBoolean held = new AtomicBoolean();
    // test-and-test-and-set spin lock: waiters never park
    private void lockSpin() { while (true) { while (held.get()) Thread.onSpinWait(); if (held.compareAndSet(false, true)) return; } }
    private void unlockSpin() { held.set(false); }

    public void push(int x) {
        Node node = new Node(x);
        while (true) {
            Node observed = top;
            node.next = observed;
            lockSpin();
            try {
                if (top == observed) { top = node; return; }      // validate, LP
            } finally { unlockSpin(); }
        }
    }

    public Integer pop() {
        while (true) {
            Node observed = top;
            if (observed == null) return null;                    // LP (empty)
            lockSpin();
            try {
                if (top == observed) { top = observed.next; return observed.value; }   // LP
            } finally { unlockSpin(); }
        }
    }

    public Integer peek() { Node t = top; return t == null ? null : t.value; }

    public boolean read(int k) { return peek() != null; }
    public boolean insert(int k) { push(k); return true; }
    public boolean delete(int k) { return pop() != null; }
}
