import java.util.concurrent.locks.ReentrantLock;
import java.util.concurrent.atomic.LongAdder;

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
public class LockStackCount implements ConcurrentDS {
    static final class Node { final int value; Node next; Node(int v) { value = v; } }

    volatile Node top;
    final ReentrantLock lock = new ReentrantLock();
    static final LongAdder attempts = new LongAdder(), failures = new LongAdder();

    public void push(int x) {
        Node node = new Node(x);
        while (true) {
            Node observed = top;
            node.next = observed;
            lock.lock(); attempts.increment();
            try {
                if (top == observed) { top = node; return; }      // validate, LP
                failures.increment();
            } finally { lock.unlock(); }
        }
    }

    public Integer pop() {
        while (true) {
            Node observed = top;
            if (observed == null) return null;                    // LP (empty)
            lock.lock(); attempts.increment();
            try {
                if (top == observed) { top = observed.next; return observed.value; }   // LP
                failures.increment();
            } finally { lock.unlock(); }
        }
    }

    public Integer peek() { Node t = top; return t == null ? null : t.value; }

    public boolean read(int k) { return peek() != null; }
    public boolean insert(int k) { push(k); return true; }
    public boolean delete(int k) { return pop() != null; }
}
