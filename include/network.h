/*
 * Network setup for the board: WiFi, the fallback access point and mDNS.
 * The implementation and the reasoning behind it are in src/network.cpp.
 */

#ifndef NETWORK_H
#define NETWORK_H

/*
 * Starts the network as the saved settings say (see settings.h); call
 * loadSettings() first.
 *
 * In access point mode, the board starts its own access point. Otherwise it
 * joins the WiFi network, blocking for at most WIFI_CONNECT_TIMEOUT_MS; if
 * the network cannot be joined in that time, the board starts its access
 * point instead, so the application stays reachable.
 *
 * Returns true when the WiFi network was joined, false when the access
 * point was started.
 */
bool connectWiFi();

/*
 * Makes the web server reachable by name, as "<host name>.local" (for
 * example esp32-sql.local), on the local network. Call it after
 * connectWiFi().
 */
void announceWebServer();

/*
 * Restarts the board RESTART_DELAY_MS from now, from handleNetwork(). Used
 * after new settings were saved, to apply them; the delay lets the answer
 * to the request that saved them reach the browser first.
 */
void restartSoon();

/*
 * Network housekeeping. Call it on every pass of loop().
 *
 * It carries out a restart asked for by restartSoon().
 *
 * On the WiFi network, it watches the connection: if it stays lost for
 * WIFI_LOST_TIMEOUT_MS, the board starts its fallback access point. While
 * the fallback access point is running, it restarts the board every
 * AP_RETRY_MS to try the WiFi network again, unless somebody is connected to
 * the access point. In access point mode it leaves the access point alone.
 */
void handleNetwork();

#endif
