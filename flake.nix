{
  description = "Sunrise development environment";

  inputs = {
    nixpkgs.url = "https://channels.nixos.org/nixpkgs-unstable/nixexprs.tar.xz";

    flake-compat = {
      url = "github:edolstra/flake-compat";
      flake = false;
    };

    systems = {
      url = "github:nix-systems/default";
      flake = false;
    };

    zig = {
      url = "github:mitchellh/zig-overlay";
      inputs = {
        nixpkgs.follows = "nixpkgs";
        flake-compat.follows = "flake-compat";
        systems.follows = "systems";
      };
    };
  };

  outputs = {
    nixpkgs,
    zig,
    ...
  }: let
    inherit (nixpkgs) lib legacyPackages;

    # Our supported systems are the same systems as the Zig binaries.
    platforms = lib.attrNames zig.packages;
    forAllPlatforms = function:
      lib.genAttrs platforms (system: function legacyPackages.${system});
  in {
    devShells = forAllPlatforms (pkgs: {
      default = pkgs.callPackage ./nix/devShell.nix {
        zig = zig.packages.${pkgs.stdenv.hostPlatform.system}."0.16.0";
      };
    });

    formatter = forAllPlatforms (pkgs: pkgs.alejandra);
  };
}
