{ lib
, stdenv
, pkg-config
, attr
, libuuid
, libsodium
, keyutils
, liburcu
, zlib
, libaio
, udev
, zstd
, lz4
, nix-gitignore
, rustPlatform
, binutils
 }:

let
  src = nix-gitignore.gitignoreSource [] ./. ;

  commit = lib.strings.substring 0 7 (builtins.readFile ./.bcachefs_revision);
  version = "git-${commit}";

in stdenv.mkDerivation {
  inherit src version;

  pname = "bcachefs-tools";

  nativeBuildInputs = [
    pkg-config
    rustPlatform.cargoSetupHook
    rustPlatform.rust.cargo
    rustPlatform.rust.rustc
    rustPlatform.bindgenHook
  ] ++ lib.optional stdenv.hostPlatform.isStatic [binutils.bintools];

  buildInputs = [
    libaio
    keyutils # libkeyutils
    lz4 # liblz4

    libsodium
    liburcu
    libuuid
    zstd # libzstd
    zlib # zlib1g
    attr
    udev
  ];

  cargoRoot = "rust-src";
  cargoDeps = rustPlatform.importCargoLock {
    lockFile = "${src}/rust-src/Cargo.lock";
  };

  preBuild = ''
    makeFlagsArray+=(
      PREFIX="${placeholder "out"}"
      VERSION="${commit}"
    )
'';

  enableParallelBuilding = true;
  dontStrip = true;
  checkPhase = "./bcachefs version";
  doCheck = true;
}
