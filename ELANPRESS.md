# libfprint with elanpress

This is libfprint with the elanpress driver for the ELAN press sensors 04f3:0c3d, 0c4b and 0c6e, plus the fixes I have proposed for it that haven't been merged yet. Stock libfprint lists 0c6e under the swipe-only `elan` driver, which never matches a press ([libfprint#401](https://gitlab.freedesktop.org/libfprint/libfprint/-/issues/401)).

`main` is Mohammad Nauman's `elanpress-0c3d` branch ([libfprint!654](https://gitlab.freedesktop.org/libfprint/libfprint/-/merge_requests/654)) with these merge requests to it applied in order:

- [inauman/libfprint!2](https://gitlab.freedesktop.org/inauman/libfprint/-/merge_requests/2), faster matching
- [inauman/libfprint!3](https://gitlab.freedesktop.org/inauman/libfprint/-/merge_requests/3), frames rejected by the quality gates no longer set the contrast bar
- [inauman/libfprint!4](https://gitlab.freedesktop.org/inauman/libfprint/-/merge_requests/4), `FP_ELANPRESS_*` values that are not finite or out of range keep their defaults or get clamped
- [inauman/libfprint!5](https://gitlab.freedesktop.org/inauman/libfprint/-/merge_requests/5), a short reply to a data read fails the capture

Some commits here are newer than what those merge requests hold. The branch gets rebased and force-pushed when they change.

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
