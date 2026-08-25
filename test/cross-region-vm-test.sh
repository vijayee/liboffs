#!/bin/bash
set -eo pipefail

# Cross-region OFFS network RPC test using Azure VMs
#
# VMs expose BOTH TCP 23402 (HTTP) and UDP 23401 (QUIC) — ACI can only do one.
# This allows direct QUIC connections between test nodes across regions.

ACR_NAME=$(cat /tmp/offs-acr-name.txt)
RELAY_IP="20.163.130.127"
RELAY_PORT="14000"
BOOTSTRAP_IP="172.178.8.253"
IMAGE="${ACR_NAME}.azurecr.io/offs-offsd:latest"
ACR_USER="${ACR_NAME}"
ACR_PW=$(az acr credential show -n "${ACR_NAME}" --query 'passwords[0].value' -o tsv 2>/dev/null)
SSH_KEY=$(cut -d' ' -f1-2 /home/victor/.ssh/id_rsa.pub)

REGIONS=("eastus" "brazilsouth" "eastasia" "centralus")
NODE_NAMES=()
NODE_IPS=()
NODE_KEYS=()
NODE_RG="${NODE_RG:-offs-vm-test-$$}"
PASS=0; FAIL=0; TOTAL=0

log() { echo "[$(date +%H:%M:%S)] $*"; }
pass() { log "  ✅ PASS: $1"; PASS=$((PASS+1)); TOTAL=$((TOTAL+1)); }
fail() { log "  ❌ FAIL: $1"; FAIL=$((FAIL+1)); TOTAL=$((TOTAL+1)); }

BOOTSTRAP_KEY="${BOOTSTRAP_KEY:-}"
if [ -z "${BOOTSTRAP_KEY}" ]; then
  BOOTSTRAP_KEY=$(az vm run-command invoke --resource-group offs-relay-rg2 --name offs-bootstrap-vm \
    --command-id RunShellScript --scripts "docker logs offs-offsd 2>&1 | grep 'Generated API key' | tail -1" 2>/dev/null | \
    python3 -c "import json,sys; d=json.load(sys.stdin); m=d['value'][0]['message']; lines=[l for l in m.split(chr(10)) if 'Generated' in l]; print(lines[0].split(': ')[-1].strip() if lines else '')" 2>/dev/null)
fi
log "  Bootstrap API key: ${BOOTSTRAP_KEY:0:16}..."

BOOTSTRAP_PEER="${BOOTSTRAP_PEER:-}"
if [ -z "${BOOTSTRAP_PEER}" ] && [ -n "${BOOTSTRAP_KEY}" ]; then
  BOOTSTRAP_PEER=$(curl -s -H "Authorization: Bearer ${BOOTSTRAP_KEY}" \
    "http://${BOOTSTRAP_IP}:23402/peer/info?format=base58" 2>/dev/null | tr -d '\n')
fi
log "  Bootstrap peer info: ${#BOOTSTRAP_PEER} chars"

# ─── 1. Create resource group ──────────────────────────────────────────────
log "=== Creating resource group ${NODE_RG} ==="
az group create --name "${NODE_RG}" --location eastus -o table 2>&1 | tail -2

