# Tang Nano 20K gateware with the open toolchain (yosys, nextpnr-himbaechel, apicula, openFPGALoader).

FPGA_DEVICE = "GW2AR-LV18QN88C8/I7"
FPGA_FAMILY = "GW2A-18C"
FPGA_BOARD_DIR = "fpga/boards/tangnano20k"
FPGA_TOP = ENV.fetch("TOP", "safe_top")
FPGA_BUILD = "fpga/build"
FPGA_BITSTREAM = "#{FPGA_BUILD}/#{FPGA_TOP}.fs"

# The OSS CAD Suite pinned in mise.toml; on PATH when mise is active, else found through mise.
def fpga_tool(name)
  found = ENV["PATH"].split(File::PATH_SEPARATOR).map { |d| File.join(d, name) }.find { |p| File.executable?(p) }
  found ||= IO.popen(["mise", "which", name], err: File::NULL, &:read).strip
  found.empty? ? abort("#{name} not found; run mise install") : found
end

# openFPGALoader sometimes hangs at "Erase SRAM"; reconfiguring the FPGA from flash (-r) clears it.
# The Nano by its BL616's serial when P4VRX_NANO_SERIAL is set; with several and none pinned, refuse.
def nano_args
  pin = pin_env("P4VRX_NANO_SERIAL")
  return ["--ftdi-serial", pin] if pin && !pin.empty?

  serials = usb_ports.select { |_, i, _| i == "0403:6010" }.map(&:last).uniq
  abort "several Nanos (serials #{serials.join(", ")}); set P4VRX_NANO_SERIAL in mise.local.toml" if serials.size > 1
  []
end

def fpga_loader_once(args, seconds)
  cmd = [fpga_tool("openFPGALoader"), "-b", "tangnano20k", *nano_args, *args]
  puts cmd.join(" ")
  pid = Process.spawn(*cmd)
  deadline = Time.now + seconds
  until Process.wait(pid, Process::WNOHANG)
    if Time.now > deadline
      Process.kill("TERM", pid)
      Process.wait(pid)
      return :timeout
    end
    sleep 0.2
  end
  $?.success? ? :ok : :failed
end

def fpga_loader(*args, seconds: 20)
  result = fpga_loader_once(args, seconds)
  if result == :timeout
    warn "openFPGALoader timed out after #{seconds} s; reconfiguring from flash and retrying"
    fpga_loader_once(["-r"], 10)
    result = fpga_loader_once(args, seconds)
  end
  abort "openFPGALoader #{result}" unless result == :ok
end

namespace :fpga do
  sources = Dir["#{FPGA_BOARD_DIR}/*.v", "#{FPGA_BOARD_DIR}/*.cst", "fpga/rtl/**/*.v"]

  desc "Synthesise, place and route, and pack #{FPGA_BITSTREAM} (TOP=module, default safe_top)"
  task build: FPGA_BITSTREAM

  file FPGA_BITSTREAM => sources + [__FILE__] do
    mkdir_p FPGA_BUILD
    verilog = sources.grep(/\.v\z/)
    json = "#{FPGA_BUILD}/#{FPGA_TOP}.json"
    pnr = "#{FPGA_BUILD}/#{FPGA_TOP}_pnr.json"
    sh fpga_tool("yosys"), "-q", "-l", "#{FPGA_BUILD}/yosys.log", "-p",
       "read_verilog #{verilog.join(" ")}; synth_gowin -top #{FPGA_TOP} -json #{json}; stat"
    sh fpga_tool("nextpnr-himbaechel"), "--json", json, "--write", pnr,
       "--device", FPGA_DEVICE, "--vopt", "family=#{FPGA_FAMILY}",
       # The demod and sync run at 40 MHz; nextpnr checks every clock against this one target.
       "--vopt", "cst=#{FPGA_BOARD_DIR}/#{FPGA_TOP}.cst", "--freq", "40",
       "--report", "#{FPGA_BUILD}/report.json", "--log", "#{FPGA_BUILD}/nextpnr.log", "-q"
    sh fpga_tool("gowin_pack"), "-d", FPGA_FAMILY, "-o", FPGA_BITSTREAM, pnr
  end

  desc "Load #{FPGA_BITSTREAM} into the Nano's SRAM (lost at power off)"
  task load: :build do
    fpga_loader FPGA_BITSTREAM
  end

  desc "Write #{FPGA_BITSTREAM} to the Nano's flash (kept across power cycles)"
  task flash: :build do
    fpga_loader "-f", "--verify", FPGA_BITSTREAM, seconds: 90
  end

  desc "List the cables and FPGA openFPGALoader sees on the Nano"
  task :detect do
    sh fpga_tool("openFPGALoader"), "--detect", "-b", "tangnano20k", *nano_args
  end
end
