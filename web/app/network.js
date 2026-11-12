import { DraftForm } from "./core.js";
import { reportedAddress } from "./status.js";
import {
  networkPath,
  validNetwork,
  validScan,
  validateNetwork,
  draftFrom,
  matchesNetwork,
  NetworkTransaction,
  wait,
  ipv4
} from "./network-model.js";
import { el } from "./core.js";
export function mountNetwork(container, api, notice) {
  let config = null,
    destroyed = false,
    scanController = null,
    changing = false,
    loadController = null,
    activeForm = null;
  const forms = [],
    transaction = new NetworkTransaction(api);
  let selectedMode = null,
    runtime = null;
  const tabs = new Map(),
    panels = new Map();
  const overview = el("section", "", "network-overview");
  overview.hidden = true;
  overview.setAttribute("aria-label", "Current connection");
  const intro = el("p", "Choose how ESP Serial2WiFi connects. Nothing changes until you save.");
  const recovery = el("section", "", "network-recovery");
  recovery.hidden = true;
  recovery.setAttribute("aria-label", "Connection progress");
  const guidance = el("p");
  const transactionState = el("p");
  transactionState.id = "network-stage";
  transactionState.setAttribute("role", "status");
  const candidates = el("div");
  recovery.append(transactionState, guidance, candidates);
  const loadMessage = el("p", "Loading network settings…");
  loadMessage.setAttribute("role", "status");
  const reload = el("button", "Reload saved settings", "quiet");
  reload.type = "button";
  reload.id = "network-reload";
  const body = el("div");
  body.id = "network-forms";
  container.replaceChildren(overview, loadMessage, body, reload);
  function showConnection() {
    if (!config) return;
    overview.hidden = false;
    const ap = config.mode === "ap",
      both = config.mode === "station_ap";
    const heading = el("h2", "Current connection"),
      mode = el(
        "strong",
        ap
          ? "Standalone Wi-Fi"
          : both
            ? "Wi-Fi network + recovery AP"
            : config.mode === "disabled"
              ? "Wi-Fi off"
              : "Wi-Fi network"
      );
    mode.id = "network-current-mode";
    const details = el("dl"),
      name = ap ? config.ap?.ssid : config.station?.ssid;
    details.append(
      el("dt", "Wi-Fi name"),
      el("dd", name || "Not set"),
      el("dt", "Device address"),
      el(
        "dd",
        (runtime?.wifi?.mode === config.mode
          ? reportedAddress(runtime.wifi, location.hostname)
          : null) || "Address unavailable"
      )
    );
    if (both && config.ap) details.append(el("dt", "Recovery Wi-Fi"), el("dd", config.ap.ssid));
    if (runtime?.wifi?.mode === config.mode) {
      const wifi = runtime.wifi;
      const state =
        wifi.state === "connected"
          ? "Connected"
          : wifi.state === "ready"
            ? "Ready"
            : wifi.state === "connecting"
              ? "Connecting"
              : wifi.state === "failed"
                ? "Connection failed"
                : "Waiting for status";
      details.append(el("dt", "Status"), el("dd", state));
    }
    overview.replaceChildren(heading, mode, details);
    const apSave = body.querySelector("#network-ap [type=submit]");
    if (apSave)
      apSave.textContent = ap ? "Save Wi-Fi settings" : "Save & switch to standalone Wi-Fi";
  }
  function updateTabs() {
    const scan = body.querySelector("[data-scan]");
    if (scan) scan.disabled = changing || !!scanController;
    for (const button of tabs.values()) button.disabled = changing || forms.some((f) => f.busy);
  }
  function selectMode(mode, focus = false) {
    if (!panels.has(mode) || changing || forms.some((f) => f.busy)) return;
    selectedMode = mode;
    for (const [key, panel] of panels) {
      const button = tabs.get(key),
        chosen = key === mode;
      panel.hidden = !chosen;
      button.setAttribute("aria-selected", String(chosen));
      button.tabIndex = chosen ? 0 : -1;
    }
    if (focus) tabs.get(mode).focus();
  }
  transaction.addEventListener("stage", (event) => {
    recovery.hidden = false;
    transactionState.dataset.state = event.detail.state;
    transactionState.textContent = event.detail.message;
    if (activeForm) activeForm.dataset.setupState = event.detail.state;
  });
  function candidate(label, host) {
    if (!host) return;
    const p = el("p", label + " "),
      a = el("a", host);
    a.href = "http://" + host + "/index.html#/network";
    a.rel = "noopener";
    a.target = "_blank";
    p.append(a);
    candidates.append(p);
  }
  function reconnect(draft, kind) {
    candidates.replaceChildren();
    if (kind === "ap" || (kind === "station" && !draft.dhcp))
      candidate("New IP address:", draft.address);
    if (config?.identity?.mdns && config.identity.hostname)
      candidate("mDNS address:", config.identity.hostname + ".local");
    if (config?.ap?.ssid)
      guidance.textContent =
        "If you lose the connection, look for the recovery AP “" +
        config.ap.ssid +
        "”. Reconnect to change the Wi-Fi settings or switch back to Standalone Wi-Fi.";
    if (kind === "ap")
      guidance.textContent =
        "After saving, reconnect to “" +
        draft.ssid +
        "” with your chosen password, then open " +
        draft.address +
        ".";
  }
  function control(form, name, label, { type = "text", options, value, max, help } = {}) {
    const wrapper = el("div", "", "form-field"),
      id = (form.dataset.fieldPrefix || form.id) + "-" + name;
    const lab = el("label", label);
    lab.htmlFor = id;
    const field = el(options ? "select" : "input");
    field.id = id;
    field.name = name;
    if (options)
      for (const [v, t] of options) {
        const option = el("option", t);
        option.value = v;
        field.append(option);
      }
    else {
      field.type = type;
      field.autocomplete = type === "password" ? "new-password" : "off";
      if (max) field.maxLength = max;
    }
    if (value !== undefined) field.value = value;
    const error = el("p", "", "field-error");
    error.dataset.error = name;
    error.id = id + "-error";
    error.hidden = true;
    field.setAttribute("aria-describedby", error.id + (help ? " " + id + "-help" : ""));
    wrapper.append(lab, field);
    if (help) {
      const hint = el("p", help, "field-hint");
      hint.id = id + "-help";
      wrapper.append(hint);
    }
    wrapper.append(error);
    form.append(wrapper);
    return field;
  }
  function section(kind, title, explanation) {
    const identity = kind === "identity",
      section = el(identity ? "details" : "section", "", "network-section");
    if (identity) {
      section.classList.add("network-identity");
      section.id = "network-identity-settings";
      section.append(el("summary", title));
    } else {
      section.id = "network-" + kind + "-panel";
      section.setAttribute("role", "tabpanel");
      section.setAttribute("aria-labelledby", "network-tab-" + kind);
      panels.set(kind, section);
    }
    section.append(el("p", explanation));
    const form = el("form");
    form.id = "network-" + kind;
    form.noValidate = true;
    form.setAttribute("aria-label", title);
    section.append(form);
    body.append(section);
    return form;
  }
  function advanced(form, title) {
    const details = el("details", "", "network-advanced");
    details.id = form.id + "-advanced";
    details.dataset.fieldPrefix = form.id;
    details.append(el("summary", title));
    form.append(details);
    return details;
  }
  function credentials(form, kind) {
    const ssid = control(form, "ssid", "Wi-Fi name (SSID)", {
      max: 32,
      help:
        kind === "station"
          ? "Choose a scanned network or enter its name."
          : "Shown in your Wi-Fi list."
    });
    const action = control(form, "password_action", "Password", {
      options: [
        [
          "keep",
          config[kind]?.password_configured ? "Use saved password" : "No password (current setting)"
        ],
        ["replace", "Enter a new password"],
        ["clear", "No password (open network)"]
      ]
    });
    const pw = control(form, "password", "New password", {
      type: "password",
      max: kind === "station" ? 64 : 63,
      help:
        kind === "station"
          ? "Enter the password for this Wi-Fi network."
          : "Use at least 8 characters."
    });
    const show = el("button", "Show password", "quiet");
    show.type = "button";
    show.dataset.passwordToggle = "";
    show.setAttribute("aria-pressed", "false");
    show.addEventListener("click", () => {
      const visible = pw.type === "password";
      pw.type = visible ? "text" : "password";
      show.textContent = visible ? "Hide password" : "Show password";
      show.setAttribute("aria-pressed", String(visible));
    });
    pw.dataset.secret = "true";
    pw.closest(".form-field").classList.add("network-new-password");
    form.append(show);
    const update = () => {
      action.querySelector("[value=keep]").textContent = config[kind]?.password_configured
        ? "Use saved password"
        : "No password (current setting)";
      const replacing = action.value === "replace";
      pw.closest(".form-field").hidden = !replacing;
      show.hidden = !replacing;
    };
    const change = (event) => {
      const security = form.elements.security;
      if (!security) return;
      if (event.target === action) {
        if (action.value === "clear") security.value = "open";
        else if (action.value === "replace" && security.value === "open")
          security.value = kind === "station" ? "protected" : "wpa2";
        else if (action.value === "keep")
          security.value =
            kind === "station"
              ? config.station?.password_configured
                ? "protected"
                : "open"
              : config.ap.security;
      } else if (event.target === security) {
        if (security.value === "open") action.value = "clear";
        else if (action.value === "clear" || !config[kind]?.password_configured)
          action.value = "replace";
      } else if (
        event.target === ssid &&
        kind === "station" &&
        ssid.value !== config.station?.ssid &&
        action.value === "keep"
      ) {
        action.value = "replace";
        security.value = "protected";
      }
      if (action.value !== "replace") {
        pw.value = "";
        pw.type = "password";
        show.textContent = "Show password";
        show.setAttribute("aria-pressed", "false");
      }
      update();
    };
    form.addEventListener("input", change);
    form.addEventListener("change", change);
    pw.refresh = update;
    update();
    return pw;
  }
  function attach(kind, form, pw) {
    const message = el("p");
    message.setAttribute("role", "status");
    form.append(message);
    const actions = el("div", "", "form-actions"),
      save = el(
        "button",
        kind === "station"
          ? "Connect to Wi-Fi"
          : kind === "ap"
            ? "Save standalone Wi-Fi"
            : "Save hostname & discovery"
      );
    save.type = "submit";
    const cancel = el("button", "Discard changes", "quiet");
    cancel.type = "button";
    cancel.dataset.cancel = "";
    actions.append(save, cancel);
    form.append(actions);
    let saved = config;
    const draft = new DraftForm(form, {
      allowCleanSave: kind !== "identity",
      read: async (signal) => {
        const c =
          saved || (await api.request(networkPath, { signal, validate: validNetwork, retries: 0 }));
        saved = null;
        config = c;
        return draftFrom(kind, c);
      },
      validate: (v) => {
        message.hidden = false;
        const errors = validateNetwork(kind, v);
        if (
          kind !== "identity" &&
          v.password_action === "keep" &&
          config?.[kind]?.password_configured !== (v.security !== "open")
        )
          errors.password_action =
            "Choose Enter a new password, or No password for an open network.";
        if (kind === "station" && v.password_action === "keep" && config?.station?.ssid !== v.ssid)
          errors.password_action =
            "Enter the password for this network, or choose No password if it’s open.";
        for (const details of form.querySelectorAll("details"))
          if (
            Object.keys(errors).some((name) =>
              details.querySelector('[name="' + CSS.escape(name) + '"]')
            )
          )
            details.open = true;
        return errors;
      },
      write: async (values, signal) => {
        if (changing) {
          const error = new Error("Network busy");
          error.code = "NETWORK_BUSY";
          throw error;
        }
        message.hidden = true;
        changing = true;
        cancelScan();
        activeForm = form;
        form.append(recovery);
        forms.forEach((f) => f.setLocked(true));
        updateTabs();
        try {
          if (pw) {
            pw.type = "password";
            const toggle = form.querySelector("[data-password-toggle]");
            toggle.textContent = "Show password";
            toggle.setAttribute("aria-pressed", "false");
          }
          reconnect(values, kind);
          const receipt = await transaction.run(kind, values, signal);
          saved = transaction.latest;
          return receipt;
        } catch (error) {
          const current = transaction.latest;
          if (
            current?.operation.state === "applied" &&
            current.operation.id === transaction.receipt?.id &&
            matchesNetwork(kind, values, current)
          ) {
            config = current;
            draft.rebase(draftFrom(kind, current));
          }
          throw error;
        } finally {
          changing = false;
          if (!destroyed) {
            forms.forEach((f) => f.setLocked(false));
            updateTabs();
          }
        }
      },
      verify: (values, committed, receipt) =>
        receipt?.state === "accepted" &&
        transaction.state === "connected" &&
        transaction.latest?.operation.id === receipt.id &&
        matchesNetwork(kind, values, transaction.latest),
      message,
      notice,
      describeError: (error) =>
        error.code === "NETWORK_BUSY"
          ? "Another network change is still running. Your edits are here when it finishes."
          : transactionState.textContent ||
            "We couldn’t confirm the change. Your edits are still here.",
      describe: () =>
        transaction.state === "connected"
          ? transactionState.textContent
          : "Settings could not be verified."
    });
    form.dataset.setupState = "idle";
    form.addEventListener("input", () => {
      if (!draft.busy) {
        form.dataset.setupState = "credentials";
        if (activeForm === form) {
          recovery.hidden = true;
          transactionState.textContent = "";
          message.textContent = "";
        }
      }
    });
    if (kind === "station")
      draft.addEventListener("state", () => {
        if (!draft.busy && ["recovery", "rejected", "unverified"].includes(form.dataset.setupState))
          save.textContent = "Try connecting again";
      });
    draft.addEventListener("state", () => {
      pw?.refresh();
      updateTabs();
      showConnection();
    });
    forms.push(draft);
    draft.load();
    return draft;
  }
  function cancelScan() {
    if (!scanController) return;
    scanController.abort();
    scanController = null;
    const results = body.querySelector("[data-scan-results]");
    if (results) {
      results.replaceChildren();
      results.closest("form").dataset.scanState = "cancelled";
    }
  }
  async function scan(form, pw) {
    if (destroyed || changing || loadController) return;
    scanController?.abort();
    const controller = new AbortController();
    scanController = controller;
    const button = form.querySelector("[data-scan]"),
      output = form.querySelector("[data-scan-results]");
    button.disabled = true;
    form.dataset.scanState = "scanning";
    output.replaceChildren(
      el("p", "Scanning 2.4 GHz networks. The recovery AP may pause briefly.")
    );
    try {
      let response = { state: "scanning" };
      try {
        response = await api.request(networkPath + "/scan", {
          method: "POST",
          body: new URLSearchParams({ action: "start" }),
          signal: controller.signal,
          retries: 0,
          validate: validScan
        });
      } catch (error) {
        if (!error.ambiguous) throw error;
      }
      const deadline = Date.now() + 25000;
      for (let i = 0; response.state === "scanning" && i < 18 && Date.now() < deadline; i++) {
        await wait(1000, controller.signal);
        if (document.hidden) continue;
        try {
          response = await api.request(networkPath + "/scan/0", {
            signal: controller.signal,
            retries: 0,
            timeout: 2500,
            validate: validScan
          });
        } catch (error) {
          if (controller.signal.aborted || error.kind === "malformed" || error.kind === "http")
            throw error;
          output.replaceChildren(
            el(
              "p",
              "Still waiting for the scan. If Wi-Fi disconnected, reconnect to the recovery AP."
            )
          );
        }
      }
      if (response.state !== "complete") throw new Error("Scan did not complete");
      const networks = [...response.networks];
      for (let page = 1; page < Math.ceil(response.retained / 4); page++) {
        const part = await api.request(networkPath + "/scan/" + page, {
          signal: controller.signal,
          retries: 0,
          validate: validScan
        });
        if (part.state !== "complete" || part.total !== response.total)
          throw new Error("Scan expired");
        networks.push(...part.networks);
      }
      if (destroyed || controller.signal.aborted || scanController !== controller) return;
      form.dataset.scanState = networks.length ? "selection" : "empty";
      output.replaceChildren();
      if (!networks.length)
        output.append(
          el(
            "p",
            "No networks found. Rescan, move closer to the access point, or enter a hidden 2.4 GHz SSID manually."
          )
        );
      else {
        output.append(
          el(
            "p",
            "Networks are sorted by signal strength. The BSSID identifies access points with the same name. " +
              (response.truncated ? "Showing the strongest 16; enter another SSID manually." : "")
          )
        );
        const list = el("ul", "", "scan-list");
        for (const ap of networks) {
          const li = el("li"),
            select = el("button", "", "quiet");
          select.type = "button";
          select.append(
            el("strong", ap.ssid || "Hidden network"),
            el(
              "span",
              ap.security.toUpperCase() + " · " + ap.rssi_dbm + " dBm · Channel " + ap.channel
            ),
            el("small", ap.bssid)
          );
          select.disabled = ["wep", "unsupported"].includes(ap.security);
          if (select.disabled) li.append(el("p", "This Wi-Fi security type isn’t supported."));
          select.addEventListener("click", () => {
            const fields = form.elements;
            fields.ssid.value = ap.ssid;
            fields.bssid.value = ap.bssid;
            fields.security.value = ap.security === "open" ? "open" : "protected";
            fields.password_action.value = ap.security === "open" ? "clear" : "replace";
            pw.value = "";
            form.dispatchEvent(new Event("input"));
            (ap.ssid ? pw : fields.ssid).focus();
            output
              .querySelectorAll("[aria-pressed]")
              .forEach((n) => n.setAttribute("aria-pressed", "false"));
            select.setAttribute("aria-pressed", "true");
          });
          select.setAttribute("aria-pressed", "false");
          li.append(select);
          list.append(li);
        }
        output.append(list);
      }
    } catch (error) {
      if (!controller.signal.aborted) {
        form.dataset.scanState = "error";
        output.replaceChildren(
          el("p", "The scan didn’t finish. Try again, or enter the Wi-Fi name yourself.")
        );
      }
    } finally {
      if (scanController === controller) {
        scanController = null;
        if (!destroyed) button.disabled = changing;
      }
    }
  }
  function build() {
    body.replaceChildren();
    tabs.clear();
    panels.clear();
    recovery.hidden = true;
    const title = el("h2", "Wi-Fi setup"),
      tablist = el("div", "", "network-mode-tabs");
    tablist.setAttribute("role", "tablist");
    tablist.setAttribute("aria-label", "Connection setup");
    body.append(title, intro, tablist);
    for (const [kind, label] of [
      ["station", "Join a network"],
      ["ap", "Standalone Wi-Fi"]
    ])
      if (config[kind]) {
        const button = el("button", label, "quiet");
        button.type = "button";
        button.id = "network-tab-" + kind;
        button.setAttribute("role", "tab");
        button.setAttribute("aria-controls", "network-" + kind + "-panel");
        button.setAttribute("aria-selected", "false");
        button.tabIndex = -1;
        button.addEventListener("click", () => selectMode(kind));
        button.addEventListener("keydown", (event) => {
          const keys = [...tabs.keys()],
            index = keys.indexOf(kind);
          let next;
          if (event.key === "ArrowRight") next = keys[(index + 1) % keys.length];
          else if (event.key === "ArrowLeft") next = keys[(index + keys.length - 1) % keys.length];
          else if (event.key === "Home") next = keys[0];
          else if (event.key === "End") next = keys.at(-1);
          if (next) {
            event.preventDefault();
            selectMode(next, true);
          }
        });
        tabs.set(kind, button);
        tablist.append(button);
      }
    if (config.station) {
      const form = section(
        "station",
        "Join your Wi-Fi network",
        "Connect ESP Serial2WiFi to your router’s 2.4 GHz Wi-Fi, then access it from devices on that network."
      );
      const scanButton = el("button", "Scan networks", "quiet");
      scanButton.type = "button";
      scanButton.dataset.scan = "";
      const result = el("div");
      result.dataset.scanResults = "";
      result.setAttribute("aria-live", "polite");
      form.append(scanButton, result);
      const pw = credentials(form, "station");
      scanButton.addEventListener("click", () => scan(form, pw));
      const extra = advanced(form, "IP address & advanced settings");
      control(extra, "security", "Wi-Fi security", {
        options: [
          ["protected", "WPA / WPA2 personal"],
          ["open", "Open (no password)"]
        ]
      });
      control(extra, "bssid", "Access point BSSID (optional)", {
        max: 17,
        help: "Leave blank to connect to any access point with this Wi-Fi name."
      });
      const dhcp = control(extra, "dhcp", "Get an IP address automatically (DHCP)", {
        type: "checkbox"
      });
      const staticArea = el("fieldset");
      staticArea.append(el("legend", "Static IP address"));
      const addressForm = el("div");
      addressForm.id = form.id + "-ip";
      for (const [name, label] of [
        ["address", "IPv4 address"],
        ["mask", "Subnet mask"],
        ["gateway", "Gateway"],
        ["dns1", "Primary DNS"],
        ["dns2", "Secondary DNS"]
      ])
        control(addressForm, name, label, {
          max: 15,
          help: ["gateway", "dns1", "dns2"].includes(name)
            ? "Enter 0.0.0.0 to leave this unset."
            : undefined
        });
      const preview = el("p");
      preview.dataset.addressPreview = "";
      staticArea.append(
        addressForm,
        preview,
        el(
          "p",
          "Choose an unused address on the same subnet as your gateway. A wrong address can prevent you from reconnecting."
        )
      );
      extra.append(staticArea);
      const update = () => {
        staticArea.hidden = dhcp.checked;
        if (form.dataset.state !== "loading" && !dhcp.checked) extra.open = true;
        const ip = form.elements.address.value;
        preview.textContent =
          ipv4(ip) !== null
            ? "After saving, open http://" + ip + "/index.html#/network"
            : "Enter an IPv4 address to see the new URL.";
      };
      form.addEventListener("change", update);
      form.addEventListener("input", update);
      form.append(
        el(
          "p",
          "Connecting can disconnect this page and any serial sessions. Keep this page open while ESP Serial2WiFi reconnects.",
          "field-hint"
        )
      );
      const draft = attach("station", form, pw);
      draft.addEventListener("state", update);
      update();
    }
    if (config.ap) {
      const form = section(
        "ap",
        "Standalone recovery Wi-Fi",
        "Connect your computer or phone directly to ESP Serial2WiFi. This works without a router."
      );
      const pw = credentials(form, "ap"),
        extra = advanced(form, "IP address, channel & advanced settings");
      const ip = control(extra, "address", "Device IPv4 address", {
        max: 15,
        help: "Used when you connect directly to this Wi-Fi network. Default: 192.168.4.1."
      });
      extra.append(
        el(
          "p",
          "Subnet mask: 255.255.255.0. Client addresses are assigned automatically.",
          "field-hint"
        )
      );
      const addressHint = el("p", "", "field-hint");
      const updateAddress = () => {
        const valid = ipv4(ip.value) !== null;
        addressHint.textContent = valid
          ? "After saving, reconnect to this Wi-Fi network and open http://" + ip.value + "/."
          : "Enter an IPv4 address.";
      };
      form.addEventListener("input", updateAddress);
      form.addEventListener("change", updateAddress);
      control(extra, "channel", "Wi-Fi channel", {
        options: Array.from({ length: 13 }, (_, i) => [String(i + 1), String(i + 1)])
      });
      control(extra, "security", "Wi-Fi security", {
        options: [
          ["wpa2", "WPA2 personal"],
          ["mixed", "WPA / WPA2 mixed"],
          ["wpa", "WPA personal (legacy)"],
          ["open", "Open (no password)"]
        ]
      });
      control(extra, "hidden", "Hide the Wi-Fi name", {
        type: "checkbox",
        help: "You’ll need to enter the name manually when connecting."
      });
      form.append(addressHint);
      const draft = attach("ap", form, pw);
      draft.addEventListener("state", updateAddress);
      updateAddress();
    }
    const identity = section(
      "identity",
      "Hostname & discovery",
      "Your router uses the hostname to identify ESP Serial2WiFi. Restart ESP Serial2WiFi after changing the hostname or mDNS settings."
    );
    control(identity, "hostname", "Hostname", { max: 31 });
    control(identity, "mdns", "Enable mDNS (.local names)", { type: "checkbox" });
    control(identity, "mdns_name", "Discovery service name", { max: 31 });
    attach("identity", identity);
    selectMode(
      selectedMode && panels.has(selectedMode)
        ? selectedMode
        : config.mode === "ap" && panels.has("ap")
          ? "ap"
          : panels.has("station")
            ? "station"
            : "ap"
    );
    showConnection();
  }
  async function load() {
    if (loadController || forms.some((f) => f.busy)) return;
    if (forms.some((f) => f.dirty)) {
      if (!globalThis.confirm("Discard your changes and reload the saved network settings?"))
        return;
    }
    cancelScan();
    forms.forEach((f) => f.setLocked(true));
    body.inert = true;
    const controller = new AbortController();
    loadController = controller;
    reload.disabled = true;
    try {
      config = await api.request(networkPath, {
        validate: validNetwork,
        retries: 0,
        signal: controller.signal
      });
      if (destroyed || controller.signal.aborted) return;
      forms.splice(0).forEach((f) => f.destroy());
      loadMessage.hidden = true;
      build();
    } catch (error) {
      if (!destroyed && !controller.signal.aborted) {
        loadMessage.hidden = false;
        loadMessage.textContent =
          "Couldn’t load network settings. Check the connection, then reload.";
      }
    } finally {
      loadController = null;
      if (!destroyed) {
        body.inert = false;
        forms.forEach((f) => f.setLocked(false));
        reload.disabled = false;
      }
    }
  }
  reload.addEventListener("click", load);
  load();
  return {
    canLeave: () =>
      !forms.some((f) => f.busy || f.dirty) ||
      globalThis.confirm(
        "Leave Network? You’ll lose unsaved changes. Any save already sent to ESP Serial2WiFi will continue."
      ),
    updateStatus: (status) => {
      runtime = status;
      showConnection();
    },
    refresh: () => {
      if (!forms.some((f) => f.busy || f.dirty)) load();
    },
    destroy: () => {
      destroyed = true;
      cancelScan();
      loadController?.abort();
      forms.forEach((f) => f.destroy());
    }
  };
}
