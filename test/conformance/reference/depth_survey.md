# Published depth survey — BTCUSDT

Captured 2026-09-18 19:01:40Z. Regenerate with `python3 test/conformance/reference/measure_depth.py`.

## Per venue, at maximum available REST depth

| venue | side | levels | BTC | notional | span |
|---|---|---:|---:|---:|---:|
| binance | bids | 5000 | 214.19305000 | $17271318.08312050 | 106.3 bps |
| binance | asks | 5000 | 374.37040000 | $30400923.46888440 | 95.9 bps |
| okx | bids | 400 | 54.45535455 | $4403035.84133133 | 13.5 bps |
| okx | asks | 400 | 66.48595789 | $5385152.61275647 | 29.1 bps |
| bybit | bids | 200 | 26.49958900 | $2142455.71025570 | 16.8 bps |
| bybit | asks | 200 | 35.59700500 | $2883245.08107040 | 15.1 bps |

## Consolidated

| side | levels | BTC | notional | span |
|---|---:|---:|---:|---:|
| bids | 5478 | 295.14799355 | $23816809.63470753 | 106.3 bps |
| asks | 5524 | 476.45336289 | $38669321.16271127 | 96.1 bps |

## What this means for the requested bands

| band | bids | asks |
|---|---|---|
| 1M | fills | fills |
| 5M | fills | fills |
| 10M | fills | fills |
| 25M | **does not fill** | fills |
| 50M | **does not fill** | **does not fill** |

| offset | bids | asks |
|---|---|---|
| 50 bps | within published depth | within published depth |
| 100 bps | within published depth | **depth limited** |
| 200 bps | **depth limited** | **depth limited** |
| 500 bps | **depth limited** | **depth limited** |
| 1000 bps | **depth limited** | **depth limited** |

## Reading this

The consolidated book spans roughly 100 bps and holds tens of millions of dollars, not hundreds. So the specification's headline 50M band cannot be filled from what these venues publish, and the 200/500/1000 bps bands lie outside the published ladder entirely. Both are reported honestly rather than hidden: `fully_filled=false` and `depth_limited=true` are the correct answers, and the trailing open-ended band reports all available liquidity, which is the meaningful response to "50M+" when 50M exceeds the book.
