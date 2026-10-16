/*
 * The settings from the setup page, kept in NVS (non-volatile storage).
 *
 * NVS is a small key-value store in its own flash partition ("nvs" in
 * defpart.csv), separate from the firmware and the filesystem. Uploading new
 * firmware or a new filesystem image leaves it alone, so the settings
 * survive both. The Arduino Preferences library is the interface to it.
 */

#include "settings.h"
#include <Arduino.h>
#include <Preferences.h>
#include <ctype.h>
#include "Definitions.h"
#include "Secrets.h"

Settings settings;

/*
 * The countries on the page, in the order it lists them. The channel counts
 * follow each country's rules for 2.4 GHz WiFi: 1 to 11 in North America
 * and Taiwan, 1 to 13 in most other places. Japan also allows channel 14,
 * but only for the old 802.11b standard, so it is left out. "World (safe
 * mode)" keeps to the channels allowed everywhere. The default is not the
 * first entry but DEFAULT_COUNTRY (settings.h).
 */
const WifiCountry kWifiCountries[] = {
    {"01", "World (safe mode)", 11},
    {"AT", "Austria", 13},
    {"AU", "Australia", 13},
    {"BE", "Belgium", 13},
    {"BG", "Bulgaria", 13},
    {"BR", "Brazil", 13},
    {"CA", "Canada", 11},
    {"CH", "Switzerland", 13},
    {"CN", "China", 13},
    {"CY", "Cyprus", 13},
    {"CZ", "Czechia", 13},
    {"DE", "Germany", 13},
    {"DK", "Denmark", 13},
    {"EE", "Estonia", 13},
    {"ES", "Spain", 13},
    {"FI", "Finland", 13},
    {"FR", "France", 13},
    {"GB", "United Kingdom", 13},
    {"GR", "Greece", 13},
    {"HK", "Hong Kong", 13},
    {"HR", "Croatia", 13},
    {"HU", "Hungary", 13},
    {"IE", "Ireland", 13},
    {"IN", "India", 13},
    {"IS", "Iceland", 13},
    {"IT", "Italy", 13},
    {"JP", "Japan", 13},
    {"KR", "South Korea", 13},
    {"LI", "Liechtenstein", 13},
    {"LT", "Lithuania", 13},
    {"LU", "Luxembourg", 13},
    {"LV", "Latvia", 13},
    {"MT", "Malta", 13},
    {"MX", "Mexico", 11},
    {"NL", "Netherlands", 13},
    {"NO", "Norway", 13},
    {"NZ", "New Zealand", 13},
    {"PL", "Poland", 13},
    {"PT", "Portugal", 13},
    {"RO", "Romania", 13},
    {"SE", "Sweden", 13},
    {"SI", "Slovenia", 13},
    {"SK", "Slovakia", 13},
    {"TW", "Taiwan", 11},
    {"US", "United States", 11},
};
const size_t kWifiCountryCount = sizeof(kWifiCountries) / sizeof(kWifiCountries[0]);

// The Arduino core's wifi_power_t levels, strongest first.
const TxPowerLevel kTxPowerLevels[] = {
    {78, "19.5 dBm (maximum)"},
    {76, "19 dBm"},
    {74, "18.5 dBm"},
    {68, "17 dBm"},
    {60, "15 dBm"},
    {52, "13 dBm"},
    {44, "11 dBm"},
    {34, "8.5 dBm"},
    {28, "7 dBm"},
    {20, "5 dBm"},
    {8, "2 dBm"},
    {-4, "-1 dBm (minimum)"},
};
const size_t kTxPowerLevelCount = sizeof(kTxPowerLevels) / sizeof(kTxPowerLevels[0]);

const ValueOption DNS_INTERVALS[] = {
    {0, "Every pass"},
    {10, "10 ms"},
    {25, "25 ms"},
    {50, "50 ms"},
    {100, "100 ms"},
};
const size_t DNS_INTERVAL_COUNT = sizeof(DNS_INTERVALS) / sizeof(DNS_INTERVALS[0]);

/* The defaults of the numbers; the texts' defaults are in loadSettings(). */
static const uint8_t DEFAULT_CHANNEL = 1;
static const uint8_t DEFAULT_MAX_CLIENTS = 4;
static const uint16_t DEFAULT_DNS_INTERVAL_MS = 10;

/*
 * The NVS namespace and the key of each value. NVS limits both to 15
 * characters.
 */
static const char NAMESPACE[] = "netcfg";
static const char KEY_MODE[] = "ap_mode";
static const char KEY_HOSTNAME[] = "hostname";
static const char KEY_STA_SSID[] = "wifi_ssid";
static const char KEY_STA_PASSWORD[] = "wifi_pass";
static const char KEY_AP_SSID[] = "ap_ssid";
static const char KEY_CHANNEL[] = "channel";
static const char KEY_MAX_CLIENTS[] = "max_clients";
static const char KEY_DNS_INTERVAL[] = "dns_ms";
static const char KEY_COUNTRY[] = "country";
static const char KEY_TX_POWER[] = "tx_power";

