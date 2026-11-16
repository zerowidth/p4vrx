# SPDX-License-Identifier: GPL-3.0-only
# Adapted from the Rakefile in C5VRX (zerowidth/C5VRX PR #3): USB port discovery and ESP-IDF helpers.

require "rake"
include FileUtils

IDF_VERSION = ENV.fetch("IDF_VERSION", "v6.1")
IDF_ACTIVATE = File.expand_path("~/.espressif/tools/activate_idf_#{IDF_VERSION}.sh")
IDF_PYTHON = Dir[File.expand_path("~/.espressif/tools/python/#{IDF_VERSION}/venv/bin/python")].first ||
             "python3"

# USB IDs (VID:PID) of the serial ports tasks talk to.
USB_IDS = {
  p4_console: "303a:8000", # the P4 firmware's CDC console on the high-speed port
  p4_rom: "303a:0012",     # the P4's ROM loader on the same port
  p4_uart: "1a86:55d3",    # the CH343 USB-UART on the P4 board's other USB-C
  esp_usb_jtag: "303a:1001" # a C5 or S3 on its own native USB
}.freeze

# Serial ports as [device, "vid:pid", serial], read through pyserial.
def usb_ports
  script = <<~PY
    from serial.tools.list_ports import comports
    for p in comports():
        if p.vid is not None:
            print(p.device, "%04x:%04x" % (p.vid, p.pid), p.serial_number or "-")
  PY
  IO.popen([IDF_PYTHON, "-c", script], &:read).lines.map(&:split)
end

# Environment variables (set in mise.local.toml) that pin a USB ID to one board's serial number, so tasks
# never pick another board that shares the ID. A ROM loader reports the MAC with colons, the app without.
SERIAL_PINS = {
  USB_IDS[:p4_console] => "P4VRX_P4_USB_SERIAL",
  USB_IDS[:p4_rom] => "P4VRX_P4_USB_SERIAL",
  USB_IDS[:p4_uart] => "P4VRX_P4_UART_SERIAL",
  "0403:6010" => "P4VRX_NANO_SERIAL"
}.freeze

# [env] from mise.local.toml, for rake run outside mise; the process environment wins.
LOCAL_ENV = begin
  path = File.expand_path("../mise.local.toml", __dir__)
  section = nil
  File.exist?(path) ? File.readlines(path).each_with_object({}) do |line, env|
    line = line.sub(/#.*/, "").strip
    if line.start_with?("[") then section = line
    elsif section == "[env]" && line.include?("=")
      k, v = line.split("=", 2)
      env[k.strip] = v.strip.delete('"')
    end
  end : {}
end.freeze

def pin_env(name)
  ENV[name] || LOCAL_ENV[name]
end

def same_serial?(a, b)
  a.to_s.delete(":").casecmp?(b.to_s.delete(":"))
end

# The ports with this USB ID, narrowed to the pinned serial if one is set.
def pinned_ports(ports, id)
  found = ports.select { |_, i, _| i == id }
  pin = pin_env(SERIAL_PINS[id].to_s)
  pin && !pin.empty? ? found.select { |_, _, s| same_serial?(s, pin) } : found
end

# The one port matching the first USB ID that has any. PORT overrides. With several boards sharing an ID and
# no serial pinned for it, it refuses rather than guess.
def port_for(name, *ids)
  return ENV["PORT"] if ENV["PORT"]

  ports = usb_ports
  ids.each do |id|
    found = pinned_ports(ports, id)
    next if found.empty?
    if found.size > 1
      abort "several #{name} ports (#{found.map { |d, _, s| "#{d} serial #{s}" }.join(", ")}); " \
            "set #{SERIAL_PINS[id] || "PORT"} in mise.local.toml"
    end
    return found.first.first
  end
  abort "no #{name} port found; plug it in, check #{ids.map { |i| SERIAL_PINS[i] }.compact.uniq.join("/")}, or set PORT"
end

def wait_for_port(name, id, seconds: 10)
  deadline = Time.now + seconds
  until Time.now > deadline
    found = pinned_ports(usb_ports, id).first
    return found.first if found
    sleep 0.2
  end
  abort "#{name} did not appear within #{seconds} s (a new USB device may need approving in macOS)"
end

# Runs a command in dir under the local ESP-IDF, where idf.py is a shell function.
def local_idf(dir, *cmd)
  abort "ESP-IDF #{IDF_VERSION} not found at #{IDF_ACTIVATE}" unless File.exist?(IDF_ACTIVATE)

  sh "bash", "-c", 'args=("$@"); set --; . "${args[0]}" >/dev/null; cd "${args[1]}" && "${args[@]:2}"',
     "bash", IDF_ACTIVATE, dir, *cmd
end

desc "List USB serial ports with their IDs and serial numbers"
task :ports do
  usb_ports.each { |d, i, s| puts format("%-36s %s  serial %s", d, i, s) }
end
