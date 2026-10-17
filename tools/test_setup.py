"""Checks the setup page (/setup/) and the settings interface (/api/settings).

Usage (from the project folder):
    python tools/test_setup.py [http://address-of-the-board] [--no-restart]

The default address is http://esp32-sql.local.

It checks the page's address and its redirect, that the page arrives compressed
and unpacks to web/setup.html, what GET /api/settings returns, that invalid
settings are refused field by field without anything being saved, and the page
in a real browser (Microsoft Edge or Google Chrome, without a window): that it
shows the saved settings and the board's lists, and its own checks of the
fields.

Then, unless --no-restart is given, it saves changed settings through the page
(host name, channel, maximum clients, DNS interval, country and transmit
power), waits for the board to restart, checks that they were kept, and saves
the original settings back (one more restart). The mode and the WiFi network
stay as they are, so the board stays on the same network throughout. The WiFi
password is never changed: its fields are left empty, which keeps it.

The exit code is 0 when every check passed and 1 otherwise.
"""

import gzip
import json
import os
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from test_page import BROWSERS, Browser  # noqa: E402

PROJECT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# How long the board may take to restart and answer again, in seconds.
RESTART_TIMEOUT = 60

# The settings themselves; the rest of the answer describes the page's lists.
VALUES = ["hostname", "network_name", "mode", "sta_ssid", "sta_password_set", "ap_ssid",
          "wifi_channel", "max_clients", "dns_interval", "country", "tx_power"]

failures = 0


def check(ok, what):
    """Prints one result line, "ok" or "FAIL" followed by a description, and counts failures."""
    global failures
    if not ok:
        failures += 1
    print("%s %s" % ("ok  " if ok else "FAIL", what), flush=True)


class NoRedirect(urllib.request.HTTPRedirectHandler):
    """Makes urllib return a redirect as it is, instead of following it."""

    def redirect_request(self, *args, **kwargs):
        return None


OPENER = urllib.request.build_opener(NoRedirect)


def request(base, path, form=None, headers=None):
    """Sends a GET, or a POST of the form if one is given. Returns (status, headers, body)."""
    data = urllib.parse.urlencode(form).encode() if form is not None else None
    try:
        with OPENER.open(urllib.request.Request(base + path, data=data, headers=headers or {}), timeout=10) as response:
            return response.status, response.headers, response.read()
    except urllib.error.HTTPError as error:
        return error.code, error.headers, error.read()


def settings_of(base):
    """The saved settings, without the lists."""
    try:
        status, _, body = request(base, "/api/settings")
    except Exception:
        return None
    return {k: v for k, v in json.loads(body).items() if k in VALUES} if status == 200 else None


def form_of(saved, **changes):
    """The form the page would send for the saved settings, with some fields changed."""
    form = {"hostname": saved["hostname"], "mode": saved["mode"], "sta_ssid": saved["sta_ssid"],
            "sta_pass": "", "sta_pass_confirm": "", "ap_ssid": saved["ap_ssid"],
            "wifi_channel": str(saved["wifi_channel"]), "max_clients": str(saved["max_clients"]),
            "dns_interval": str(saved["dns_interval"]), "country": saved["country"], "tx_power": str(saved["tx_power"])}
    if not saved["sta_password_set"]:
        form["sta_open"] = "1"
    form.update(changes)
    return form


def uptime(base):
    try:
        status, _, body = request(base, "/api/status")
        return json.loads(body)["uptime"] if status == 200 else None
    except Exception:
        return None


def wait_for_restart(base):
    """Waits until the board answers with an uptime shorter than the time waited. Returns the seconds taken."""
    started = time.time()
    time.sleep(3)
    while time.time() - started < RESTART_TIMEOUT:
        now = uptime(base)
        if now is not None and now < time.time() - started + 2:
            return time.time() - started
        time.sleep(1)
    return None


