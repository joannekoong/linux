#!/bin/bash
#
# fuse-over-io-uring zero-copy benchmark: baseline vs zero-copy
#
# Reads are measured against NVMe-backed storage (the headline path).
# Writes are measured against tmpfs-backed storage, because a single
# commodity NVMe is write-throttled and would mask the copy savings.
#
# Run as root (zero-copy requires CAP_SYS_ADMIN), ideally under tmux:
#   tmux new -s bench
#   ./bench.sh 2>&1 | tee /root/bench/console.log
#
# Before running, do ONE manual smoke test to confirm the modes engage:
#   <server> /root/src_nvme /root/mnt --nopassthrough -o io_uring_q_depth=8 \
#            -o io_uring_zero_copy -d
#   (in another shell: cat a file under /root/mnt, watch for ADD_BUFPOOL /
#    FUSE_URING_ZERO_COPY / FOPEN_IO_URING_ZERO_COPY in the log, then Ctrl-C)
#
set -u

# ---------- config (edit these for your box / libfuse fork) ----------
SERVER=${SERVER:-$HOME/libfuse/build/example/passthrough_hp}
MNT=${MNT:-/root/mnt}
SRC_NVME=${SRC_NVME:-/root/src_nvme}
SRC_TMPFS=${SRC_TMPFS:-/root/src_tmpfs}
RESULTS=${RESULTS:-/root/bench/run-$(uname -r)}
SZ=${SZ:-8G}              # per-job file size; keep TMPFS_SZ > 2*SZ-ish
TMPFS_SZ=${TMPFS_SZ:-16G}
RUNTIME=${RUNTIME:-60}
RAMP=${RAMP:-10}
ITERS=${ITERS:-3}
NUMJOBS=${NUMJOBS:-2}
BS=${BS:-1M}
# configs to run this invocation (space-separated subset of: baseline regbuf zc).
# You can run a subset and re-run later into the same RESULTS dir; deltas are
# computed from whatever is present.
CONFIGS=${CONFIGS:-"baseline regbuf zc"}

# Map these to your libfuse fork's option names if different.
# Baseline = io-uring without zero-copy; ZC = io-uring with zero-copy.
BASE_OPTS=${BASE_OPTS:-"-o io_uring_q_depth=8"}
# NOTE: this fork's -o parser is order-sensitive; the feature flag must come
# before io_uring_q_depth.
REGBUF_OPTS=${REGBUF_OPTS:-"-o io_uring_registered_buffers -o io_uring_q_depth=8"}
ZC_OPTS=${ZC_OPTS:-"-o io_uring_zero_copy -o io_uring_q_depth=8"}
# --------------------------------------------------------------------

die() { echo "ERROR: $*" >&2; exit 1; }

command -v fio >/dev/null || die "fio not found"
command -v jq  >/dev/null || die "jq not found"
[ -x "$SERVER" ] || die "server not executable at: $SERVER"

mkdir -p "$MNT" "$SRC_NVME" "$SRC_TMPFS" "$RESULTS"

# zero-copy/registered buffers pin memory against RLIMIT_MEMLOCK; the default
# can be too small for many queues. Raise it (best effort; needs privilege).
ulimit -l unlimited 2>/dev/null || ulimit -l 4194304 2>/dev/null || true
echo "memlock limit (KB): $(ulimit -l)"

# enable fuse io-uring (module param; writable at runtime)
if [ -w /sys/module/fuse/parameters/enable_uring ]; then
	echo 1 > /sys/module/fuse/parameters/enable_uring
fi
grep -qi '^[Y1]' /sys/module/fuse/parameters/enable_uring 2>/dev/null \
	|| die "fuse enable_uring is not on (need CONFIG/boot param fuse.enable_uring=1)"

