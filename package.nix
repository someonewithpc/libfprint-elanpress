{ libfprint, src }:

libfprint.overrideAttrs {
  version = "1.94.100-elanpress";
  inherit src;
  # nixpkgs names tests/test-runner.sh, which this tree no longer has
  postPatch = "patchShebangs tests";
}
