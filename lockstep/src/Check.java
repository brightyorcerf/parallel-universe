import java.util.*;
import java.util.concurrent.*;

/** Concurrency sanity check for every implementation. Run: java Check */
public class Check {
    public static void main(String[] args) throws Exception {
        int T = 8, PER = 20_000;
        for (String impl : new String[]{"LazyDList", "LockFreeDList"}) {
            ConcurrentDS s = Bench.make(impl);
            // each thread owns keys k % T == id: insert all, delete the odd ones, contended by peers' reads
            par(T, id -> {
                for (int k = id; k < T * PER; k += T) check(s.insert(k));
                for (int k = id; k < T * PER; k += T) if (k % 2 == 1) check(s.delete(k));
                for (int k = id; k < T * PER; k += T) check(s.read(k) == (k % 2 == 0));
            });
            // contended same-key insert/delete: per key, successful inserts - deletes must be 0 or 1
            ConcurrentHashMap<Integer, Integer> bal = new ConcurrentHashMap<>();
            ConcurrentDS c = Bench.make(impl);
            par(T, id -> {
                Random r = new Random(id);
                for (int i = 0; i < 200_000; i++) {
                    int k = r.nextInt(64);
                    if (r.nextBoolean()) { if (c.insert(k)) bal.merge(k, 1, Integer::sum); }
                    else if (c.delete(k)) bal.merge(k, -1, Integer::sum);
                }
            });
            for (int k = 0; k < 64; k++) check(bal.getOrDefault(k, 0) == (c.read(k) ? 1 : 0));
            if (impl.equals("LazyDList")) {               // prev links must mirror next links
                LazyDList l = (LazyDList) c;
                for (LazyDList.Node x = l.head; x.next != null; x = x.next) check(x.next.prev == x);
            }
            System.out.println(impl + " OK");
        }
        for (String impl : new String[]{"LockStack", "TreiberStack", "EliminationStack", "LockQueue", "MSQueue"}) {
            ConcurrentDS ds = Bench.make(impl);
            // every pushed value is popped exactly once
            Set<Integer> popped = ConcurrentHashMap.newKeySet();
            par(T, id -> {
                for (int i = 0; i < PER; i++) {
                    ds.insert(id * PER + i);
                    Integer v = pop(ds);
                    if (v != null) check(popped.add(v));
                }
            });
            Integer v;
            while ((v = pop(ds)) != null) check(popped.add(v));
            check(popped.size() == T * PER);
            System.out.println(impl + " OK");
        }
    }

    static Integer pop(ConcurrentDS ds) {
        if (ds instanceof TreiberStack) return ((TreiberStack) ds).pop();
        if (ds instanceof LockStack) return ((LockStack) ds).pop();
        if (ds instanceof LockQueue) return ((LockQueue) ds).dequeue();
        return ((MSQueue) ds).dequeue();
    }

    static void check(boolean ok) { if (!ok) throw new AssertionError(); }

    interface Body { void run(int id) throws Exception; }
    static void par(int t, Body b) throws Exception {
        ExecutorService ex = Executors.newFixedThreadPool(t);
        List<Future<?>> fs = new ArrayList<>();
        for (int i = 0; i < t; i++) { int id = i; fs.add(ex.submit(() -> { b.run(id); return null; })); }
        for (Future<?> f : fs) f.get();
        ex.shutdown();
    }
}
