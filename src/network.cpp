/*
 * Network setup: joining the WiFi network, the access point (chosen on the
 * setup page, or as a fallback that keeps the board reachable when the
 * network is not) with its captive DNS server, and the mDNS name.
 *
 * Everything the setup page sets comes from the saved settings (settings.h);
 * the access point password from Secrets.h; timeouts and the optional fixed
 * address from Definitions.h.
 */

#include "network.h"
#include <WiFi.h>
#include <ESPmDNS.h>
#include <DNSServer.h>
#include "esp_wifi.h"
#include "Definitions.h"
#include "Secrets.h"
#include "settings.h"

/*
 * The millis() timestamp at which the fallback access point was started.
 *
 * The value 0 has a special meaning: "the board is on the WiFi network, no
 * access point is running". Because of that, a real timestamp must never be
 * 0, which is why it is always stored through max(millis(), 1UL) below.
 */
static unsigned long accessPointSince = 0;

/*
 * The millis() timestamp at which the WiFi connection was found lost while
 * the board was running, or 0 while it is connected (or the access point is
 * running). Like accessPointSince, a real timestamp is never stored as 0.
 */
static unsigned long connectionLostSince = 0;

/*
 * The millis() timestamp after which the board restarts, set by
 * restartSoon(), or 0 when no restart is due. Never 0 when set, like the
 * timestamps above.
 */
static unsigned long restartAt = 0;

/*
 * The host name in the form the network uses (see networkName() in
 * settings.h), for example "esp32-sql". Worked out once, in connectWiFi().
 */
static char hostName[HOSTNAME_MAX + 1] = "";

/*
 * The captive DNS server, running while the access point is. It answers
 * every name with the board's own address, so a phone that joins the access
 * point and checks for internet access lands on the board, and shows its
 * "sign in to network" prompt with the application in it.
 */
static DNSServer dnsServer;
static bool dnsRunning = false;

/* When the DNS server last answered waiting questions, for settings.dnsIntervalMs. */
static unsigned long dnsServedAt = 0;

/*
 * Applies the radio settings: the country, which decides the channels and
 * power levels that are allowed, and the transmit power. Both can only be
 * set while WiFi is running, so this is called right after WiFi.mode().
 */
static void applyRadioSettings()
{
  const WifiCountry *country = findCountry(settings.country);

  /*
   * The ESP32 country record: the code, the first channel, the number of
   * channels and the highest power in dBm. MANUAL means "use exactly this",
   * rather than adopting the country an access point announces.
   */
  wifi_country_t record = {};
  memcpy(record.cc, settings.country, 2);
  record.cc[2] = ' ';
  record.schan = 1;
  record.nchan = country ? country->channels : 11;
  record.max_tx_power = 20;
  record.policy = WIFI_COUNTRY_POLICY_MANUAL;

  if (esp_wifi_set_country(&record) != ESP_OK)
    LOG_PRINTLN("The country could not be set");

  WiFi.setTxPower((wifi_power_t)settings.txPower);
}

/*
 * Applies the fixed IP address from Definitions.h, if one is configured.
 *
 * Must be called before WiFi.begin(). When STATIC_IP is empty (the default)
 * nothing is done and the board asks the network for an address via DHCP.
 */
static void applyStaticAddress()
{
  /* An empty string means "not configured": keep DHCP. */
  if (STATIC_IP[0] == '\0')
    return;

  IPAddress ip, gateway, subnet, dns;

  /*
   * fromString() returns false for text that is not a valid dotted address.
   * A half-valid configuration could leave the board unreachable, so if any
   * of the three required values is wrong the whole fixed setup is skipped
   * and DHCP is used instead.
   */
  if (!ip.fromString(STATIC_IP) || !gateway.fromString(STATIC_GATEWAY) || !subnet.fromString(STATIC_SUBNET))
  {
    LOG_PRINTLN("Fixed address settings are not valid, using DHCP");
    return;
  }

  /*
   * The DNS server is optional. On a home network the router (the gateway)
   * normally answers DNS queries as well, so it is the fallback.
   */
  if (!dns.fromString(STATIC_DNS))
    dns = gateway;

  WiFi.config(ip, gateway, subnet, dns);
}

