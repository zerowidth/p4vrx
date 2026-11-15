# frozen_string_literal: true

# Shared helpers (ports, ESP-IDF) live in tasks/ports.rb; each area has its own tasks/*.rake.
require_relative "tasks/ports"
Dir.glob(File.join(__dir__, "tasks", "*.rake")).sort.each { |f| import f }

task default: "-T"
task "-T" do
  sh "rake", "-T"
end
