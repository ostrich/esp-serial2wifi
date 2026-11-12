import { ApiClient, createPoll, Notice, readPreference, savePreference } from "./core.js";
import {
  validStatus,
  validCapabilities,
  reportedAddress,
  signalText,
  health,
  uptime,
  bytes,
  count,
  SerialErrorTracker
} from "./status.js";

const $ = (selector) => document.querySelector(selector);
// A response may remain allocated on the ESP until its TCP ACK arrives.
// Serialize API reads/writes so capture/status/forms share one memory budget.
const api = new ApiClient({ concurrency: 1 }),
  notice = new Notice($("#notice"));
const statusPath = "/api/v1/status",
  capabilitiesPath = "/api/v1/capabilities";
let workflowView = null,
  currentRoute = "dashboard",
  routeGeneration = 0,
  networkModule = null;
function loadNetworkModule() {
  if (!networkModule)
    networkModule = import("./network.js").catch((error) => {
      networkModule = null;
      throw error;
    });
  return networkModule;
}
let serialModule = null;
function loadSerialModule() {
  if (!serialModule)
    serialModule = import("./serial.js").catch((error) => {
      serialModule = null;
      throw error;
    });
  return serialModule;
}
let managementModule = null;
function loadManagementModule() {
  if (!managementModule)
    managementModule = import("./management.js").catch((error) => {
      managementModule = null;
      throw error;
    });
  return managementModule;
}
let updatesModule = null;
function loadUpdatesModule() {
  if (!updatesModule)
    updatesModule = import("./updates.js").catch((error) => {
      updatesModule = null;
      throw error;
    });
  return updatesModule;
}
const managedRoutes = [
  "network",
  "serial",
  "settings",
  "integrations",
  "diagnostics",
  "firmware",
  "pages"
];
let deviceOperation = null,
  pageActive = true,
  routeLoading = false,
  capGeneration = 0;
function available() {
  return pageActive && !routeLoading && !deviceOperation;
}
function syncShell() {
  const allowed = available();
  if (allowed) poll?.start();
  else poll?.stop();
  $("#refresh").disabled = !allowed || !poll || !!api.pending.get(statusPath);
  $("#retry").disabled = !allowed || !!api.pending.get(statusPath);
  $("#retry-features").disabled = !allowed || !!api.pending.get(capabilitiesPath);
}
const serialTracker = new SerialErrorTracker();
let serialReport = null;
let poll,
  snapshot = null,
  capabilities = null,
  updated = null,
  stale = false,
  capFlight = null,
  capController;
const time = new Intl.DateTimeFormat(undefined, {
  hour: "numeric",
  minute: "2-digit",
  second: "2-digit"
});
const narrow = matchMedia("(max-width:48rem)");
$(".skip").addEventListener("click", (event) => {
  event.preventDefault();
  $("#main").focus();
});

function theme(value) {
  document.documentElement.dataset.theme = value;
  $("#theme").setAttribute("aria-label", value === "dark" ? "Use light theme" : "Use dark theme");
  savePreference("theme", value);
}
const savedTheme = readPreference("theme", ["light", "dark"], null);
theme(
  ["light", "dark"].includes(savedTheme)
    ? savedTheme
    : matchMedia("(prefers-color-scheme:dark)").matches
      ? "dark"
      : "light"
);
$("#theme").addEventListener("click", () =>
  theme(document.documentElement.dataset.theme === "dark" ? "light" : "dark")
);
function menu(open) {
  $("#navigation").hidden = narrow.matches && !open;
  $("#menu").setAttribute("aria-expanded", String(open && narrow.matches));
}
$("#menu").addEventListener("click", () =>
  menu($("#menu").getAttribute("aria-expanded") !== "true")
);
document.addEventListener("keydown", (event) => {
  if (
    event.key === "Escape" &&
    narrow.matches &&
    $("#menu").getAttribute("aria-expanded") === "true"
  ) {
    menu(false);
    $("#menu").focus();
  }
});
narrow.addEventListener("change", () => menu(false));
menu(false);