/*
 * Turns the board into its own WiFi access point.
 *
 * Used in access point mode, and as a fallback when the configured network
 * cannot be joined. A phone or laptop can then connect to the network named
 * settings.apSsid and reach the application at the address printed below
 * (normally 192.168.4.1).
 */
static void startAccessPoint()
{
  /*
   * Stop the station side completely first. disconnect(true) also switches
   * the station radio off, so it does not keep scanning for the missing
   * network in the background and disturb the access point.
   */
  WiFi.disconnect(true);
  WiFi.mode(WIFI_AP);
  applyRadioSettings();

  /*
   * The access point on the chosen channel, visible (0 = the name is
   * broadcast), for at most settings.maxClients devices at once.
   */
  WiFi.softAP(settings.apSsid, AP_PASSWORD, settings.channel, 0, settings.maxClients);

  /* Remember when the access point came up; never 0, see accessPointSince. */
  accessPointSince = max(millis(), 1UL);

  /*
   * The captive DNS server: "*" answers every name with the access point's
   * address. NoError makes it answer questions it has no record for (other
   * than address lookups) with an empty answer rather than an error, which
   * some phones expect.
   */
  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  dnsRunning = dnsServer.start(53, "*", WiFi.softAPIP());

  LOG_PRINTF("Started access point %s on channel %u, IP address: %s, captive DNS %s\n", settings.apSsid,
             settings.channel, WiFi.softAPIP().toString().c_str(), dnsRunning ? "running" : "failed");
}

/*
 * Starts the network as the settings say.
 *
 * In access point mode, the access point is started and false is returned.
 * Otherwise the board joins the WiFi network in the settings, waiting up to
 * WIFI_CONNECT_TIMEOUT_MS and printing a dot every half second, and true is
 * returned once it has. If the time runs out, the fallback access point is
 * started instead and false is returned; the board is reachable in all cases.
 */
bool connectWiFi()
{
  networkName(settings.hostname, hostName, sizeof(hostName));

  /*
   * The name the board gives the router when it asks for an address (DHCP),
   * which routers show in their list of devices. It has to be set before
   * the WiFi mode is chosen, because that is when the framework applies it.
   */
  WiFi.setHostname(hostName);

  if (settings.accessPointMode)
  {
    startAccessPoint();
    return false;
  }

  /* Station mode: the board is a client of an existing network. */
  WiFi.mode(WIFI_STA);
  applyRadioSettings();

  /*
   * By default the ESP32 lets its radio sleep between the access point's
   * beacons to save power. An incoming request then waits for the next
   * beacon, adding up to a beacon interval (typically ~100 ms) of delay.
   * This is a web server on mains power, so responsiveness wins: keep the
   * radio awake.
   */
  WiFi.setSleep(false);

  applyStaticAddress();
  WiFi.begin(settings.staSsid, settings.staPassword);

  LOG_PRINT("Connecting to ");
  LOG_PRINT(settings.staSsid);

  const unsigned long start = millis();

  while (WiFi.status() != WL_CONNECTED)
  {
    /*
     * Unsigned subtraction gives the correct elapsed time even if millis()
     * wraps around (after about 49 days), so this comparison is always safe.
     */
    if (millis() - start >= WIFI_CONNECT_TIMEOUT_MS)
    {
      /* End the line of dots, then fall back to the access point. */
      LOG_PRINTLN("");
      LOG_PRINTLN("WiFi network not reachable");
      startAccessPoint();
      return false;
    }

    delay(500);
    LOG_PRINT(".");
  }

  LOG_PRINTLN("");
  LOG_PRINT("Connected, IP address: ");
  LOG_PRINTLN(WiFi.localIP());
  return true;
}

