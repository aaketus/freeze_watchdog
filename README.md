# Freeze Watchdog

Freeze Watchdog is a monitoring plugin for OBS Studio that automatically detects and recovers frozen, blank, or unresponsive video sources in the active scene.

It helps prevent broken streams and recordings by automatically restarting problematic sources.

---

## Overview

The plugin continuously monitors selected sources by capturing small screenshots and analyzing them for visual changes.

It detects:

- Frozen frames (no change over time)
- Blank frames (very dark or very bright)
- Flat frames (low variation)
- Invalid sources (not rendering or zero resolution)

When a problem is detected, the plugin:

1. Restarts the source
2. Temporarily hides it
3. Restores it after a short delay

---

## Features

- Automatic freeze detection
- Works with most OBS source types
- Configurable sensitivity
- Per-source monitoring
- Low CPU usage
- Integrated into OBS Tools menu

---

## User Interface
Available in OBS: 32.1.1
