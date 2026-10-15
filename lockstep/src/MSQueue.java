import java.util.concurrent.atomic.AtomicReference;

/**
 * Michael-Scott lock-free queue. Lagging tail is helped forward by any thread that sees it.
 *
 * Linearization points:
 *  enqueue          : successful CAS(last.next, null, node)
 *  dequeue success  : successful CAS(head, first, next)
 *  dequeue empty    : read of first.next == null while head == tail
 *  peek             : read of head.next, confirmed by head being unchanged
 */
public class MSQueue implements ConcurrentDS {
    static final class Node {
        final int value;
        final AtomicReference<Node> next = new AtomicReference<>();
        Node(int v) { value = v; }
    }

    final AtomicReference<Node> head, tail;

    public MSQueue() { Node s = new Node(0); head = new AtomicReference<>(s); tail = new AtomicReference<>(s); }

    public void enqueue(int x) {
        Node node = new Node(x);
        while (true) {
            Node last = tail.get(), next = last.next.get();
            if (last != tail.get()) continue;
            if (next == null) {
                if (last.next.compareAndSet(null, node)) {      // LP
                    tail.compareAndSet(last, node);
                    return;
                }
            } else {
                tail.compareAndSet(last, next);                  // help lagging tail
            }
        }
    }

    public Integer dequeue() {
        while (true) {
            Node first = head.get(), last = tail.get(), next = first.next.get();
            if (first != head.get()) continue;
            if (first == last) {
                if (next == null) return null;                   // LP (empty)
                tail.compareAndSet(last, next);                  // help
            } else if (head.compareAndSet(first, next)) {     // LP
                first.next.set(first);   // self-link dead sentinel against GC nepotism; stale readers fail their head/tail checks
                return next.value;
            }
        }
    }

    public Integer peek() {
        while (true) {
            Node h = head.get(), first = h.next.get();
            if (head.get() == h) return first == null ? null : first.value;
        }
    }

    public boolean read(int k) { return peek() != null; }
    public boolean insert(int k) { enqueue(k); return true; }
    public boolean delete(int k) { return dequeue() != null; }
}
