/*
 * Template for Secrets.h.
 *
 * Copy this file to Secrets.h (in the same folder) and replace the three
 * placeholder values with your own. The project does not compile without
 * Secrets.h.
 *
 * Secrets.h is listed in .gitignore, so your real network name and
 * passwords never end up in version control. This example file is safe to
 * commit because it contains no real values.
 */

#ifndef SECRETS_H
#define SECRETS_H

/*
 * The WiFi network the board joins at startup: its name (SSID) and its
 * password. The ESP32 only supports 2.4 GHz networks.
 */
static const char WIFI_SSID[] = "your-network";
static const char WIFI_PASSWORD[] = "your-password";

/*
 * The password of the fallback access point the board starts when it cannot
 * join the network above (its name is AP_SSID in Definitions.h). WiFi
 * requires this password to be at least 8 characters long; with a shorter
 * one the access point fails to start.
 */
static const char AP_PASSWORD[] = "change-me-please";

#endif
