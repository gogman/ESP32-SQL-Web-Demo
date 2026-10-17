/*
 * The settings a user can change on the setup page (/setup/), kept in
 * the ESP32's non-volatile storage (NVS) so they survive restarts and
 * firmware updates. The implementation is in src/settings.cpp.
 */

#ifndef SETTINGS_H
#define SETTINGS_H

#include <stddef.h>
#include <stdint.h>

/*
 * The longest texts, in bytes, not counting the terminating zero. WiFi
 * limits a network name (SSID) to 32 bytes and a WPA2 password to 63
 * characters. The host name limit is this application's choice: it is also
 * the DHCP host name, which many routers cut off at 32 characters.
 */
static const size_t SSID_MAX = 32;
static const size_t PASSWORD_MAX = 63;
static const size_t HOSTNAME_MAX = 32;

/* WPA2 refuses passwords shorter than this. */
static const size_t PASSWORD_MIN = 8;

/* The ESP32's access point serves at most this many devices at once. */
static const int MAX_CLIENTS_LIMIT = 10;

/*
 * A country the radio can be set up for: its two-letter code (as the ESP32
 * expects it; "01" stands for "world, safe mode"), its name for the page, and
 * how many channels, counted from 1, may be used there.
 */
struct WifiCountry
{
  const char *code;
  const char *name;
  uint8_t channels;
};

/*
 * A transmit power level: as the radio counts it (quarters of a dBm, the
 * values of the framework's wifi_power_t; -4 is -1 dBm), and its label.
 */
struct TxPowerLevel
{
  int8_t value;
  const char *label;
};

/* A choice in one of the page's other lists: the value saved, and its label. */
struct ValueOption
{
  int value;
  const char *label;
};

/* The countries the page offers, in the order it lists them. */
extern const WifiCountry kWifiCountries[];
extern const size_t kWifiCountryCount;

/* The country used until another is saved: the United States. */
static const char DEFAULT_COUNTRY[] = "US";

/*
 * The transmit power levels the page offers, strongest first. The first one
 * is the default.
 */
extern const TxPowerLevel kTxPowerLevels[];
extern const size_t kTxPowerLevelCount;

/*
 * How often the captive DNS server answers waiting questions, in
 * milliseconds; 0 means on every pass of loop(). See network.cpp.
 */
extern const ValueOption DNS_INTERVALS[];
extern const size_t DNS_INTERVAL_COUNT;

struct Settings
{
  /*
   * true: the board only runs its own access point.
   * false: the board joins the WiFi network below, and falls back to the
   * access point if it cannot (see network.cpp).
   */
  bool accessPointMode;

  /*
   * The board's name on the network: letters, digits and hyphens, not
   * starting or ending with a hyphen. It answers to <host name>.local.
   */
  char hostname[HOSTNAME_MAX + 1];

  /* The WiFi network to join, and its password (empty for an open network). */
  char staSsid[SSID_MAX + 1];
  char staPassword[PASSWORD_MAX + 1];

  /* The board's own access point: its name (its password is AP_PASSWORD in Secrets.h). */
  char apSsid[SSID_MAX + 1];

  /* The access point's channel, 1 to the country's last channel. */
  uint8_t channel;

  /* How many devices may be connected to the access point at once, 1 to MAX_CLIENTS_LIMIT. */
  uint8_t maxClients;

  /* One of DNS_INTERVALS. */
  uint16_t dnsIntervalMs;

  /* One of the codes in kWifiCountries. */
  char country[3];

  /* One of kTxPowerLevels. */
  int8_t txPower;
};

/*
 * The settings in use. loadSettings() fills them at startup; they change
 * only through saveSettings(), and the board restarts to apply them.
 */
extern Settings settings;

/*
 * Reads the settings from NVS. A value that has never been saved, or is no
 * longer valid, gets its default:
 *
 *   mode            join the WiFi network
 *   host name       "ESP32-SQL" (DEFAULT_HOSTNAME)
 *   WiFi network    WIFI_SSID and WIFI_PASSWORD from Secrets.h
 *   access point    "ESP32 SQL XXXXXX", where XXXXXX is a random hex number
 *                   made at the first startup and saved right away, so the
 *                   name stays the same from then on
 *   channel 1, 4 clients, DNS every 10 ms, United States, full power
 *
 * Call it once, at the start of setup().
 */
void loadSettings();

/*
 * Checks new settings. Returns nullptr when they are valid, otherwise a
 * message saying what is wrong, and sets field to the name of the form
 * field it is about (see handleSettingsPost() in web_handlers.cpp), so the
 * page can mark it.
 */
const char *checkSettings(const Settings &candidate, const char *&field);

/*
 * Stores settings in NVS and makes them the settings in use. They take
 * effect at the next restart. Returns false if NVS could not be written.
 */
bool saveSettings(const Settings &newSettings);

/* The country with this code, or nullptr. */
const WifiCountry *findCountry(const char *code);

/*
 * The host name as the network uses it: in lower case, for example
 * "esp32-sql" for "ESP32-SQL". The board answers to http://esp32-sql.local.
 */
void networkName(const char *hostname, char *out, size_t size);

#endif
