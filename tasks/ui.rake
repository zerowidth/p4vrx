# The P4's bitmap fonts, baked from Inter (SIL OFL 1.1) as `brew install --cask font-inter` installs it.
INTER = ENV["INTER_TTF"] ||
        Dir["/opt/homebrew/Caskroom/font-inter/*/extras/ttf/Inter-SemiBold.ttf"].max_by { |p| Gem::Version.new(p[%r{font-inter/([^/]+)/}, 1]) rescue Gem::Version.new("0") }
# Font Awesome Free as `brew install --cask font-fontawesome` installs it (the desktop release's folder).
FONTAWESOME = ENV["FONTAWESOME_DIR"] ||
              Dir["/opt/homebrew/Caskroom/font-fontawesome/*/fontawesome-free-*-desktop"].max_by { |p| Gem::Version.new(p[%r{font-fontawesome/([^/]+)/}, 1]) rescue Gem::Version.new("0") }
UI_ICONS = %w[brands/usb desktop slash hand-pointer circle sd-card bars table-cells grip].join(",")
UI_SOURCES = %w[ui/src/ui.c ui/src/canvas.c ui/fonts/inter.c ui/fonts/icons.c ui/sim/sim.c ui/sim/libc.c].freeze
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
  desc "Bake ui/fonts/inter.c from Inter SemiBold and ui/fonts/icons.c from Font Awesome (INTER_TTF=, FONTAWESOME_DIR=)"
  task :fonts do
    abort "no Inter-SemiBold.ttf: brew install --cask font-inter, or set INTER_TTF" unless INTER && File.file?(INTER)
    sh "uv", "run", "-q", "--with", "freetype-py", "ui/fonts/bake.py", INTER, "ui/fonts/inter.c",
       "font_small:20", "font_medium:36", "font_large:80: 0123456789ABEFLRXabefhlrx-"
    abort "no Font Awesome: brew install --cask font-fontawesome, or set FONTAWESOME_DIR" unless FONTAWESOME && File.directory?(FONTAWESOME)
    sh "uv", "run", "-q", "--with", "freetype-py", "ui/fonts/bake.py", "icons", FONTAWESOME, "ui/fonts/icons.c", UI_ICONS,
       "icons_24:24", "icons_36:36"
  end

  desc "Build #{UI_WASM}, the UI for the browser simulator"
  task :wasm do
    check_ui_toolchain
    mkdir_p "ui/build"
    objects = UI_SOURCES.map do |src|
      obj = "ui/build/#{File.basename(src, ".c")}.o"
      sh UI_CLANG, "--target=wasm32", "-O2", "-ffreestanding", "-fno-builtin", "-nostdlib", "-Wall",
         "-Iui/src", "-Iui/fonts", "-c", src, "-o", obj, verbose: false
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
