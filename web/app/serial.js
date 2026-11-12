import { createTerminalConsole } from "./terminal.js";
import {
  DraftForm,
  copy as copyText,
  download as downloadText,
  readPreference,
  savePreference
} from "./core.js";
import { prepareHex, ProgramTransaction } from "./program-model.js";
import { reportedAddress } from "./status.js";
import {
  CaptureLog,
  CommandHistory,
  commandBody,
  validCapture,
  validSerial,
  validateUART
} from "./serial-model.js";
let retainedTerminal = null;
import { el } from "./core.js";
export function mountSerial(container, api, notice, initialStatus, initialCapabilities = null) {
  let destroyed = false,
    timer = null,
    flight = null,
    failures = 0,
    follow = true,
    paused = false,
    config = null,
    loading = null,
    changing = false,
    uartBusy = false;
  let configReadAt = 0,
    configRetry = null,
    deviceAddress = null;
  const lifetime = new AbortController();
  const log = new CaptureLog(),
    history = new CommandHistory(),
    programTx = new ProgramTransaction(api);
  let prepared = null,
    fileGeneration = 0,
    programController = null,
    programCapabilities = initialCapabilities;
  const intro = el("p", "The terminal opens automatically when the serial port is available.");
  const errorState = el("p");
  errorState.id = "serial-health";
  errorState.hidden = true;
  errorState.setAttribute("role", "status");
  const capture = el("section");
  capture.setAttribute("aria-label", "Serial console");
  const toolbar = el("div", "", "form-actions"),
    followButton = el("button", "Following output"),
    pause = el("button", "Pause capture"),
    retry = el("button", "Retry capture", "quiet");
  followButton.type = pause.type = retry.type = "button";
  followButton.setAttribute("aria-pressed", "true");
  retry.hidden = true;
  toolbar.append(followButton, pause, retry);
  const pre = el("pre", "", "serial-output");
  pre.id = "serial-output";
  pre.tabIndex = 0;
  pre.setAttribute("aria-label", "UTF-8 serial capture");
  pre.setAttribute("aria-live", "off");
  const state = el("p", "Loading device capture…");
  state.id = "serial-capture-state";
  state.setAttribute("role", "status");
  const trimmed = el("p");
  trimmed.id = "serial-trimmed";
  trimmed.hidden = true;
  const actions = el("div", "", "form-actions"),
    copy = el("button", "Copy view", "quiet"),
    download = el("button", "Download view", "quiet"),
    clear = el("button", "Clear browser view", "quiet"),
    clearDevice = el("button", "Clear device capture", "quiet");
  for (const b of [copy, download, clear, clearDevice]) {
    b.type = "button";
    actions.append(b);
  }
  const raw = el("details");
  raw.id = "serial-raw-capture";
  raw.className = "serial-disclosure";
  raw.open = readPreference("serial-mode", ["raw", "terminal"], "terminal") === "raw";
  const terminalHandlers = {
    blocked: () =>
      !config
        ? "Loading serial settings…"
        : raw.open
          ? "Terminal paused while raw serial tools are open."
          : changing || uartBusy
            ? "Applying serial settings…"
            : programTx.busy
              ? "Terminal paused during MCU programming."
              : false,
    activeChanged: (active) => {
      if (active) stop();
      else if (!destroyed && !paused) tick();
    }
  };
  if (retainedTerminal?.failed) {
    retainedTerminal.destroy();
    retainedTerminal = null;
  }
  const terminal =
    retainedTerminal || (retainedTerminal = createTerminalConsole(api, terminalHandlers));
  terminal.attach(terminalHandlers);
  raw.append(
    el("summary", "Raw serial tools"),
    el(
      "p",
      "Read and send text or hex bytes without terminal emulation or automatic query replies. The live terminal pauses while these tools are open.",
      "field-hint"
    )
  );
  const formatLabel = el("label", "Raw format"),
    format = el("select");
  format.id = "serial-raw-format";
  formatLabel.htmlFor = format.id;
  for (const [v, t] of [
    ["text", "UTF-8 text"],
    ["hex", "Hex bytes"]
  ]) {
    const o = el("option", t);
    o.value = v;
    format.append(o);
  }
  raw.append(formatLabel, format);
  format.value = readPreference("raw-format", ["hex", "text"], "text");
  const showFormat = () => {
    savePreference("raw-format", format.value);
    if (format.value === "hex") suffix.value = "none";
    commandForm.setAttribute("aria-label", format.value === "hex" ? "Send bytes" : "Send command");
    pre.setAttribute(
      "aria-label",
      format.value === "hex" ? "Hex serial capture" : "UTF-8 serial capture"
    );
    send.textContent = format.value === "hex" ? "Send bytes" : "Send command";
    commandLabel.textContent = format.value === "hex" ? "Hex bytes" : "Command";
    sendState.textContent =
      format.value === "hex"
        ? "Enter byte pairs, for example 00 FF 1B. Up to 256 bytes including the selected line ending."
        : "Up to 256 UTF-8 bytes, including the line ending. Use Up/Down to recall previous commands.";
    show();
  };
  format.addEventListener("change", showFormat);
  capture.append(terminal.root);
  const commandForm = el("form");
  commandForm.id = "serial-command";
  commandForm.setAttribute("aria-label", "Send command");
  const commandLabel = el("label", "Command");
  commandLabel.htmlFor = "serial-command-text";
  const command = el("input");
  command.type = "text";
  command.id = "serial-command-text";
  command.autocomplete = "off";
  command.spellcheck = false;
  const suffixLabel = el("label", "Line ending");
  suffixLabel.htmlFor = "serial-suffix";
  const suffix = el("select");
  suffix.id = "serial-suffix";
  for (const [v, t] of [
    ["none", "Nothing"],
    ["cr", "CR"],
    ["lf", "LF"],
    ["crlf", "CR + LF"]
  ]) {
    const o = el("option", t);
    o.value = v;
    suffix.append(o);
  }
  suffix.value = "crlf";
  const send = el("button", "Send command");
  send.type = "submit";
  send.disabled = true;
  const sendState = el(
    "p",
    "Up to 256 UTF-8 bytes, including the line ending. Use Up/Down to recall previous commands."
  );
  sendState.setAttribute("role", "status");
  commandForm.append(commandLabel, command, suffixLabel, suffix, send, sendState);
  raw.append(
    commandForm,
    el("h3", "Raw capture"),
    toolbar,
    pre,
    state,
    trimmed,
    actions,
    el(
      "p",
      "Open these tools to collect raw output. This browser keeps up to 1 MiB or 10,000 lines.",
      "field-hint"
    )
  );
  const connections = el("details", "", "serial-disclosure");
  connections.id = "serial-tcp";
  const endpoints = el("div"),
    clients = el("p"),
    telnetCommand = el("pre", "", "serial-guide");
  connections.append(
    el("summary", "Telnet and TCP access"),
    endpoints,
    telnetCommand,
    clients,
    el(
      "p",
      "Use one interactive client at a time. The web terminal waits while a TCP client is connected."
    )
  );
  const details = el("details", "", "serial-disclosure");
  details.id = "serial-uart-details";
  details.append(el("summary", "Serial settings"));
  const form = el("form");
  form.id = "serial-uart";
  form.noValidate = true;
  form.setAttribute("aria-label", "UART settings");
  const fields = [
    ["baud", "Baud rate", null],
    [
      "data_bits",
      "Data bits",
      [
        ["5", "5"],
        ["6", "6"],
        ["7", "7"],
        ["8", "8"]
      ]
    ],
    [
      "parity",
      "Parity",
      [
        ["N", "None"],
        ["O", "Odd"],
        ["E", "Even"]
      ]
    ],
    [
      "stop_bits",
      "Stop bits",
      [
        ["1", "1"],
        ["1.5", "1.5"],
        ["2", "2"]
      ]
    ]
  ];
  for (const [name, label, choices] of fields) {
    const wrap = el("div", "", "form-field"),
      lab = el("label", label),
      field = el(choices ? "select" : "input");
    lab.htmlFor = "serial-" + name;
    field.id = lab.htmlFor;
    field.name = name;
    if (choices)
      for (const [v, t] of choices) {
        const option = el("option", t);
        option.value = v;
        field.append(option);
      }
    else {
      field.type = "text";
      field.inputMode = "numeric";
      field.maxLength = 7;
    }
    const error = el("p", "", "field-error");
    error.id = field.id + "-error";
    error.dataset.error = name;
    error.hidden = true;
    field.setAttribute("aria-describedby", error.id);
    wrap.append(lab, field, error);
    form.append(wrap);
  }
  const uartMessage = el("p");
  uartMessage.setAttribute("role", "status");
  const save = el("button", "Save UART settings");
  save.type = "submit";
  const cancel = el("button", "Discard changes", "quiet");
  cancel.type = "button";
  cancel.dataset.cancel = "";
  const formActions = el("div", "", "form-actions");
  formActions.append(save, cancel);
  form.append(
    el(
      "p",
      "Use the same baud rate and format as the attached device. Changes take effect as soon as you save."
    ),
    formActions,
    uartMessage
  );
  details.append(
    form,
    el(
      "p",
      "Match the attached device’s serial format. Terminal display dimensions are under Terminal options.",
      "field-hint"
    )
  );
  const resetSection = el("section", "", "network-section");
  const reset = el("button", "Reset attached MCU", "quiet");
  reset.type = "button";
  reset.disabled = true;
  const resetState = el("p", "Loading the attached-MCU reset pin…");
  resetState.setAttribute("role", "status");
  resetSection.append(
    el("h2", "Attached MCU"),
    el("p", "Reset the attached MCU using its reset pin. ESP Serial2WiFi will keep running."),
    reset,
    resetState
  );
  const programming = el("section", "", "network-section");
  programming.id = "serial-programming";
  programming.append(
    el("h2", "Program attached MCU"),
    el(
      "p",
      "Built-in browser programmer: ATmega328P with Optiboot and a 512-byte bootloader area. This replaces the attached MCU application; it does not update ESP Serial2WiFi."
    )
  );
  const fileLabel = el("label", "Application firmware (Intel HEX)");
  fileLabel.htmlFor = "serial-program-file";
  const file = el("input");
  file.id = fileLabel.htmlFor;
  file.type = "file";
  file.accept = ".hex";
  const fileInfo = el("p", "Select and validate the file before requesting an MCU reset.");
  fileInfo.id = "serial-program-file-info";
  fileInfo.setAttribute("role", "status");
  const programReadiness = el("p", "Checking programmer availability…");
  programReadiness.id = "serial-program-readiness";
  const programButton = el("button", "Program attached ATmega328P");
  programButton.type = "button";
  programButton.id = "serial-program-start";
  programButton.disabled = true;
  const programState = el("p");
  programState.id = "serial-program-state";
  programState.setAttribute("role", "status");
  programming.append(
    fileLabel,
    file,
    fileInfo,
    el(
      "p",
      "Writes complete 128-byte pages, filling any gaps in those pages with FF. The bootloader and other pages are left alone. TCP access pauses while programming.",
      "field-hint"
    ),
    programReadiness,
    programButton,
    programState
  );
  const guide = el("details", "", "network-section");
  guide.id = "serial-external-guide";
  guide.append(el("summary", "External programmers and other AVR targets"));
  guide.append(
    el(
      "p",
      "For another MCU, use the appropriate programmer and bootloader. Match the UART baud rate to the bootloader and close other serial clients before flashing. The AVRDUDE examples below verify the written data."
    )
  );
  const guideLabel = el("label", "Example target / bootloader");
  guideLabel.htmlFor = "serial-guide-target";
  const target = el("select");
  target.id = guideLabel.htmlFor;
  for (const [value, label] of [
    ["uno", "ATmega328P / Arduino Optiboot (115200 baud)"],
    ["mega", "ATmega2560 / Wiring STK500v2 (115200 baud)"]
  ]) {
    const option = el("option", label);
    option.value = value;
    target.append(option);
  }
  const commandGuide = el("pre", "", "serial-guide");
  commandGuide.id = "serial-guide-command";
  commandGuide.tabIndex = 0;
  const copyGuide = el("button", "Copy programmer command", "quiet");
  copyGuide.type = "button";
  const guideState = el("p");
  guideState.setAttribute("role", "status");
  guide.append(
    guideLabel,
    target,
    commandGuide,
    copyGuide,
    el(
      "p",
      "Replace firmware.hex with your application file and confirm its MCU/bootloader and baud before running. Port 2323 enters programming mode and pulses the configured target reset/ISP signals. Check the wiring and bootloader for your board.",
      "field-hint"
    ),
    guideState
  );
  const guideLink = el("a", "AVRDUDE command reference");
  guideLink.href = "https://avrdudes.github.io/avrdude/8.0/avrdude.pdf";
  guideLink.target = "_blank";
  guideLink.rel = "noopener";
  guide.append(guideLink);
  const updateGuide = () => {
    copyGuide.disabled = !deviceAddress;
    commandGuide.textContent = deviceAddress
      ? "avrdude -p " +
        (target.value === "mega" ? "atmega2560 -c wiring" : "atmega328p -c arduino") +
        " -P net:" +
        deviceAddress +
        ":2323 -b 115200 -D -U flash:w:firmware.hex:i"
      : "Waiting for the device IP address…";
  };
  target.addEventListener("change", updateGuide);
  updateGuide();
  copyGuide.addEventListener("click", async () => {
    guideState.textContent = await copyText(commandGuide);
  });
  const mcu = el("details", "", "serial-disclosure");
  mcu.id = "serial-mcu-tools";
  mcu.append(el("summary", "MCU programming and reset"), resetSection, programming, guide);
  const tools = el("div", "", "serial-tools");
  tools.append(terminal.options, details, connections, raw, mcu);
  container.replaceChildren(intro, errorState, capture, tools);
  details.addEventListener("toggle", () => {
    if (details.open) refresh();
  });
  raw.addEventListener("toggle", () => {
    if (destroyed) return;
    savePreference("serial-mode", raw.open ? "raw" : "terminal");
    terminal.refresh();
    if (raw.open) tick();
    else stop();
  });
  function show() {
    const position = pre.scrollTop;
    pre.textContent = format.value === "hex" ? log.hex : log.text;
    pre.scrollTop = follow ? pre.scrollHeight : position;
    trimmed.hidden = !(format.value === "hex" ? log.hexTrimmedBytes : log.trimmedBytes);
    trimmed.textContent =
      format.value === "hex"
        ? `Locally trimmed ${log.hexTrimmedBytes.toLocaleString()} raw bytes. Earlier bytes are no longer in this browser view.`
        : `Locally trimmed ${log.trimmedBytes.toLocaleString()} displayed UTF-8 bytes (${log.trimmedLines.toLocaleString()} newlines). Earlier text is no longer in this browser view.`;
  }
  function followState(value) {
    follow = value;
    followButton.setAttribute("aria-pressed", String(value));
    followButton.textContent = value ? "Following output" : "Resume following output";
  }
  followButton.addEventListener("click", () => {
    followState(!follow);
    if (follow) pre.scrollTop = pre.scrollHeight;
  });
  pre.addEventListener("scroll", () => {
    if (pre.scrollHeight - pre.clientHeight - pre.scrollTop > 4) followState(false);
  });
  function stop() {
    clearTimeout(timer);
    timer = null;
    flight?.abort();
  }
  async function tick() {
    clearTimeout(timer);
    if (destroyed || paused || document.hidden || !raw.open || flight || terminal.active) return;
    const controller = new AbortController();
    flight = controller;
    try {
      const packet = await api.request(log.path, {
        signal: controller.signal,
        retries: 0,
        timeout: 4000,
        validate: validCapture,
        operation: "/api/v1/serial/capture"
      });
      if (controller.signal.aborted || destroyed) return;
      log.ingest(packet);
      show();
      failures = 0;
      retry.hidden = true;
      if (config && (config.queued_bytes > 0 || Date.now() - configReadAt > 3000))
        await readConfig(controller.signal);
      state.textContent =
        "Capture reachable. " +
        (packet.reason === "gap"
          ? `Gap reported: ${packet.lost.toLocaleString()} bytes lost from device capture.`
          : follow
            ? "Following new output."
            : "Following paused; capture continues.");
    } catch (error) {
      if (controller.signal.aborted || destroyed) return;
      failures++;
      state.textContent =
        "Couldn’t read the capture. Earlier output is still shown." +
        (failures >= 3 ? " Click Retry capture to check again." : " Trying again shortly.");
      retry.hidden = false;
    } finally {
      if (flight === controller) flight = null;
      if (!destroyed && !paused && !document.hidden && raw.open && !terminal.active && failures < 3)
        timer = setTimeout(tick, 500);
    }
  }
  pause.addEventListener("click", () => {
    paused = !paused;
    pause.textContent = paused ? "Resume capture" : "Pause capture";
    if (paused) {
      stop();
      state.textContent =
        "Capture paused. The device rolling window continues; resuming may show a gap.";
    } else {
      failures = 0;
      tick();
    }
  });
  retry.addEventListener("click", () => {
    failures = 0;
    tick();
  });
  const visibility = () => {
    if (document.hidden) stop();
    else if (!paused && failures < 3) tick();
  };
  document.addEventListener("visibilitychange", visibility);
  clear.addEventListener("click", () => {
    log.clear();
    show();
    state.textContent = "Browser view cleared.";
  });
  copy.addEventListener("click", async () => notice.show(await copyText(pre)));
  download.addEventListener("click", () =>
    downloadText(pre.textContent, "esp-serial2wifi-console.txt", "text/plain;charset=utf-8")
  );
  function showConnections() {
    endpoints.replaceChildren();
    for (const port of config?.ports || []) {
      const label = el("p");
      label.append(
        el("strong", port === 23 ? "Serial console" : "MCU programming"),
        " — " + (deviceAddress ? deviceAddress + ":" + port : "Port " + port)
      );
      endpoints.append(
        label,
        el(
          "p",
          port === 23
            ? "Use Telnet or a TCP client to communicate with the attached serial device."
            : "Use a compatible tool to program an attached microcontroller. Programming mode uses the configured reset and ISP pins."
        )
      );
    }
    if (config && !config.ports.length)
      endpoints.append(el("p", "No serial TCP listeners are ready."));
    telnetCommand.hidden = !deviceAddress || !config?.ports.includes(23);
    telnetCommand.textContent = deviceAddress ? "telnet -8 " + deviceAddress + " 23" : "";
  }
  function updateAddress(s) {
    deviceAddress = reportedAddress(s?.wifi, location.hostname);
    showConnections();
    updateGuide();
  }
  function applyConfig(v) {
    if (destroyed) return;
    config = v;
    configReadAt = Date.now();
    terminal.setSettings(v);
    terminal.refresh();
    send.disabled = changing || programTx.busy || v.programming || v.queued_bytes > 0;
    reset.disabled = programTx.busy || v.reset_pin === null || v.programming || v.queued_bytes > 0;
    updateProgramControls();
    showConnections();
    clients.textContent =
      v.clients.toLocaleString() +
      " active TCP clients." +
      (v.programming ? " MCU programming is active." : "") +
      (v.queued_bytes ? ` ${v.queued_bytes.toLocaleString()} command bytes queued for UART.` : "") +
      (v.cancelled_bytes
        ? ` ${v.cancelled_bytes.toLocaleString()} queued bytes were cancelled by programming activity.`
        : "");
    resetState.textContent =
      v.reset_pin === null ? "No MCU reset pin is set." : `MCU reset pin: GPIO ${v.reset_pin}.`;
  }
  async function readConfig(signal = lifetime.signal) {
    const v = await api.request("/api/v1/serial", { signal, validate: validSerial, retries: 0 });
    if (!destroyed) applyConfig(v);
    return Object.fromEntries(
      ["baud", "data_bits", "parity", "stop_bits"].map((k) => [k, String(v[k])])
    );
  }
  const draft = new DraftForm(form, {
    read: readConfig,
    validate: validateUART,
    write: async (v, signal) => {
      terminal.disconnect("Disconnected for UART settings change.");
      const saved = await api.request("/api/v1/serial/uart", {
        method: "POST",
        body: new URLSearchParams(v),
        signal,
        validate: validSerial,
        retries: 0
      });
      applyConfig(saved);
      return saved;
    },
    verify: (values, saved) =>
      ["baud", "data_bits", "parity", "stop_bits"].every(
        (k) => String(values[k]) === String(saved[k])
      ),
    message: uartMessage,
    notice,
    describe: () => "UART settings saved."
  });
  async function refresh() {
    if (destroyed) return;
    if (loading) return loading;
    clearTimeout(configRetry);
    configRetry = null;
    const retryConfig = () => {
      if (destroyed) return;
      resetState.textContent = "Serial settings are unavailable. Retrying…";
      if (!destroyed) configRetry = setTimeout(refresh, 3000);
    };
    loading = (draft.dirty || draft.busy || programTx.busy ? readConfig() : draft.load())
      .then(async () => {
        if (!config || draft.state === "error") {
          retryConfig();
          return;
        }
        updateProgramControls();
      })
      .catch(retryConfig)
      .finally(() => {
        loading = null;
      });
    return loading;
  }
  function updateProgramControls() {
    if (destroyed) return;
    terminal.refresh();
    const supported =
      programCapabilities?.features.optiboot_upload &&
      programCapabilities.features.optiboot_readback;
    const occupied = changing || draft.busy || programTx.busy;
    send.disabled = occupied || !config || config.programming || config.queued_bytes > 0;
    reset.disabled =
      occupied ||
      !config ||
      config.reset_pin === null ||
      config.programming ||
      config.queued_bytes > 0;
    clearDevice.disabled = occupied || !config;
    programButton.disabled =
      occupied ||
      !prepared ||
      !supported ||
      !config ||
      config.reset_pin === null ||
      config.programming ||
      config.queued_bytes > 0 ||
      config.clients > 0 ||
      draft.dirty ||
      draft.busy;
    file.disabled = occupied;
    if (programTx.busy) {
      for (const control of form.querySelectorAll("input,select,button")) control.disabled = true;
      send.disabled = reset.disabled = true;
    }
    programReadiness.textContent = !programCapabilities
      ? "Couldn’t check the programmer. Click Refresh device to try again."
      : !supported
        ? "The browser programmer isn’t included in this firmware. Use the external-tool guide below."
        : !config
          ? "Load serial settings first."
          : config.reset_pin === null
            ? "Configure the attached-MCU reset pin first."
            : config.clients || config.programming || config.queued_bytes
              ? "Close TCP serial/programming clients and wait for queued input before programming."
              : draft.dirty || draft.busy
                ? "Save or discard your UART changes before programming."
                : "Ready to program an ATmega328P. Select a HEX file to continue.";
  }
  draft.addEventListener("state", () => {
    uartBusy = draft.busy;
    updateProgramControls();
  });
  file.addEventListener("change", async () => {
    const generation = ++fileGeneration;
    prepared = null;
    updateProgramControls();
    const selected = file.files[0];
    if (!selected) {
      fileInfo.textContent = "Select and validate the file before requesting an MCU reset.";
      return;
    }
    fileInfo.textContent = "Validating the selected Intel HEX file…";
    try {
      if (selected.size > 262144) throw new Error("Choose a HEX file of at most 256 KiB.");
      const value = prepareHex(await selected.text());
      if (destroyed || generation !== fileGeneration) return;
      prepared = value;
      fileInfo.textContent = `Validated ${value.payloadBytes.toLocaleString()} application bytes; ${value.pages.toLocaleString()} pages (${value.programBytes.toLocaleString()} bytes to write). The MCU hasn’t been reset.`;
    } catch (error) {
      if (!destroyed && generation === fileGeneration) fileInfo.textContent = error.message;
    }
    if (!destroyed) updateProgramControls();
  });
  programTx.addEventListener("stage", (event) => {
    if (destroyed) return;
    programState.dataset.state = event.detail.state;
    programState.textContent = event.detail.message;
    updateProgramControls();
  });
  programButton.addEventListener("click", async () => {
    if (programButton.disabled || !prepared) return;
    if (
      !confirm(
        "Replace the attached ATmega328P application with " +
          prepared.programBytes.toLocaleString() +
          " bytes and verify every page? This resets the MCU; ESP Serial2WiFi firmware is unchanged."
      )
    )
      return;
    terminal.disconnect("Disconnected for MCU programming.");
    const wasPaused = paused;
    paused = true;
    stop();
    pause.disabled = true;
    state.textContent = "Capture checks paused during MCU programming.";
    programController = new AbortController();
    try {
      await programTx.run(prepared, programController.signal);
    } catch (_) {
      /* The transaction retains a specific terminal outcome. */
    } finally {
      if (!destroyed) {
        paused = wasPaused;
        pause.disabled = false;
        draft.change(draft.state);
        await refresh();
        updateProgramControls();
        if (!paused) tick();
      }
    }
  });
  command.addEventListener("keydown", (event) => {
    if (event.key === "ArrowUp" || event.key === "ArrowDown") {
      event.preventDefault();
      command.value = history.move(event.key === "ArrowUp" ? -1 : 1, command.value);
    }
  });
  function beginChange() {
    if (destroyed || changing || draft.busy || programTx.busy) return false;
    changing = true;
    terminal.disconnect();
    draft.setLocked(true);
    updateProgramControls();
    return true;
  }
  async function endChange() {
    changing = false;
    if (!destroyed) {
      draft.setLocked(false);
      await refresh();
      updateProgramControls();
      if (raw.open) tick();
    }
  }
  commandForm.addEventListener("submit", async (event) => {
    event.preventDefault();
    if (changing || send.disabled) return;
    let body;
    try {
      body = commandBody(command.value, suffix.value, format.value);
    } catch (error) {
      sendState.textContent = error.message;
      return;
    }
    if (!beginChange()) return;
    const text = command.value;
    try {
      const value = await api.request("/api/v1/serial/send", {
        method: "POST",
        body,
        signal: lifetime.signal,
        retries: 0,
        validate: (v) =>
          v?.schema === 1 &&
          Number.isInteger(v.accepted_bytes) &&
          v.accepted_bytes === body.get("hex").length / 2
      });
      if (destroyed) return;
      history.sent(text);
      command.value = "";
      sendState.textContent = `${value.accepted_bytes.toLocaleString()} bytes sent to the UART queue.`;
    } catch (error) {
      if (destroyed) return;
      sendState.textContent = error.ambiguous
        ? "ESP Serial2WiFi didn’t reply. The command may have been sent; check the device before sending it again. Your text is still here."
        : "The command wasn’t sent. Check the serial settings and try again. Your text is still here.";
    } finally {
      await endChange();
    }
  });
  clearDevice.addEventListener("click", async () => {
    if (
      !confirm(
        "Clear the device rolling capture for every browser client? Transparent TCP traffic is unaffected."
      )
    )
      return;
    if (!beginChange()) return;
    try {
      const value = await api.request("/api/v1/serial/clear", {
        method: "POST",
        body: new URLSearchParams({ action: "clear_capture" }),
        signal: lifetime.signal,
        retries: 0,
        validate: validSerial
      });
      if (destroyed) return;
      applyConfig(value);
      state.textContent = "Device capture cleared. The next capture read marks the new generation.";
    } catch (error) {
      if (destroyed) return;
      state.textContent = error.ambiguous
        ? "ESP Serial2WiFi didn’t reply. Check the capture before clearing it again."
        : "Device capture could not be cleared.";
    } finally {
      await endChange();
    }
  });
  reset.addEventListener("click", async () => {
    if (
      !config ||
      !confirm(
        "Reset the attached MCU using GPIO " +
          config.reset_pin +
          "? ESP Serial2WiFi will keep running."
      )
    )
      return;
    if (!beginChange()) return;
    try {
      const value = await api.request("/api/v1/serial/reset-mcu", {
        method: "POST",
        body: new URLSearchParams({ action: "reset_mcu" }),
        signal: lifetime.signal,
        retries: 0,
        validate: validSerial
      });
      if (destroyed) return;
      applyConfig(value);
      resetState.textContent = "Reset signal sent to the MCU.";
    } catch (error) {
      if (destroyed) return;
      resetState.textContent = error.ambiguous
        ? "ESP Serial2WiFi didn’t reply. Check whether the MCU reset before trying again."
        : "Attached-MCU reset was not accepted.";
    } finally {
      await endChange();
    }
  });
  const beforeUnload = (event) => {
    if (command.value || changing || programTx.busy) {
      event.preventDefault();
      event.returnValue = "";
    }
  };
  window.addEventListener("beforeunload", beforeUnload);
  updateAddress(initialStatus);
  if (format.value === "hex") showFormat();
  // Finish the small settings/capability reads before starting the larger
  // capture responses; the ESP shares its heap with the active TCP bridge.
  refresh().then(() => {
    if (!destroyed) {
      terminal.refresh();
      if (raw.open) tick();
    }
  });
  return {
    refresh,
    updateStatus(s, c, report) {
      programCapabilities = c || null;
      updateProgramControls();
      updateAddress(s);
      errorState.hidden = !report?.hasErrors;
      if (report) errorState.textContent = report.label + ". " + report.detail;
    },
    updateFailure() {
      if (!errorState.hidden)
        errorState.textContent =
          "Serial error readings are out of date. Refresh after reconnecting.";
    },
    canLeave: () =>
      !(draft.dirty || draft.busy || command.value || changing || programTx.busy) ||
      confirm(
        "Leave Serial & MCU? You’ll lose unsaved changes. Leaving during programming can leave the MCU partly written."
      ),
    destroy() {
      destroyed = true;
      lifetime.abort();
      clearTimeout(configRetry);
      terminal.suspend();
      fileGeneration++;
      programController?.abort();
      prepared = null;
      file.value = "";
      stop();
      draft.destroy();
      command.value = "";
      history.items.length = 0;
      document.removeEventListener("visibilitychange", visibility);
      window.removeEventListener("beforeunload", beforeUnload);
    }
  };
}
