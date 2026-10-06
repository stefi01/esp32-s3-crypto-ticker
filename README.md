# Desk Coin

A 4 inch touchscreen desk display for live crypto prices. It shows one coin at a time: the price, whether it is up or down today, the day's low and high, and a candlestick chart. The clock and the date come from the internet.

The sketch is `firmware/4 inch version/desk_coin/desk_coin.ino`.

This file is only for this board. Do not flash it on the 7 inch, and do not use it in place of the train tracker or the plane tracker.

## Board

Arduino IDE, board package ESP32 by Espressif.

| Setting | Value |
| --- | --- |
| Board | ESP32 Dev Module |
| Flash Size | 4MB |
| Partition Scheme | Huge APP (3MB No OTA) |
| Upload Speed | 921600 |

The same settings are in `firmware/4 inch version/arduinio settings.txt`.

Libraries, from the Library Manager:

- GFX Library for Arduino

Wi-Fi, HTTP, and Preferences come with the ESP32 board package.

The screen is a Hosyond / LCD wiki E32R40T. The picture is drawn in an 800×480 layout and scaled onto the 480×320 panel. The SD card slot is on top.

There is no second app slot, so this desk has no Check for updates button. A new build is flashed from Arduino over USB.

## What it shows

Green is up. Red is down. A very small move says flat today. The bar under the price is today's low and high. The chart buttons are **1H**, **4H**, and **1D**.

**AUTO** walks the coins about every 20 seconds. Tapping a coin stays on that coin and turns AUTO off. The price refreshes about every 20 seconds.

The coins are BTC, ETH, SOL, XRP, DOGE, ADA, AVAX, LINK, LTC, DOT, TRX, TON, UNI, NEAR, ATOM, and PEPE.

The time zone button is **PT**, **MT**, **CT**, or **ET** (Pacific, Mountain, Central, Eastern). On the coin page, the lamp button steps through dim, mid, and bright.

It needs 2.4 GHz Wi-Fi and USB power. There is no account and no exchange login. It does not buy or sell.

## Saved settings

Wi-Fi, the coin, the chart, AUTO, the time zone, and the brightness are stored under the name `deskcoin`. Factory on the Wi-Fi page clears only this desk. It does not clear the train tracker or the plane tracker.

The first time the screen asks you to touch the arrow in each corner, that lines up the touch. That calibration is stored separately and is not cleared by Factory.

## Data

Live prices come from Kraken's public ticker. Candles come from Kraken's public OHLC feed. The clock comes from the internet time servers. No key is required.

## Listing pictures

`etsy/` holds example screens for a listing. The prices in those pictures are not a live quote. Listing text is in `etsy/ETSY-LISTING.txt`. A buyer guide is in `etsy/Crypto-Ticker-User-Manual.pdf`.
