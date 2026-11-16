# The P4's bitmap fonts, baked from Inter (SIL OFL 1.1) as installed by `brew install --cask font-inter`.
INTER = ENV.fetch("INTER_TTF", "/opt/homebrew/Caskroom/font-inter/4.1/extras/ttf/Inter-SemiBold.ttf")
UI_SOURCES = %w[ui/src/ui.c ui/src/canvas.c ui/fonts/inter.c ui/sim/sim.c ui/sim/libc.c].freeze
UI_WASM = "ui/build/ui.wasm"
UI_PORT = ENV.fetch("PORT", "8790")

# Xcode's clang compiles to WebAssembly; the linker is the wasm-ld that ships with rustup's toolchain.
def wasm_ld
  sysroot = IO.popen(%w[rustc --print sysroot], err: File::NULL, &:read).strip
  found = Dir["#{sysroot}/lib/rustlib/*/bin/rust-lld"].first
  abort "no rust-lld found; install a Rust toolchain with rustup" unless found
  [found, "-flavor", "wasm"]
end

namespace :ui do
  desc "Bake ui/fonts/inter.c from Inter SemiBold (INTER_TTF=path to use another file)"
  task :fonts do
    sh "uv", "run", "-q", "--with", "freetype-py", "ui/fonts/bake.py", INTER, "ui/fonts/inter.c",
       "font_small:20", "font_medium:36", "font_large:80: 0123456789ABEFLRXabefhlrx-"
  end

  desc "Build #{UI_WASM}, the UI for the browser simulator"
  task :wasm do
    mkdir_p "ui/build"
    objects = UI_SOURCES.map do |src|
      obj = "ui/build/#{File.basename(src, ".c")}.o"
      sh "/usr/bin/clang", "--target=wasm32", "-O2", "-ffreestanding", "-fno-builtin", "-nostdlib", "-Wall",
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
