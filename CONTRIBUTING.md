# Contributing

Start with the [development guide](docs/development.md) for setup, tests, code
layout and release builds.

Keep features usable on both 512 KB and 4 MB devices; OTA is the intentional
capacity difference. Keep serial input explicit: navigation, reconnects and
resizing must not send shell commands or assume the attached device runs Linux.
Do not automatically retry writes whose outcome is unknown.

Run `make check` for firmware changes and the browser/layout checks for UI
changes. Report hardware checks separately from local tests. Preserve flash-region
guards, SDK transport bindings and stored-format compatibility; changes to these
need a migration or recovery plan.

Update the user guide when behavior changes and the
[UI navigation notes](docs/ui-navigation.md) when reusable routes change. Keep
credentials, backups, generated files and local configuration out of commits.
Retain project and third-party license notices.
