{
  description = "Experiments behind my WG21 work on asynchronous C++";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-26.05-darwin";
    # Only for the `stdexec` shell, which is defined there.
    devshells.url = "github:ispeters/devshells";
  };

  outputs =
    { nixpkgs, devshells, ... }:
    let
      systems = [
        "aarch64-darwin"
        "aarch64-linux"
        "x86_64-linux"
      ];
    in
    {
      devShells = nixpkgs.lib.genAttrs systems (
        system:
        let
          pkgs = nixpkgs.legacyPackages.${system};
        in
        {
          # What the CMake presets need, and nothing more. The presets' toolchain
          # files call compilers by name from PATH: `cc` and `c++` for the
          # appleclang presets, `gcc`/`g++` and `clang`/`clang++` on Linux. On
          # macOS this shell provides no compiler, so those names stay Apple's.
          default =
            (if pkgs.stdenv.hostPlatform.isDarwin then pkgs.mkShellNoCC else pkgs.mkShell)
              {
                packages = [
                  pkgs.cmake
                  pkgs.ninja
                ]
                ++ pkgs.lib.optional pkgs.stdenv.hostPlatform.isLinux pkgs.clang;
              };
        }
        # The shell I work on stdexec in, at the revision locked here: Clang 23
        # with its own libc++. The appleclang presets pick it up as `c++`.
        // nixpkgs.lib.optionalAttrs (system == "aarch64-darwin") {
          inherit (devshells.devShells.${system}) stdexec;
        }
      );
    };
}
