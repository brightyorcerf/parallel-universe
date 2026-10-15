# parallel-universe

High-performance computing coursework: parallel kernels and concurrent data
structures, each measured carefully enough that the numbers mean something.

| project | what | language |
|---|---|---|
| [threadmill](threadmill/) | five OpenMP kernels (A², X¹⁵, sorting, primes in [2⁴⁰, 2⁴¹), dot product) with algorithmic shortcuts, sequential twins and checksum verification | C11 + OpenMP |
| [lockstep](lockstep/) | lazy vs lock-free lists, stacks, queues and an elimination backoff stack, 1,890 timed runs from 1 to 16 threads | Java 17 |

Each project is self-contained: its own build, scripts, raw results, gnuplot
figures and report. Both READMEs end with the measurement traps that were hit
along the way, since those are the parts most worth reading.
