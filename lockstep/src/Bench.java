import java.util.Arrays;
import java.util.Random;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ThreadLocalRandom;

/**
 * Usage: java Bench <impl> <N> <workload> <threads,csv> <runs> <durationMs>
 * Prints CSV: impl,N,workload,threads,run,ops,elapsed_ns,mops
 * Timing uses only System.nanoTime(). Prefill, workload generation and thread creation are outside it.
 */
public class Bench {
    static final int WORK_LEN = 1 << 18, MASK = WORK_LEN - 1, CHECK_EVERY = 64;
    static volatile long deadline;

    static ConcurrentDS make(String impl) {
        switch (impl) {
            case "LazyDList": return new LazyDList();
            case "LockFreeDList": return new LockFreeDList();
            case "LockStack": return new LockStack();
            case "TreiberStack": return new TreiberStack();
            case "EliminationStack": return new EliminationStack();
            case "LockQueue": return new LockQueue();
            case "MSQueue": return new MSQueue();
            default: throw new IllegalArgumentException(impl);
        }
    }

    static int readPct(String wl) {
        switch (wl) {
            case "0C-50I-50D": return 0;
            case "50C-25I-25D": return 50;
            case "100C-0I-0D": return 100;
            default: throw new IllegalArgumentException(wl);
        }
    }

    /** Returns {completedOps, elapsedNs}. */
    static long[] run(String impl, int n, int readPct, int threads, long durNs, long seed) throws Exception {
        ConcurrentDS ds = make(impl);
        // 50% prefill with distinct keys in random order
        int[] keys = new int[n];
        for (int i = 0; i < n; i++) keys[i] = i;
        Random rnd = new Random(seed);
        for (int i = n - 1; i > 0; i--) { int j = rnd.nextInt(i + 1), t = keys[i]; keys[i] = keys[j]; keys[j] = t; }
        for (int i = 0; i < n / 2; i++) ds.insert(keys[i]);

        // pre-generated workload: entry = key << 2 | op (0 = C, 1 = I, 2 = D)
        int insPct = (100 - readPct) / 2;
        int[][] work = new int[threads][WORK_LEN];
        for (int[] w : work) {
            ThreadLocalRandom r = ThreadLocalRandom.current();
            for (int i = 0; i < WORK_LEN; i++) {
                int p = r.nextInt(100), op = p < readPct ? 0 : p < readPct + insPct ? 1 : 2;
                w[i] = r.nextInt(n) << 2 | op;
            }
        }

        long[] counts = new long[threads];
        CountDownLatch ready = new CountDownLatch(threads), go = new CountDownLatch(1);
        Thread[] ts = new Thread[threads];
        for (int t = 0; t < threads; t++) {
            final int id = t;
            ts[t] = new Thread(() -> {
                int[] w = work[id];
                ready.countDown();
                try { go.await(); } catch (InterruptedException e) { return; }
                long dl = deadline, c = 0;
                int i = 0;
                // Deadline checked every CHECK_EVERY ops: on macOS System.nanoTime() CASes a global
                // monotonicity word, so calling it per op caps total throughput near 29 M calls/s.
                while (System.nanoTime() < dl) {
                    for (int j = 0; j < CHECK_EVERY; j++) {
                        int e = w[i++ & MASK], k = e >>> 2;
                        switch (e & 3) {
                            case 0: ds.read(k); break;
                            case 1: ds.insert(k); break;
                            default: ds.delete(k);
                        }
                    }
                    c += CHECK_EVERY;
                }
                counts[id] = c;
            });
            ts[t].start();
        }
        ready.await();
        System.gc();
        long start = System.nanoTime();
        deadline = start + durNs;
        go.countDown();
        for (Thread t : ts) t.join();
        long end = System.nanoTime();
        return new long[]{Arrays.stream(counts).sum(), end - start};
    }

    public static void main(String[] a) throws Exception {
        String impl = a[0], wl = a[2];
        int n = Integer.parseInt(a[1]), runs = Integer.parseInt(a[4]), rp = readPct(wl);
        int[] threads = Arrays.stream(a[3].split(",")).mapToInt(Integer::parseInt).toArray();
        long durNs = Long.parseLong(a[5]) * 1_000_000L;

        run(impl, n, rp, 4, Math.min(durNs, 1_000_000_000L), 0);           // JIT warm-up, discarded
        for (int t : threads)
            for (int r = 1; r <= runs; r++) {
                long[] res = run(impl, n, rp, t, durNs, r);
                double mops = res[0] / (res[1] / 1e9) / 1e6;
                System.out.printf("%s,%d,%s,%d,%d,%d,%d,%.6f%n", impl, n, wl, t, r, res[0], res[1], mops);
            }
    }
}
