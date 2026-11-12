# UI navigation for development

Verified 2026-11-11 with Chrome for Testing 151.0.7922.34 in an isolated headless
profile. Tested widths: 360/768/1440 px; fractional DPI and 200% text are covered
by the layout suite. Host versions for this run: Python 3.14.9 and Node.js 26.13.0.

Use isolated loopback fixtures, not an attached device, for routine UI testing.
Run `make ui`, then `.venv/bin/python tests/support/fixture_server.py --port 8765 --assets build/web`.
Prefer the automated test commands in
[development guide](development.md#test-and-develop-the-ui), which create their own server/profile and close them.

## Start → device overview

Open the fixture origin's `/index.html`. Identify the page by the
`ESP Serial2WiFi · Device overview` title, `Main navigation` landmark and
`Refresh device` button. Readiness is `body[data-ready=true]` and the connection
status showing the synthetic device `recovery-demo`. An empty description is blank.
`/` redirects to `/index.html`.

## Overview → a settings screen

Use the navigation's named link, or the Menu button on narrow screens. Hash routes
are `#/network`, `#/serial`, `#/integrations`, `#/diagnostics`, `#/settings`,
`#/firmware` and `#/pages`. Check the current-page navigation link and the relevant
form's loaded state before interacting. A dirty-form departure may ask to discard
changes; do not bypass that as a default route.

## Serial & MCU → terminal

Open `#/serial`, wait for UART settings to load, and let the terminal restore
recent output before live input becomes available. Entering the screen and
resizing the browser terminal send no serial commands. Raw serial tools and
terminal options are separate expanders. Unix-shell commands are optional and
must be explicitly sent. Do not use the live screen to test on a real device
without checking its attached target and other terminal owners.

## Recovery and readiness

Use semantic roles, labels and IDs; reacquire controls after navigation/reload.
A `Retry connection` control returns a failed fixture workflow to a usable state.
The fixture control API can reset synthetic scenarios. Automated suites use
bounded waits, screenshots on meaningful states and a fresh profile each run.
No fixed desktop coordinates or signed-in browser handles are required.

These routes are inherited from the locally validated UI. The browser/layout
suites verify the current source at 360/768/1440 px, fractional DPI and enlarged
text. Reaching a screen does not establish API writes or hardware behavior;
assertions and test results are separate from this navigation record.
