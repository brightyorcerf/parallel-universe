import java.util.concurrent.atomic.AtomicMarkableReference;

/**
 * Non-blocking doubly linked list (sorted set), Harris-Michael style.
 * The next chain (AtomicMarkableReference: successor + deleted mark) is the authoritative
 * structure and every update on it is a CAS. prev links are maintained as hints after every
 * successful insert/unlink (a plain volatile write, never needed for correctness), the same
 * approach used by most practical lock-free deques/lists to avoid multi-word CAS.
 * Traversals help: find() physically unlinks any marked node it meets (CAS) and retries on failure.
 *
 * Linearization points:
 *  insert  success : successful CAS pred.next: curr -> node
 *  insert  fail    : read in find() that returned unmarked curr with curr.key == key
 *  delete  success : successful CAS marking curr.next (logical delete)
 *  delete  fail    : read in find() that returned curr with curr.key != key
 *  contains        : read of curr.next's mark (true) / point curr was observed or became marked (false)
 */
public class LockFreeDList implements ConcurrentDS {
    static final class Node {
        final int key;
        final AtomicMarkableReference<Node> next;
        volatile Node prev;
        Node(int key, Node succ) { this.key = key; this.next = new AtomicMarkableReference<>(succ, false); }
    }

    final Node tail = new Node(Integer.MAX_VALUE, null);
    final Node head = new Node(Integer.MIN_VALUE, tail);
    { tail.prev = head; }

    /** Returns {pred, curr} with pred.key < key <= curr.key, unlinking marked nodes on the way. */
    private Node[] find(int key) {
        boolean[] marked = {false};
        retry:
        while (true) {
            Node pred = head, curr = pred.next.getReference();
            while (true) {
                Node succ = curr.next.get(marked);
                while (marked[0]) {                                  // helping: snip curr
                    if (!pred.next.compareAndSet(curr, succ, false, false)) continue retry;
                    succ.prev = pred;
                    curr = succ;
                    succ = curr.next.get(marked);
                }
                if (curr.key >= key) return new Node[]{pred, curr};
                pred = curr;
                curr = succ;
            }
        }
    }

    public boolean insert(int key) {
        while (true) {
            Node[] w = find(key);
            Node pred = w[0], curr = w[1];
            if (curr.key == key) return false;
            Node node = new Node(key, curr);
            node.prev = pred;
            if (pred.next.compareAndSet(curr, node, false, false)) {   // LP
                curr.prev = node;
                return true;
            }                                                        // CAS failed: retry
        }
    }

    public boolean delete(int key) {
        while (true) {
            Node[] w = find(key);
            Node pred = w[0], curr = w[1];
            if (curr.key != key) return false;
            Node succ = curr.next.getReference();
            // compareAndSet (not attemptMark) so a node already marked by another thread fails here
            if (!curr.next.compareAndSet(succ, succ, false, true)) continue;   // LP on success
            if (pred.next.compareAndSet(curr, succ, false, false)) succ.prev = pred;
            return true;                                // if unlink CAS failed, a later find() helps
        }
    }

    public boolean contains(int key) {                 // wait-free
        Node curr = head;
        while (curr.key < key) curr = curr.next.getReference();
        return curr.key == key && !curr.next.isMarked();
    }

    public boolean read(int key) { return contains(key); }
}
