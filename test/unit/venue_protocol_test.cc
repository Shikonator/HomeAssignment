// Sequencing and resync behaviour for all three venues, driven by recorded
// frames. No sockets: every venue's hard part is pure logic and is tested as
// such.
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "src/venues/binance.h"
#include "src/venues/bybit.h"
#include "src/venues/okx.h"

namespace md {
namespace {

constexpr std::int64_t kRecv = 1'000'000'000;

std::string BinanceDepth(std::int64_t first_id, std::int64_t final_id) {
  return R"({"e":"depthUpdate","E":1700000000000,"s":"BTCUSDT","U":)" +
         std::to_string(first_id) + R"(,"u":)" + std::to_string(final_id) +
         R"(,"b":[["100.00","1.5"]],"a":[["101.00","2.0"]]})";
}

std::string BinanceSnapshot(std::int64_t last_update_id) {
  return R"({"lastUpdateId":)" + std::to_string(last_update_id) +
         R"(,"bids":[["100.00","1.0"],["99.00","2.0"]],"asks":[["101.00","1.0"]]})";
}

// The classic Binance failure is fetching the snapshot before you start
// listening. Events that arrive during the REST round trip must be buffered and
// replayed, or the book begins life already wrong.
TEST(Binance, BuffersEventsArrivingDuringTheSnapshotRoundTrip) {
  BinanceProtocol venue{BinanceProtocol::Config{}};
  std::vector<FeedUpdate> updates;

  // Events land while the REST request is still outstanding.
  EXPECT_EQ(venue.OnFrame(BinanceDepth(90, 95), kRecv, &updates), FrameVerdict::kOk);
  EXPECT_EQ(venue.OnFrame(BinanceDepth(96, 101), kRecv, &updates), FrameVerdict::kOk);
  EXPECT_EQ(venue.OnFrame(BinanceDepth(102, 110), kRecv, &updates), FrameVerdict::kOk);
  EXPECT_TRUE(updates.empty());
  EXPECT_FALSE(venue.synced());

  // The snapshot is older than the last two events but newer than the first.
  ASSERT_EQ(venue.OnRestSnapshot(BinanceSnapshot(100), kRecv, &updates), FrameVerdict::kOk);
  EXPECT_TRUE(venue.synced());

  // Snapshot, then only the events not already reflected in it.
  ASSERT_EQ(updates.size(), 3u);
  EXPECT_TRUE(updates[0].is_snapshot);
  EXPECT_FALSE(updates[1].is_snapshot);
  EXPECT_FALSE(updates[2].is_snapshot);

  // The stream continues from the last replayed event.
  updates.clear();
  EXPECT_EQ(venue.OnFrame(BinanceDepth(111, 115), kRecv, &updates), FrameVerdict::kOk);
  EXPECT_EQ(updates.size(), 1u);
}

TEST(Binance, RejectsSnapshotWithAHoleBeforeTheStream) {
  BinanceProtocol venue{BinanceProtocol::Config{}};
  std::vector<FeedUpdate> updates;
  // Buffered stream starts at 200, but the snapshot only reaches 100: events
  // 101..199 were never seen by anyone.
  EXPECT_EQ(venue.OnFrame(BinanceDepth(200, 210), kRecv, &updates), FrameVerdict::kOk);
  EXPECT_EQ(venue.OnRestSnapshot(BinanceSnapshot(100), kRecv, &updates),
            FrameVerdict::kNeedsResync);
}

TEST(Binance, SequenceGapForcesResync) {
  BinanceProtocol venue{BinanceProtocol::Config{}};
  std::vector<FeedUpdate> updates;
  ASSERT_EQ(venue.OnFrame(BinanceDepth(101, 110), kRecv, &updates), FrameVerdict::kOk);
  ASSERT_EQ(venue.OnRestSnapshot(BinanceSnapshot(100), kRecv, &updates), FrameVerdict::kOk);
  updates.clear();
  // 111 is expected; 130 means events were lost.
  EXPECT_EQ(venue.OnFrame(BinanceDepth(130, 140), kRecv, &updates), FrameVerdict::kNeedsResync);
}

TEST(Binance, IgnoresSubscriptionAcknowledgements) {
  BinanceProtocol venue{BinanceProtocol::Config{}};
  std::vector<FeedUpdate> updates;
  EXPECT_EQ(venue.OnFrame(R"({"result":null,"id":1})", kRecv, &updates), FrameVerdict::kOk);
  EXPECT_TRUE(updates.empty());
}

TEST(Binance, ParsesLevelsAsExactFixedPoint) {
  BinanceProtocol venue{BinanceProtocol::Config{}};
  std::vector<FeedUpdate> updates;
  ASSERT_EQ(venue.OnRestSnapshot(BinanceSnapshot(10), kRecv, &updates), FrameVerdict::kOk);
  ASSERT_EQ(updates.size(), 1u);
  ASSERT_EQ(updates[0].bids.size(), 2u);
  EXPECT_EQ(updates[0].bids[0].px, 100 * kScale);
  EXPECT_EQ(updates[0].bids[0].qty, 1 * kScale);
  EXPECT_EQ(updates[0].asks[0].px, 101 * kScale);
}

std::string OkxFrame(const char* action, std::int64_t prev_seq, std::int64_t seq) {
  return std::string(R"({"arg":{"channel":"books","instId":"BTC-USDT"},"action":")") + action +
         R"(","data":[{"bids":[["100.0","1.5","0","2"]],"asks":[["101.0","2.0","0","1"]],)" +
         R"("ts":"1700000000000","checksum":-1,"prevSeqId":)" + std::to_string(prev_seq) +
         R"(,"seqId":)" + std::to_string(seq) + "}]}";
}

TEST(Okx, SnapshotThenContinuousUpdates) {
  OkxProtocol venue{OkxProtocol::Config{}};
  std::vector<FeedUpdate> updates;
  ASSERT_EQ(venue.OnFrame(OkxFrame("snapshot", -1, 10), kRecv, &updates), FrameVerdict::kOk);
  EXPECT_TRUE(venue.synced());
  ASSERT_EQ(updates.size(), 1u);
  EXPECT_TRUE(updates[0].is_snapshot);
  // OKX level entries carry four fields; only price and size are used.
  EXPECT_EQ(updates[0].bids[0].px, 100 * kScale);
  EXPECT_EQ(updates[0].bids[0].qty, 150000000);

  updates.clear();
  EXPECT_EQ(venue.OnFrame(OkxFrame("update", 10, 11), kRecv, &updates), FrameVerdict::kOk);
  ASSERT_EQ(updates.size(), 1u);
  EXPECT_FALSE(updates[0].is_snapshot);
}

TEST(Okx, DiscontinuousPrevSeqIdForcesResync) {
  OkxProtocol venue{OkxProtocol::Config{}};
  std::vector<FeedUpdate> updates;
  ASSERT_EQ(venue.OnFrame(OkxFrame("snapshot", -1, 10), kRecv, &updates), FrameVerdict::kOk);
  EXPECT_EQ(venue.OnFrame(OkxFrame("update", 12, 13), kRecv, &updates),
            FrameVerdict::kNeedsResync);
}

TEST(Okx, RepeatedSequenceMeansNoChange) {
  OkxProtocol venue{OkxProtocol::Config{}};
  std::vector<FeedUpdate> updates;
  ASSERT_EQ(venue.OnFrame(OkxFrame("snapshot", -1, 10), kRecv, &updates), FrameVerdict::kOk);
  updates.clear();
  // prevSeqId == seqId is OKX's way of saying nothing moved.
  EXPECT_EQ(venue.OnFrame(OkxFrame("update", 10, 10), kRecv, &updates), FrameVerdict::kOk);
  EXPECT_TRUE(updates.empty());
}

// REGRESSION. The no-change shortcut must not run before the continuity check.
//
// OKX repeats the sequence number when nothing moved. If that shortcut were
// evaluated first, this exact sequence -- sync at 10, lose everything up to
// 150, then receive a no-change frame at 150 -- would skip validation entirely
// and the book would diverge permanently with no error and no resync. The OKX
// checksum would normally be the backstop for this class of failure and is
// deliberately not implemented, so ordering here is the only defence.
TEST(Okx, NoChangeHeartbeatDoesNotBypassGapDetection) {
  OkxProtocol venue{OkxProtocol::Config{}};
  std::vector<FeedUpdate> updates;
  ASSERT_EQ(venue.OnFrame(OkxFrame("snapshot", -1, 10), kRecv, &updates), FrameVerdict::kOk);
  updates.clear();

  // Everything between 10 and 150 was lost; this frame says "nothing changed".
  EXPECT_EQ(venue.OnFrame(OkxFrame("update", 150, 150), kRecv, &updates),
            FrameVerdict::kNeedsResync);
  EXPECT_TRUE(updates.empty());
}

// A genuine no-change frame in sequence is still a no-op, not a resync.
TEST(Okx, InSequenceNoChangeHeartbeatIsAccepted) {
  OkxProtocol venue{OkxProtocol::Config{}};
  std::vector<FeedUpdate> updates;
  ASSERT_EQ(venue.OnFrame(OkxFrame("snapshot", -1, 10), kRecv, &updates), FrameVerdict::kOk);
  updates.clear();
  EXPECT_EQ(venue.OnFrame(OkxFrame("update", 10, 10), kRecv, &updates), FrameVerdict::kOk);
  EXPECT_TRUE(updates.empty());
  // And the stream continues normally afterwards.
  EXPECT_EQ(venue.OnFrame(OkxFrame("update", 10, 11), kRecv, &updates), FrameVerdict::kOk);
  EXPECT_EQ(updates.size(), 1u);
}

// A rejected subscription is permanent. Reconnecting cannot fix an unknown
// symbol, so it must not be fed into the resync loop.
TEST(Okx, SubscribeErrorIsFatalNotTransient) {
  OkxProtocol venue{OkxProtocol::Config{}};
  std::vector<FeedUpdate> updates;
  EXPECT_EQ(venue.OnFrame(R"({"event":"error","code":"60012","msg":"Invalid instId"})", kRecv,
                          &updates),
            FrameVerdict::kFatal);
  EXPECT_EQ(venue.fatal_reason(), "Invalid instId");
}

TEST(Okx, IgnoresFramesForAnotherInstrument) {
  OkxProtocol venue{OkxProtocol::Config{}};
  std::vector<FeedUpdate> updates;
  const std::string other =
      R"({"arg":{"channel":"books","instId":"ETH-USDT"},"action":"snapshot","data":[{)"
      R"("bids":[["1.0","1.0","0","1"]],"asks":[],"ts":"1700000000000","checksum":-1,)"
      R"("prevSeqId":-1,"seqId":1}]})";
  EXPECT_EQ(venue.OnFrame(other, kRecv, &updates), FrameVerdict::kOk);
  EXPECT_TRUE(updates.empty());
  EXPECT_FALSE(venue.synced());
}

TEST(Bybit, SubscribeFailureIsFatalNotTransient) {
  BybitProtocol venue{BybitProtocol::Config{}};
  std::vector<FeedUpdate> updates;
  EXPECT_EQ(venue.OnFrame(
                R"({"success":false,"ret_msg":"Invalid symbol","op":"subscribe","conn_id":"x"})",
                kRecv, &updates),
            FrameVerdict::kFatal);
  EXPECT_EQ(venue.fatal_reason(), "Invalid symbol");
}

// On any verdict other than kOk the caller must discard `out`. The adapters
// enforce that structurally by staging, so a failure partway through a
// multi-entry frame appends nothing at all.
TEST(Okx, FailedFrameAppendsNothing) {
  OkxProtocol venue{OkxProtocol::Config{}};
  std::vector<FeedUpdate> updates;
  ASSERT_EQ(venue.OnFrame(OkxFrame("snapshot", -1, 10), kRecv, &updates), FrameVerdict::kOk);
  updates.clear();

  // Two entries: the first is valid and in sequence, the second has a broken
  // sequence. Neither may reach the caller.
  const std::string two_entries =
      R"({"arg":{"channel":"books","instId":"BTC-USDT"},"action":"update","data":[)"
      R"({"bids":[["100.0","1.0","0","1"]],"asks":[],"ts":"1700000000000","prevSeqId":10,"seqId":11},)"
      R"({"bids":[["101.0","1.0","0","1"]],"asks":[],"ts":"1700000000000","prevSeqId":99,"seqId":100}]})";
  EXPECT_EQ(venue.OnFrame(two_entries, kRecv, &updates), FrameVerdict::kNeedsResync);
  EXPECT_TRUE(updates.empty());
}

// A stateful failure must not persist. If the overflow left the buffer full,
// every subsequent frame would return the same verdict forever.
TEST(Binance, BufferOverflowClearsStateSoRecoveryIsPossible) {
  BinanceProtocol::Config config;
  config.max_buffered_events = 4;
  BinanceProtocol venue{config};
  std::vector<FeedUpdate> updates;

  for (int i = 0; i < 4; ++i) {
    ASSERT_EQ(venue.OnFrame(BinanceDepth(100 + i * 10, 109 + i * 10), kRecv, &updates),
              FrameVerdict::kOk);
  }
  EXPECT_EQ(venue.OnFrame(BinanceDepth(200, 210), kRecv, &updates), FrameVerdict::kNeedsResync);

  // After the failure the adapter is usable again without an explicit Reset().
  venue.Reset();
  ASSERT_EQ(venue.OnFrame(BinanceDepth(300, 310), kRecv, &updates), FrameVerdict::kOk);
  ASSERT_EQ(venue.OnRestSnapshot(BinanceSnapshot(305), kRecv, &updates), FrameVerdict::kOk);
  EXPECT_TRUE(venue.synced());
}

TEST(Binance, IgnoresFramesForAnotherSymbol) {
  BinanceProtocol venue{BinanceProtocol::Config{}};
  std::vector<FeedUpdate> updates;
  const std::string other =
      R"({"e":"depthUpdate","E":1700000000000,"s":"ETHUSDT","U":1,"u":2,)"
      R"("b":[["1.0","1.0"]],"a":[]})";
  EXPECT_EQ(venue.OnFrame(other, kRecv, &updates), FrameVerdict::kOk);
  EXPECT_TRUE(updates.empty());
}

TEST(Okx, UpdateBeforeSnapshotForcesResync) {
  OkxProtocol venue{OkxProtocol::Config{}};
  std::vector<FeedUpdate> updates;
  EXPECT_EQ(venue.OnFrame(OkxFrame("update", 10, 11), kRecv, &updates),
            FrameVerdict::kNeedsResync);
}

// The keepalive reply is a bare word, not JSON, and must not be a parse error.
TEST(Okx, PongIsNotAParseError) {
  OkxProtocol venue{OkxProtocol::Config{}};
  std::vector<FeedUpdate> updates;
  EXPECT_EQ(venue.OnFrame("pong", kRecv, &updates), FrameVerdict::kOk);
  EXPECT_EQ(venue.OnFrame(R"({"event":"subscribe","arg":{"channel":"books"}})", kRecv, &updates),
            FrameVerdict::kOk);
  EXPECT_TRUE(updates.empty());
}

std::string BybitFrame(const char* type, std::int64_t update_id) {
  return std::string(R"({"topic":"orderbook.200.BTCUSDT","type":")") + type +
         R"(","ts":1700000000000,"data":{"s":"BTCUSDT","b":[["100.00","1.5"]],)" +
         R"("a":[["101.00","2.0"]],"u":)" + std::to_string(update_id) + R"(,"seq":1}})";
}

TEST(Bybit, SnapshotThenContinuousDeltas) {
  BybitProtocol venue{BybitProtocol::Config{}};
  std::vector<FeedUpdate> updates;
  ASSERT_EQ(venue.OnFrame(BybitFrame("snapshot", 100), kRecv, &updates), FrameVerdict::kOk);
  EXPECT_TRUE(venue.synced());
  updates.clear();
  EXPECT_EQ(venue.OnFrame(BybitFrame("delta", 101), kRecv, &updates), FrameVerdict::kOk);
  ASSERT_EQ(updates.size(), 1u);
  EXPECT_FALSE(updates[0].is_snapshot);
}

TEST(Bybit, DeltaGapForcesResync) {
  BybitProtocol venue{BybitProtocol::Config{}};
  std::vector<FeedUpdate> updates;
  ASSERT_EQ(venue.OnFrame(BybitFrame("snapshot", 100), kRecv, &updates), FrameVerdict::kOk);
  EXPECT_EQ(venue.OnFrame(BybitFrame("delta", 105), kRecv, &updates), FrameVerdict::kNeedsResync);
}

// Bybit pushes a fresh snapshot mid-stream after its own internal reconnects.
// That is normal traffic, not an error, and must replace the book wholesale.
TEST(Bybit, MidStreamSnapshotResetsSequencing) {
  BybitProtocol venue{BybitProtocol::Config{}};
  std::vector<FeedUpdate> updates;
  ASSERT_EQ(venue.OnFrame(BybitFrame("snapshot", 100), kRecv, &updates), FrameVerdict::kOk);
  ASSERT_EQ(venue.OnFrame(BybitFrame("delta", 101), kRecv, &updates), FrameVerdict::kOk);
  updates.clear();

  // A snapshot with an unrelated, much lower update id: accepted, not a gap.
  ASSERT_EQ(venue.OnFrame(BybitFrame("snapshot", 1), kRecv, &updates), FrameVerdict::kOk);
  ASSERT_EQ(updates.size(), 1u);
  EXPECT_TRUE(updates[0].is_snapshot);
  // Sequencing now continues from the new snapshot.
  updates.clear();
  EXPECT_EQ(venue.OnFrame(BybitFrame("delta", 2), kRecv, &updates), FrameVerdict::kOk);
}

TEST(Bybit, IgnoresOperationAcknowledgements) {
  BybitProtocol venue{BybitProtocol::Config{}};
  std::vector<FeedUpdate> updates;
  EXPECT_EQ(venue.OnFrame(R"({"success":true,"op":"subscribe","conn_id":"x"})", kRecv, &updates),
            FrameVerdict::kOk);
  EXPECT_EQ(venue.OnFrame(R"({"op":"pong","args":["1700000000000"]})", kRecv, &updates),
            FrameVerdict::kOk);
  EXPECT_TRUE(updates.empty());
}

TEST(AllVenues, ResetClearsSyncState) {
  BinanceProtocol binance{BinanceProtocol::Config{}};
  OkxProtocol okx{OkxProtocol::Config{}};
  BybitProtocol bybit{BybitProtocol::Config{}};
  std::vector<FeedUpdate> updates;

  ASSERT_EQ(binance.OnRestSnapshot(BinanceSnapshot(10), kRecv, &updates), FrameVerdict::kOk);
  ASSERT_EQ(okx.OnFrame(OkxFrame("snapshot", -1, 10), kRecv, &updates), FrameVerdict::kOk);
  ASSERT_EQ(bybit.OnFrame(BybitFrame("snapshot", 10), kRecv, &updates), FrameVerdict::kOk);
  EXPECT_TRUE(binance.synced() && okx.synced() && bybit.synced());

  binance.Reset();
  okx.Reset();
  bybit.Reset();
  EXPECT_FALSE(binance.synced());
  EXPECT_FALSE(okx.synced());
  EXPECT_FALSE(bybit.synced());
}

}  // namespace
}  // namespace md
