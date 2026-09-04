/**
 * @file portal.h
 * @brief Local HTTP dashboard + JSON API + OTA update page.
 *
 * Split out of the original walter_feels_solar_test.ino so main.cpp stays
 * transport-orchestration-only, mirroring the pv-logger-c3 firmware's
 * main.cpp/portal.cpp separation.
 *
 * OTA uses the ESP32 <Update.h> API directly (multipart upload streamed to
 * flash), NOT ElegantOTA - this drops a dependency that needed a manually
 * patched local copy due to a confirmed upstream bug, and matches the
 * pv-logger-c3 firmware's already-proven /update handler exactly.
 */

#ifndef PORTAL_H
#define PORTAL_H

#include "telemetry.h"

/** Call once from setup(), after WiFi is up. Registers all routes and starts
 * the HTTP server. */
void portalBegin();

/** Call every loop() iteration. */
void portalLoop();

/** True while an OTA upload is being written to flash - callers should skip
 * anything else that shouldn't run concurrently with a flash write. */
bool portalOtaInProgress();

/** Feed one fresh sample into the dashboard's in-memory sparkline history and
 * make it the sample served by /telemetry. Call once per collectTelemetry(). */
void portalPushSample(const telemetry_sample_t *sample);

#endif
