#!/bin/bash
# Scenario C: Heavy — 4 VMs + 4 forks simultaneously
# This should push PSI full.avg10 above 5%

echo "[Workload: Heavy] Starting..."
exec stress-ng --vm 4 --vm-bytes 20% --vm-keep \
          --fork 4 \
          --timeout 0 \
          --metrics-brief