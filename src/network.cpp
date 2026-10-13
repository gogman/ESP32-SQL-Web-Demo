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
    Serial.println("Fixed address settings are not valid, using DHCP");
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

  Serial.print("WiFi network not reachable. Started access point ");
  Serial.print(AP_SSID);
  Serial.print(", IP address: ");
  Serial.println(WiFi.softAPIP());
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

  Serial.print("Connecting to ");
  Serial.print(WIFI_SSID);

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
      Serial.println("");
      startAccessPoint();
      return false;
    }

    delay(500);
    Serial.print(".");
  }

  Serial.println("");
  Serial.print("Connected, IP address: ");
  Serial.println(WiFi.localIP());
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
    Serial.println("MDNS responder could not be started");
    return;
  }

  /*
   * Advertise the web server as a service too. Tools that browse the local
   * network for web servers will then list the board.
   */
  MDNS.addService("http", "tcp", HTTP_PORT);
  Serial.println("MDNS responder started");
}

/*
 * Periodic network housekeeping; call it from loop().
 *
 * On the WiFi network there is nothing to do: the ESP32 reconnects by
 * itself after a dropped connection. The work here is for the fallback
 * case. An access point is only a stopgap, so every AP_RETRY_MS the board
 * restarts to try the real network again - but only if nobody is using the
 * access point at that moment, so a person working through it is not cut off.
 */
void handleNetwork()
{
  /*
   * accessPointSince is 0 while on the WiFi network, which makes the whole
   * block a no-op in the normal case.
   */
  if (accessPointSince != 0 && millis() - accessPointSince >= AP_RETRY_MS)
  {
    if (WiFi.softAPgetStationNum() == 0)
    {
      /*
       * Restarting is the simplest reliable way back: setup() runs again
       * and connectWiFi() makes a fresh attempt.
       */
      Serial.println("Restarting to try the WiFi network again");
      ESP.restart();
    }

    /*
     * Somebody is connected to the access point. Leave them alone and start
     * a new waiting period before checking again.
     */
    accessPointSince = max(millis(), 1UL);
  }
}
