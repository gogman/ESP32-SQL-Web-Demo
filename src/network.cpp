/*
 * Network setup: joining the WiFi network, the fallback access point that
 * keeps the board reachable when the network is not, and the mDNS name.
 *
 * The network name and passwords come from Secrets.h; timeouts, the access
 * point name and the optional fixed address come from Definitions.h.
 */

#include "network.h"
#include <WiFi.h>
#include <ESPmDNS.h>
#include "Definitions.h"
#include "Secrets.h"

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
 * Used when the configured network cannot be joined. A phone or laptop can
 * then connect to the network named AP_SSID and reach the application at
 * the address printed below (normally 192.168.4.1).
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
  WiFi.softAP(AP_SSID, AP_PASSWORD);

  /* Remember when the access point came up; never 0, see accessPointSince. */
  accessPointSince = max(millis(), 1UL);

  LOG_PRINT("WiFi network not reachable. Started access point ");
  LOG_PRINT(AP_SSID);
  LOG_PRINT(", IP address: ");
  LOG_PRINTLN(WiFi.softAPIP());
}

/*
 * Joins the WiFi network named in Secrets.h.
 *
 * Waits up to WIFI_CONNECT_TIMEOUT_MS, printing a dot every half second.
 * Returns true once the network is joined. If the time runs out, the
 * fallback access point is started instead and false is returned; the board
 * is reachable in both cases.
 */
bool connectWiFi()
{
  /* Station mode: the board is a client of an existing network. */
  WiFi.mode(WIFI_STA);

  /*
   * By default the ESP32 lets its radio sleep between the access point's
   * beacons to save power. An incoming request then waits for the next
   * beacon, adding up to a beacon interval (typically ~100 ms) of delay.
   * This is a web server on mains power, so responsiveness wins: keep the
   * radio awake.
   */
  WiFi.setSleep(false);

  applyStaticAddress();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  LOG_PRINT("Connecting to ");
  LOG_PRINT(WIFI_SSID);

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
 * Starts the mDNS responder so the board answers to "<MDNS_HOSTNAME>.local".
 *
 * Without it the application can only be reached by its IP address. A
 * failure here is reported but otherwise ignored, because the IP address
 * still works.
 */
void announceWebServer()
{
  if (!MDNS.begin(MDNS_HOSTNAME))
  {
    LOG_PRINTLN("MDNS responder could not be started");
    return;
  }

  /*
   * Advertise the web server as a service too. Tools that browse the local
   * network for web servers will then list the board.
   */
  MDNS.addService("http", "tcp", HTTP_PORT);
  LOG_PRINTLN("MDNS responder started");
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
 * Periodic network housekeeping; call it from loop(). accessPointSince is 0
 * while on the WiFi network, so it tells which of the two jobs applies.
 */
void handleNetwork()
{
  if (accessPointSince == 0)
    watchConnection();
  else
    handleAccessPoint();
}
