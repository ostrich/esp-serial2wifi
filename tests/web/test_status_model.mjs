import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import {
  validStatus,
  validCapabilities,
  health,
  uptime,
  SerialErrorTracker,
  serialErrorSummary,
  serialErrorFields
} from "../../web/app/status.js";
const fixtures = JSON.parse(
  await readFile(new URL("../support/fixtures/api-v1.json", import.meta.url), "utf8")
);
const status = fixtures["/api/v1/status"],
  caps = fixtures["/api/v1/capabilities"];
assert.equal(validStatus(status), true);
assert.equal(validCapabilities(caps), true);
let cards = health(status, caps);
assert.equal(cards.find((card) => card.id === "wifi").label, "Access point ready");
assert.equal(cards.find((card) => card.id === "serial").label, "TCP listener ready");
assert.equal(cards.find((card) => card.id === "mqtt").state, "disabled");
assert.ok(cards.find((card) => card.id === "serial").detail.includes("Check the console"));
const minimal = structuredClone(caps);
minimal.features.mqtt = false;
minimal.features.custom_pages = false;
assert.equal(
  health(status, minimal).some((card) => card.id === "mqtt"),
  false
);
const absent = structuredClone(status);
delete absent.mqtt;
assert.equal(health(absent, caps).find((card) => card.id === "mqtt").primary, "Not reported");
const recovery = structuredClone(status);
recovery.wifi.mode = "station_ap";
recovery.wifi.state = "failed";
recovery.wifi.reason = "wrong_password";
recovery.wifi.ip = null;
assert.equal(health(recovery, caps)[0].state, "degraded");
assert.equal(health(recovery, caps)[0].label, "Recovery AP available");
assert.ok(
  health(status, caps, true).every(
    (card) => card.badgeState === "unknown" && card.badgeLabel.startsWith("Out of date")
  )
);
for (const change of [
  (value) => (value.schema = 2),
  (value) => (value.system.heap_free_bytes = "20904"),
  (value) => (value.wifi.state = "excellent"),
  (value) => (value.serial.target_connected = "yes"),
  (value) => (value.serial.configured_format = ["8N1"]),
  (value) => (value.firmware.ota_eligible = "true")
]) {
  const invalid = structuredClone(status);
  change(invalid);
  assert.equal(validStatus(invalid), false);
}
const invalidCaps = structuredClone(caps);
invalidCaps.features.ota = true;
assert.equal(validCapabilities(invalidCaps), false);
assert.equal(uptime(0), "0s");
assert.equal(uptime(90061000), "1d 1h 1m");
assert.equal(uptime(null), "Not reported");
let native = 0;
for (const file of ["full-results.json", "minimal-results.json"]) {
  const cases = JSON.parse(
    await readFile(new URL("../../.build-tests/api/" + file, import.meta.url), "utf8")
  );
  for (const [name, [code, value]] of Object.entries(cases)) {
    if (name === "invalid-utf8" || code !== 200) continue;
    assert.equal(
      name.includes("capabilities") || name === "programmer-only"
        ? validCapabilities(value)
        : validStatus(value),
      true,
      name
    );
    native++;
  }
}
assert(native > 0, "Native status fixtures must be generated before model tests");
console.log(
  `PASS dashboard models: typed schema, disabled/absent features, stale states, recovery AP, unknown target and ${native} actual C responses`
);

const tracker = new SerialErrorTracker(),
  sample = structuredClone(status);
sample.serial.errors.uart_frames = 2;
sample.serial.loss_detected = false;
let report = tracker.observe(sample);
assert.equal(report.label, "Serial errors recorded");
assert.equal(report.changes, null);
assert(report.detail.includes("UART framing errors: 2"));
assert(!report.detail.includes("bytes dropped"));
sample.system.uptime_ms += 1000;
report = tracker.observe(sample);
assert.equal(report.label, "Earlier serial errors");
assert.equal(report.state, "ready");
assert.equal(report.interval_ms, 1000);
sample.serial.errors.uart_frames++;
sample.system.uptime_ms += 2000;
report = tracker.observe(sample);
assert.equal(report.label, "New serial errors");
assert.equal(report.changes.uart_frames, 1);
assert.equal(report.state, "degraded");
tracker.reset();
sample.system.uptime_ms += 2000;
assert.equal(tracker.observe(sample).changes, null);
sample.serial.boot_id++;
sample.system.uptime_ms += 2000;
assert.equal(tracker.observe(sample).changes, null);
sample.serial.errors.uart_frames = 1;
report = tracker.observe(sample);
assert.equal(report.changes, null);
assert(report.incomplete);
assert(report.detail.includes("incomplete"));
sample.serial.boot_id++;
sample.serial.errors.uart_frames = 0;
report = tracker.observe(sample);
assert.equal(report.hasErrors, false);
assert.equal(report.incomplete, false);
const unsupported = structuredClone(status);
delete unsupported.serial.errors;
delete unsupported.serial.boot_id;
assert.equal(validStatus(unsupported), false);
const oldCaps = structuredClone(caps);
delete oldCaps.features.optiboot_readback;
assert.equal(validCapabilities(oldCaps), false);
for (const patch of [{ uart_frames: -1 }, { uart_frames: 4294967296 }, { uart_frames: "2" }]) {
  const bad = structuredClone(status);
  Object.assign(bad.serial.errors, patch);
  assert.equal(validStatus(bad), false);
}
const missing = structuredClone(status);
delete missing.serial.errors.tcp_send;
assert.equal(validStatus(missing), false);
assert.equal(serialErrorFields.length, 11);
console.log(
  "PASS serial error observations: events versus bytes, first/steady/increasing readings, reconnect baselines, boot IDs, wraps and rejection of unsupported telemetry"
);

for (const boot of [-1, 4294967296, "42"]) {
  const bad = structuredClone(status);
  bad.serial.boot_id = boot;
  assert.equal(validStatus(bad), false);
}
