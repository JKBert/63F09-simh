#!/bin/sh
# run_all.sh : every oracle of the 63F09 simulator, one line of summary each (about 10 minutes).  Run from anywhere.
cd "$(dirname "$0")" || exit 1
for t in run_isa run_wide run_misc run_exc run_exc16 run_irq run_acia; do
  printf '%-10s ' $t; python3 $t.py 2>&1 | tail -1
done
printf '%-10s ' run_shn; python3 run_shn.py 2>&1 | tail -1
for s in 1 2; do printf 'run_fpu %s ' $s; python3 run_fpu.py 600 $s 2>&1 | tail -1; done
for s in 1 2; do printf 'run_fpu2 %s ' $s; python3 run_fpu2.py 300 $s 2>&1 | tail -1; done