def main():
    arguments = [a for a in sys.argv[1:] if not a.startswith("--")]
    base = (arguments[0] if arguments else "http://esp32-sql.local").rstrip("/")
    restart = "--no-restart" not in sys.argv

    # --- Addresses -----------------------------------------------------------
    print("\nAddresses")
    status, headers, _ = request(base, "/setup")
    check(status == 301 and headers.get("Location") == "/setup/", "/setup is sent on to /setup/ (%d)" % status)

    status, headers, body = request(base, "/setup/")
    with open(os.path.join(PROJECT, "web", "setup.html"), "rb") as file:
        source = file.read()
    check(status == 200 and headers.get("Content-Encoding") == "gzip" and gzip.decompress(body) == source,
          "/setup/ is web/setup.html, gzip-compressed (%d bytes)" % len(body))
    status, _, _ = request(base, "/setup/", headers={"If-None-Match": headers.get("ETag", "")})
    check(status == 304, "asked again with its ETag, it is answered 304 Not Modified")

    # --- Reading the settings -------------------------------------------------
    print("\nReading the settings")
    status, headers, body = request(base, "/api/settings")
    answer = json.loads(body)
    saved = {k: v for k, v in answer.items() if k in VALUES}
    print("     %s" % ", ".join("%s=%s" % item for item in saved.items()))
    check(status == 200 and all(k in answer for k in VALUES), "GET /api/settings answers with the eleven values")
    check("no-store" in headers.get("Cache-Control", ""), "and is not cached")
    check(saved["mode"] in ("sta", "ap"), "the mode is sta or ap: %s" % saved["mode"])
    check(not any("pass" in k and k != "sta_password_set" for k in answer), "the WiFi password itself is not sent")
    check(saved["network_name"] == saved["hostname"].lower(), "the network name %r is the host name in lower case" % saved["network_name"])
    countries = {c["code"]: c for c in answer.get("countries", [])}
    check(len(countries) == 45 and countries.get("US", {}).get("channels") == 11 and countries.get("GB", {}).get("channels") == 13
          and countries.get("TW", {}).get("channels") == 11 and list(countries)[0] == "01",
          "45 countries, World (safe mode) first, with 11 channels in the US and Taiwan and 13 in the UK")
    powers = [o["value"] for o in answer.get("tx_powers", [])]
    check(len(powers) == 12 and powers[0] == 78 and powers[-1] == -4, "12 transmit power levels, from 19.5 dBm down to -1 dBm")
    check([o["value"] for o in answer.get("dns_intervals", [])] == [0, 10, 25, 50, 100], "DNS intervals 0, 10, 25, 50 and 100 ms")
    check(answer.get("max_clients_limit") == 10, "at most 10 clients")

    # --- Refused settings ----------------------------------------------------
    # Each of these must be answered 400, naming the field, and change nothing.
    print("\nRefused settings")
    bad = [
        ({"mode": "wifi"}, "mode"),
        ({"hostname": ""}, "hostname"),
        ({"hostname": "ESP32 SQL"}, "hostname"),
        ({"hostname": "-esp32"}, "hostname"),
        ({"hostname": "esp32-"}, "hostname"),
        ({"hostname": "a" * 33}, "hostname"),
        ({"mode": "sta", "sta_ssid": ""}, "sta_ssid"),
        ({"sta_ssid": "y" * 33}, "sta_ssid"),
        ({"sta_pass": "short", "sta_pass_confirm": "short"}, "sta_pass"),
        ({"sta_pass": "p" * 64, "sta_pass_confirm": "p" * 64}, "sta_pass"),
        ({"sta_pass": "password1", "sta_pass_confirm": "password2"}, "sta_pass_confirm"),
        ({"ap_ssid": ""}, "ap_ssid"),
        ({"ap_ssid": "x" * 33}, "ap_ssid"),
        ({"wifi_channel": "0"}, "wifi_channel"),
        ({"country": "US", "wifi_channel": "12"}, "wifi_channel"),
        ({"wifi_channel": "one"}, "wifi_channel"),
        ({"max_clients": "0"}, "max_clients"),
        ({"max_clients": "11"}, "max_clients"),
        ({"dns_interval": "5"}, "dns_interval"),
        ({"country": "XX"}, "country"),
        ({"country": "USA"}, "country"),
        ({"tx_power": "80"}, "tx_power"),
        ({"tx_power": "-8"}, "tx_power"),
        ({"tx_power": "1.5"}, "tx_power"),
    ]
    for changes, field in bad:
        status, _, body = request(base, "/api/settings", form_of(saved, **changes))
        reply = json.loads(body)
        shown = ", ".join("%s=%r" % (k, v if len(v) < 12 else v[:4] + "...(%d)" % len(v)) for k, v in changes.items())
        check(status == 400 and reply.get("field") == field and reply.get("error"), "%s is refused: %s" % (shown, reply.get("error")))
    check(settings_of(base) == saved, "nothing was saved")
    check(uptime(base) is not None, "and the board did not restart")

    status, _, _ = request(base, "/api/names", {"startswith": "Bob"})
    check(status == 405, "POST is still refused elsewhere (/api/names: %d)" % status)

    # --- The page in a browser -----------------------------------------------
    print("\nThe page in a browser")
    executable = next((path for path in BROWSERS if os.path.exists(path)), None)
    if not executable:
        sys.exit("No browser found.")

    ids = ["hostname", "sta_ssid", "sta_pass", "sta_pass_confirm", "ap_ssid", "wifi_channel", "max_clients", "dns_interval", "country", "tx_power"]
    browser = Browser(executable)
    try:
        browser.open(base + "/setup/")
        check(browser.wait_for("!document.getElementById('hostname').disabled"), "the form is filled in and enabled")
        values = browser.run("%s.map(function (id) { return document.getElementById(id).value; })" % json.dumps(ids))
        check(values == [saved["hostname"], saved["sta_ssid"], "", "", saved["ap_ssid"], str(saved["wifi_channel"]),
                         str(saved["max_clients"]), str(saved["dns_interval"]), saved["country"], str(saved["tx_power"])],
              "it shows the saved settings, with empty password fields")
        check(browser.run("document.getElementById('mode-%s').checked" % saved["mode"]), "the saved mode is selected")
        check(browser.run("document.getElementById('sta_open').checked") == (not saved["sta_password_set"]),
              "'Open network' is ticked only when no password is saved")
        check(browser.run("document.getElementById('country').options.length") == 45 and
              browser.run("document.getElementById('tx_power').options.length") == 12 and
              browser.run("document.getElementById('dns_interval').options.length") == 5,
              "the lists hold the board's 45 countries, 12 power levels and 5 DNS intervals")
        check(browser.run("document.body.innerHTML.indexOf('{{') < 0"), "no template placeholder is left on the page")
        check(browser.wait_for("getComputedStyle(document.documentElement).getPropertyValue('--bs-blue').trim() != ''"),
              "the Bootstrap stylesheet is applied")
        uptime_text = "document.getElementById('uptime').textContent"
        check(browser.wait_for("/^\\d+d \\d+:\\d\\d:\\d\\d$/.test(%s)" % uptime_text), "the uptime is shown as on the search page: %s" % browser.run(uptime_text))
        first = browser.run(uptime_text)
        time.sleep(2.2)
        check(browser.run(uptime_text) != first, "and counts on by itself (%s, then %s)" % (first, browser.run(uptime_text)))
        theme = "document.documentElement.getAttribute('data-bs-theme')"
        browser.run("document.getElementById('themeDark').click()")
        check(browser.wait_for(theme + " == 'dark'"), "the theme switcher's 'Dark' turns the page dark")
        browser.run("document.getElementById('themeLight').click()")
        check(browser.wait_for(theme + " == 'light'"), "and 'Light' turns it light")
        browser.run("document.getElementById('themeAuto').click()")
        check(browser.run("localStorage.getItem('theme')") == "auto", "the choice is kept with the search page's ('auto' again)")
        check(browser.run("document.getElementById('networkName').textContent") == saved["network_name"],
              "the address shows the host name: http://%s.local/" % saved["network_name"])

        browser.run("var h = document.getElementById('hostname'); h.value = 'My-Board-2'; h.dispatchEvent(new Event('input'))")
        check(browser.run("document.getElementById('networkName').textContent") == "my-board-2", "typing 'My-Board-2' shows my-board-2")

        browser.run("var c = document.getElementById('country'); c.value = 'GB'; c.dispatchEvent(new Event('change'))")
        check(browser.run("document.getElementById('wifi_channel').max") == "13", "choosing the United Kingdom allows channel 13")
        browser.run("var c = document.getElementById('country'); c.value = 'US'; c.dispatchEvent(new Event('change'))")
        check(browser.run("document.getElementById('wifi_channel').max") == "11", "and the United States 11")

        browser.run("var o = document.getElementById('sta_open'); o.checked = true; o.dispatchEvent(new Event('change'))")
        check(browser.run("document.getElementById('sta_pass').disabled && document.getElementById('sta_pass_confirm').disabled"),
              "ticking 'Open network' switches the password fields off")
        browser.run("var o = document.getElementById('sta_open'); o.checked = %s; o.dispatchEvent(new Event('change'))"
                    % json.dumps(not saved["sta_password_set"]))

        # The page's own checks: nothing is sent while a field is wrong.
        browser.run("document.getElementById('hostname').value = 'bad name';"
                    "document.getElementById('sta_open').checked = false; document.getElementById('sta_open').dispatchEvent(new Event('change'));"
                    "document.getElementById('sta_pass').value = 'password1';"
                    "document.getElementById('sta_pass_confirm').value = 'password2';"
                    "document.getElementById('wifi_channel').value = '12';"
                    "document.getElementById('max_clients').value = '11';"
                    "document.getElementById('setup').requestSubmit()")
        marked = browser.run("['hostname','sta_pass_confirm','wifi_channel','max_clients'].map(function (id) {"
                             " return document.getElementById(id).classList.contains('is-invalid'); })")
        check(marked == [True, True, True, True], "a host name with a space, passwords that differ, channel 12 in the US and 11 clients are marked")
        check("letters, digits" in browser.run("document.querySelector('[data-for=hostname]').textContent"), "with a message under the field")
        check(browser.run("getComputedStyle(document.querySelector('#sta_pass_confirm ~ .invalid-feedback')).display") != "none",
              "the 'passwords don't match' message is shown")
        time.sleep(1)
        check(settings_of(base) == saved, "and nothing is sent to the board")

        browser.run("document.getElementById('mode-sta').click(); document.getElementById('sta_ssid').value = ''; check()")
        check(browser.run("document.getElementById('sta_ssid').classList.contains('is-invalid')"), "in WiFi mode an empty network name is marked")
        browser.run("document.getElementById('mode-ap').click(); check()")
        check(not browser.run("document.getElementById('sta_ssid').classList.contains('is-invalid')"), "in access point mode it is allowed")

        if not restart:
            print("\n(--no-restart: saving was not tried)")
            return

        # --- Saving through the page -----------------------------------------
        print("\nSaving through the page")
        changed = {"hostname": "ESP32-SQL-Test", "wifi_channel": "6", "max_clients": "3", "dns_interval": "25", "country": "CA", "tx_power": "60"}
        browser.open(base + "/setup/")
        browser.wait_for("!document.getElementById('hostname').disabled")
        browser.run("var values = %s; Object.keys(values).forEach(function (id) { document.getElementById(id).value = values[id]; });"
                    "document.getElementById('setup').requestSubmit()" % json.dumps(changed))
        check(browser.wait_for("!document.getElementById('message').hidden && document.getElementById('message').textContent.indexOf('Saved') == 0"),
              "the page says: %s" % browser.run("document.getElementById('message').textContent"))
        took = wait_for_restart(base)
        check(took is not None, "the board restarted and answers again after %s s" % (round(took) if took else "-"))
        now = settings_of(base) or {}
        expected = dict(saved, hostname="ESP32-SQL-Test", network_name="esp32-sql-test", wifi_channel=6, max_clients=3,
                        dns_interval=25, country="CA", tx_power=60)
        check(now == expected, "the new settings were kept, and the rest, including the WiFi password, is unchanged")
        if now != expected:
            print("     got %s" % now)
        check(browser.wait_for("document.getElementById('message').textContent.indexOf('has restarted') >= 0"),
              "the page noticed the restart: %s" % browser.run("document.getElementById('message').textContent"))
    finally:
        browser.close()

        # Put the original settings back, whatever happened above.
        if restart:
            current = settings_of(base)
            if current and current != saved:
                status, _, _ = request(base, "/api/settings", form_of(saved))
                took = wait_for_restart(base)
                check(status == 200 and settings_of(base) == saved,
                      "the original settings are saved back (restart %s s)" % (round(took) if took else "-"))


if __name__ == "__main__":
    main()
    print("\n%s" % ("ALL CHECKS PASSED" if failures == 0 else "%d CHECKS FAILED" % failures))
    sys.exit(1 if failures else 0)