# tmpfs for the write tests; guard against backing it with too little RAM
avail_gb=$(awk '/MemAvailable/{print int($2/1024/1024)}' /proc/meminfo)
need_gb=$(( ${TMPFS_SZ%G} + 2 ))
echo "MemAvailable=${avail_gb}GiB  tmpfs=${TMPFS_SZ}  (need >=${need_gb}GiB)"
[ "$avail_gb" -ge "$need_gb" ] || die "not enough RAM for ${TMPFS_SZ} tmpfs"
mountpoint -q "$SRC_TMPFS" || mount -t tmpfs -o size="$TMPFS_SZ" tmpfs "$SRC_TMPFS" \
	|| die "tmpfs mount failed"

# record environment for reproducibility / the cover letter
{ echo "date(local box clock): $(date)"; echo
  uname -a; echo
  lscpu | grep -E 'Model name|Socket\(s\)|Core\(s\)|Thread\(s\)'; echo
  fio --version; echo
  echo "SERVER=$SERVER"; echo "BASE_OPTS=$BASE_OPTS"; echo "ZC_OPTS=$ZC_OPTS"
  echo "SZ=$SZ BS=$BS NUMJOBS=$NUMJOBS RUNTIME=$RUNTIME RAMP=$RAMP ITERS=$ITERS"; echo
  df -h "$SRC_NVME" "$SRC_TMPFS"
} > "$RESULTS/env.txt" 2>&1
cat "$RESULTS/env.txt"

SERVER_PID=""
cleanup() {
	pkill -9 fio 2>/dev/null
	if [ -n "$SERVER_PID" ]; then
		fusermount3 -u "$MNT" 2>/dev/null || umount -l "$MNT" 2>/dev/null
		kill "$SERVER_PID" 2>/dev/null
	fi
}
trap cleanup EXIT INT TERM

start_server() {            # start_server <baseline|zc> <srcdir>
	local cfg=$1 src=$2 opts
	case "$cfg" in
		baseline) opts="$BASE_OPTS" ;;
		regbuf)   opts="$REGBUF_OPTS" ;;
		zc)       opts="$ZC_OPTS" ;;
		*) die "unknown config: $cfg" ;;
	esac
	# never stack a mount on top of a stale one
	mountpoint -q "$MNT" && die "$MNT already mounted before $cfg; clean up stale mounts/servers first"
	echo ">>> start server: cfg=$cfg src=$src opts=$opts"
	# shellcheck disable=SC2086
	"$SERVER" "$src" "$MNT" --nopassthrough $opts \
		2>"$RESULTS/server_${cfg}.log" &
	SERVER_PID=$!
	sleep 2
	mountpoint -q "$MNT" \
		|| die "mount failed for cfg=$cfg (see $RESULTS/server_${cfg}.log)"
}

stop_server() {
	fusermount3 -u "$MNT" 2>/dev/null || umount -l "$MNT" 2>/dev/null
	if [ -n "$SERVER_PID" ]; then
		kill "$SERVER_PID" 2>/dev/null
		for _ in 1 2 3 4 5; do kill -0 "$SERVER_PID" 2>/dev/null || break; sleep 1; done
		kill -9 "$SERVER_PID" 2>/dev/null
		wait "$SERVER_PID" 2>/dev/null
		SERVER_PID=""
	fi
	# pop anything left so the next config starts clean (no stacking)
	while mountpoint -q "$MNT"; do umount -l "$MNT" 2>/dev/null || break; done
	sleep 1
}

fio_one() {                 # fio_one <tag> <rw> <directflag> [extra...]
	local tag=$1 rw=$2 dflag=$3; shift 3
	local i
	for i in $(seq 1 "$ITERS"); do
		echo "    fio $tag run$i"
		fio --name="$tag" --filename="$MNT/testfile" \
			--ioengine=sync --bs="$BS" --size="$SZ" \
			--numjobs="$NUMJOBS" --group_reporting=1 \
			--time_based --runtime="$RUNTIME" --ramp_time="$RAMP" \
			--rw="$rw" $dflag "$@" \
			--output-format=json \
			--output="$RESULTS/$tag.run$i.json" >/dev/null \
			|| echo "      (fio $tag run$i returned nonzero)"
	done
}

