# libfprint with elanpress

This is libfprint with the elanpress driver for the ELAN press sensors 04f3:0c3d, 0c4b and 0c6e. Stock libfprint lists 0c6e under the swipe-only `elan` driver, which never matches a press ([libfprint#401](https://gitlab.freedesktop.org/libfprint/libfprint/-/issues/401)).

`main` is Mohammad Nauman's `elanpress-0c3d` branch, under review upstream as [libfprint!654](https://gitlab.freedesktop.org/libfprint/libfprint/-/merge_requests/654), with a Nix flake and this file on top. That branch already has my merge requests to it:

- [inauman/libfprint!3](https://gitlab.freedesktop.org/inauman/libfprint/-/merge_requests/3), frames rejected by the quality gates no longer set the contrast bar
- [inauman/libfprint!4](https://gitlab.freedesktop.org/inauman/libfprint/-/merge_requests/4), `FP_ELANPRESS_*` values that are not finite or out of range keep their defaults or get clamped, and the driver logs them
- [inauman/libfprint!5](https://gitlab.freedesktop.org/inauman/libfprint/-/merge_requests/5), a failed open releases the USB interface and a short reply to a data read fails the capture
- [inauman/libfprint!2](https://gitlab.freedesktop.org/inauman/libfprint/-/merge_requests/2), faster matching

It also has Mohammad's fix to libfprint itself that closes the USB device when a driver's open fails ([libfprint#905](https://gitlab.freedesktop.org/libfprint/libfprint/-/issues/905)).

The branch gets rebased and force-pushed when !654 changes.

## NixOS

```nix
inputs.libfprint-elanpress = {
  url = "github:someonewithpc/libfprint-elanpress";
  inputs.nixpkgs.follows = "nixpkgs";
};
```

```nix
services.fprintd.enable = true;
services.fprintd.package = inputs.libfprint-elanpress.packages.${pkgs.stdenv.hostPlatform.system}.fprintd;
```

`packages.<system>.libfprint` is the library alone, and `overlays.default` replaces `libfprint` in nixpkgs, which rebuilds everything that depends on it. `packages.<system>.default` is a `callPackage` result, so `.override { libfprint = ...; }` builds the same tree on top of another libfprint derivation.
