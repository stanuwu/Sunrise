{
  mkShell,
  lib,
  stdenv,
  alejandra,
  llvmPackages_latest,
  xwin,
  zig,
}:
mkShell {
  name = "sunrise";

  packages = [
    alejandra
    llvmPackages_latest.clang-tools
    llvmPackages_latest.lld
    llvmPackages_latest.llvm
    xwin
    zig
  ];

  shellHook =
    ''
      export XWIN_DIR="$PWD/.xwin-cache"
      if [ ! -d "$XWIN_DIR/sdk" ]; then
        xwin --accept-license splat --include-debug-libs --output "$XWIN_DIR"
      fi

      unset NIX_CC NIX_CFLAGS_COMPILE NIX_LDFLAGS
      unset LD CC CXX CFLAGS CPPFLAGS LDFLAGS
    ''
    + (lib.optionalString stdenv.hostPlatform.isDarwin ''
      unset SDKROOT DEVELOPER_DIR
    '');
}
