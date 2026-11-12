import { serialErrorFields, serialErrorSummary, uptime, signalText } from "./status.js";
import { DraftForm, copy, download } from "./core.js";
import { CaptureLog, decodeCapture } from "./serial-model.js";
import { el } from "./core.js";
const integer = (v) => Number.isSafeInteger(v),
  object = (v) => v !== null && typeof v === "object",
  text = (v) => typeof v === "string";
export function validSettings(v) {
  return (
    object(v) &&
    v.schema === 1 &&
    integer(v.sequence) &&
    typeof v.restart_required === "boolean" &&
    object(v.identity) &&
    text(v.identity.hostname) &&
    text(v.identity.description) &&
    object(v.pins) &&
    ["reset", "isp", "conn", "ser"].every((k) => integer(v.pins[k])) &&
    typeof v.pins.swap === "boolean" &&
    typeof v.pins.rx_pullup === "boolean" &&
    object(v.services) &&
    text(v.services.sntp_server) &&
    integer(v.services.timezone_offset) &&
    text(v.services.mdns_name) &&
    typeof v.services.mdns_enable === "boolean" &&
    (v.services.syslog === null || (object(v.services.syslog) && text(v.services.syslog.host)))
  );
}
export function validIntegrations(v) {
  return (
    object(v) &&
    v.schema === 1 &&
    integer(v.sequence) &&
    object(v.slip) &&
    typeof v.slip.enabled === "boolean" &&
    (v.mqtt === null ||
      (object(v.mqtt) &&
        typeof v.mqtt.enabled === "boolean" &&
        text(v.mqtt.host) &&
        text(v.mqtt.client_id) &&
        text(v.mqtt.username) &&
        typeof v.mqtt.password_configured === "boolean" &&
        ["port", "timeout", "keepalive"].every((k) => integer(v.mqtt[k])) &&
        object(v.mqtt.reporting) &&
        text(v.mqtt.reporting.topic)))
  );
}
const pinValues = [-1, 0, 1, 2, 3, 4, 5, 12, 13, 14, 15],
  encoder = new TextEncoder();
