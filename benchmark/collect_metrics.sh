#!/bin/bash
# collect_metrics.sh — samples PSI, vmstat, zram every 5 seconds
# Usage: ./collect_metrics.sh <output.csv>

OUTFILE=${1:-metrics.csv}
echo 'timestamp,mem_avail_kb,swap_out_rate,psi_some_avg10,psi_full_avg10,zram_ratio' > "$OUTFILE"

prev_pswpout=0
prev_ts=$(date +%s)

while true; do
    TS=$(date +%s)
    MEM_AVAIL=$(awk '/MemAvailable/ {print $2}' /proc/meminfo)
    MEM_AVAIL=${MEM_AVAIL:-0}
    SWAP_OUT_CUR=$(awk '/^pswpout/ {print $2}' /proc/vmstat)
    SWAP_OUT_CUR=${SWAP_OUT_CUR:-0}
    PSI_SOME=$(awk '/^some/ {split($2,a,"="); print a[2]}' /proc/pressure/memory)
    PSI_SOME=${PSI_SOME:-0}
    PSI_FULL=$(awk '/^full/ {split($2,a,"="); print a[2]}' /proc/pressure/memory)
    PSI_FULL=${PSI_FULL:-0}

    # zram compression ratio
    if [ -f /sys/block/zram0/mm_stat ]; then
        read -r ORIG COMPR REST < /sys/block/zram0/mm_stat
        if [ "$COMPR" -gt 0 ] 2>/dev/null; then
            RATIO=$(awk "BEGIN {printf \"%.2f\", $ORIG / $COMPR}")
        else
            RATIO=0
        fi
    else
        RATIO=0
    fi

    DT=$((TS - prev_ts))
    if [ $DT -gt 0 ]; then
        SWAP_RATE=$(( (SWAP_OUT_CUR - prev_pswpout) / DT ))
    else
        SWAP_RATE=0
    fi

    echo "$TS,$MEM_AVAIL,$SWAP_RATE,$PSI_SOME,$PSI_FULL,$RATIO" >> "$OUTFILE"

    prev_pswpout=$SWAP_OUT_CUR
    prev_ts=$TS
    sleep 5
done