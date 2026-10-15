/** Common view used by the benchmark: C = read, I = insert, D = delete. */
public interface ConcurrentDS {
    boolean read(int key);
    boolean insert(int key);
    boolean delete(int key);
}
