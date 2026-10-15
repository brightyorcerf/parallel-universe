import java.util.concurrent.ThreadLocalRandom;
import java.util.concurrent.atomic.AtomicStampedReference;

/**
 * Elimination backoff stack (Hendler, Shavit, Yerushalmi): a Treiber stack whose failed CAS
 * attempts back off into an elimination array of lock-free exchangers. A push and a pop that meet
 * in the same randomly chosen slot exchange the value and both finish without touching top.
 * Backoff policy: each visit spins on a slot for at most TIMEOUT_NS, then retries the central stack.
 *
 * Linearization points:
 *  push/pop via central stack : as in TreiberStack (successful CAS on top)
 *  eliminated push/pop pair   : the successful CAS(WAITING -> BUSY) by the second thread;
 *                               the push is linearized immediately before the pop
 *  pop empty / peek           : read of top
 */
public class EliminationStack extends TreiberStack {
    static final long TIMEOUT_NS = 2_000;
    static final int CAPACITY = 8;
    static final Object TIMEOUT = new Object();

    static final class Exchanger {
        static final int EMPTY = 0, WAITING = 1, BUSY = 2;
        final AtomicStampedReference<Object> slot = new AtomicStampedReference<>(null, EMPTY);

        /** Returns the partner's item, or TIMEOUT. */
        Object exchange(Object mine) {
            long bound = System.nanoTime() + TIMEOUT_NS;
            int[] st = {EMPTY};
            while (System.nanoTime() < bound) {
                Object yours = slot.get(st);
                switch (st[0]) {
                    case EMPTY:
                        if (slot.compareAndSet(yours, mine, EMPTY, WAITING)) {
                            while (System.nanoTime() < bound) {
                                yours = slot.get(st);
                                if (st[0] == BUSY) { slot.set(null, EMPTY); return yours; }
                            }
                            if (slot.compareAndSet(mine, null, WAITING, EMPTY)) return TIMEOUT;
                            yours = slot.get(st);          // partner arrived at the last moment
                            slot.set(null, EMPTY);
                            return yours;
                        }
                        break;
                    case WAITING:
                        if (slot.compareAndSet(yours, mine, WAITING, BUSY)) return yours;
                        break;
                    default: break;                        // BUSY: another pair is finishing
                }
            }
            return TIMEOUT;
        }
    }

    final Exchanger[] elimination = new Exchanger[CAPACITY];
    { for (int i = 0; i < CAPACITY; i++) elimination[i] = new Exchanger(); }

    private Object visit(Object item) {
        return elimination[ThreadLocalRandom.current().nextInt(CAPACITY)].exchange(item);
    }

    @Override public void push(int x) {
        Node node = new Node(x);
        Integer item = x;
        while (true) {
            if (tryPush(node)) return;
            if (visit(item) == null) return;               // met a pop (pops offer null)
        }
    }

    @Override public Integer pop() {
        while (true) {
            Node n = tryPop();
            if (n == EMPTY_POP) return null;
            if (n != null) return n.value;
            Object other = visit(null);
            if (other != TIMEOUT && other != null) return (Integer) other;   // met a push
        }
    }
}
