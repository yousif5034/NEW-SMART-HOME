/*
 * secrets.h  –  Smart Home Energy & Wellness System
 * ===================================================
 * ADD "secrets.h" to your .gitignore – never commit this file.
 * Copy this into your sketch folder and fill in your values.
 */

#pragma once

// ── WiFi ────────────────────────────────────────────────────
#define SECRET_SSID  "YourNetworkName"
#define SECRET_PASS  "YourWiFiPassword"

// ── Login credentials (used on BOTH TFT screen and web) ─────
// Must be exactly 6 digits each.
#define USER_ID      "123456"   // 6-digit user ID
#define USER_PASS    "654321"   // 6-digit password

/*
 * SECURITY NOTES
 * 1. Never commit this file. Add to .gitignore.
 * 2. Web server uses HttpOnly session cookie (16-hex token).
 * 3. Plain HTTP – safe on private LAN only.
 * 4. Both TFT and web share the same credentials + lockout.
 * 5. Change USER_ID and USER_PASS before first deployment.
 */
