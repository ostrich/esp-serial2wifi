import { delay as wait } from "./core.js";
export { delay as wait } from "./core.js";
import { validStatus } from "./status.js";
/* Network configuration is never inferred from a successful POST. */
export const networkPath = "/api/v1/network";
const object = (v) => v !== null && typeof v === "object" && !Array.isArray(v);
const integer = (v) => Number.isInteger(v) && v >= 0;
const string = (v) => typeof v === "string";
export const validReceipt = (v) =>
  object(v) &&
  integer(v.id) &&
  ["idle", "accepted", "applying", "applied", "failed"].includes(v.state) &&
  (v.error === null || string(v.error));
export const validNetwork = (v) =>
  object(v) &&
  v.schema === 1 &&
  validReceipt(v.operation) &&
  ["ap", "station", "station_ap", "disabled"].includes(v.mode) &&
  (v.station === null ||
    (object(v.station) &&
      string(v.station.ssid) &&
      typeof v.station.password_configured === "boolean" &&
      (v.station.bssid === null || string(v.station.bssid)))) &&
  (v.ap === null ||
    (object(v.ap) &&
      string(v.ap.ssid) &&
      typeof v.ap.password_configured === "boolean" &&
      string(v.ap.security) &&
      integer(v.ap.channel) &&
      typeof v.ap.hidden === "boolean" &&
      (v.ap.address === undefined || string(v.ap.address)))) &&
  object(v.ip) &&
  typeof v.ip.dhcp === "boolean" &&
  ["address", "mask", "gateway", "dns1", "dns2"].every((k) => string(v.ip[k])) &&
  object(v.identity) &&
  string(v.identity.hostname) &&
  string(v.identity.mdns_name) &&
  typeof v.identity.mdns === "boolean";
export const validScan = (v) =>
  object(v) &&
  v.schema === 1 &&
  ["idle", "scanning", "complete", "expired", "failed"].includes(v.state) &&
  integer(v.total) &&
  integer(v.retained) &&
  v.retained <= 16 &&
  integer(v.page) &&
  v.page <= 3 &&
  typeof v.truncated === "boolean" &&
  Array.isArray(v.networks) &&
  v.networks.length <= 4 &&
  v.networks.every(
    (n) =>
      object(n) &&
      string(n.ssid) &&
      /^(?:[\da-f]{2}:){5}[\da-f]{2}$/i.test(n.bssid) &&
      Number.isInteger(n.rssi_dbm) &&
      integer(n.channel) &&
      string(n.security)
  );
const utf8Bytes = (value) => new TextEncoder().encode(value).length;
export function ipv4(value) {
  if (!/^(?:\d{1,3}\.){3}\d{1,3}$/.test(value)) return null;
  const parts = value.split(".").map(Number);
  if (parts.some((v) => v > 255)) return null;
  return parts.reduce((n, v) => (n * 256 + v) >>> 0, 0);
}
const unicast = (n) =>
  n !== null && n >>> 24 > 0 && n >>> 24 !== 127 && n >>> 24 < 224 && n !== 0xffffffff;
