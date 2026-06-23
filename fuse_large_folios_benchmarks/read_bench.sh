#!/bin/bash
set -u

SRC="${1:?usage: read_bench.sh <backing-src-dir>}"
PASS_FLAGS="--nopassthrough --foreground"
MNT=~/mounts/tmp2
SIZE=4G
NODE=0
NC="numactl --cpunodebind=$NODE --membind=$NODE"

mkdir -p "$MNT"
fusermount3 -u "$MNT" 2>/dev/null
$NC ~/libfuse/build/example/passthrough_hp $PASS_FLAGS "$SRC" "$MNT" &
sleep 2
cd "$MNT"

DEV=$(awk -v m="$(readlink -f .)" '$5==m{print $3}' /proc/self/mountinfo)
CONN=/sys/fs/fuse/connections/${DEV#0:}
echo "=== SRC=$SRC CONN=$CONN gov=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor) ==="
cat "$CONN/large_folios"

# gate sanity (lf=0 must be order 0)
echo 0 | sudo tee "$CONN/large_folios" >/dev/null
rm -f g0
$NC fio --name=lay --rw=write --bs=1M --size=64M --direct=0 --filename=g0 >/dev/null
GINO=$(stat -c '%i' g0)
sudo bpftrace -e "kprobe:filemap_add_folio { \$m=(struct address_space *)arg0; if (\$m->host->i_ino==$GINO){ @[(\$m->flags>>21)&0x1f]=count(); } }" &
sleep 2
sync; echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null
$NC fio --name=c --rw=read --bs=1M --size=64M --direct=0 --filename=g0 >/dev/null
sleep 1
sudo pkill -INT bpftrace
echo "=== ^ gate lf=0 : expect order 0 ==="

run_read() {  # label rw bs reps cold|warm extra
	local label="$1" rw="$2" bs="$3" reps="$4" cache="$5" extra="$6"
	for tog in 0 1; do
		echo "$tog" | sudo tee "$CONN/large_folios" >/dev/null
		local F="b_${label}_${tog}"
		rm -f b_* wf_*
		$NC fio --name=lay --rw=write --bs=1M --size=$SIZE --direct=0 --filename="$F" >/dev/null
		[ "$cache" = warm ] && $NC fio --name=warm --rw=read --bs=1M --size=$SIZE --direct=0 --filename="$F" >/dev/null
		echo "######## $label lf=$tog ########"
		for r in $(seq 1 "$reps"); do
			[ "$cache" = cold ] && { sync; echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null; }
			$NC fio --name=c --ioengine=psync --rw="$rw" --bs="$bs" --size=$SIZE --numjobs=1 --direct=0 $extra --group_reporting=1 --filename="$F" | grep -E 'READ:|read:|cpu '
		done
	done
}

TB="--time_based --runtime=20 --ramp_time=3"

run_read seq_cold_128k  read     128k 10 cold ""
run_read seq_warm_128k  read     128k 5  warm "$TB"
run_read seq_cold_1M    read     1M   10 cold ""
run_read seq_warm_1M    read     1M   5  warm "$TB"
run_read rand_cold_4k   randread 4k   5  cold "$TB"
run_read rand_warm_4k   randread 4k   5  warm "$TB"
run_read rand_cold_128k randread 128k 5  cold "$TB"
run_read rand_warm_128k randread 128k 5  warm "$TB"
