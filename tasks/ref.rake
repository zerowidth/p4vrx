# SPDX-License-Identifier: MIT
# Build the read-only reference extracts, to check they still build as baselines.

namespace :ref do
  desc "Build reference/p4usb with the local ESP-IDF"
  task :p4usb do
    local_idf "reference/p4usb", "idf.py", "build"
  end

  desc "Build reference/c5rx with the local ESP-IDF"
  task :c5rx do
    local_idf "reference/c5rx", "idf.py", "build"
  end

  desc "Build both reference extracts"
  task build: %i[p4usb c5rx]
end
