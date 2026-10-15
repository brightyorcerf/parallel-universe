import java.util.concurrent.atomic.AtomicReference;

/**
 * Treiber lock-free stack. No ABA problem in Java: nodes are never reused while referenced (GC).
 *
 * Linearization points:
 *  push         : successful CAS(top, oldTop, node)
 *  pop success  : successful CAS(top, oldTop, oldTop.next)
 *  pop empty    : read of top == null
 *  peek         : read of top
 */
public class TreiberStack implements ConcurrentDS {
    static final class Node { final int value; Node next; Node(int v) { value = v; } }

    final AtomicReference<Node> top = new AtomicReference<>();

    protected boolean tryPush(Node node) {
        Node oldTop = top.get();
        node.next = oldTop;
        return top.compareAndSet(oldTop, node);
    }

    /** Returns the popped node, EMPTY_POP if empty, or null if the CAS lost a race. */
    protected Node tryPop() {
        Node oldTop = top.get();
        if (oldTop == null) return EMPTY_POP;
        return top.compareAndSet(oldTop, oldTop.next) ? oldTop : null;
    }
    static final Node EMPTY_POP = new Node(0);

    public void push(int x) {
        Node node = new Node(x);
        while (!tryPush(node)) { }                              // retry on CAS failure
    }

    public Integer pop() {
        while (true) {
            Node n = tryPop();
            if (n == EMPTY_POP) return null;
            if (n != null) return n.value;
        }
    }

    public Integer peek() { Node t = top.get(); return t == null ? null : t.value; }

    public boolean read(int k) { return peek() != null; }
    public boolean insert(int k) { push(k); return true; }
    public boolean delete(int k) { return pop() != null; }
}
