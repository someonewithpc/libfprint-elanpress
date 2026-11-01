{
  description = "libfprint with the elanpress driver for ELAN 04f3:0c3d, 0c4b and 0c6e press sensors";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      forAllSystems = nixpkgs.lib.genAttrs [ "x86_64-linux" "aarch64-linux" ];
      src = nixpkgs.lib.cleanSource self;
    in
    {
      overlays.default = final: prev: {
        libfprint = final.callPackage ./package.nix {
          inherit (prev) libfprint;
          inherit src;
        };
      };

      packages = forAllSystems (system:
        let
          pkgs = nixpkgs.legacyPackages.${system};
          libfprint = pkgs.callPackage ./package.nix { inherit src; };
        in
        {
          default = libfprint;
          inherit libfprint;
          fprintd = pkgs.fprintd.override { inherit libfprint; };
        });
    };
}