# ─── 2. Deploy 8 VMs (2 per region) ────────────────────────────────────────
log "=== Deploying 8 offsd VMs across 4 regions ==="
NODE_IDX=0
for region in "${REGIONS[@]}"; do
  for replica in 1 2; do
    NODE_NAME="offs-vm-${region}-${replica}"
    NODE_NAMES+=("${NODE_NAME}")
    log "  Deploying ${NODE_NAME} in ${region}..."
    NODE_IP=$(az deployment group create \
      --resource-group "${NODE_RG}" \
      --name "deploy-${NODE_NAME}" \
      --template-file /tmp/vm-test-template.json \
      --parameters vmName="${NODE_NAME}" location="${region}" \
        sshKeyData="${SSH_KEY}" \
      --query 'properties.outputs.publicIp.value' -o tsv 2>&1 | tail -1)
    if [ -n "${NODE_IP}" ] && [ "${NODE_IP}" != "null" ]; then
      NODE_IPS+=("${NODE_IP}")
      log "  ${NODE_NAME}: ${NODE_IP} — installing Docker via run-command..."
      az vm run-command invoke --resource-group "${NODE_RG}" --name "${NODE_NAME}" \
        --command-id RunShellScript --scripts \
        "apt-get update -qq && apt-get install -y -qq docker.io >/dev/null 2>&1 && systemctl enable --now docker && mkdir -p /data && docker login -u ${ACR_USER} -p ${ACR_PW} ${ACR_NAME}.azurecr.io 2>&1 | tail -1 && docker pull ${ACR_NAME}.azurecr.io/offs-offsd:latest 2>&1 | tail -2 && docker run -d --name offs-offsd --network host --restart unless-stopped -v /data:/data -e RELAY_URL=${RELAY_IP}:${RELAY_PORT} ${ACR_NAME}.azurecr.io/offs-offsd:latest && echo DONE" \
        -o tsv 2>&1 | tail -3 || true
      log "  ${NODE_NAME}: Docker install complete"
    else
      log "  ⚠️  Deploy failed for ${NODE_NAME}, skipping"
      NODE_IPS+=("")
    fi
    NODE_IDX=$((NODE_IDX+1))
  done
done

# ─── 3. Wait for all nodes to start (cloud-init installs Docker + pulls image) ─
log "=== Waiting for all nodes to start (cloud-init ~2 min) ==="
for i in "${!NODE_NAMES[@]}"; do
  IP="${NODE_IPS[$i]:-}"
  if [ -z "${IP}" ]; then continue; fi
  for attempt in $(seq 1 60); do
    if curl -sf "http://${IP}:23402/health" -o /dev/null 2>/dev/null; then
      KEY=$(az vm run-command invoke --resource-group "${NODE_RG}" --name "${NODE_NAMES[$i]}" \
        --command-id RunShellScript --scripts "docker logs offs-offsd 2>&1 | grep 'Generated API key' | head -1 | sed 's/.*: //'" 2>/dev/null | \
        python3 -c "import json,sys; d=json.load(sys.stdin); m=d['value'][0]['message']; lines=[l for l in m.split(chr(10)) if 'Generated' in l]; print(lines[0].split(': ')[-1].strip() if lines else '')" 2>/dev/null)
      NODE_KEYS[$i]="${KEY}"
      log "  ${NODE_NAMES[$i]} (${IP}): ready, key=${KEY:0:16}..."
      break
    fi
    sleep 5
  done
done

NUM_READY=0
for k in "${NODE_KEYS[@]:-}"; do [ -n "$k" ] && NUM_READY=$((NUM_READY+1)); done
log "=== ${NUM_READY} nodes ready ==="
if [ "${NUM_READY}" -lt 2 ]; then log "❌ Not enough nodes ready (${NUM_READY}), aborting"; exit 1; fi

# ─── 4. Connect all nodes to bootstrap (staggered) ────────────────────────
log "=== Connecting all nodes to bootstrap (staggered 10s apart) ==="
if [ -n "${BOOTSTRAP_PEER}" ]; then
  for i in "${!NODE_NAMES[@]}"; do
    IP="${NODE_IPS[$i]:-}"; KEY="${NODE_KEYS[$i]:-}"
    if [ -z "${IP}" ] || [ -z "${KEY}" ]; then continue; fi
    [ $i -gt 0 ] && { log "  Waiting 10s..."; sleep 10; }
    curl -s --connect-timeout 10 --max-time 30 -X POST -H "Authorization: Bearer ${KEY}" -H "Content-Type: text/plain" -d "${BOOTSTRAP_PEER}" "http://${IP}:23402/peer/connect" > /dev/null 2>&1
    curl -s --connect-timeout 10 --max-time 30 -X POST -H "Authorization: Bearer ${KEY}" -H "Content-Type: text/plain" -d "${BOOTSTRAP_PEER}" "http://${IP}:23402/friends" > /dev/null 2>&1
    log "  ${NODE_NAMES[$i]}: connected + friend added"
  done
fi

