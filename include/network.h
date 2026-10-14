/*
 * Network setup for the board: WiFi, the fallback access point and mDNS.
 * The implementation and the reasoning behind it are in src/network.cpp.
 */

#ifndef NETWORK_H
#define NETWORK_H

/*
 * Joins the WiFi network named in Secrets.h.
 *
 * Blocks for at most WIFI_CONNECT_TIMEOUT_MS. If the network cannot be
 * joined in that time, the board starts its own access point (AP_SSID) so
 * the application stays reachable.
 *
 * Returns true when the WiFi network was joined, false when the access
 * point was started instead.
 */
bool connectWiFi();

/*
 * Makes the web server reachable by name, as "<MDNS_HOSTNAME>.local", on the
 * local network. Call it after connectWiFi().
 */
void announceWebServer();

/*
 * Network housekeeping. Call it on every pass of loop().
 *
 * On the WiFi network, it watches the connection: if it stays lost for
 * WIFI_LOST_TIMEOUT_MS, the board starts its fallback access point. While
 * the access point is running, it restarts the board every AP_RETRY_MS to
 * try the WiFi network again, unless somebody is connected to the access
 * point.
 */
void handleNetwork();

#endif
