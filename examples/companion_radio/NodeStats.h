#pragma once
#include <cstdint>

// A snapshot of live device/radio counters for a "Node info" screen. Mirrors the
// values the BLE companion fetches via CMD_GET_STATS. Filled by MyMesh::getNodeStats()
// (backend), which has access to the dispatcher / radio / packet-manager internals.
// POD so it can be copied across the dual-core boundary like NodePrefs.
struct NodeStats {
  uint32_t uptime_secs;
  uint16_t err_flags;     // ERR_EVENT_* bitmask
  uint8_t  queue_len;     // outbound packet queue depth
  int16_t  noise_floor;
  int8_t   last_rssi;     // dBm of the last packet
  int8_t   last_snr_q4;   // SNR x4 (0.25 dB units)
  uint32_t tx_air_secs;
  uint32_t rx_air_secs;
  uint32_t pkts_recv;     // raw PHY counters
  uint32_t pkts_sent;
  uint32_t recv_errors;
  uint32_t sent_flood;    // mesh-level counters
  uint32_t sent_direct;
  uint32_t recv_flood;
  uint32_t recv_direct;
};
