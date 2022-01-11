{ kversion ? "linux_5_15"
, pkgs ? import <nixpkgs> {} }:

with pkgs;

let
  tools = pkgs.callPackage ./default.nix { doCheck = false ;} ;
in
mkShell {
  buildInputs = [
    linuxKernel.packages.${kversion}.perf
    gdb
    ccls # code completion in neovim/emacs
  ];
  inputsFrom = [
    tools
  ];
}