/*
 * Starts the mDNS responder so the board answers to "<host name>.local", for
 * example "esp32-sql.local".
 *
 * Without it the application can only be reached by its IP address. A
 * failure here is reported but otherwise ignored, because the IP address
 * still works.
 */
void announceWebServer()
{
  if (!MDNS.begin(hostName))
  {
    LOG_PRINTLN("MDNS responder could not be started");
    return;
  }

  /*
   * Advertise the web server as a service too. Tools that browse the local
   * network for web servers will then list the board.
   */
  MDNS.addService("http", "tcp", HTTP_PORT);
  LOG_PRINTF("MDNS responder started: http://%s.local\n", hostName);
}

/*
 * Watches the WiFi connection while the board is on the network.
 *
 * When the connection drops, the ESP32 tries to reconnect by itself, and a
 * short drop is over within seconds. Only if the connection stays lost for
 * WIFI_LOST_TIMEOUT_MS does the board give up on the network and start the
 * fallback access point, exactly as it would have at startup. From then on
 * handleAccessPoint() takes over, including the regular attempts to get
 * back onto the network.
 */
static void watchConnection()
{
  if (WiFi.status() == WL_CONNECTED)
  {
    if (connectionLostSince != 0)
    {
      LOG_PRINTLN("WiFi connection is back");
      connectionLostSince = 0;
    }
    return;
  }

  if (connectionLostSince == 0)
  {
    /* Just noticed; never 0, see connectionLostSince. */
    connectionLostSince = max(millis(), 1UL);
    LOG_PRINTLN("WiFi connection lost, waiting for it to come back");
    return;
  }

  if (millis() - connectionLostSince >= WIFI_LOST_TIMEOUT_MS)
  {
    connectionLostSince = 0;
    startAccessPoint();
  }
}

/*
 * While the fallback access point is running.
 *
 * An access point is only a stopgap, so every AP_RETRY_MS the board
 * restarts to try the real network again - but only if nobody is using the
 * access point at that moment, so a person working through it is not cut off.
 */
static void handleAccessPoint()
{
  /* In access point mode the access point is meant to stay; there is no network to go back to. */
  if (settings.accessPointMode)
    return;

  if (millis() - accessPointSince >= AP_RETRY_MS)
  {
    if (WiFi.softAPgetStationNum() == 0)
    {
      /*
       * Restarting is the simplest reliable way back: setup() runs again
       * and connectWiFi() makes a fresh attempt.
       */
      LOG_PRINTLN("Restarting to try the WiFi network again");
      ESP.restart();
    }

    /*
     * Somebody is connected to the access point. Leave them alone and start
     * a new waiting period before checking again.
     */
    accessPointSince = max(millis(), 1UL);
  }
}

/*
 * Asks for a restart RESTART_DELAY_MS from now; handleNetwork() carries it out.
 */
void restartSoon()
{
  restartAt = max(millis() + RESTART_DELAY_MS, 1UL);
}

/*
 * Periodic network housekeeping; call it from loop(). accessPointSince is 0
 * while on the WiFi network, so it tells which of the two jobs applies.
 */
void handleNetwork()
{
  /* A restart asked for by restartSoon(), once its time has come. */
  if (restartAt != 0 && (long)(millis() - restartAt) >= 0)
  {
    LOG_PRINTLN("Restarting to apply the new settings");
    ESP.restart();
  }

  /*
   * The captive DNS server answers the questions waiting for it, every
   * settings.dnsIntervalMs (0: on every pass). Answering more often makes
   * pages start loading sooner on the access point, at the cost of more
   * processor time.
   */
  if (dnsRunning && millis() - dnsServedAt >= settings.dnsIntervalMs)
  {
    dnsServer.processNextRequest();
    dnsServedAt = millis();
  }

  if (accessPointSince == 0)
    watchConnection();
  else
    handleAccessPoint();
}
