#pragma once

// Minimal, allocation-light parser for the capture format written by
// scripts/download_binance.py. This is deliberately NOT a general JSON
// parser: the input is machine-generated with a fixed schema (no escapes,
// no nesting surprises), so scanning for known keys is both faster and
// smaller than pulling in a JSON dependency.

#include <charconv>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "nanolob/types.hpp"

namespace nanolob::replay {

// Fixed-point scaling at the feed boundary. BTCUSDT spot: tick 0.01 USDT,
// lot step 1e-5 BTC.
inline constexpr int kPriceDecimals = 2;
inline constexpr int kQtyDecimals = 5;

struct LevelUpdate {
  Price price = 0;
  Qty qty = 0;  // absolute quantity after the update; 0 removes the level
};

struct SnapshotRec {
  std::uint64_t last_update_id = 0;
  std::int64_t ts = 0;
  std::vector<LevelUpdate> bids;
  std::vector<LevelUpdate> asks;
};

struct DiffRec {
  std::uint64_t first_id = 0;  // "U"
  std::uint64_t last_id = 0;   // "u"
  std::int64_t ts = 0;
  std::vector<LevelUpdate> bids;
  std::vector<LevelUpdate> asks;
};

struct TradeRec {
  std::int64_t ts = 0;  // exchange trade time "T"
  Price price = 0;
  Qty qty = 0;
  bool buyer_is_maker = false;  // "m": true => sell aggressor
};

struct Record {
  enum class Kind { Snapshot, Diff, Trade } kind = Kind::Diff;
  SnapshotRec snapshot;
  DiffRec diff;
  TradeRec trade;
};

// "117234.56000000" with decimals=2 -> 11723456. Truncates extra fractional
// digits; missing ones are treated as zeros.
inline std::optional<std::int64_t> parse_fixed(std::string_view s, int decimals) {
  const std::size_t dot = s.find('.');
  const std::string_view int_part = s.substr(0, dot);
  std::int64_t value = 0;
  auto [p, ec] = std::from_chars(int_part.data(), int_part.data() + int_part.size(), value);
  if (ec != std::errc{} || p != int_part.data() + int_part.size() || value < 0) {
    return std::nullopt;
  }
  for (int i = 0; i < decimals; ++i) {
    const std::size_t idx = dot == std::string_view::npos ? std::string_view::npos
                                                          : dot + 1 + static_cast<std::size_t>(i);
    const char c = idx < s.size() ? s[idx] : '0';
    if (c < '0' || c > '9') return std::nullopt;
    value = value * 10 + (c - '0');
  }
  return value;
}

namespace detail {

inline std::optional<std::uint64_t> find_u64(std::string_view line, std::string_view key) {
  const std::size_t at = line.find(key);
  if (at == std::string_view::npos) return std::nullopt;
  std::size_t i = at + key.size();
  std::uint64_t value = 0;
  auto [p, ec] = std::from_chars(line.data() + i, line.data() + line.size(), value);
  if (ec != std::errc{}) return std::nullopt;
  return value;
}

inline std::optional<std::string_view> find_string(std::string_view line, std::string_view key) {
  const std::size_t at = line.find(key);
  if (at == std::string_view::npos) return std::nullopt;
  const std::size_t start = at + key.size();
  const std::size_t end = line.find('"', start);
  if (end == std::string_view::npos) return std::nullopt;
  return line.substr(start, end - start);
}

// Parses `[["price","qty"],...]` starting at the '[' following `key`.
inline bool parse_level_array(std::string_view line, std::string_view key,
                              std::vector<LevelUpdate>& out) {
  const std::size_t at = line.find(key);
  if (at == std::string_view::npos) return false;
  std::size_t i = at + key.size();  // at '['
  if (i >= line.size() || line[i] != '[') return false;
  ++i;
  while (i < line.size() && line[i] == '[') {
    // ["price","qty"]
    const std::size_t p_start = i + 2;
    const std::size_t p_end = line.find('"', p_start);
    if (p_end == std::string_view::npos) return false;
    const std::size_t q_start = line.find('"', p_end + 1);
    if (q_start == std::string_view::npos) return false;
    const std::size_t q_end = line.find('"', q_start + 1);
    if (q_end == std::string_view::npos) return false;

    const auto px = parse_fixed(line.substr(p_start, p_end - p_start), kPriceDecimals);
    const auto qty = parse_fixed(line.substr(q_start + 1, q_end - q_start - 1), kQtyDecimals);
    if (!px || !qty) return false;
    out.push_back(LevelUpdate{*px, static_cast<Qty>(*qty)});

    i = q_end + 2;  // past `"]`
    if (i < line.size() && line[i] == ',') ++i;
  }
  return true;
}

}  // namespace detail

// Parses one capture line. Returns nullopt on lines that are malformed or of
// unknown type (the caller counts those; a handful in a multi-hour capture
// is normal — half-written last line on Ctrl-C, etc.).
inline std::optional<Record> parse_line(std::string_view line) {
  const auto type = detail::find_string(line, "\"type\":\"");
  if (!type) return std::nullopt;
  const auto ts = detail::find_u64(line, "\"ts\":");

  Record rec;
  if (*type == "snapshot") {
    rec.kind = Record::Kind::Snapshot;
    const auto luid = detail::find_u64(line, "\"lastUpdateId\":");
    if (!luid) return std::nullopt;
    rec.snapshot.last_update_id = *luid;
    rec.snapshot.ts = static_cast<std::int64_t>(ts.value_or(0));
    if (!detail::parse_level_array(line, "\"bids\":", rec.snapshot.bids)) return std::nullopt;
    if (!detail::parse_level_array(line, "\"asks\":", rec.snapshot.asks)) return std::nullopt;
    return rec;
  }
  if (*type == "diff") {
    rec.kind = Record::Kind::Diff;
    const auto first = detail::find_u64(line, "\"U\":");
    const auto last = detail::find_u64(line, "\"u\":");
    if (!first || !last) return std::nullopt;
    rec.diff.first_id = *first;
    rec.diff.last_id = *last;
    // Exchange event time "E", NOT the wrapper's local capture "ts": trades
    // are timestamped with exchange time, and the capture host's clock can be
    // seconds off (measured ~1.4s on the dev box). Mixing the two clocks
    // silently skews every fill-relative horizon downstream. One clock only.
    rec.diff.ts = static_cast<std::int64_t>(
        detail::find_u64(line, "\"E\":").value_or(ts.value_or(0)));
    if (!detail::parse_level_array(line, "\"b\":", rec.diff.bids)) return std::nullopt;
    if (!detail::parse_level_array(line, "\"a\":", rec.diff.asks)) return std::nullopt;
    return rec;
  }
  if (*type == "trade") {
    rec.kind = Record::Kind::Trade;
    const auto px = detail::find_string(line, "\"p\":\"");
    const auto qty = detail::find_string(line, "\"q\":\"");
    const auto trade_ts = detail::find_u64(line, "\"T\":");
    if (!px || !qty || !trade_ts) return std::nullopt;
    const auto px_fixed = parse_fixed(*px, kPriceDecimals);
    const auto qty_fixed = parse_fixed(*qty, kQtyDecimals);
    if (!px_fixed || !qty_fixed) return std::nullopt;
    rec.trade.price = *px_fixed;
    rec.trade.qty = static_cast<Qty>(*qty_fixed);
    rec.trade.ts = static_cast<std::int64_t>(*trade_ts);
    rec.trade.buyer_is_maker = line.find("\"m\":true") != std::string_view::npos;
    return rec;
  }
  return std::nullopt;
}

}  // namespace nanolob::replay
