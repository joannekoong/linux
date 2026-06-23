#!/bin/bash
# Write benchmark suite. Assumes passthrough_hp is ALREADY mounted at ~/mounts/tmp2
# with the cache mode you want to test (writeback vs writethrough).
# Run once per mount mode:
#   (mount wb)  bash ~/bench_write.sh | tee ~/wb_write.txt
#   (mount wt)  bash ~/bench_write.sh | tee ~/wt_write.txt
set -u
MNT=~/mounts/tmp2; SIZE=4G; NODE=0
NC="numactl --cpunodebind=$NODE --membind=$NODE"
cd "$MNT" || exit 1
DEV=$(awk -v m="$(readlink -f .)" '$5==m{print $3}' /proc/self/mountinfo)
CONN=/sys/fs/fuse/connections/${DEV#0:}
echo "=== CONN=$CONN gov=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor) ==="
cat "$CONN/large_folios"

# gate sanity (lf=0 must be order 0)
echo 0 | sudo tee "$CONN/large_folios" >/dev/null; rm -f g0
$NC fio --name=lay --rw=write --bs=1M --size=64M --direct=0 --filename=g0 >/dev/null
GINO=$(stat -c '%i' g0)
sudo bpftrace -e "kprobe:filemap_add_folio { \$m=(struct address_space *)arg0; if (\$m->host->i_ino==$GINO){ @[(\$m->flags>>21)&0x1f]=count(); } }" & sleep 2
sync; echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null
$NC fio --name=c --rw=read --bs=1M --size=64M --direct=0 --filename=g0 >/dev/null
sleep 1; sudo pkill -INT bpftrace
echo "=== ^ gate lf=0 : expect order 0 ==="

# sequential write: fresh allocating write each rep, end_fsync, single-pass x reps
run_seqwrite() {  # label bs reps
  local label="$1" bs="$2" reps="$3"
  for tog in 0 1; do
    echo "$tog" | sudo tee "$CONN/large_folios" >/dev/null
    echo "######## $label lf=$tog ########"
    for r in $(seq 1 "$reps"); do
      rm -f sw; sync; echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null
      $NC fio --name=w --ioengine=psync --rw=write --bs="$bs" --size=$SIZE --numjobs=1 --direct=0 --end_fsync=1 --group_reporting=1 --filename=sw | grep -E 'WRITE:|write:|cpu '
    done
  done
}

# random write: lay file once (overwrite, inode gets toggle), drop caches, time_based, end_fsync
run_randwrite() {  # label bs reps
  local label="$1" bs="$2" reps="$3"
  for tog in 0 1; do
    echo "$tog" | sudo tee "$CONN/large_folios" >/dev/null
    rm -f rw; $NC fio --name=lay --rw=write --bs=1M --size=$SIZE --direct=0 --filename=rw >/dev/null
    echo "######## $label lf=$tog ########"
    for r in $(seq 1 "$reps"); do
      sync; echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null
      $NC fio --name=w --ioengine=psync --rw=randwrite --bs="$bs" --size=$SIZE --numjobs=1 --direct=0 --end_fsync=1 --time_based --runtime=20 --ramp_time=3 --group_reporting=1 --filename=rw | grep -E 'WRITE:|write:|cpu '
    done
  done
}

run_seqwrite  seq_write_1M    1M   10
run_seqwrite  seq_write_128k  128k 10
run_randwrite rand_write_1M   1M   5
run_randwrite rand_write_128k 128k 5
