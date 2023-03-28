{
  description = "Userspace tools for bcachefs";

  # Nixpkgs / NixOS version to use.
  inputs.nixpkgs.url = "github:nixos/nixpkgs/nixos-unstable";
  inputs.woob-nixpkgs.url = "github:YellowOnion/nixpkgs/static-fix-libkeyutils";
  inputs.utils.url = "github:numtide/flake-utils";
  inputs.flake-compat = {
    url = "github:edolstra/flake-compat";
    flake = false;
  };

  outputs = { self, nixpkgs, woob-nixpkgs, utils, ... }:
    utils.lib.eachDefaultSystem (system:
      let
        pkgs = nixpkgs.legacyPackages.${system};
        woob-pkgs = woob-nixpkgs.legacyPackages.${system};
        bcachefs = pkgs.callPackage ./build.nix {};
        bcachefs-static = pkgs.pkgsStatic.callPackage ./build.nix {inherit (woob-pkgs.pkgsStatic) keyutils ;};
      in {
        packages = {
          inherit bcachefs bcachefs-static;
          default = bcachefs;
        };
      });
}