const WifiCountry *findCountry(const char *code)
{
  for (size_t i = 0; i < kWifiCountryCount; i++)
  {
    if (strcmp(kWifiCountries[i].code, code) == 0)
      return &kWifiCountries[i];
  }
  return nullptr;
}

/* Whether a value is one of the transmit power levels. */
static bool isTxPowerLevel(int value)
{
  for (size_t i = 0; i < kTxPowerLevelCount; i++)
  {
    if (kTxPowerLevels[i].value == value)
      return true;
  }
  return false;
}

/* Whether a value is one of a list's choices. */
static bool isChoice(const ValueOption *options, size_t count, int value)
{
  for (size_t i = 0; i < count; i++)
  {
    if (options[i].value == value)
      return true;
  }
  return false;
}

/*
 * Reads one text value into a fixed buffer, or the default when it has
 * never been saved. getString() with a buffer returns 0 when the key does
 * not exist.
 */
static void readText(Preferences &store, const char *key, char *out, size_t size, const char *fallback)
{
  if (!store.isKey(key) || store.getString(key, out, size) == 0)
    strlcpy(out, fallback, size);
}

/* The settings every value starts from: the defaults described in settings.h. */
static void setDefaults(Settings &values, const char *apName)
{
  values.accessPointMode = false;
  strlcpy(values.hostname, DEFAULT_HOSTNAME, sizeof(values.hostname));
  strlcpy(values.staSsid, WIFI_SSID, sizeof(values.staSsid));
  strlcpy(values.staPassword, WIFI_PASSWORD, sizeof(values.staPassword));
  strlcpy(values.apSsid, apName, sizeof(values.apSsid));
  values.channel = DEFAULT_CHANNEL;
  values.maxClients = DEFAULT_MAX_CLIENTS;
  values.dnsIntervalMs = DEFAULT_DNS_INTERVAL_MS;
  strlcpy(values.country, DEFAULT_COUNTRY, sizeof(values.country));
  values.txPower = kTxPowerLevels[0].value;
}

void loadSettings()
{
  /*
   * The default access point name: DEFAULT_AP_PREFIX and six random hex
   * digits. esp_random() uses the radio's noise as a source of randomness,
   * so each board gets its own name. It is saved at once (below), so the
   * name is made only at the first startup and stays the same afterwards; a
   * name that changed at every restart could not be found again.
   */
  char randomName[SSID_MAX + 1];
  snprintf(randomName, sizeof(randomName), "%s %06X", DEFAULT_AP_PREFIX, (unsigned)(esp_random() & 0xFFFFFF));

  Settings loaded;
  setDefaults(loaded, randomName);

  /*
   * Open read-write: the access point name may have to be created and saved.
   * If NVS cannot be opened at all, every value keeps its default for this
   * run.
   */
  Preferences store;
  if (!store.begin(NAMESPACE, false))
  {
    LOG_PRINTLN("NVS could not be opened, using the default settings");
    settings = loaded;
    return;
  }

  if (!store.isKey(KEY_AP_SSID))
    store.putString(KEY_AP_SSID, randomName);

  loaded.accessPointMode = store.getBool(KEY_MODE, loaded.accessPointMode);
  readText(store, KEY_HOSTNAME, loaded.hostname, sizeof(loaded.hostname), DEFAULT_HOSTNAME);
  readText(store, KEY_STA_SSID, loaded.staSsid, sizeof(loaded.staSsid), WIFI_SSID);
  readText(store, KEY_AP_SSID, loaded.apSsid, sizeof(loaded.apSsid), randomName);

  /*
   * The password is read separately because an empty saved password is
   * meaningful (an open network) and must not fall back to Secrets.h.
   */
  if (store.isKey(KEY_STA_PASSWORD))
    store.getString(KEY_STA_PASSWORD, loaded.staPassword, sizeof(loaded.staPassword));

  loaded.channel = store.getUChar(KEY_CHANNEL, loaded.channel);
  loaded.maxClients = store.getUChar(KEY_MAX_CLIENTS, loaded.maxClients);
  loaded.dnsIntervalMs = store.getUShort(KEY_DNS_INTERVAL, loaded.dnsIntervalMs);
  readText(store, KEY_COUNTRY, loaded.country, sizeof(loaded.country), DEFAULT_COUNTRY);
  loaded.txPower = store.getChar(KEY_TX_POWER, loaded.txPower);
  store.end();

  /*
   * Saved settings were checked when they were saved, but an older firmware
   * may have saved values this one does not accept (for example a host name
   * with a space). Rather than start with something invalid, all settings go
   * back to their defaults then; the access point name is kept, so the board
   * can still be found.
   */
  const char *field;
  const char *problem = checkSettings(loaded, field);
  if (problem)
  {
    LOG_PRINTF("Saved setting '%s' is not valid (%s); using the defaults\n", field, problem);
    char apName[SSID_MAX + 1];
    strlcpy(apName, loaded.apSsid, sizeof(apName));
    setDefaults(loaded, apName);
  }

  settings = loaded;

  LOG_PRINTF("Settings: %s, host name '%s', WiFi network '%s', access point '%s' on channel %u, country %s\n",
             settings.accessPointMode ? "access point mode" : "WiFi mode", settings.hostname, settings.staSsid,
             settings.apSsid, settings.channel, settings.country);
}

