#pragma once
// Start the captive-portal SoftAP (PROV mode). Brings up the AP
// (CONFIG_AP_SSID / CONFIG_AP_PASS), DNS + HTTP server, and serves the
// config form. Blocks until the user saves a valid config and the device
// reboots into RUN mode.
void captive_portal_start(void);
