#!/bin/sh
# Rebuilds the lockstep sources with this directory's Bench (which knows the
# three variants) and reruns both experiments. Run from this directory, on AC
# power; caffeinate keeps macOS from sleeping mid-run.
set -e
mkdir -p build results
javac -d build $(ls ../../src/*.java | grep -v '/Bench.java$') *.java

caffeinate -dims sh -c '
  for impl in LockStack LockStackFair LockStackSpin LockStackCount TreiberStack; do
    java -Xms2g -Xmx2g -cp build Bench $impl 1000 0C-50I-50D 1,2,3,4,6,8 5 2000
  done' > results/sweep.csv

for t in 1 2 3 4 8; do
  caffeinate -dims java -Xms2g -Xmx2g \
    -XX:StartFlightRecording:settings=$PWD/park.jfc,filename=$PWD/build/p$t.jfr \
    -cp build Bench LockStack 1000 0C-50I-50D $t 1 2000 2>/dev/null | grep -v '^\['
  java -cp build CountParks build/p$t.jfr
done
