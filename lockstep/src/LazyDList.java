import java.util.concurrent.locks.ReentrantLock;

/**
 * Lazy doubly linked list (sorted set). Optimistic lock-free traversal, lock pred+curr,
 * validate, logical delete (marked) then physical unlink of next and prev links.
 *
 * Invariant for prev links: x.prev is only written while holding the lock of x's predecessor,
 * so prev links are never torn by concurrent updates.
 *
 * Linearization points:
 *  insert  success : write pred.next = node (both locks held, validated)
 *  insert  fail    : successful validation that found curr.key == key (unmarked)
 *  delete  success : write curr.marked = true
 *  delete  fail    : successful validation that found curr.key != key
 *  contains        : true -> read of curr.marked == false; false -> the later of the read of the
 *                    first node with key >= key, or the moment the matching node was marked
 */
public class LazyDList implements ConcurrentDS {
    static final class Node {
        final int key;
        volatile Node next, prev;
        volatile boolean marked;
        final ReentrantLock lock = new ReentrantLock();
        Node(int key) { this.key = key; }
    }

    final Node head = new Node(Integer.MIN_VALUE), tail = new Node(Integer.MAX_VALUE);
    { head.next = tail; tail.prev = head; }

    private static boolean validate(Node pred, Node curr) {
        return !pred.marked && !curr.marked && pred.next == curr;
    }

    public boolean contains(int key) {           // wait-free, no locks
        Node curr = head;
        while (curr.key < key) curr = curr.next;
        return curr.key == key && !curr.marked;
    }

    public boolean insert(int key) {
        while (true) {
            Node pred = head, curr = head.next;
            while (curr.key < key) { pred = curr; curr = curr.next; }
            pred.lock.lock();
            try {
                curr.lock.lock();
                try {
                    if (validate(pred, curr)) {
                        if (curr.key == key) return false;
                        Node node = new Node(key);
                        node.next = curr;
                        node.prev = pred;
                        pred.next = node;             // LP
                        curr.prev = node;
                        return true;
                    }
                } finally { curr.lock.unlock(); }
            } finally { pred.lock.unlock(); }
        }
    }

    public boolean delete(int key) {
        while (true) {
            Node pred = head, curr = head.next;
            while (curr.key < key) { pred = curr; curr = curr.next; }
            pred.lock.lock();
            try {
                curr.lock.lock();
                try {
                    if (validate(pred, curr)) {
                        if (curr.key != key) return false;
                        curr.marked = true;           // LP (logical delete)
                        Node succ = curr.next;
                        pred.next = succ;             // physical delete
                        succ.prev = pred;             // safe: we hold succ's predecessor (curr) lock
                        return true;
                    }
                } finally { curr.lock.unlock(); }
            } finally { pred.lock.unlock(); }
        }
    }

    public boolean read(int key) { return contains(key); }
}