export function validateStatic(values) {
  const e = {},
    nums = Object.fromEntries(
      ["address", "mask", "gateway", "dns1", "dns2"].map((k) => [k, ipv4(values[k])])
    );
  for (const [key, n] of Object.entries(nums))
    if (n === null) e[key] = "Enter four IPv4 octets from 0 to 255.";
  if (Object.keys(e).length) return e;
  const mask = nums.mask,
    host = ~mask >>> 0;
  if (!mask || host < 3 || host & (host + 1))
    e.mask = "Use a contiguous subnet mask from /1 through /30.";
  for (const k of ["address", "gateway", "dns1"])
    if ((k === "address" || nums[k]) && !unicast(nums[k]))
      e[k] = "Enter a usable unicast IPv4 address.";
  if (nums.dns2 && !unicast(nums.dns2)) e.dns2 = "Use a unicast IPv4 address, or 0.0.0.0 for none.";
  if (
    nums.gateway &&
    ((nums.address & mask) !== (nums.gateway & mask) || nums.address === nums.gateway)
  )
    e.gateway = "Gateway must be another address on the same subnet.";
  for (const k of ["address", "gateway"])
    if (nums[k] && (!(nums[k] & host) || (nums[k] & host) === host))
      e[k] = "Do not use the subnet or broadcast address.";
  return e;
}
export function validateNetwork(kind, v) {
  const errors = {};
  if (kind === "identity") {
    for (const key of ["hostname", "mdns_name"])
      if (!/^[a-z\d](?:[a-z\d-]{0,29}[a-z\d])?$/i.test(v[key]))
        errors[key] = "Use 1–31 letters, digits and interior hyphens.";
    return errors;
  }
  if (!v.ssid || utf8Bytes(v.ssid) > 32 || /[\x00-\x1f\x7f]/.test(v.ssid))
    errors.ssid = "Use 1–32 UTF-8 bytes without control characters.";
  const protectedMode = v.security !== "open";
  if (v.password_action === "replace") {
    const valid =
      /^[\x20-\x7e]{8,63}$/.test(v.password) ||
      (kind === "station" && /^[\da-f]{64}$/i.test(v.password));
    if (!protectedMode || !valid)
      errors.password =
        kind === "ap"
          ? "Use 8–63 printable ASCII characters."
          : "Use 8–63 printable ASCII characters, or a 64-digit hex key.";
  } else if (v.password_action === "clear") {
    if (protectedMode) errors.password_action = "Choose Open before removing the password.";
  } else if (v.password_action !== "keep")
    errors.password_action = "Choose how to handle the password.";
  if (v.password_action !== "replace" && v.password)
    errors.password = "Choose Replace password to send this value.";
  if (
    !["open", ...(kind === "station" ? ["protected"] : ["wpa", "wpa2", "mixed"])].includes(
      v.security
    )
  )
    errors.security = "Choose a supported security mode.";
  if (kind === "station") {
    if (v.bssid && !/^(?:[\da-f]{2}:){5}[\da-f]{2}$/i.test(v.bssid))
      errors.bssid = "Use a valid BSSID or leave it blank for any access point.";
    if (!v.dhcp) Object.assign(errors, validateStatic(v));
  } else {
    if (!Number.isInteger(Number(v.channel)) || Number(v.channel) < 1 || Number(v.channel) > 13)
      errors.channel = "Use a channel from 1 through 13.";
    const ip = ipv4(v.address || "");
    if (!unicast(ip) || !(ip & 255) || (ip & 255) === 255)
      errors.address = "Use an IPv4 host address ending in 1 through 254.";
    else if (v.address.split(".").some((o) => String(Number(o)) !== o))
      errors.address = "Write each octet without leading zeros.";
  }
  return errors;
}
export function draftFrom(kind, config) {
  if (kind === "identity") return { ...config.identity };
  const source = kind === "ap" ? config.ap : config.station;
  if (!source) return null;
  return {
    ...source,
    security:
      kind === "station" ? (source.password_configured ? "protected" : "open") : source.security,
    password_action: "keep",
    password: "",
    ...(kind === "station"
      ? { bssid: source.bssid || "", ...config.ip }
      : { channel: String(source.channel), address: source.address || "192.168.4.1" })
  };
}
const fields = {
  station: [
    "ssid",
    "security",
    "password_action",
    "password",
    "bssid",
    "dhcp",
    "address",
    "mask",
    "gateway",
    "dns1",
    "dns2"
  ],
  ap: ["ssid", "security", "password_action", "password", "channel", "hidden", "address"],
  identity: ["hostname", "mdns", "mdns_name"]
};
export function bodyFor(kind, draft) {
  return new URLSearchParams(fields[kind].map((k) => [k, String(draft[k] ?? "")]));
}
export function matchesNetwork(kind, draft, c) {
  if (kind === "identity")
    return ["hostname", "mdns", "mdns_name"].every((k) => c.identity[k] === draft[k]);
  const saved = c[kind];
  if (!saved || saved.ssid !== draft.ssid) return false;
  if (saved.password_configured !== (draft.security !== "open")) return false;
  if (kind === "ap")
    return (
      saved.security === draft.security &&
      saved.channel === Number(draft.channel) &&
      saved.hidden === draft.hidden &&
      ipv4(saved.address || "192.168.4.1") === ipv4(draft.address) &&
      c.mode === "ap"
    );
  if ((saved.bssid || "").toLowerCase() !== draft.bssid.toLowerCase() || c.ip.dhcp !== draft.dhcp)
    return false;
  return (
    draft.dhcp ||
    ["address", "mask", "gateway", "dns1", "dns2"].every((k) => ipv4(c.ip[k]) === ipv4(draft[k]))
  );
}
export class NetworkTransaction extends EventTarget {
  constructor(
    client,
    {
      now = Date.now,
      pause = wait,
      visible = () => !globalThis.document?.hidden,
      budget = 90000
    } = {}
  ) {
    super();
    this.client = client;
    this.now = now;
    this.pause = pause;
    this.visible = visible;
    this.budget = budget;
    this.state = "idle";
    this.receipt = null;
    this.latest = null;
  }
  stage(state, message) {
    this.state = state;
    this.dispatchEvent(new CustomEvent("stage", { detail: { state, message } }));
  }
  async run(kind, draft, signal) {
    if (signal?.aborted) {
      this.stage("cancelled", "Cancelled before sending settings.");
      throw new Error("Cancelled");
    }
    this.stage("applying", "Saving network settings…");
    this.receipt = null;
    this.latest = null;
    try {
      this.receipt = await this.client.request(networkPath + "/" + kind, {
        method: "POST",
        body: bodyFor(kind, draft),
        signal,
        validate: validReceipt,
        retries: 0,
        timeout: 10000
      });
    } catch (error) {
      this.stage(
        error.ambiguous ? "unverified" : "rejected",
        error.ambiguous
          ? "The reply was lost. The change may have taken effect. Reconnect and check before retrying."
          : "ESP Serial2WiFi rejected the change. Your edits are still here."
      );
      throw error;
    }
    if (!this.receipt.id || this.receipt.state !== "accepted") {
      this.stage(
        "unverified",
        "ESP Serial2WiFi did not confirm this change. Reload the saved settings before retrying."
      );
      throw new Error("Invalid change receipt");
    }
    const end = this.now() + this.budget;
    this.stage(
      "awaiting_reconnect",
      "Settings sent. Reconnect to ESP Serial2WiFi or your Wi-Fi network while we check the connection."
    );
    for (let attempt = 0; attempt < 35 && this.now() < end; attempt++) {
      try {
        await this.pause(attempt ? 2000 : 1500, signal);
        if (signal?.aborted) throw new Error("Cancelled");
      } catch (error) {
        this.stage(
          "unverified",
          "Checking stopped. Settings may already be saved; reconnect and reload them before retrying."
        );
        throw error;
      }
      if (!this.visible()) continue;
      try {
        const c = await this.client.request(networkPath, {
          signal,
          retries: 0,
          timeout: 2500,
          validate: validNetwork
        });
        if (c.operation.id !== this.receipt.id) {
          this.latest = null;
          this.stage(
            "unverified",
            "ESP Serial2WiFi no longer has a record of this change. Reconnect and check the saved settings. Your edits are still here."
          );
          throw new Error("Receipt changed");
        }
        if (c.operation.state === "failed") {
          this.latest = null;
          this.stage(
            "rejected",
            "The device could not finish applying the change. Some settings may have changed; reload them before retrying."
          );
          throw new Error("Apply failed");
        }
        if (c.operation.state !== "applied" || !matchesNetwork(kind, draft, c)) continue;
        this.latest = c;
        this.stage("verifying", "Settings saved. Checking the connection…");
        const status = await this.client.request("/api/v1/status", {
          signal,
          retries: 0,
          timeout: 2500,
          validate: validStatus
        });
        let connected = kind === "identity";
        if (kind === "ap")
          connected =
            status?.wifi?.mode === "ap" &&
            status.wifi.state === "ready" &&
            status.wifi.ssid === draft.ssid &&
            ipv4(status.wifi.ap_ip || status.wifi.ip) === ipv4(draft.address);
        if (kind === "station") {
          connected =
            ["station", "station_ap"].includes(status?.wifi?.mode) &&
            status.wifi.state === "connected" &&
            status.wifi.ssid === draft.ssid &&
            Boolean(status.wifi.ip) &&
            (draft.dhcp || ipv4(status.wifi.ip) === ipv4(draft.address));
          if (!connected && status?.wifi?.reason === "dhcp_pending")
            this.stage("verifying", "Joined the network. Waiting for an IP address…");
          if (status?.wifi?.state === "failed" && status.wifi.ssid === draft.ssid) {
            const reasons = {
              wrong_password: "The access point rejected the password.",
              ap_not_found: "The selected access point was not found.",
              association_failed: "Association failed.",
              dhcp_timeout: "The network did not provide a DHCP address before its timeout."
            };
            this.stage(
              "recovery",
              "Settings were saved. " +
                (reasons[status.wifi.reason] || "The station connection failed.") +
                " Reconnect to the recovery AP to change the settings or try again."
            );
            throw new Error("Connection failed");
          }
        }
        if (connected) {
          this.latest = c;
          this.stage(
            "connected",
            kind === "identity"
              ? "Saved. Restart ESP Serial2WiFi to use the new hostname or mDNS settings."
              : "Connected. Network settings saved."
          );
          return this.receipt;
        }
      } catch (error) {
        if (signal?.aborted)
          this.stage(
            "unverified",
            "Checking stopped. Settings may already be saved; reconnect and reload them before retrying."
          );
        if (["rejected", "recovery", "unverified"].includes(this.state) || signal?.aborted)
          throw error;
        this.stage(
          "awaiting_reconnect",
          "Waiting for ESP Serial2WiFi to reconnect. Keep this page open and reconnect to its Wi-Fi, or open the new IP address."
        );
      }
    }
    this.stage(
      "unverified",
      "ESP Serial2WiFi hasn’t reconnected. Your edits are still here. Reconnect and check the saved settings before trying again."
    );
    throw new Error("Network verification timed out");
  }
}