const max = (v, n) => encoder.encode(v || "").length <= n;
const host = (v) => /^[a-z0-9.-]+$/i.test(v);
const number = (v, low, high) => /^-?\d+$/.test(String(v)) && Number(v) >= low && Number(v) <= high;
export function validateManagement(kind, v) {
  const e = {};
  if (kind === "description" && !max(v.description, 128))
    e.description = "Use at most 128 UTF-8 bytes.";
  if (kind === "mqtt") {
    if ((v.enabled && !v.host) || (v.host && !host(v.host)) || !max(v.host, 63))
      e.host = "Enter a hostname or IPv4 address, without a scheme or port.";
    if ((v.enabled && !v.client_id) || !max(v.client_id, 47))
      e.client_id = "Use a client ID of 1–47 UTF-8 bytes when enabled.";
    for (const [key, low, high] of [
      ["port", 1, 65535],
      ["timeout", 1, 255],
      ["keepalive", 1, 65535]
    ])
      if (!number(v[key], low, high)) e[key] = `Use ${low}–${high}.`;
    if (!max(v.username, 69)) e.username = "Use at most 69 UTF-8 bytes.";
    if (
      !["keep", "replace", "clear"].includes(v.password_action) ||
      (v.password_action === "replace" && !v.password) ||
      (v.password_action !== "replace" && v.password)
    )
      e.password =
        "Choose Replace and enter a password, or leave the password empty for Keep/Clear.";
    if (!max(v.password, 69)) e.password = "Use at most 69 UTF-8 bytes.";
  }
  if (
    kind === "reporting" &&
    ((v.enabled && !v.topic) || !max(v.topic, 31) || /[+#]/.test(v.topic))
  )
    e.topic = "Use a publish topic of up to 31 UTF-8 bytes, without + or #.";
  if (kind === "pins") {
    const reserved = v.swap ? [13, 15] : [1, 3],
      seen = new Set(reserved);
    for (const key of ["reset", "isp", "conn", "ser"]) {
      const pin = Number(v[key]);
      if (!number(v[key], -1, 15) || !pinValues.includes(pin))
        e[key] = "Choose an available GPIO or Disabled.";
      else if (pin >= 0) {
        if (seen.has(pin)) e[key] = "This GPIO conflicts with another function or the UART.";
        seen.add(pin);
      }
    }
  }
  if (kind === "clock") {
    if ((v.sntp_server && !host(v.sntp_server)) || !max(v.sntp_server, 31))
      e.sntp_server =
        "Enter a hostname or IP address, or leave blank to turn off time sync. Maximum 31 bytes.";
    if (!number(v.timezone_offset, -11, 13))
      e.timezone_offset = "Use a fixed whole-hour offset from −11 to +13.";
  }
  if (kind === "syslog") {
    if (
      (v.syslog_host && !/^[a-z0-9.-]+(?::\d+)?$/i.test(v.syslog_host)) ||
      !max(v.syslog_host, 31) ||
      (v.syslog_host.includes(":") && !number(v.syslog_host.split(":")[1], 1, 65535))
    )
      e.syslog_host =
        "Enter a host and optional :port, or leave blank to turn off syslog. Maximum 31 bytes.";
    if (!number(v.syslog_min_heap, 0, 65535)) e.syslog_min_heap = "Use 0–65535 bytes.";
    if (!number(v.syslog_filter, 0, 7)) e.syslog_filter = "Use a severity from 0 to 7.";
  }
  return e;
}
const definitions = {
  description: [
    "Device description",
    "A short label to help you identify this device.",
    [["description", "Description", "text", 128]]
  ],
  mqtt: [
    "MQTT broker",
    "A broker relays messages between devices. Connect ESP Serial2WiFi to one to use MQTT, for example with a home-automation system. Connections are unencrypted.",
    [
      ["enabled", "Connect to an MQTT broker", "checkbox"],
      ["host", "Broker hostname / IPv4", "text", 63],
      ["port", "Broker TCP port", "number"],
      ["client_id", "Client ID", "text", 47],
      ["username", "Username", "text", 69],
      [
        "password_action",
        "Password action",
        [
          ["keep", "Keep saved password"],
          ["replace", "Replace password"],
          ["clear", "Clear password"]
        ]
      ],
      ["password", "New password (for Replace)", "password", 69],
      ["clean_session", "Clean session", "checkbox"],
      ["timeout", "Send timeout (seconds)", "number"],
      ["keepalive", "Keepalive (seconds)", "number"]
    ]
  ],
  reporting: [
    "Publish device status",
    "Send ESP Serial2WiFi’s Wi-Fi signal strength and free memory to an MQTT topic every minute. Requires a working broker connection above.",
    [
      ["enabled", "Publish device status every minute", "checkbox"],
      ["topic", "Publish topic", "text", 31]
    ]
  ],
  slip: [
    "Microcontroller integration",
    "For attached microcontrollers running ELClient or compatible firmware. They can use ESP Serial2WiFi for networking and exchange data with custom web pages. Leave this off for ordinary serial devices.",
    [["enabled", "Enable microcontroller integration", "checkbox"]]
  ],
  pins: [
    "GPIO and UART routing",
    "Pin changes take effect after saving. GPIO 0, 2 and 15 control how the ESP boots. Swapping UART0 moves RX/TX from GPIO 3/1 to 13/15; check your wiring before saving.",
    [
      ["reset", "Attached MCU reset", "pin"],
      ["isp", "Attached MCU ISP", "pin"],
      ["conn", "Connection LED", "pin"],
      ["ser", "Serial activity LED", "pin"],
      ["swap", "Swap UART0 to RX13 / TX15", "checkbox"],
      ["rx_pullup", "Enable UART RX pull-up", "checkbox"]
    ]
  ],
  clock: [
    "Time service",
    "Leave the server blank to turn off time sync. Set the UTC offset in whole hours; daylight saving time isn’t adjusted automatically.",
    [
      ["sntp_server", "SNTP server", "text", 31],
      ["timezone_offset", "UTC offset (hours)", "number"]
    ]
  ],
  syslog: [
    "Syslog",
    "Send debug messages to a syslog server over UDP. Leave the destination blank to turn this off.",
    [
      ["syslog_host", "Destination host[:port]", "text", 31],
      ["syslog_min_heap", "Minimum free heap to queue (bytes)", "number"],
      ["syslog_filter", "Maximum severity (0 emergency – 7 debug)", "number"],
      ["syslog_show_tick", "Include microsecond tick", "checkbox"],
      ["syslog_show_date", "Include date", "checkbox"]
    ]
  ]
};
const presets = [
  ["esp-01", "ESP-01: reset0, connection LED2", [0, -1, 2, -1, false, true]],
  ["esp-12", "ESP-12: reset12, ISP14, LEDs0/2", [12, 14, 0, 2, false, true]],
  ["esp-12-swap", "ESP-12 swapped UART: reset1, ISP3, LEDs0/2", [1, 3, 0, 2, true, true]],
  ["esp-bridge", "ESP bridge: reset12, ISP13, LEDs0/14", [12, 13, 0, 14, false, false]],
  ["wifi-link", "Wi-Fi link12: swapped UART, reset1, ISP3", [1, 3, 0, 2, true, false]]
];
export function managementDraft(kind, c) {
  if (kind === "mqtt") return { ...c.mqtt, password_action: "keep", password: "" };
  if (kind === "reporting") return c.mqtt.reporting;
  if (kind === "slip") return c.slip;
  if (kind === "description") return { description: c.identity.description };
  if (kind === "pins") return c.pins;
  const s = c.services;
  if (kind === "clock") return { sntp_server: s.sntp_server, timezone_offset: s.timezone_offset };
  return Object.fromEntries(Object.entries(s.syslog).map(([k, v]) => ["syslog_" + k, v]));
}
export function mountManagement(container, api, notice, kind = "settings") {
  if (kind === "diagnostics") return mountDiagnostics(container, api, notice);
  const integrations = kind === "integrations",
    path = "/api/v1/" + kind,
    validate = integrations ? validIntegrations : validSettings;
  let destroyed = false,
    current = null,
    loading = null,
    loadController = null,
    snapshot = null,
    capabilities = null;
  const forms = [];
  const lifetime = new AbortController();
  let restarting = null,
    restartButton = null,
    restartMessage = null,
    restartText = "";
  function showRestart() {
    if (destroyed) return;
    if (restartButton) restartButton.disabled = !!restarting;
    if (restartMessage) restartMessage.textContent = restartText;
  }
  function observeRestart(s) {
    if (restarting && restarting.sent) {
      const old = restarting.before;
      if (
        old &&
        ((s.system.uptime_ms !== null &&
          old.system.uptime_ms !== null &&
          s.system.uptime_ms < old.system.uptime_ms) ||
          s.firmware.version !== old.firmware.version ||
          s.firmware.slot !== old.firmware.slot)
      ) {
        restarting = null;
        restartText = "ESP Serial2WiFi restarted and is responding.";
      } else if (Date.now() - restarting.at > 30000) {
        restarting = null;
        restartText =
          "ESP Serial2WiFi is responding, but its restart could not be confirmed. Check the uptime before restarting again.";
      }
    }
    showRestart();
  }
  const message = el("p", "Loading saved settings…");
  message.setAttribute("role", "status");
  message.id = "management-load";
  const state = el("p");
  state.id = "management-state";
  state.setAttribute("role", "status");
  const reload = el("button", "Reload saved settings", "quiet");
  reload.type = "button";
  reload.id = "management-reload";
  const toolbar = el("div", "", "management-toolbar");
  toolbar.append(reload, message);
  const body = el("div", "", "management-forms");
  body.id = "management-forms";
  container.replaceChildren(toolbar, body);
  if (integrations)
    container.prepend(
      el(
        "p",
        "Use these features with MQTT services or a microcontroller running custom firmware. You don’t need them for a normal serial console.",
        "management-intro"
      )
    );
  function showIntegrationStatus() {
    if (!integrations) return;
    const mqtt = snapshot?.mqtt,
      labels = {
        disabled: "Off",
        disconnected: "Disconnected",
        connecting: "Connecting…",
        connected: "Connected",
        reconnecting: "Reconnecting…",
        failed: "Connection failed"
      };
    state.textContent =
      "Broker connection: " +
      (mqtt
        ? mqtt.enabled
          ? labels[mqtt.state] || "Checking…"
          : "Off"
        : current?.mqtt?.enabled
          ? "Checking…"
          : "Off");
    const available = body.querySelector("#integration-services");
    if (available && capabilities) {
      const services = [
        capabilities.features.rest ? "HTTP requests" : null,
        capabilities.features.mqtt ? "MQTT" : null,
        capabilities.features.socket ? "socket connections" : null
      ].filter(Boolean);
      available.textContent = services.length
        ? "This build supports " + services.join(", ") + "."
        : "No MCU network services are included in this build.";
    }
  }

  function field(form, spec, parent = form) {
    const [name, label, type, limit] = spec,
      id = form.id + "-" + name,
      wrap = el("div", "", "form-field"),
      lab = el("label", label);
    lab.htmlFor = id;
    const input = el(Array.isArray(type) || type === "pin" ? "select" : "input");
    input.id = id;
    input.name = name;
    if (Array.isArray(type) || type === "pin")
      for (const [value, label] of type === "pin"
        ? pinValues.map((p) => [String(p), p < 0 ? "Disabled" : "GPIO " + p])
        : type) {
        const option = el("option", label);
        option.value = value;
        input.append(option);
      }
    else {
      input.type = type === "number" ? "text" : type;
      input.autocomplete = type === "password" ? "new-password" : "off";
      if (type === "number") input.inputMode = "numeric";
      if (limit) input.maxLength = limit;
    }
    const error = el("p", "", "field-error");
    error.id = id + "-error";
    error.dataset.error = name;
    error.hidden = true;
    input.setAttribute("aria-describedby", error.id);
    wrap.append(lab, input, error);
    parent.append(wrap);
    if (type === "checkbox") {
      wrap.classList.add("check-field");
      wrap.prepend(input);
    }
    if (type === "password") {
      input.dataset.secret = "true";
      const show = el("button", "Show password", "quiet");
      show.type = "button";
      show.setAttribute("aria-pressed", "false");
      show.addEventListener("click", () => {
        const visible = input.type === "password";
        input.type = visible ? "text" : "password";
        show.textContent = visible ? "Hide password" : "Show password";
        show.setAttribute("aria-pressed", String(visible));
      });
      wrap.append(show);
    }
  }
  function render(c) {
    forms.forEach((f) => f.destroy());
    forms.length = 0;
    body.replaceChildren();
    const groups = integrations
      ? c.mqtt
        ? ["mqtt", "reporting", "slip"]
        : ["slip"]
      : ["description", "pins", "clock", ...(c.services.syslog ? ["syslog"] : [])];
    if (integrations && !c.mqtt)
      body.append(el("p", "MQTT and device-status publishing aren’t included in this firmware."));
    for (const group of groups) {
      const [title, help, specs] = definitions[group],
        section = el("section", "", "network-section"),
        form = el("form");
      form.id = "management-" + group;
      form.noValidate = true;
      form.setAttribute("aria-label", title);
      section.append(el("h2", title), el("p", help));
      body.append(section);
      if (group === "pins") {
        const label = el("label", "Board preset — save to apply");
        label.htmlFor = "management-preset";
        const select = el("select");
        select.id = label.htmlFor;
        const none = el("option", "Choose a preset");
        none.value = "";
        select.append(none);
        for (const [id, title] of presets) {
          const option = el("option", title);
          option.value = id;
          select.append(option);
        }
        section.append(label, select);
        select.addEventListener("change", () => {
          const preset = presets.find((p) => p[0] === select.value);
          if (!preset) return;
          ["reset", "isp", "conn", "ser", "swap", "rx_pullup"].forEach((k, i) => {
            const field = form.elements[k];
            if (field.type === "checkbox") field.checked = preset[2][i];
            else field.value = preset[2][i];
          });
          form.dispatchEvent(new Event("input"));
          select.value = "";
        });
      }
      let settings = section,
        advanced = null,
        auth = null;
      if (integrations) {
        section.id = "integration-" + group;
        settings = el("details", "", "integration-settings");
        settings.id = "integration-" + group + "-settings";
        settings.open =
          group === "mqtt"
            ? c.mqtt.enabled
            : group === "reporting"
              ? c.mqtt.reporting.enabled
              : c.slip.enabled;
        settings.append(
          el(
            "summary",
            group === "mqtt"
              ? "Broker settings"
              : group === "reporting"
                ? "Publishing settings"
                : "Advanced: serial command protocol"
          )
        );
        section.append(settings);
        if (group === "mqtt") {
          section.insertBefore(state, settings);
          state.setAttribute("role", "status");
          auth = el("details", "", "management-options");
          auth.append(el("summary", "Authentication"));
          auth.open = !!c.mqtt.username || c.mqtt.password_configured;
          advanced = el("details", "", "management-options");
          advanced.append(el("summary", "Connection options"));
        }
        if (group === "slip") {
          settings.append(
            el(
              "p",
              "ESP Serial2WiFi sends structured commands over the serial wires using SLIP message framing. The attached firmware must understand these commands; enabling this does not add support to the attached device."
            )
          );
          const available = el("p", "", "field-hint");
          available.id = "integration-services";
          settings.append(available);
        }
      }
      specs.forEach((spec) =>
        field(
          form,
          spec,
          group === "mqtt" && ["username", "password_action", "password"].includes(spec[0])
            ? auth
            : group === "mqtt" && ["clean_session", "timeout", "keepalive"].includes(spec[0])
              ? advanced
              : form
        )
      );
      if (auth) form.append(auth, advanced);

      if (group === "pins") {
        const table = el("table", "", "pin-table"),
          caption = el("caption", "Pin assignments"),
          head = el("thead"),
          row = el("tr"),
          tbody = el("tbody");
        row.append(el("th", "Function"), el("th", "GPIO"), el("th", "Hardware note"));
        for (const th of row.children) th.scope = "col";
        head.append(row);
        table.append(caption, head, tbody);
        section.append(table);
        const update = () => {
          tbody.replaceChildren();
          const swapped = form.elements.swap.checked;
          for (const [label, value] of [
            ["UART TX", swapped ? 15 : 1],
            ["UART RX", swapped ? 13 : 3],
            ...specs
              .filter((s) => s[2] === "pin")
              .map(([key, label]) => [label, Number(form.elements[key].value)])
          ]) {
            const tr = el("tr");
            const notes = {
              0: "Boot selection: HIGH for normal boot",
              2: "Boot strap: keep HIGH at boot",
              15: "Boot strap: keep LOW at boot"
            };
            tr.append(
              el("td", label),
              el("td", value < 0 ? "Disabled" : "GPIO " + value),
              el("td", value < 0 ? "Unassigned" : notes[value] || "Confirm board wiring")
            );
            tbody.append(tr);
          }
        };
        form.addEventListener("input", update);
        form.addEventListener("change", update);
        form._updatePins = update;
      }
      if (group === "mqtt")
        auth.append(
          el(
            "p",
            c.mqtt.password_configured
              ? "A password is saved. Leave it unchanged or choose a new one."
              : "No MQTT password is saved.",
            "field-hint"
          )
        );
      const feedback = el("p");
      feedback.setAttribute("role", "status");
      const actions = el("div", "", "form-actions"),
        save = el(
          "button",
          integrations
            ? {
                mqtt: "Save MQTT settings",
                reporting: "Save publishing settings",
                slip: "Save integration settings"
              }[group]
            : group === "pins"
              ? "Save GPIO and UART routing"
              : "Save " + title.toLowerCase()
        ),
        cancel = el("button", "Discard changes", "quiet");
      save.type = "submit";
      cancel.type = "button";
      cancel.dataset.cancel = "";
      actions.append(save, cancel);
      form.append(actions, feedback);
      settings.append(form);
      let initial = c,
        sequence = 0;
      const draft = new DraftForm(form, {
        read: async (signal) => {
          const value = initial || (await api.request(path, { signal, retries: 0, validate }));
          initial = null;
          sequence = value.sequence;
          current = value;
          return managementDraft(group, value);
        },
        validate: (v) => validateManagement(group, v),
        write: async (v, signal) => {
          if (
            group === "pins" &&
            !confirm("Apply this GPIO/UART routing? Connected serial devices can be interrupted.")
          )
            throw new Error("Pin change cancelled");
          const saved = await api.request(path + "/" + group, {
            method: "POST",
            body: new URLSearchParams(v),
            signal,
            retries: 0,
            validate
          });
          return { sequence: saved.sequence };
        },
        verify: (values, saved, receipt) =>
          receipt?.sequence === sequence &&
          specs.every(
            ([key, , type]) =>
              type === "password" ||
              key === "password_action" ||
              String(values[key]) === String(saved[key])
          ),
        message: feedback,
        notice,
        describe: () => (group === "mqtt" ? "MQTT settings saved." : "Saved.")
      });
      forms.push(draft);
      draft.addEventListener("state", () => {
        for (const field of form.querySelectorAll("[aria-invalid=true]")) {
          for (
            let parent = field.parentElement;
            parent && parent !== section;
            parent = parent.parentElement
          )
            if (parent.tagName === "DETAILS") parent.open = true;
        }
      });
      if (form._updatePins) draft.addEventListener("state", form._updatePins);
      draft.load();
    }
    if (integrations && c.mqtt) {
      const reporting = c.mqtt.reporting;
      const sample = el(
        "p",
        "Example status message: " + JSON.stringify(reporting.sample) + ".",
        "field-hint"
      );
      body.querySelector("#integration-reporting-settings").append(sample);
    }
    if (!integrations) {
      const section = el("section", "", "network-section"),
        restart = el("button", "Restart ESP Serial2WiFi", "quiet"),
        outcome = el("p");
      restart.type = "button";
      outcome.setAttribute("role", "status");
      section.append(
        el("h2", "Restart ESP Serial2WiFi"),
        el(
          "p",
          "Restarting ESP Serial2WiFi disconnects Wi-Fi and serial sessions. Boot messages will be sent over serial. Make sure the attached device can safely receive them, or disconnect ESP TX from its RX pin."
        ),
        restart,
        outcome
      );
      body.append(section);
      restartButton = restart;
      restartMessage = outcome;
      showRestart();
      restart.addEventListener("click", async () => {
        if (
          destroyed ||
          restarting ||
          !confirm(
            "Restart ESP Serial2WiFi? Wi-Fi/TCP sessions will disconnect and boot messages will be sent to the attached serial device."
          )
        )
          return;
        const own = { before: snapshot, at: Date.now(), sent: false };
        restarting = own;
        restartText = "Requesting restart…";
        showRestart();
        try {
          await api.request("/api/v1/settings/restart", {
            method: "POST",
            body: new URLSearchParams({ action: "restart_esp" }),
            signal: lifetime.signal,
            retries: 0,
            validate: (v) => v?.accepted === true
          });
          if (destroyed) return;
          own.sent = true;
          restartText =
            "Restart requested. Reconnect to the device; its status will confirm the restart.";
        } catch (error) {
          if (destroyed) return;
          own.sent = !!error.ambiguous;
          if (!error.ambiguous) restarting = null;
          restartText = error.ambiguous
            ? "The reply was lost. Checking whether ESP Serial2WiFi restarted."
            : "ESP Serial2WiFi couldn’t restart. Check the connection and try again.";
        }
        showRestart();
      });
    }
    showIntegrationStatus();
  }
  async function refresh(force = false) {
    if (destroyed || loading) return loading;
    if (forms.some((f) => f.busy || (!force && f.dirty))) return;
    if (
      force &&
      forms.some((f) => f.dirty) &&
      !confirm("Discard your changes and reload the saved settings?")
    )
      return;
    forms.forEach((f) => f.setLocked(true));
    body.inert = true;
    reload.disabled = true;
    loadController = new AbortController();
    loading = api
      .request(path, { signal: loadController.signal, validate, retries: 0 })
      .then((c) => {
        if (destroyed) return;
        current = c;
        render(c);
        message.textContent = "Saved settings loaded.";
      })
      .catch(() => {
        if (!destroyed)
          message.textContent =
            "Settings could not be loaded. Check the connection and use Reload saved settings.";
      })
      .finally(() => {
        loading = null;
        loadController = null;
        if (!destroyed) {
          body.inert = false;
          forms.forEach((f) => f.setLocked(false));
          reload.disabled = false;
        }
      });
    return loading;
  }
  reload.addEventListener("click", () => refresh(true));
  refresh();
  return {
    refresh,
    updateStatus: (s, caps) => {
      observeRestart(s);
      snapshot = s;
      capabilities = caps;
      showIntegrationStatus();
    },
    canLeave: () =>
      !forms.some((f) => f.dirty || f.busy) || confirm("Leave and discard your unsaved changes?"),
    destroy() {
      destroyed = true;
      lifetime.abort();
      loadController?.abort();
      forms.forEach((f) => f.destroy());
    }
  };
}
export function redactedDiagnostics(status, capabilities, report = serialErrorSummary(status)) {
  const pick = (source, keys) =>
    Object.fromEntries(keys.filter((k) => source && k in source).map((k) => [k, source[k]]));
  const result = {
    schema: 1,
    firmware: pick(status.firmware, [
      "version",
      "sdk",
      "slot",
      "image_abi",
      "flash_bytes",
      "ota_eligible"
    ]),
    system: pick(status.system, ["uptime_ms", "heap_free_bytes", "reset_reason", "reset_code"]),
    wifi: pick(status.wifi, [
      "mode",
      "state",
      "reason",
      "ip",
      "ap_ip",
      "mac",
      "rssi_dbm",
      "channel"
    ]),
    serial: pick(status.serial, [
      "state",
      "baud",
      "configured_format",
      "clients",
      "loss_detected",
      "boot_id"
    ]),
    mqtt: pick(status.mqtt, ["enabled", "state"]),
    features: pick(capabilities?.features, [
      "mqtt",
      "rest",
      "socket",
      "syslog",
      "custom_pages",
      "ota",
      "optiboot_upload",
      "optiboot_readback"
    ])
  };
  if (report.totals) result.serial.errors = report.totals;
  result.serial.error_observation = {
    interval_ms: report.interval_ms,
    changes: report.changes,
    totals_incomplete: report.incomplete,
    stale: !!report.stale
  };
  return result;
}
function mountDiagnostics(container, api, notice) {
  let snapshot = null,
    caps = null,
    serialReport = null,
    seenAt = 0,
    destroyed = false,
    paused = false,
    timer = null,
    controller = null,
    failures = 0,
    logStarted = false;
  let modeController = null,
    modeStarted = false,
    modeBusy = false;
  const modeButtons = [];
  const fields = el("dl", "", "details"),
    hint = el("p"),
    age = el("p", "Waiting for device status…");
  age.id = "diagnostics-age";
  age.setAttribute("role", "status");
  const exportButton = el("button", "Download redacted diagnostics", "quiet");
  exportButton.type = "button";
  exportButton.disabled = true;
  exportButton.id = "diagnostics-export";
  const log = new CaptureLog(),
    pre = el("pre", "", "serial-output");
  pre.id = "diagnostics-log";
  pre.tabIndex = 0;
  pre.setAttribute("aria-label", "Device debug log");
  const logState = el("p", "Waiting for a device response…");
  logState.setAttribute("role", "status");
  const pause = el("button", "Pause debug log", "quiet");
  pause.type = "button";
  const copyButton = el("button", "Copy visible log", "quiet");
  copyButton.type = "button";
  const serialState = el("p");
  serialState.id = "diagnostics-serial-health";
  serialState.setAttribute("role", "status");
  const errorDetails = el("details", "", "management-options"),
    errorFields = el("dl", "", "details");
  errorDetails.id = "diagnostics-error-counters";
  errorDetails.hidden = true;
  errorDetails.append(
    el("summary", "Serial error counters"),
    el(
      "p",
      "These counts reset when ESP Serial2WiFi restarts. Dropped bytes are counted separately for UART input, TCP clients and the web terminal."
    ),
    errorFields
  );
  const exportActions = el("div", "", "form-actions management-toolbar");
  exportActions.append(exportButton);
  const logHeading = el("h2", "Device debug log");
  logHeading.id = "diagnostics-log-heading";
  const logActions = el("div", "", "form-actions management-toolbar");
  logActions.id = "diagnostics-log-actions";
  logActions.append(pause, copyButton);
  container.replaceChildren(
    el(
      "p",
      "View device status and recent debug messages. Download diagnostics to save the status details without passwords or log messages."
    ),
    fields,
    hint,
    serialState,
    errorDetails,
    age,
    exportActions,
    logHeading,
    el("p", "Check the log for passwords or other private details before sharing it."),
    logActions,
    pre,
    logState
  );
  const modeGroup = el("section"),
    modeActions = el("div", "", "form-actions"),
    modeState = el("p", "Loading debug UART mode…");
  modeState.setAttribute("role", "status");
  modeGroup.append(
    el("h2", "Debug UART output"),
    el(
      "p",
      "Off keeps debug messages away from the attached console. Automatic follows Wi-Fi state; UART0 shares the target TX pin. UART1 uses GPIO2."
    )
  );
  const validMode = (v) =>
    v?.schema === 1 &&
    ["auto", "off", "on0", "on1"].includes(v.mode) &&
    typeof v.uart_enabled === "boolean";
  function showMode(value) {
    for (const b of modeButtons) {
      b.setAttribute("aria-pressed", String(b.dataset.mode === value.mode));
      b.disabled = false;
    }
    modeState.textContent =
      "Saved mode: " +
      value.mode +
      ". UART debug output is " +
      (value.uart_enabled ? "active." : "off.");
  }
  async function loadMode() {
    if (destroyed || modeController || modeBusy) return;
    const own = new AbortController();
    modeController = own;
    modeButtons.forEach((b) => (b.disabled = true));
    try {
      const v = await api.request("/api/v1/diagnostics/uart-log", {
        signal: own.signal,
        retries: 0,
        validate: validMode
      });
      if (!destroyed) showMode(v);
    } catch (_) {
      if (!destroyed)
        modeState.textContent = "Debug UART mode unavailable. Use Refresh device to retry.";
    } finally {
      if (modeController === own) modeController = null;
    }
  }
  for (const [value, label] of [
    ["off", "Off"],
    ["auto", "Automatic"],
    ["on0", "UART0 TX"],
    ["on1", "UART1 TX"]
  ]) {
    const button = el("button", label, "quiet");
    button.type = "button";
    button.dataset.mode = value;
    button.setAttribute("aria-pressed", "false");
    button.disabled = true;
    modeButtons.push(button);
    modeActions.append(button);
    button.addEventListener("click", async () => {
      if (
        destroyed ||
        modeBusy ||
        modeController ||
        (value !== "off" &&
          !confirm(
            "Enable debug UART output? Messages can interfere with the attached serial device."
          ))
      )
        return;
      modeBusy = true;
      modeButtons.forEach((b) => (b.disabled = true));
      const own = new AbortController();
      modeController = own;
      try {
        const saved = await api.request("/api/v1/diagnostics/uart-log", {
          method: "POST",
          body: new URLSearchParams({ mode: value }),
          signal: own.signal,
          retries: 0,
          validate: validMode
        });
        const checked = await api.request("/api/v1/diagnostics/uart-log", {
          signal: own.signal,
          retries: 0,
          validate: validMode
        });
        if (checked.mode !== saved.mode || checked.mode !== value)
          throw new Error("Mode not verified");
        if (!destroyed) showMode(checked);
      } catch (_) {
        if (!destroyed)
          modeState.textContent =
            "We couldn’t read back the debug UART setting. Refresh before trying again.";
      } finally {
        modeBusy = false;
        if (modeController === own) modeController = null;
      }
    });
  }
  modeGroup.append(modeActions, modeState);
  container.append(modeGroup);
  exportButton.addEventListener("click", () => {
    if (snapshot)
      download(
        JSON.stringify(redactedDiagnostics(snapshot, caps, serialReport || undefined), null, 2),
        "esp-serial2wifi-diagnostics.json",
        "application/json"
      );
  });
  copyButton.addEventListener("click", async () => notice.show(await copy(pre)));
  const logDownload = el("button", "Download visible log", "quiet");
  logDownload.type = "button";
  logDownload.addEventListener("click", () =>
    download(log.text, "esp-serial2wifi-debug-log.txt", "text/plain;charset=utf-8")
  );
  logActions.append(logDownload);
  async function tick() {
    clearTimeout(timer);
    timer = null;
    if (destroyed || paused || document.hidden || controller || failures >= 3) return;
    controller = new AbortController();
    try {
      const packet = await api.request("/api/v1/diagnostics/log", {
        signal: controller.signal,
        retries: 0,
        validate: (v) => {
          try {
            decodeCapture(v);
            return true;
          } catch (_) {
            return false;
          }
        }
      });
      if (destroyed) return;
      let bytes = decodeCapture(packet);
      const previous = log.cursor;
      if (previous && previous.boot === packet.boot) {
        const seen = (previous.end - packet.start) >>> 0;
        if (seen <= bytes.length) {
          bytes = bytes.slice(seen);
          packet.start = previous.end;
        } else {
          packet.reason = "gap";
          packet.lost = (packet.start - previous.end) >>> 0;
        }
      }
      packet.data = btoa(String.fromCharCode(...bytes));
      log.ingest(packet);
      const tail = pre.scrollHeight - pre.scrollTop - pre.clientHeight < 4;
      pre.textContent = log.text;
      if (tail) pre.scrollTop = pre.scrollHeight;
      logState.textContent =
        "Debug log connected." +
        (log.trimmedBytes ? " Older messages have been removed from this view." : "");
      failures = 0;
    } catch (error) {
      if (!destroyed && !controller.signal.aborted) {
        failures++;
        logState.textContent =
          "The debug log isn’t responding. Earlier messages are still shown." +
          (failures >= 3 ? " Use Refresh device to retry." : "");
      }
    } finally {
      controller = null;
      if (!destroyed && !paused && !document.hidden && failures < 3) timer = setTimeout(tick, 1500);
    }
  }
  pause.addEventListener("click", () => {
    paused = !paused;
    pause.textContent = paused ? "Resume debug log" : "Pause debug log";
    if (paused) {
      clearTimeout(timer);
      controller?.abort();
    } else {
      failures = 0;
      tick();
    }
  });
  const visibility = () => {
    if (document.hidden) {
      clearTimeout(timer);
      controller?.abort();
    } else if (failures < 3) tick();
  };
  document.addEventListener("visibilitychange", visibility);
  const clock = setInterval(() => {
    if (seenAt)
      age.textContent = `Last updated ${Math.floor((Date.now() - seenAt) / 1000).toLocaleString()} seconds ago.`;
  }, 1000);
  return {
    updateStatus(s, c, report) {
      snapshot = s;
      caps = c;
      serialReport = report || serialErrorSummary(s);
      serialState.textContent = serialReport.label + ". " + serialReport.detail;
      seenAt = Date.now();
      exportButton.disabled = false;
      fields.replaceChildren();
      for (const [label, value] of [
        ["Firmware", s.firmware.version],
        ["SDK", s.firmware.sdk],
        ["Slot", s.firmware.slot],
        ["Reset reason", s.system.reset_reason],
        ["Uptime", uptime(s.system.uptime_ms)],
        ["Uptime (ms)", s.system.uptime_ms],
        ["Free heap (bytes)", s.system.heap_free_bytes],
        ["Wi-Fi state", s.wifi.state],
        ["IP", s.wifi.ip],
        ["MAC", s.wifi.mac],
        ["Station signal", signalText(s.wifi)],
        ["UART", `${s.serial.baud} / ${s.serial.configured_format || "unknown"}`]
      ])
        fields.append(el("dt", label), el("dd", value === null ? "Not reported" : String(value)));
      errorDetails.hidden = !serialReport.totals;
      errorFields.replaceChildren();
      if (serialReport.totals) {
        for (const [key, label] of serialErrorFields)
          errorFields.append(el("dt", label), el("dd", String(serialReport.totals[key])));
      }
      const reasons = {
        wrong_password: "Check the Wi-Fi password.",
        ap_not_found: "Make sure the Wi-Fi network is running on 2.4 GHz.",
        dhcp_timeout: "Check the router’s DHCP settings, or set a static IP address."
      };
      hint.textContent =
        reasons[s.wifi.reason] ||
        (s.system.reset_reason.includes("watchdog")
          ? "The watchdog restarted ESP Serial2WiFi. Check the debug log if this keeps happening."
          : "If serial output is garbled, check both devices’ baud and framing.");
      if (c) {
        for (const [key, label] of [
          ["mqtt", "MQTT"],
          ["rest", "MCU REST"],
          ["socket", "Socket"],
          ["syslog", "Syslog"]
        ])
          fields.append(
            el("dt", label + " support"),
            el("dd", c.features[key] ? "Available" : "Not included")
          );
      }
      if (s.mqtt?.enabled && s.mqtt.state === "failed")
        hint.textContent +=
          " MQTT couldn’t connect. Check the broker address, username and password.";
      if (!modeStarted) {
        modeStarted = true;
        loadMode();
      }
      if (!logStarted) {
        logStarted = true;
        tick();
      }
    },
    updateFailure() {
      if (serialReport) serialReport = { ...serialReport, stale: true };
      serialState.textContent =
        "Serial error readings are out of date. Refresh after reconnecting.";
    },
    refresh() {
      failures = 0;
      tick();
      loadMode();
    },
    canLeave: () =>
      !modeBusy || confirm("Leave this page? The debug UART change is still being saved."),
    destroy() {
      destroyed = true;
      modeController?.abort();
      clearTimeout(timer);
      clearInterval(clock);
      controller?.abort();
      document.removeEventListener("visibilitychange", visibility);
    }
  };
}
