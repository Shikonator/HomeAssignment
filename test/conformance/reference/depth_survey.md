# Published depth survey — BTCUSDT

Captured 2026-09-17 20:50:07Z. Regenerate with `python3 test/conformance/reference/measure_depth.py`.

## Per venue, at maximum available REST depth

| venue | side | levels | BTC | notional | span |
|---|---|---:|---:|---:|---:|
| binance | bids | 5000 | 350.97046000 | $26756219.11086400 | 101.6 bps |
| binance | asks | 5000 | 291.19912000 | $22367284.89869740 | 118.7 bps |
| okx | bids | 400 | 60.07557038 | $4594063.43372828 | 11.8 bps |
| okx | asks | 400 | 85.88111295 | $6580179.41901423 | 30.2 bps |
| bybit | bids | 200 | 36.90191500 | $2821635.13727790 | 16.3 bps |
| bybit | asks | 200 | 53.01059600 | $4060081.73426730 | 19.4 bps |

## Consolidated

| side | levels | BTC | notional | span |
|---|---:|---:|---:|---:|
| bids | 5452 | 447.94794538 | $34171917.68187018 | 101.6 bps |
| asks | 5517 | 430.09082895 | $33007546.05197893 | 119.1 bps |

## What this means for the requested bands

| band | bids | asks |
|---|---|---|
| 1M | fills | fills |
| 5M | fills | fills |
| 10M | fills | fills |
| 25M | fills | fills |
| 50M | **does not fill** | **does not fill** |

| offset | bids | asks |
|---|---|---|
| 50 bps | within published depth | within published depth |
| 100 bps | within published depth | within published depth |
| 200 bps | **depth limited** | **depth limited** |
| 500 bps | **depth limited** | **depth limited** |
| 1000 bps | **depth limited** | **depth limited** |

## Reading this

The consolidated book spans roughly 100 bps and holds tens of millions of dollars, not hundreds. So the specification's headline 50M band cannot be filled from what these venues publish, and the 200/500/1000 bps bands lie outside the published ladder entirely. Both are reported honestly rather than hidden: `fully_filled=false` and `depth_limited=true` are the correct answers, and the trailing open-ended band reports all available liquidity, which is the meaningful response to "50M+" when 50M exceeds the book.
