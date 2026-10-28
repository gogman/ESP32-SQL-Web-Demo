# ESP32 SQL Web Demo

A small web application that runs entirely on an ESP32. The board keeps a SQLite database of US baby names (25,192 rows) on its own flash and serves a Bootstrap page for searching it: names starting with some text, or a range of names, with paging, sorting of the whole result, a primary-sex filter and a light/dark theme. No PC or internet connection is needed once the board is running.

A setup page at `/setup/` changes the network settings, kept in the board's NVS: join a WiFi network or run as an access point (with a captive portal), host name, access point name, channel, client limit, country and transmit power. By default the board answers at `http://esp32-sql.local`.

It is a read-only demonstration for a trusted network: the database is never changed on the board, and there is no login.

## Hardware and tools

- Freenove ESP32-WROVER (4 MB flash, 4 MB PSRAM)
- PlatformIO, Arduino framework, ESPAsyncWebServer, AsyncTCP and the Sqlite3Esp32 library

## Getting started

1. Copy `include/Secrets.example.h` to `include/Secrets.h` and fill in your WiFi network and the access point password.
2. Upload the filesystem (the database and stylesheet): `pio run -e Release -t uploadfs`
3. Upload the firmware: `pio run -e Release -t upload`
4. Open `http://esp32-sql.local`.

`python tools/run_tests.py` builds, uploads and runs the board tests in one step.

## Documentation

[docs/how-it-works.md](docs/how-it-works.md) describes how the program works, the web interfaces, building and testing, known issues and possible improvements.

## License

This project is licensed under the MIT License. See [LICENSE](LICENSE) for details.