const char *checkSettings(const Settings &candidate, const char *&field)
{
  /*
   * The host name: letters, digits and hyphens, not starting or ending with
   * a hyphen, as names on a network must be.
   */
  const size_t hostLength = strlen(candidate.hostname);
  bool hostValid = hostLength >= 1 && hostLength <= HOSTNAME_MAX &&
                   candidate.hostname[0] != '-' && candidate.hostname[hostLength - 1] != '-';

  for (const char *c = candidate.hostname; *c && hostValid; c++)
    hostValid = isalnum((unsigned char)*c) || *c == '-';

  if (!hostValid)
  {
    field = "hostname";
    return "The host name must be 1 to 32 letters, digits and hyphens, not starting or ending with a hyphen";
  }

  /* The WiFi network is required only when the board is to join it. */
  const size_t staLength = strlen(candidate.staSsid);
  if (staLength > SSID_MAX || (!candidate.accessPointMode && staLength == 0))
  {
    field = "sta_ssid";
    return "The WiFi network name must be 1 to 32 bytes long";
  }

  /* Empty means an open network; otherwise WPA2's limits apply. */
  const size_t passwordLength = strlen(candidate.staPassword);
  if (passwordLength != 0 && (passwordLength < PASSWORD_MIN || passwordLength > PASSWORD_MAX))
  {
    field = "sta_pass";
    return "The WiFi password must be 8 to 63 characters long";
  }

  const size_t apLength = strlen(candidate.apSsid);
  if (apLength == 0 || apLength > SSID_MAX)
  {
    field = "ap_ssid";
    return "The access point name must be 1 to 32 bytes long";
  }

  const WifiCountry *country = findCountry(candidate.country);
  if (!country)
  {
    field = "country";
    return "Choose a country from the list";
  }

  if (candidate.channel < 1 || candidate.channel > country->channels)
  {
    field = "wifi_channel";
    return country->channels == 11 ? "The channel must be 1 to 11 in this country" : "The channel must be 1 to 13 in this country";
  }

  if (candidate.maxClients < 1 || candidate.maxClients > MAX_CLIENTS_LIMIT)
  {
    field = "max_clients";
    return "The maximum number of clients must be 1 to 10";
  }

  if (!isChoice(DNS_INTERVALS, DNS_INTERVAL_COUNT, candidate.dnsIntervalMs))
  {
    field = "dns_interval";
    return "Choose a DNS interval from the list";
  }

  if (!isTxPowerLevel(candidate.txPower))
  {
    field = "tx_power";
    return "Choose a transmit power from the list";
  }

  field = "";
  return nullptr;
}

bool saveSettings(const Settings &newSettings)
{
  Preferences store;

  if (!store.begin(NAMESPACE, false))
    return false;

  /*
   * put...() returns the number of bytes written, 0 on failure. An empty
   * string writes 0 bytes but is still stored, so the two texts that may be
   * empty (the WiFi network in access point mode, and the password of an
   * open network) are checked with isKey() afterwards instead.
   */
  bool ok = store.putBool(KEY_MODE, newSettings.accessPointMode) > 0;
  ok = store.putString(KEY_HOSTNAME, newSettings.hostname) > 0 && ok;
  store.putString(KEY_STA_SSID, newSettings.staSsid);
  store.putString(KEY_STA_PASSWORD, newSettings.staPassword);
  ok = store.isKey(KEY_STA_SSID) && store.isKey(KEY_STA_PASSWORD) && ok;
  ok = store.putString(KEY_AP_SSID, newSettings.apSsid) > 0 && ok;
  ok = store.putUChar(KEY_CHANNEL, newSettings.channel) > 0 && ok;
  ok = store.putUChar(KEY_MAX_CLIENTS, newSettings.maxClients) > 0 && ok;
  ok = store.putUShort(KEY_DNS_INTERVAL, newSettings.dnsIntervalMs) > 0 && ok;
  ok = store.putString(KEY_COUNTRY, newSettings.country) > 0 && ok;
  ok = store.putChar(KEY_TX_POWER, newSettings.txPower) > 0 && ok;
  store.end();

  if (ok)
    settings = newSettings;

  return ok;
}

void networkName(const char *hostname, char *out, size_t size)
{
  size_t length = 0;

  for (const char *c = hostname; *c && length + 1 < size; c++)
    out[length++] = tolower((unsigned char)*c);

  out[length] = '\0';
}
