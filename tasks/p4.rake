# SPDX-License-Identifier: GPL-3.0-only
# Adapted from the Rakefile in C5VRX (zerowidth/C5VRX PR #3): P4 flashing through its ROM loader and esptool
# against the C5 through the P4 bridge.

P4_DIR = "firmware/p4"

def p4_console_port
  port_for("P4 console", USB_IDS[:p4_console], USB_IDS[:p4_uart])
end

# The P4's ROM loader port on its high-speed USB, rebooting the firmware into it with esptool's reset
# sequence if needed. Nil when only the CH343 USB-C is available.
def p4_rom_port
  return nil if ENV["PORT"]

  ports = usb_ports
  rom = ports.find { |_, i| i == USB_IDS[:p4_rom] }
  return rom.first if rom

  console = ports.find { |_, i| i == USB_IDS[:p4_console] }
  return nil unless console

  script = <<~PY
    import sys, time, serial
    s = serial.Serial(sys.argv[1])
    s.dtr = False; s.rts = True; time.sleep(0.1)
    s.dtr = True; s.rts = False; time.sleep(0.05)
    s.close()
  PY
  system(IDF_PYTHON, "-c", script, console.first) or abort "could not reset the P4 console"
  wait_for_port("P4 ROM loader", USB_IDS[:p4_rom])
end

# esptool against the C5 through the P4 bridge, which resets the C5 into download mode when it sees
# esptool's SYNC and follows esptool's baud change.
def c5_via_p4(dir, *args)
  local_idf dir, "esptool", "--chip", "esp32c5", "-p", p4_console_port, "-b", ENV.fetch("BAUD", "921600"),
            "--before", "no-reset", "--after", "watchdog-reset", *args
end

namespace :p4 do
  desc "Build the P4 firmware"
  task :build do
    local_idf P4_DIR, "idf.py", "build"
  end

  desc "Build, then flash the P4 over its high-speed USB ROM loader, or its CH343 USB-C if that is all there is"
  task flash: :build do
    if (rom = p4_rom_port)
      local_idf "#{P4_DIR}/build", "esptool", "--chip", "esp32p4", "-p", rom, "--before", "no-reset",
                "--after", "watchdog-reset", "write-flash", "@flash_args"
    else
      local_idf P4_DIR, "idf.py", "-p", port_for("P4 USB-C", USB_IDS[:p4_uart]), "flash"
    end
  end

  desc "Open the P4 console in picocom without resetting the P4 (exit with C-a C-x)"
  task :console do
    sh "picocom", "-b", "115200", "--imap", "lfcrlf", p4_console_port
  end
end

namespace :c5 do
  desc "Build the C5 pin walk test firmware and flash it through the P4 bridge"
  task :pinwalk do
    local_idf "firmware/c5-pinwalk", "idf.py", "build"
    c5_via_p4 "firmware/c5-pinwalk/build", "write-flash", "@flash_args"
  end

  desc "Read the C5's chip ID through the P4 bridge"
  task :chip_id do
    c5_via_p4 ".", "chip-id"
  end
end
