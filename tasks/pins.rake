# Wiring check across the breadboard: P4 <-> Nano and C5 -> Nano, with the C5 running firmware/c5-pinwalk
# (rake c5:pinwalk) and the P4 answering on its CH343 console.
namespace :pins do
  desc "Load the Nano pin test bitstream, check every P4 and C5 wire, then reload the safe bitstream"
  task :check do
    sh({ "TOP" => "pintest_top" }, "rake", "fpga:load")
    begin
      sh IDF_PYTHON, "-I", "tools/pinmap.py"
      sh IDF_PYTHON, "-I", "tools/c5walk.py"
    ensure
      sh "rake", "fpga:load"
    end
  end
end
