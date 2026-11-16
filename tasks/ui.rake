# The P4's bitmap fonts, baked from Inter (SIL OFL 1.1) as installed by `brew install --cask font-inter`.
INTER = ENV.fetch("INTER_TTF", "/opt/homebrew/Caskroom/font-inter/4.1/extras/ttf/Inter-SemiBold.ttf")
UI_SOURCES = %w[ui/src/ui.c ui/src/canvas.c ui/fonts/inter.c ui/sim/sim.c ui/sim/libc.c].freeze
UI_WASM = "ui/build/ui.wasm"
UI_PORT = ENV.fetch("PORT", "8790")

UI_CLANG = "/usr/bin/clang"
UI_NEEDS = "The UI simulator needs Xcode's clang (xcode-select --install) and rust-lld from a rustup Rust toolchain (rustup.rs)"

# Xcode's clang compiles to WebAssembly. The linker is rust-lld run as wasm-ld, from the active rustup toolchain:
# $(rustc --print sysroot)/lib/rustlib/<host>/bin/rust-lld.
def wasm_ld
  sysroot = IO.popen(%w[rustc --print sysroot], err: File::NULL, &:read).to_s.strip rescue ""
  found = sysroot.empty? ? nil : Dir["#{sysroot}/lib/rustlib/*/bin/rust-lld"].first
  [found, "-flavor", "wasm"] if found
end

def check_ui_toolchain
  clang = File.executable?(UI_CLANG) &&
          IO.popen([UI_CLANG, "--print-targets"], err: File::NULL, &:read).to_s.include?("wasm32")
  missing = []
  missing << "#{UI_CLANG} with a wasm32 target" unless clang
  missing << "rust-lld (rustc --print sysroot found none)" unless wasm_ld
  abort "#{UI_NEEDS}; missing: #{missing.join(", ")}" unless missing.empty?
end

namespace :ui do
  desc "Bake ui/fonts/inter.c from Inter SemiBold (INTER_TTF=path to use another file)"
  task :fonts do
    sh "uv", "run", "-q", "--with", "freetype-py", "ui/fonts/bake.py", INTER, "ui/fonts/inter.c",
       "font_small:20", "font_medium:36", "font_large:80: 0123456789ABEFLRXabefhlrx-"
  end

  desc "Build #{UI_WASM}, the UI for the browser simulator"
  task :wasm do
    check_ui_toolchain
    mkdir_p "ui/build"
    objects = UI_SOURCES.map do |src|
      obj = "ui/build/#{File.basename(src, ".c")}.o"
      sh UI_CLANG, "--target=wasm32", "-O2", "-ffreestanding", "-fno-builtin", "-nostdlib", "-Wall",
         "-Iui/src", "-c", src, "-o", obj, verbose: false
      obj
    end
    sh(*wasm_ld, "--no-entry", "-o", UI_WASM, *objects, verbose: false)
    puts "built #{UI_WASM}"
  end

  desc "Serve the UI simulator on http://127.0.0.1:#{UI_PORT}/, rebuilding and reloading on every change (PORT=...)"
  task dev: :wasm do
    sh "uv", "run", "-q", "ui/sim/serve.py", UI_PORT
  end
end
