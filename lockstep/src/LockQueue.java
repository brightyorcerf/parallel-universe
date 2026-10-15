import java.util.concurrent.locks.ReentrantLock;

/**
 * Two-lock queue with lazy validation (Michael-Scott two-lock variant). Separate head and tail locks
 * let one enqueuer and one dequeuer proceed in parallel. Dequeue reads head/head.next optimistically,
 * then locks and validates that head is unchanged before advancing it.
 *
 * Linearization points:
 *  enqueue          : write tail.next = node (under tailLock)
 *  dequeue success  : write head = first (under headLock, after validation)
 *  dequeue empty    : volatile read of head.next == null
 *  peek             : volatile read of head.next, confirmed by head being unchanged
 */
public class LockQueue implements ConcurrentDS {
    static final class Node { final int value; volatile Node next; Node(int v) { value = v; } }

    volatile Node head;
    Node tail;                                  // guarded by tailLock
    final ReentrantLock headLock = new ReentrantLock(), tailLock = new ReentrantLock();

    public LockQueue() { head = tail = new Node(0); }

    public void enqueue(int x) {
        Node node = new Node(x);
        tailLock.lock();
        try {
            tail.next = node;                   // LP
            tail = node;
        } finally { tailLock.unlock(); }
    }

    public Integer dequeue() {
        while (true) {
            Node h = head, first = h.next;
            if (first == null) return null;     // LP (empty)
            headLock.lock();
            try {
                if (head == h) {
                    head = first;                                       // validate, LP
                    h.next = h;     // self-link the dead sentinel so it can't keep younger nodes alive (GC nepotism)
                    return first.value;
                }
            } finally { headLock.unlock(); }
        }
    }

    public Integer peek() {
        while (true) {
            Node h = head, first = h.next;
            if (head == h) return first == null ? null : first.value;
        }
    }

    public boolean read(int k) { return peek() != null; }
    public boolean insert(int k) { enqueue(k); return true; }
    public boolean delete(int k) { return dequeue() != null; }
}