# ─── 5. Test: Gossip discovery ─────────────────────────────────────────────
log "=== Test 1: Gossip discovery across regions ==="
log "  Waiting 120s for gossip..."
sleep 120
for i in "${!NODE_NAMES[@]}"; do
  IP="${NODE_IPS[$i]:-}"; KEY="${NODE_KEYS[$i]:-}"
  if [ -z "${IP}" ] || [ -z "${KEY}" ]; then continue; fi
  PEERS=$(curl -s -H "Authorization: Bearer ${KEY}" "http://${IP}:23402/peers" 2>/dev/null | python3 -c 'import json,sys; print(len(json.load(sys.stdin)))' 2>/dev/null || echo 0)
  if [ "${PEERS}" -ge 2 ]; then pass "${NODE_NAMES[$i]}: ${PEERS} peers via gossip"
  else fail "${NODE_NAMES[$i]}: only ${PEERS} peers"; fi
done

# ─── 6. Test: STORE + FIND_BLOCK cross-region ─────────────────────────────
log "=== Test 2: STORE_BLOCK + FIND_BLOCK cross-region ==="
NODE0_IP="${NODE_IPS[0]:-}"; NODE0_KEY="${NODE_KEYS[0]:-}"
STORE_RESP=$(curl -s --connect-timeout 10 --max-time 30 -X PUT -H "type: text/plain" -H "file-name: xregion.txt" -H "stream-length: 29" --data "cross-region block test data" "http://${NODE0_IP}:23402/offsystem" 2>/dev/null)
ORI=$(echo "${STORE_RESP}" | grep -o 'http://[^ ]*' | sed "s|localhost:23402|${NODE0_IP}:23402|")
if [ -n "${ORI}" ]; then
  pass "STORE_BLOCK: ${ORI}"
  DOWNLOAD=$(curl -s --connect-timeout 10 --max-time 30 "${ORI}" 2>/dev/null)
  if echo "${DOWNLOAD}" | grep -q "cross-region block test data"; then pass "FIND_BLOCK same-node"
  else fail "FIND_BLOCK same-node"; fi
  LAST_IP=""; for j in $(seq $((${#NODE_IPS[@]} - 1)) -1 0); do [ -n "${NODE_IPS[$j]:-}" ] && [ "${NODE_IPS[$j]:-}" != "${NODE0_IP}" ] && { LAST_IP="${NODE_IPS[$j]}"; break; }; done
  if [ -n "${LAST_IP}" ]; then
    CROSS_ORI=$(echo "${ORI}" | sed "s|${NODE0_IP}:23402|${LAST_IP}:23402|")
    DOWNLOAD2=$(curl -s --connect-timeout 10 --max-time 30 "${CROSS_ORI}" 2>/dev/null)
    if echo "${DOWNLOAD2}" | grep -q "cross-region block test data"; then pass "FIND_BLOCK cross-region from ${LAST_IP}"
    else fail "FIND_BLOCK cross-region from ${LAST_IP}"; fi
  else fail "FIND_BLOCK cross-region: no valid node"; fi
else fail "STORE_BLOCK: no ORI"; fi

# ─── 7. Test: PING ────────────────────────────────────────────────────────
log "=== Test 3: PING (health) ==="
for i in "${!NODE_NAMES[@]}"; do
  IP="${NODE_IPS[$i]:-}"; [ -z "${IP}" ] && continue
  curl -sf "http://${IP}:23402/health" -o /dev/null 2>/dev/null && pass "${NODE_NAMES[$i]}: PING OK" || fail "${NODE_NAMES[$i]}: PING fail"
done

# ─── 8. Test: PING_CAPACITY ───────────────────────────────────────────────
log "=== Test 4: PING_CAPACITY ==="
for i in "${!NODE_NAMES[@]}"; do
  IP="${NODE_IPS[$i]:-}"; KEY="${NODE_KEYS[$i]:-}"
  [ -z "${IP}" ] || [ -z "${KEY}" ] && continue
  PEERS=$(curl -s -H "Authorization: Bearer ${KEY}" "http://${IP}:23402/peers" 2>/dev/null | python3 -c 'import json,sys; peers=json.load(sys.stdin); print(sum(1 for p in peers if p["connected"]))' 2>/dev/null || echo 0)
  [ "${PEERS}" -ge 1 ] && pass "${NODE_NAMES[$i]}: ${PEERS} connected" || fail "${NODE_NAMES[$i]}: 0 connected"
done

# ─── 9. Test: Relay forwarding ────────────────────────────────────────────
log "=== Test 5: Relay forwarding ==="
RELAY_RAW=$(az vm run-command invoke --resource-group offs-relay-rg2 --name offs-relay-vm \
  --command-id RunShellScript --scripts "docker logs offs-relay 2>&1 | grep -c 'client connected'" 2>/dev/null | \
  python3 -c "import json,sys,re; d=json.load(sys.stdin); m=d['value'][0]['message']; n=re.findall(r'\d+',m); print(n[-1] if n else '0')" 2>/dev/null || echo "0")
[ "${RELAY_RAW}" -ge "${NUM_READY}" ] && pass "Relay: ${RELAY_RAW} connections" || fail "Relay: only ${RELAY_RAW} (expected ${NUM_READY}+)"

# ─── 10. Test: Peer reconnection after restart ────────────────────────────
log "=== Test 6: Peer reconnection after in-container restart ==="
RESTART_IP="${NODE_IPS[0]:-}"; RESTART_KEY="${NODE_KEYS[0]:-}"
PEERS_BEFORE=$(curl -s -H "Authorization: Bearer ${RESTART_KEY}" "http://${RESTART_IP}:23402/peers" 2>/dev/null | python3 -c 'import json,sys; print(len(json.load(sys.stdin)))' 2>/dev/null || echo 0)
log "  ${NODE_NAMES[0]}: ${PEERS_BEFORE} peers before restart"
az vm run-command invoke --resource-group "${NODE_RG}" --name "${NODE_NAMES[0]}" --command-id RunShellScript \
  --scripts "docker restart offs-offsd" 2>/dev/null > /dev/null || true
for attempt in $(seq 1 60); do
  curl -sf "http://${RESTART_IP}:23402/health" -o /dev/null 2>/dev/null && break
  sleep 2
done
log "  Waiting 90s for reconnection from persisted peer_store..."
sleep 90
NEW_KEY=$(az vm run-command invoke --resource-group "${NODE_RG}" --name "${NODE_NAMES[0]}" --command-id RunShellScript \
  --scripts "docker logs offs-offsd 2>&1 | grep 'Generated API key' | tail -1 | sed 's/.*: //'" 2>/dev/null | \
  python3 -c "import json,sys; d=json.load(sys.stdin); m=d['value'][0]['message']; lines=[l for l in m.split(chr(10)) if 'Generated' in l]; print(lines[0].split(': ')[-1].strip() if lines else '')" 2>/dev/null)
PEERS_AFTER=$(curl -s -H "Authorization: Bearer ${NEW_KEY}" "http://${RESTART_IP}:23402/peers" 2>/dev/null | python3 -c 'import json,sys; print(len(json.load(sys.stdin)))' 2>/dev/null || echo 0)
log "  ${NODE_NAMES[0]}: ${PEERS_AFTER} peers after restart"
[ "${PEERS_AFTER}" -ge 1 ] && pass "Peer reconnection: ${PEERS_AFTER} peers" || fail "Peer reconnection: 0 peers"

# ─── 11. Test: Bootstrap stability ────────────────────────────────────────
log "=== Test 7: Bootstrap stability ==="
BS_HEALTH=$(curl -s "http://${BOOTSTRAP_IP}:23402/health" 2>/dev/null)
echo "${BS_HEALTH}" | grep -q "running" && pass "Bootstrap: stable" || fail "Bootstrap: down"

# ─── Summary ─────────────────────────────────────────────────────────────
log ""; log "========================================"
log "  RESULTS: ${PASS} passed, ${FAIL} failed, ${TOTAL} total"
log "========================================"

# ─── Teardown ─────────────────────────────────────────────────────────────
log "=== Tearing down test VMs ==="
az group delete --name "${NODE_RG}" --yes --no-wait 2>/dev/null || true
log "  Resource group ${NODE_RG} deleting (async)"
log "=== Done ==="