# Pre-create the read test file directly in the NVMe backing (no fuse, fast,
# with progress). passthrough mirrors the source, so $MNT/testfile == this.
if [ ! -f "$SRC_NVME/testfile" ]; then
	echo "creating $SRC_NVME/testfile ($SZ) on NVMe ..."
	count=$(( $(numfmt --from=iec "$SZ") / 1048576 ))   # bytes -> 1M blocks
	dd if=/dev/zero of="$SRC_NVME/testfile" bs=1M count="$count" \
		oflag=direct status=progress || die "failed to create read test file"
fi

echo "NOTE: each fio run takes ~$((RUNTIME+RAMP))s of silence after its line; total ~1h."

for cfg in $CONFIGS; do
	# ---- reads: warm backing cache, so every config measures the same
	#      cache-served regime (this is the data-path / copy-cost measure) ----
	start_server "$cfg" "$SRC_NVME"
	echo "    pre-warming backing cache for $cfg"
	dd if="$MNT/testfile" of=/dev/null bs=1M iflag=direct status=none 2>/dev/null || true
	fio_one "${cfg}_dread_rand" randread --direct=1
	fio_one "${cfg}_dread_seq"  read     --direct=1
	fio_one "${cfg}_bread_rand" randread ""
	fio_one "${cfg}_bread_seq"  read     ""
	stop_server

	# ---- writes: tmpfs backing (device not the bottleneck) ----
	start_server "$cfg" "$SRC_TMPFS"
	fio_one "${cfg}_dwrite_rand" randwrite --direct=1
	fio_one "${cfg}_dwrite_seq"  write     --direct=1
	fio_one "${cfg}_bwrite_rand" randwrite "" --end_fsync=1
	fio_one "${cfg}_bwrite_seq"  write     "" --end_fsync=1
	stop_server
done

echo
echo "=== mean bandwidth (KiB/s) over $ITERS runs ==="
for tag in $(ls "$RESULTS"/*.run1.json 2>/dev/null | sed 's#.*/##;s/\.run1\.json//'); do
	bw=$(for i in $(seq 1 "$ITERS"); do
		[ -f "$RESULTS/$tag.run$i.json" ] && \
			jq '(.jobs[0].read.bw // 0) + (.jobs[0].write.bw // 0)' \
				"$RESULTS/$tag.run$i.json"
	     done | awk '{s+=$1;n++} END{if(n)printf "%.0f",s/n; else print "NA"}')
	printf "%-24s %s\n" "$tag" "$bw"
done | sort | tee "$RESULTS/summary.txt"

WORKLOADS="dread_rand dread_seq bread_rand bread_seq dwrite_rand dwrite_seq bwrite_rand bwrite_seq"

delta() {                   # delta <label> <from_cfg> <to_cfg>
	local label=$1 from=$2 to=$3 wl a b
	echo
	echo "=== $label (per workload) ==="
	for wl in $WORKLOADS; do
		a=$(grep -E "^${from}_${wl} " "$RESULTS/summary.txt" | awk '{print $2}')
		b=$(grep -E "^${to}_${wl} "   "$RESULTS/summary.txt" | awk '{print $2}')
		if [[ "$a" =~ ^[0-9]+$ && "$b" =~ ^[0-9]+$ && "$a" -gt 0 ]]; then
			awk -v a="$a" -v b="$b" -v w="$wl" -v f="$from" -v t="$to" \
			  'BEGIN{printf "%-14s %s=%.0f  %s=%.0f MiB/s  (%+.1f%%)\n",
			         w, f, a/1024, t, b/1024, (b-a)*100.0/a}'
		else
			printf "%-14s (missing data)\n" "$wl"
		fi
	done
}

{
	delta "registered-buffers vs baseline" baseline regbuf
	delta "zero-copy vs baseline"          baseline zc
	delta "zero-copy vs registered-buffers" regbuf  zc
} | tee -a "$RESULTS/summary.txt"

echo
echo "Results in: $RESULTS"
echo "Server logs: $RESULTS/server_baseline.log, server_zc.log"
echo "Done."
