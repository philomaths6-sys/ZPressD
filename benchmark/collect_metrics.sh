#!/bin/bash
# collect_metrics.sh — samples all metrics ZPressD directly affects every 2s
# Columns:
#   timestamp          - unix epoch
#   mem_avail_kb       - MemAvailable (pressure signal ZPressD reads)
#   psi_some_avg10     - PSI memory some.avg10 (pressure signal ZPressD reads)
#   psi_full_avg10     - PSI memory full.avg10 (stall time -- key improvement metric)
#   pswpout_rate       - pages/s written to disk swap (ZPressD should reduce this)
#   zswap_stored_pages - pages currently held compressed in zswap (ZPressD increases this)
#   zswap_writeback    - total pages written FROM zswap TO disk swap (lower = ZPressD winning)
#   zram_ratio         - zram compression ratio if applicable

OUTFILE=${1:-metrics.csv}
echo 'timestamp,mem_avail_kb,psi_some_avg10,psi_full_avg10,pswpout_rate,zswap_stored_pages,zswap_writeback,zram_ratio' > "$OUTFILE"

prev_pswpout=$(awk '/^pswpout/ {print $2}' /proc/vmstat)
prev_pswpout=${prev_pswpout:-0}
prev_ts=$(date +%s)

while true; do
    TS=$(date +%s)

    # Memory availability (what ZPressD monitors to decide pressure level)
    MEM_AVAIL=$(awk '/MemAvailable/ {print $2}' /proc/meminfo)
    MEM_AVAIL=${MEM_AVAIL:-0}

    # PSI memory stalls (the core signal ZPressD acts on)
    PSI_SOME=$(awk '/^some/ {split($2,a,"="); print a[2]}' /proc/pressure/memory 2>/dev/null)
    PSI_SOME=${PSI_SOME:-0.00}
    PSI_FULL=$(awk '/^full/ {split($2,a,"="); print a[2]}' /proc/pressure/memory 2>/dev/null)
    PSI_FULL=${PSI_FULL:-0.00}

    # Disk swap-out rate (pages/s written to real swap disk)
    # ZPressD's MADV_PAGEOUT compresses into zswap first, reducing this
    PSWPOUT_CUR=$(awk '/^pswpout/ {print $2}' /proc/vmstat)
    PSWPOUT_CUR=${PSWPOUT_CUR:-0}

    # zswap stats — THIS is what ZPressD moves pages into
    # stored_pages: how many pages ZPressD pushed into zswap compression pool
    # writeback:    how many zswap compressed pages spilled to disk (ZPressD keeps this low)
    ZSWAP_STORED=0
    ZSWAP_WRITEBACK=0
    if [ -d /sys/kernel/debug/zswap ]; then
        ZSWAP_STORED=$(cat /sys/kernel/debug/zswap/stored_pages 2>/dev/null || echo 0)
        ZSWAP_WRITEBACK=$(cat /sys/kernel/debug/zswap/written_back_pages 2>/dev/null || echo 0)
    fi

    # zram ratio (for VMs using zram as swap backend)
    RATIO=0
    if [ -f /sys/block/zram0/mm_stat ]; then
        ZRAM_LINE=$(cat /sys/block/zram0/mm_stat 2>/dev/null || echo "0 0")
        ORIG=$(echo "$ZRAM_LINE" | awk '{print $1}')
        COMPR=$(echo "$ZRAM_LINE" | awk '{print $2}')
        if [ "${COMPR:-0}" -gt 0 ] 2>/dev/null; then
            RATIO=$(awk "BEGIN {printf \"%.2f\", ${ORIG:-0} / ${COMPR}}")
        fi
    fi

    DT=$((TS - prev_ts))
    if [ "$DT" -gt 0 ]; then
        PSWPOUT_RATE=$(( (PSWPOUT_CUR - prev_pswpout) / DT ))
    else
        PSWPOUT_RATE=0
    fi

    echo "$TS,$MEM_AVAIL,$PSI_SOME,$PSI_FULL,$PSWPOUT_RATE,$ZSWAP_STORED,$ZSWAP_WRITEBACK,$RATIO" >> "$OUTFILE"

    prev_pswpout=$PSWPOUT_CUR
    prev_ts=$TS
    sleep 2
done