import { el } from "./core.js";
function field(tag, key, className) {
  const node = el(tag, "", className);
  node.dataset.field = key;
  return node;
}
function action(label, href, primary = false) {
  const link = el("a", label, "action" + (primary ? " primary" : ""));
  link.href = href;
  return link;
}
function state(value, label) {
  $("#connection").dataset.state = value;
  $("#connection").textContent = label;
}
const systemFields = [
  ["Firmware", "version"],
  ["SDK", "sdk"],
  ["Active slot", "slot"],
  ["Flash capacity", "flash"],
  ["Free heap", "heap"],
  ["Uptime", "uptime"],
  ["Reset reason", "reset"],
  ["Image compatibility", "abi"],
  ["Web interface address", "address"],
  ["Reported IP", "ip"],
  ["Recovery AP IP", "ap_ip"],
  ["Wi-Fi MAC", "mac"],
  ["Station signal", "rssi"],
  ["Serial TCP ports", "ports"],
  ["OTA", "ota"]
];
function details(parent, rows) {
  const list = document.createElement("dl");
  list.className = "details";
  for (const [label, key] of rows) list.append(el("dt", label), field("dd", key));
  parent.append(list);
}
function mount() {
  const view = $("#view");
  view.replaceChildren();
  view.dataset.loaded = "true";
  const summary = document.createElement("section");
  summary.className = "device-summary";
  summary.append(field("h2", "hostname"), field("p", "description"));
  const quick = document.createElement("nav");
  quick.className = "quick-actions";
  quick.setAttribute("aria-label", "Quick actions");
  quick.append(
    action("Open console", "#/serial", true),
    action("Network settings", "#/network"),
    action("Device settings", "#/settings")
  );
  const firmware = action("Firmware details", "#/firmware");
  firmware.id = "firmware-action";
  quick.append(firmware);
  summary.append(quick);
  view.append(summary);
  const section = document.createElement("section");
  section.setAttribute("aria-labelledby", "health-title");
  section.append(el("h2", "Connections & firmware"));
  section.firstChild.id = "health-title";
  const grid = document.createElement("div");
  grid.className = "health-grid";
  for (const [id, label] of [
    ["wifi", "Wi-Fi"],
    ["serial", "Serial bridge"],
    ["firmware", "Firmware"],
    ["mqtt", "MQTT"]
  ]) {
    const card = document.createElement("article");
    card.className = "health-card";
    card.dataset.card = id;
    card.setAttribute("aria-labelledby", "health-" + id);
    const head = document.createElement("div");
    head.className = "health-heading";
    const title = el("h3", label);
    title.id = "health-" + id;
    const badge = el("span", "Checking", "health-badge");
    badge.dataset.part = "badge";
    head.append(title, badge);
    card.append(head);
    for (const part of ["primary", "detail", "meta"]) {
      const value = el("p", "", "health-" + part);
      value.dataset.part = part;
      card.append(value);
    }
    grid.append(card);
  }
  section.append(grid);
  view.append(section);
  const advanced = document.createElement("details");
  advanced.className = "advanced";
  advanced.append(el("summary", "Advanced system details"));
  details(advanced, systemFields);
  view.append(advanced);
}
function updateNav() {
  const integrations = $("[data-feature=integrations]"),
    pages = $("[data-feature=custom_pages]");
  integrations.hidden =
    !capabilities ||
    !(
      capabilities.features.mqtt ||
      capabilities.features.rest ||
      capabilities.features.socket ||
      capabilities.features.custom_pages
    );
  pages.hidden = !capabilities?.features.custom_pages;
}
const resets = {
  power_on: "Power on",
  hardware_watchdog: "Hardware watchdog",
  exception: "Exception",
  software_watchdog: "Software watchdog",
  software_restart: "Software restart",
  deep_sleep: "Deep-sleep wake",
  external_reset: "External reset",
  unknown: "Unknown"
};
function render() {
  updateNav();
  const view = $("#view");
  if (managedRoutes.includes(currentRoute)) return;
  if (!snapshot) {
    view.replaceChildren(el("p", "Loading device information…", "empty"));
    return;
  }
  if (view.dataset.loaded !== "true") mount();
  const s = snapshot,
    allowed = capabilities?.features.ota && s.firmware.ota_eligible;
  const values = {
    hostname: s.hostname || "Unnamed device",
    description: s.description || "",
    version: s.firmware.version,
    sdk: s.firmware.sdk,
    slot: s.firmware.slot || "Not reported",
    flash: bytes(s.firmware.flash_bytes),
    heap: bytes(s.system.heap_free_bytes),
    uptime: uptime(s.system.uptime_ms),
    reset: resets[s.system.reset_reason] || "Unknown",
    abi: s.firmware.image_abi,
    address: location.host,
    ip: s.wifi.ip || "Not reported",
    ap_ip: s.wifi.ap_ip || "Not active",
    mac: s.wifi.mac || "Not reported",
    rssi: signalText(s.wifi),
    ports: capabilities
      ? capabilities.serial_tcp_ports.join(", ") || "No ready listeners"
      : "Not confirmed",
    ota: allowed ? "Enabled" : "Unavailable on this build"
  };
  for (const node of view.querySelectorAll("[data-field]"))
    node.textContent = values[node.dataset.field] ?? "Not reported";
  const cards = health(s, capabilities, stale, serialReport || undefined);
  for (const card of view.querySelectorAll("[data-card]")) {
    const model = cards.find((item) => item.id === card.dataset.card);
    card.hidden = !model;
    if (!model) continue;
    const badge = card.querySelector("[data-part=badge]");
    badge.dataset.state = model.badgeState;
    badge.textContent = model.badgeLabel;
    for (const part of ["primary", "detail", "meta"]) {
      const node = card.querySelector("[data-part=" + part + "]");
      node.textContent = model[part];
      node.hidden = !model[part];
    }
  }
  const firmware = $("#firmware-action");
  if (firmware) {
    firmware.textContent = allowed ? "Update firmware" : "Firmware details";
    firmware.href = "#/firmware";
  }
}
async function loadCapabilities() {
  if (!available() || document.hidden) return;
  if (capFlight) return capFlight;
  capController = new AbortController();
  const controller = capController,
    generation = capGeneration;
  capFlight = api
    .request(capabilitiesPath, { validate: validCapabilities, signal: controller.signal })
    .then((value) => {
      if (controller.signal.aborted || generation !== capGeneration) return;
      capabilities = value;
      $("#cap-error").hidden = true;
      render();
      if (snapshot && !stale) workflowView?.updateStatus?.(snapshot, capabilities, serialReport);
    })
    .catch((error) => {
      if (controller.signal.aborted) return;
      $("#cap-error").hidden = false;
      $("#cap-error-message").textContent =
        "Couldn’t load the device’s features. Reconnect, then try again to enable the remaining controls.";
    })
    .finally(() => {
      capFlight = null;
      if (
        (generation !== capGeneration || controller.signal.aborted) &&
        !capabilities &&
        available() &&
        !document.hidden
      )
        loadCapabilities();
    });
  return capFlight;
}
function success(value) {
  const restarted =
    snapshot &&
    (value.firmware.version !== snapshot.firmware.version ||
      value.firmware.slot !== snapshot.firmware.slot ||
      (value.system.uptime_ms !== null &&
        snapshot.system.uptime_ms !== null &&
        value.system.uptime_ms < snapshot.system.uptime_ms));
  serialReport = serialTracker.observe(value);
  snapshot = value;
  updated = new Date();
  stale = false;
  if (restarted) {
    capabilities = null;
    capGeneration++;
    capController?.abort();
    loadCapabilities();
  }
  $("#device-name").textContent = value.hostname || "Unnamed device";
  $("#device-name").title = value.hostname;
  $("#device-address").textContent =
    reportedAddress(value.wifi, location.hostname) || "Address unavailable";
  $("#error").hidden = true;
  $("#view").setAttribute("aria-busy", "false");
  state("connected", "● Device reachable");
  $("#updated").textContent = "Last checked " + time.format(updated) + ".";
  render();
  workflowView?.updateStatus?.(value, capabilities, serialReport);
  document.body.dataset.ready = "true";
}
function failure(error, stopped) {
  serialTracker.reset();
  stale = true;
  $("#view").setAttribute("aria-busy", "false");
  $("#error").hidden = false;
  const reason =
    error.kind === "malformed"
      ? "Device information could not be read."
      : error.kind === "http"
        ? "ESP Serial2WiFi couldn’t return its status."
        : error.kind === "timeout"
          ? "The device did not reply in time."
          : "The device could not be reached.";
  $("#error-message").textContent =
    reason +
    " Check that you are connected to its Wi-Fi or local network." +
    (stopped ? " Click Retry connection to check again." : " Trying again shortly.");
  state(
    snapshot ? "degraded" : "failed",
    snapshot ? "◒ Showing earlier readings" : "○ Device unreachable"
  );
  workflowView?.updateFailure?.(error, stopped);
  $("#updated").textContent = updated
    ? "Last confirmed " + time.format(updated) + "; waiting for a new update."
    : "No device response yet.";
  if (snapshot || managedRoutes.includes(currentRoute)) render();
  else
    $("#view").replaceChildren(
      el("p", "Device details will appear after a successful connection.", "empty")
    );
  document.body.dataset.ready = "true";
}
async function route() {
  const generation = ++routeGeneration;
  const known = ["#/dashboard", ...managedRoutes.map((name) => "#/" + name)];
  const current = known.includes(location.hash) ? location.hash.slice(2) : "dashboard";
  if (workflowView && current !== currentRoute && !workflowView.canLeave()) {
    history.replaceState(null, "", "#/" + currentRoute);
    return false;
  }
  if (location.hash && !known.includes(location.hash)) {
    history.replaceState(null, "", "#/dashboard");
    notice.show("That page isn’t available. Showing the dashboard instead.");
  }
  routeLoading = true;
  workflowView?.destroy();
  workflowView = null;
  currentRoute = current;
  poll?.stop();
  for (const link of document.querySelectorAll("[data-route]")) {
    if (link.dataset.route === current) link.setAttribute("aria-current", "page");
    else link.removeAttribute("aria-current");
  }
  $("#title").textContent =
    current === "diagnostics"
      ? "Diagnostics"
      : current === "network"
        ? "Network"
        : current === "serial"
          ? "Serial & MCU"
          : current === "settings"
            ? "Device settings"
            : current === "integrations"
              ? "Integrations"
              : current === "firmware"
                ? "Firmware"
                : current === "pages"
                  ? "Custom web pages"
                  : "Device overview";
  document.title = "ESP Serial2WiFi · " + $("#title").textContent;
  menu(false);
  $("#view").dataset.loaded = "";
  $("#refresh").disabled = true;
  if (managedRoutes.includes(current)) {
    $("#view").replaceChildren(
      el("p", "Loading " + $("#title").textContent.toLowerCase() + "…", "empty")
    );
    try {
      const module = await (current === "network"
        ? loadNetworkModule()
        : current === "serial"
          ? loadSerialModule()
          : ["firmware", "pages"].includes(current)
            ? loadUpdatesModule()
            : loadManagementModule());
      if (generation !== routeGeneration) return false;
      workflowView =
        current === "network"
          ? module.mountNetwork($("#view"), api, notice)
          : current === "serial"
            ? module.mountSerial($("#view"), api, notice, snapshot, capabilities)
            : ["firmware", "pages"].includes(current)
              ? module.mountUpdates($("#view"), api, notice, current)
              : module.mountManagement($("#view"), api, notice, current);
    } catch (error) {
      if (generation !== routeGeneration) return false;
      const retry = el("button", "Reload interface");
      retry.type = "button";
      retry.addEventListener("click", () => location.reload());
      $("#view").replaceChildren(
        el("p", "This screen could not be loaded. Reload this page to retry."),
        retry
      );
    }
  } else render();
  poll = createPoll(api, statusPath, {
    onData: success,
    onError: failure,
    validate: validStatus,
    interval: current === "integrations" ? 3000 : 12000
  });
  routeLoading = false;
  syncShell();
  if (!capabilities) loadCapabilities();
  return true;
}
$("#view").addEventListener("device-operation", (event) => {
  const owner = event.detail.owner || event.target;
  if (event.detail.busy) {
    deviceOperation = owner;
    capController?.abort();
  } else if (deviceOperation === owner) deviceOperation = null;
  syncShell();
  if (!capabilities) loadCapabilities();
});
api.addEventListener("activity", (event) => {
  syncShell();
  if (event.detail.operation === statusPath)
    $("#refresh > span").textContent = event.detail.pending ? "Checking…" : "Refresh device";
});
$("#refresh").addEventListener("click", () => {
  if (deviceOperation) return;
  poll?.refresh();
  workflowView?.refresh();
});
$("#retry").addEventListener("click", () => {
  if (available()) poll?.refresh();
});
$("#retry-features").addEventListener("click", () => loadCapabilities());
addEventListener("hashchange", () => {
  route();
  $("#main").focus({ preventScroll: true });
  $("#main").scrollTo(0, 0);
});
addEventListener("pagehide", (event) => {
  pageActive = false;
  routeGeneration++;
  syncShell();
  capController?.abort();
  if (!event.persisted) workflowView?.destroy();
});
addEventListener("pageshow", (event) => {
  pageActive = true;
  if (event.persisted) {
    if (workflowView) {
      syncShell();
      if (available()) workflowView.refresh();
    } else route();
    loadCapabilities();
  }
});
document.addEventListener("visibilitychange", () => {
  if (document.hidden) capController?.abort();
  else if (!capabilities) {
    if (capFlight)
      capFlight.finally(() => {
        if (!document.hidden && !capabilities) loadCapabilities();
      });
    else loadCapabilities();
  }
});
route();
