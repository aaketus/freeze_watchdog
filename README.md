# Freeze Watchdog

## Overview
**Freeze Watchdog** is a lightweight monitoring plugin for OBS Studio that detects and automatically recovers frozen, blank, or unresponsive video sources in the active scene.

Its purpose is simple: **prevent broken streams and recordings by automatically restarting problematic sources.**

---

## Features

- Automatic detection of frozen or broken sources  
- Supports media, browser, capture, and other video sources  
- Per-source monitoring  
- Configurable detection thresholds  
- Low CPU and memory usage  
- Integrated into the OBS Tools menu  

---

## How It Works

The plugin periodically captures small image samples from selected sources and analyzes them using simple image-based metrics.

It detects:

- **Frozen frames** – no visual change over time  
- **Blank frames** – frames that are too dark or too bright  
- **Low-variation frames** – frames with minimal detail (e.g. solid color)  
- **Invalid sources** – sources that fail to render or report resolution  

### Recovery process

When an issue is detected:

1. The source is restarted  
2. It is temporarily hidden from all scenes  
3. It is restored after a short delay  

This process is fully automatic and designed to minimize disruption during streaming or recording.

---

## Compatibility

**Tested with:**
- OBS Studio 32.1.1  

**Platforms:**
- Windows (64-bit)  
- Linux (64-bit)  

> Compatibility with nearby versions is likely, but not guaranteed.

---

## Installation

### Windows

Copy the plugin files to: C:\Program Files\obs-studio\obs-plugins\64bit\


---

### Linux

Copy the plugin files to: ~/.config/obs-studio/plugins/freeze-watchdog/bin/64bit/


---

Restart OBS after installation.

---

## Usage

1. Open OBS  
2. Navigate to **Tools → Freeze Watchdog Settings**  
3. Add source names (must match exactly)  
4. Adjust settings if needed  
5. Enable the plugin  

Monitoring starts immediately.

---

## Configuration

**Watched Sources**  
List of source names (one per line).

**Check Interval**  
How often sources are analyzed.

**Unchanged Hits**  
Number of identical frames before triggering a reset.

**Difference Threshold**  
Maximum allowed frame difference (lower = stricter).

**Blank Detection**  
Thresholds for detecting dark or bright frames.

**Flat Detection**  
Detects low-variation frames.

**Hide Duration**  
How long the source stays hidden during recovery.

**Cooldown**  
Minimum time between resets for the same source.

---

## Notes

- Source names must match exactly  
- Only visible sources in the active scene are monitored  
- Strict detection settings may cause false positives  
- Temporary screenshots are automatically removed  

---

## Troubleshooting

### Plugin not visible

- Verify installation path  
- Ensure you are using 64-bit OBS  

### Sources are not resetting

- Check source names  
- Ensure sources are visible  
- Adjust detection thresholds  

### Too many resets

- Increase thresholds  
- Increase cooldown  

---

## Performance

Freeze Watchdog is designed to be lightweight:

- Uses small image samples  
- Minimal processing overhead  

Safe for continuous use during streaming and recording.

---
## Build Requirements

To build Freeze Watchdog from source, you need:

- C++17 compatible compiler
- CMake (3.16 or newer)
- OBS Studio development libraries (libobs)
- Qt (same version as OBS, if UI is used)

### Windows

- Visual Studio 2022 (with C++ workload)
- OBS Studio installed (for headers and libraries)
- CMake

### Linux

- GCC or Clang
- CMake
- OBS Studio development package (libobs-dev or obs-studio-dev)
- Qt development libraries
