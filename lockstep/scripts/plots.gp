# One throughput and one speedup figure per comparison; each is a 2x3 grid (N rows x workload columns).
set terminal pngcairo size 1600,950 font "Helvetica,11"
fams  = "dlist stack queue treiber_vs_elimination"
A     = "LazyDList LockStack LockQueue TreiberStack"
B     = "LockFreeDList TreiberStack MSQueue EliminationStack"
Ns    = "1000 10000"
wls   = "0C-50I-50D 50C-25I-25D 100C-0I-0D"
set xlabel "Threads"
set xtics (1,2,4,6,8,10,12,14,16)
set xrange [1:16]
set yrange [0:*]
set grid
set key below horizontal
set bmargin 5
set style line 1 lc rgb "#2a6fdb" lw 2 pt 7
set style line 2 lc rgb "#e0662d" lw 2 pt 5
set style line 3 lc rgb "#888888" lw 1 dt 2
do for [f=1:4] {
  do for [m=1:2] {
    set output sprintf("plots/%s_%s.png", word(fams,f), m==1 ? "throughput" : "speedup")
    set multiplot layout 2,3 title sprintf("%s vs %s - %s", word(A,f), word(B,f), m==1 ? "average throughput (MOPS, error bars = stdev of 5 runs)" : "speedup = T(p)/T(1)") font ",14"
    do for [n=1:2] {
      do for [w=1:3] {
        set title sprintf("N = %s, %s", word(Ns,n), word(wls,w))
        fa = sprintf("results/data/%s_%s_%s.dat", word(A,f), word(Ns,n), word(wls,w))
        fb = sprintf("results/data/%s_%s_%s.dat", word(B,f), word(Ns,n), word(wls,w))
        if (m == 1) {
          set ylabel "Throughput (MOPS)"
          plot fa u 1:2:3 w yerrorlines ls 1 t word(A,f), fb u 1:2:3 w yerrorlines ls 2 t word(B,f)
        } else {
          set ylabel "Speedup"
          plot fa u 1:4 w lp ls 1 t word(A,f), fb u 1:4 w lp ls 2 t word(B,f), x w l ls 3 t "Ideal (p)"
        }
      }
    }
    unset multiplot
  }
}
