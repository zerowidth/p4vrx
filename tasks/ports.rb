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

# The one port matching the first USB ID that has any. PORT overrides; SERIAL picks
# one board when several share an ID.
def port_for(name, *ids)
  return ENV["PORT"] if ENV["PORT"]

  ports = usb_ports
  ports = ports.select { |_, _, s| s == ENV["SERIAL"] } if ENV["SERIAL"]
  ids.each do |id|
    found = ports.select { |_, i, _| i == id }
    next if found.empty?
    if found.size > 1
      abort "several #{name} ports (#{found.map { |d, _, s| "#{d} serial #{s}" }.join(", ")}); set SERIAL or PORT"
    end
    return found.first.first
  end
  abort "no #{name} port found; plug it in or set PORT"
end

def wait_for_port(name, id, seconds: 10)
  deadline = Time.now + seconds
  until Time.now > deadline
    found = usb_ports.find { |_, i, _| i == id }
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
