import { decodeCapture, terminalReplay, validSerial } from "./serial-model.js";
import { el, hex, copy } from "./core.js";
export function createTerminalConsole(api, handlers = {}) {
  let { activeChanged = () => {}, blocked = () => false } = handlers,
    attached = true;
  const root = el("section");
  root.className = "live-console";
  root.setAttribute("aria-label", "Interactive serial terminal");
  const toolbar = el("div");
  toolbar.className = "terminal-toolbar";
  const heading = el("div");
  heading.className = "terminal-heading";
  const title = el("h2", "Live terminal"),
    format = el("span");
  format.id = "serial-terminal-format";
  heading.append(title, format);
  const toggle = el("button", "Pause terminal");
  toggle.type = "button";
  toggle.className = "quiet";
  toggle.disabled = true;
  toggle.id = "serial-terminal-toggle";
  toggle.setAttribute("aria-controls", "serial-terminal");
  toolbar.append(heading, toggle);
  const status = el("p", "Loading terminal…");
  status.id = "serial-terminal-state";
  status.setAttribute("role", "status");
  const host = el("div");
  host.id = "serial-terminal";
  host.className = "terminal-host";
  const hint = el(
    "p",
    "Click the screen to type. A quiet device may show nothing until it transmits. Ctrl+] moves focus to the terminal controls."
  );
  hint.className = "field-hint";
  hint.id = "serial-terminal-hint";
  const options = el("details"),
    summary = el("summary", "Terminal options"),
    geometry = el("form");
  geometry.className = "terminal-size";
  options.className = "serial-disclosure terminal-options";
  const dimensions = [];
  for (const [name, value, min, max] of [
    ["Columns", 80, 20, 240],
    ["Rows", 24, 5, 100]
  ]) {
    const label = el("label", name),
      input = el("input");
    input.type = "number";
    input.value = value;
    input.min = min;
    input.max = max;
    input.required = true;
    input.id = "terminal-" + name.toLowerCase();
    label.htmlFor = input.id;
    const field = el("div");
    field.className = "form-field";
    field.append(label, input);
    geometry.append(field);
    dimensions.push(input);
  }
  const resize = el("button", "Apply terminal size");
  resize.type = "submit";
  geometry.append(resize);
  options.append(
    summary,
    geometry,
    el(
      "p",
      "Changes this browser’s display. Devices that query the terminal size receive the current dimensions."
    )
  );
  const unix = el("details", "", "management-options"),
    command = el("pre", "stty rows 24 cols 80", "serial-guide"),
    copyButton = el("button", "Copy command", "quiet"),
    sendButton = el("button", "Send to Unix shell"),
    copyState = el("p"),
    copyActions = el("div", "", "form-actions");
  unix.id = "terminal-unix";
  command.id = "terminal-unix-command";
  copyButton.id = "terminal-unix-copy";
  sendButton.id = "terminal-unix-send";
  copyButton.type = sendButton.type = "button";
  sendButton.disabled = true;
  copyState.setAttribute("role", "status");
  copyState.hidden = true;
  copyActions.append(copyButton, sendButton);
  const sendHelp = el(
    "p",
    "Send to Unix shell sends this command and presses Enter. Copy command only copies it."
  );
  sendHelp.id = "terminal-unix-help";
  sendButton.setAttribute("aria-describedby", sendHelp.id);
  unix.append(
    el("summary", "Unix shell"),
    el("p", "Use at an empty Unix shell prompt on the attached device."),
    command,
    copyActions,
    sendHelp,
    copyState,
    el("p", "Or run resize there, if installed, to detect and apply the size.")
  );
  options.append(unix);
  const showDimensions = () => {
    command.textContent = "stty rows " + term.rows + " cols " + term.cols;
    copyState.hidden = true;
  };
  copyButton.addEventListener("click", async () => {
    const text = command.textContent,
      result = await copy(command);
    if (destroyed || text !== command.textContent) return;
    copyState.textContent = result;
    copyState.hidden = false;
  });
  sendButton.addEventListener("click", () => {
    if (!canSend()) return;
    enqueue(encoder.encode(command.textContent + "\r"));
    copyState.textContent = "Command queued with Enter. Check the terminal for the result.";
    copyState.hidden = false;
    host.scrollIntoView({ block: "nearest" });
    term.focus();
  });
  geometry.addEventListener("submit", (e) => {
    e.preventDefault();
    if (term && geometry.reportValidity()) {
      term.resize(...dimensions.map((n) => Number(n.value)));
      term.refresh(0, term.rows - 1);
    }
  });
  root.append(toolbar, status, host, hint);
  let term = null,
    sizeObserver = null,
    controller = null,
    generation = 0,
    ready = false,
    restoring = false,
    destroyed = false,
    wanted = true,
    retryTimer = null,
    retryAttempt = 0;
  let queue = [],
    queued = 0,
    sending = false,
    cursor = null,
    initializing = true,
    lineKey = null,
    pendingRender = 0;
  const encoder = new TextEncoder();
  function canSend() {
    return ready && attached && !destroyed && !blocked() && !sending && !queued;
  }
  function updateSend() {
    sendButton.disabled = !canSend();
  }
  function update() {
    toggle.textContent = wanted ? "Pause terminal" : "Resume terminal";
    toggle.disabled = !term || !!blocked();
    updateSend();
    root.dataset.state =
      !initializing && !term ? "failed" : ready ? "live" : !wanted ? "paused" : "waiting";
    if (term) term.options.disableStdin = !ready;
  }
  function closeStream() {
    generation++;
    ready = false;
    restoring = false;
    controller?.abort();
    controller = null;
    queue = [];
    queued = 0;
    clearTimeout(retryTimer);
    retryTimer = null;
    update();
    activeChanged(false);
  }
  function retry(message, delay) {
    closeStream();
    if (destroyed || !attached || !wanted) return;
    status.textContent = message;
    retryTimer = setTimeout(
      () => {
        retryTimer = null;
        reconcile();
      },
      delay ?? Math.min(10000, 500 * 2 ** Math.min(retryAttempt++, 5))
    );
  }
  function pause(message) {
    wanted = false;
    closeStream();
    status.textContent =
      message || "Paused in this browser. Unsent input was cleared; the device is still running.";
  }
  const initialized = (async () => {
    let css = document.querySelector("link[data-terminal-css]");
    if (css?.dataset.failed) {
      css.remove();
      css = null;
    }
    const styled = css?.sheet
      ? Promise.resolve()
      : new Promise((resolve, reject) => {
          const fresh = !css;
          if (fresh) {
            css = document.createElement("link");
            css.dataset.terminalCss = "";
            css.rel = "stylesheet";
            css.href = new URL("./xterm.css", import.meta.url);
          }
          let timer;
          const finish = (error) => {
            clearTimeout(timer);
            css.removeEventListener("load", loaded);
            css.removeEventListener("error", failed);
            if (error) {
              css.dataset.failed = "true";
              reject(error);
            } else resolve();
          };
          const loaded = () => finish(),
            failed = () => finish(new Error("Terminal stylesheet failed"));
          css.addEventListener("load", loaded, { once: true });
          css.addEventListener("error", failed, { once: true });
          timer = setTimeout(failed, 8000);
          if (fresh) document.head.append(css);
        });
    await Promise.all([styled, import("./xterm.js")]);
    if (destroyed) return;
    term = new globalThis.Terminal({
      cols: 80,
      rows: 24,
      scrollback: 2000,
      fontSize: 14,
      fontFamily: "ui-monospace, monospace",
      cursorBlink: false,
      minimumContrastRatio: 4.5,
      disableStdin: true,
      screenReaderMode: true,
      allowProposedApi: true,
      windowOptions: { getWinSizeChars: true },
      theme: { background: "#101820", foreground: "#e5edf0" }
    });
    term.onResize(showDimensions);
    showDimensions();
    term.open(host);
    host.querySelector("textarea")?.setAttribute("aria-label", "Serial terminal input");
    host
      .querySelector("textarea")
      ?.setAttribute("aria-describedby", "serial-terminal-state serial-terminal-hint");
    term.element.style.fontFamily = term.options.fontFamily;
    term.element.style.fontSize = term.options.fontSize + "px";
    const screen = host.querySelector(".xterm-screen"),
      viewport = host.querySelector(".xterm-viewport");
    const size = () => {
      if (destroyed || !screen || !viewport) return;
      const width = Math.ceil(
          screen.getBoundingClientRect().width + viewport.offsetWidth - viewport.clientWidth
        ),
        c = getComputedStyle(host),
        outer = Math.ceil(
          width +
            parseFloat(c.paddingLeft) +
            parseFloat(c.paddingRight) +
            parseFloat(c.borderLeftWidth) +
            parseFloat(c.borderRightWidth)
        );
      if (term.element.style.width !== width + "px") {
        term.element.style.width = width + "px";
        term.refresh(0, term.rows - 1);
      }
      root.style.setProperty("--terminal-width", outer + "px");
    };
    sizeObserver = new ResizeObserver(size);
    sizeObserver.observe(screen);
    sizeObserver.observe(viewport);
    size();
    term.parser.registerOscHandler(52, () => true);
    term.parser.registerOscHandler(8, () => true);
    term.attachCustomKeyEventHandler((e) => {
      if (e.ctrlKey && e.key === "]") {
        if (e.type === "keydown") {
          toggle.focus();
          term.blur();
        }
        return false;
      }
      return true;
    });
    term.onData((text) => {
      if (ready) enqueue(encoder.encode(text));
    });
    term.onBinary((text) => {
      if (ready) enqueue(Uint8Array.from(text, (c) => c.charCodeAt(0)));
    });
  })()
    .catch(() => {
      sizeObserver?.disconnect();
      term?.dispose();
      term = null;
      wanted = false;
      status.textContent = "Terminal assets could not load. Reload this page to retry.";
    })
    .finally(() => {
      initializing = false;
      update();
    });
  async function drain() {
    if (sending || !ready) return;
    sending = true;
    updateSend();
    const id = generation;
    try {
      while (ready && id === generation && queue.length) {
        const next = queue.shift();
        queued -= next.length;
        const start = performance.now();
        await api.request("/api/v1/serial/send", {
          method: "POST",
          body: new URLSearchParams({ hex: hex(next) }),
          retries: 0,
          signal: controller.signal,
          timeout: 4000,
          validate: (v) => v?.schema === 1 && v.accepted_bytes === next.length
        });
        if (id !== generation) break;
        await new Promise((resolve) =>
          setTimeout(resolve, Math.max(0, next.length / 3.2 - (performance.now() - start)))
        );
      }
    } catch (error) {
      if (id === generation)
        pause(
          error.ambiguous
            ? "ESP Serial2WiFi didn’t reply. Your last input may have arrived. Check the device, then resume."
            : "ESP Serial2WiFi couldn’t accept the input. Check the connection, then resume."
        );
    } finally {
      sending = false;
      if (ready && queue.length) drain();
      else updateSend();
    }
  }
  function enqueue(bytes) {
    if (!ready || !bytes.length) return;
    if (queued + bytes.length > 65536) {
      pause("Paste exceeds 64 KiB. Paused; unsent input was cleared.");
      return;
    }
    for (let i = 0; i < bytes.length; i += 64) {
      const part = bytes.slice(i, i + 64);
      queue.push(part);
      queued += part.length;
    }
    drain();
  }
  function write(bytes, done) {
    if (!term || destroyed) return;
    if (pendingRender + bytes.length > 262144) {
      pause(
        "This browser can’t keep up with the output. Paused; resume when it is responsive again."
      );
      throw new Error("render");
    }
    pendingRender += bytes.length;
    term.write(bytes, () => {
      pendingRender -= bytes.length;
      done?.();
    });
  }
  function packet(event, data) {
    if (event === "stopped") throw new Error("busy");
    if (event !== "ready" && event !== "output") return;
    const value = JSON.parse(data);
    if (event === "ready") {
      if (ready || restoring) throw new Error("Repeated stream start");
      const replay = terminalReplay(value, cursor),
        id = generation;
      if (replay.reset) write(encoder.encode("\x1bc"));
      else if (replay.gap)
        write(encoder.encode("\x18\x1a\x1b[0m\r\n[Output interrupted; showing recent output]\r\n"));
      cursor = { boot: value.boot, epoch: value.epoch, end: value.end };
      restoring = true;
      status.textContent = "Loading recent output…";
      // Do not block SSE reads on rendering. Xterm parses queued live writes only
      // after this callback; historical queries and user input stay muted until then.
      write(replay.bytes.length ? replay.bytes : "\x00", () => {
        if (id !== generation || destroyed || !attached || !wanted || blocked()) return;
        restoring = false;
        ready = true;
        retryAttempt = 0;
        update();
        // A retained terminal was briefly detached; let its viewport measure again
        // before scrolling, otherwise a zero-height scroll area can jump to line 0.
        requestAnimationFrame(() =>
          requestAnimationFrame(() => {
            if (id === generation && attached && !destroyed) term.scrollToBottom();
          })
        );
        status.textContent = replay.gap
          ? "Live. Some earlier output is no longer available."
          : "Live — click the screen to type.";
      });
      return;
    }
    const bytes = decodeCapture(value);
    if (
      !(ready || restoring) ||
      !cursor ||
      value.reason ||
      cursor.boot !== value.boot ||
      cursor.epoch !== value.epoch ||
      cursor.end !== value.start
    )
      throw new Error("gap");
    cursor = { boot: value.boot, epoch: value.epoch, end: value.end };
    write(bytes);
  }
  function setSettings(v) {
    const key = v.baud + ":" + v.data_bits + v.parity + v.stop_bits;
    if (lineKey && lineKey !== key) {
      cursor = null;
      write(encoder.encode("\x1bc"));
    }
    lineKey = key;
    format.textContent =
      v.baud.toLocaleString() + " baud · " + v.data_bits + v.parity + v.stop_bits;
  }
  async function start() {
    await initialized;
    if (!term || controller || destroyed || !attached || !wanted || blocked()) return;
    const own = new AbortController();
    controller = own;
    const id = ++generation;
    activeChanged(true);
    update();
    status.textContent = cursor ? "Reconnecting…" : "Opening terminal…";
    let watchdog;
    const arm = () => {
      clearTimeout(watchdog);
      watchdog = setTimeout(() => {
        if (id === generation) retry("Connection interrupted. Reconnecting…");
      }, 6000);
    };
    arm();
    try {
      const config = await api.request("/api/v1/serial", {
        signal: own.signal,
        retries: 0,
        validate: validSerial
      });
      setSettings(config);
      if (id !== generation || !attached) return;
      if (config.clients || config.programming) throw new Error("busy");
      const response = await fetch("/api/v1/serial/live", {
        cache: "no-store",
        signal: own.signal
      });
      if (response.status === 409) throw new Error("busy");
      if (!response.ok || !response.headers.get("content-type")?.startsWith("text/event-stream"))
        throw new Error("connection");
      const reader = response.body.getReader(),
        decoder = new TextDecoder();
      let pending = "";
      while (id === generation && !destroyed) {
        const chunk = await reader.read();
        arm();
        if (chunk.done) throw new Error("connection");
        pending += decoder.decode(chunk.value, { stream: true });
        let end;
        while ((end = pending.indexOf("\n\n")) !== -1) {
          const frame = pending.slice(0, end);
          pending = pending.slice(end + 2);
          let event = "",
            data = "";
          for (const line of frame.split("\n")) {
            if (line.startsWith("event: ")) event = line.slice(7);
            else if (line.startsWith("data: ")) data += line.slice(6);
          }
          if (event) packet(event, data);
        }
        if (pending.length > 4096) throw new Error("connection");
      }
    } catch (error) {
      if (id === generation && !destroyed)
        retry(
          error.message === "busy"
            ? "Serial port in use elsewhere. Waiting for it to become available…"
            : error.message === "gap"
              ? "Output interrupted. Reconnecting…"
              : "Connection interrupted. Reconnecting…",
          error.message === "busy" ? 3000 : undefined
        );
    } finally {
      clearTimeout(watchdog);
    }
  }
  function reconcile() {
    if (destroyed || !attached) return;
    update();
    if (initializing || !term || !wanted) return;
    const reason = blocked();
    if (reason) {
      if (controller || retryTimer) {
        closeStream();
      }
      status.textContent =
        typeof reason === "string"
          ? reason
          : "Terminal paused while serial tools or settings are in use.";
      return;
    }
    if (!controller && !retryTimer) start();
  }
  toggle.addEventListener("click", () => {
    if (wanted) pause();
    else {
      wanted = true;
      retryAttempt = 0;
      update();
      reconcile();
    }
  });
  const visibility = () => {
    if (!document.hidden) {
      term?.refresh(0, term.rows - 1);
      reconcile();
    }
  };
  document.addEventListener("visibilitychange", visibility);
  initialized.then(reconcile);
  return {
    root,
    options,
    get failed() {
      return !initializing && !term;
    },
    attach(handlers) {
      ({ activeChanged, blocked } = handlers);
      attached = true;
      initialized.then(reconcile);
    },
    suspend() {
      attached = false;
      closeStream();
      activeChanged = () => {};
      blocked = () => true;
    },
    refresh: reconcile,
    setSettings,
    disconnect(message) {
      closeStream();
      status.textContent = message || "Terminal paused for this operation.";
    },
    get active() {
      return !!controller;
    },
    destroy() {
      destroyed = true;
      closeStream();
      sizeObserver?.disconnect();
      document.removeEventListener("visibilitychange", visibility);
      term?.dispose();
    }
  };
}
