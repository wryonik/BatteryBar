/*
 * BatteryBar - macOS menu bar battery indicator for Razer wireless mice.
 *
 * Shows the battery percentage of a Razer mouse connected over its 2.4GHz
 * HyperSpeed dongle, which macOS itself does not report anywhere.
 *
 * The heavy lifting is done by the `wireless_battery_monitor` helper, which
 * speaks Razer's proprietary HID protocol over IOKit and emits JSON. This
 * process only polls it and renders the result.
 *
 * Build:
 *   make            # builds both binaries and BatteryBar.app
 *
 * The helper is located next to this executable at runtime; set
 * BATTERYBAR_BACKEND to override with an explicit path.
 */

import Cocoa
import Foundation

let batteryBarVersion = "1.0.0"

// MARK: - Backend

struct DeviceInfo: Codable {
    let product: String
    let manufacturer: String?
    let vid: String?
    let pid: String?
    let battery_raw: Int?
    let battery_percent: Float?
    let charging: Bool?
    let status: String
    let error: String?
}

struct BatteryResponse: Codable {
    let timestamp: String
    let devices: [DeviceInfo]
}

/// Locate the helper binary relative to whatever is running us, so the same
/// build works from a .app bundle, a `make install` prefix, or a source tree.
func findBackendBinary() -> String? {
    let fm = FileManager.default
    var candidates: [String] = []

    if let override = ProcessInfo.processInfo.environment["BATTERYBAR_BACKEND"] {
        candidates.append(override)
    }

    // Directory holding the running executable. Inside a bundle this is
    // BatteryBar.app/Contents/MacOS; from a plain build it is the build dir.
    let exeDir = URL(fileURLWithPath: CommandLine.arguments[0])
        .resolvingSymlinksInPath()
        .deletingLastPathComponent()
    candidates.append(exeDir.appendingPathComponent("wireless_battery_monitor").path)

    if let resources = Bundle.main.resourceURL {
        candidates.append(resources.appendingPathComponent("wireless_battery_monitor").path)
    }

    // Installed alongside the app on the usual prefixes.
    candidates.append("/usr/local/bin/wireless_battery_monitor")
    candidates.append("/opt/homebrew/bin/wireless_battery_monitor")

    return candidates.first { fm.isExecutableFile(atPath: $0) }
}

func queryBattery() -> [DeviceInfo] {
    guard let binary = findBackendBinary() else { return [] }

    let proc = Process()
    proc.executableURL = URL(fileURLWithPath: binary)
    proc.arguments = ["--json"]

    let pipe = Pipe()
    proc.standardOutput = pipe
    proc.standardError = FileHandle.nullDevice

    do {
        try proc.run()
    } catch {
        return []
    }

    // Read before waiting: the helper's output is small, but draining the pipe
    // first avoids a deadlock if it ever grows past the buffer.
    let data = pipe.fileHandleForReading.readDataToEndOfFile()
    proc.waitUntilExit()

    guard !data.isEmpty,
          let response = try? JSONDecoder().decode(BatteryResponse.self, from: data)
    else { return [] }

    return response.devices
}

// MARK: - Menu Bar App

class AppDelegate: NSObject, NSApplicationDelegate {
    var statusItem: NSStatusItem!
    var timer: Timer?
    var lastDevices: [DeviceInfo] = []
    var backendMissing = false
    var lastUpdate: Date?

    func applicationDidFinishLaunching(_ notification: Notification) {
        statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
        statusItem.button?.title = "🖱 …"

        refreshBattery()

        timer = Timer.scheduledTimer(withTimeInterval: 60, repeats: true) { [weak self] _ in
            self?.refreshBattery()
        }
    }

    func refreshBattery() {
        DispatchQueue.global(qos: .utility).async { [weak self] in
            let missing = findBackendBinary() == nil
            let devices = queryBattery()
            DispatchQueue.main.async {
                guard let self = self else { return }
                self.backendMissing = missing
                self.lastDevices = devices
                self.lastUpdate = Date()
                self.updateUI()
            }
        }
    }

    func updateUI() {
        guard let button = statusItem.button else { return }

        // --- Menu bar title ---
        let readable = lastDevices.compactMap { d -> Float? in
            d.status == "ok" ? d.battery_percent : nil
        }

        if readable.isEmpty {
            button.title = "🖱 --"
        } else {
            button.title = readable.map { "🖱 \(Int($0))%" }.joined(separator: "  ")
        }

        // --- Dropdown ---
        let menu = NSMenu()
        menu.autoenablesItems = false

        for d in lastDevices {
            let header = NSMenuItem(title: "", action: nil, keyEquivalent: "")
            header.isEnabled = false
            header.attributedTitle = NSAttributedString(
                string: "🖱  \(d.product)",
                attributes: [.font: NSFont.boldSystemFont(ofSize: 13)]
            )
            menu.addItem(header)

            if d.status == "ok", let pct = d.battery_percent, let raw = d.battery_raw {
                addDisabledItem(menu, "     \(makeBatteryBar(pct))  \(String(format: "%.1f", pct))%")
                addDisabledItem(menu, "     Raw value: \(raw) / 255")
                if let charging = d.charging {
                    addDisabledItem(menu, charging ? "     ⚡ Charging" : "     🔌 On battery")
                }
            } else {
                addDisabledItem(menu, "     ⚠ Could not read battery")
                if let err = d.error {
                    addDisabledItem(menu, "     \(err)")
                }
            }
            menu.addItem(NSMenuItem.separator())
        }

        if lastDevices.isEmpty {
            if backendMissing {
                addDisabledItem(menu, "⚠ Helper binary not found")
                addDisabledItem(menu, "     Reinstall, or set BATTERYBAR_BACKEND")
            } else {
                addDisabledItem(menu, "No supported mouse detected")
                addDisabledItem(menu, "     Check the 2.4GHz dongle is plugged in")
            }
            menu.addItem(NSMenuItem.separator())
        }

        if let stamp = lastUpdate {
            let fmt = DateFormatter()
            fmt.dateFormat = "HH:mm:ss"
            addDisabledItem(menu, "Updated: \(fmt.string(from: stamp))")
        }
        menu.addItem(NSMenuItem.separator())

        let refresh = NSMenuItem(title: "Refresh Now", action: #selector(onRefresh), keyEquivalent: "r")
        refresh.target = self
        menu.addItem(refresh)

        let quit = NSMenuItem(title: "Quit BatteryBar", action: #selector(onQuit), keyEquivalent: "q")
        quit.target = self
        menu.addItem(quit)

        statusItem.menu = menu
    }

    func addDisabledItem(_ menu: NSMenu, _ title: String) {
        let item = NSMenuItem(title: title, action: nil, keyEquivalent: "")
        item.isEnabled = false
        menu.addItem(item)
    }

    func makeBatteryBar(_ pct: Float) -> String {
        let filled = max(0, min(10, Int(pct / 10.0)))
        return "[" + String(repeating: "█", count: filled)
                   + String(repeating: "░", count: 10 - filled) + "]"
    }

    @objc func onRefresh() { refreshBattery() }
    @objc func onQuit() { NSApplication.shared.terminate(nil) }
}

// MARK: - Main

if CommandLine.arguments.contains("--version") {
    print("BatteryBar \(batteryBarVersion)")
    exit(0)
}

let app = NSApplication.shared
app.setActivationPolicy(.accessory)
let delegate = AppDelegate()
app.delegate = delegate
app.run()
