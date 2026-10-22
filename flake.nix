{
  description = "x264 MVC: stock x264 CLI with opt-in stereo MVC (experimental)";
  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAllSystems = nixpkgs.lib.genAttrs systems;
      mkPackage = system:
        let pkgs = import nixpkgs { inherit system; }; in
        pkgs.stdenv.mkDerivation {
          pname = "x264-mvc";
          version = "0.1.0-test23-ntm-input-headers-trace-incomplete-test";
          src = self;
          nativeBuildInputs = with pkgs; [ pkg-config python3 ]
            ++ lib.optionals stdenv.hostPlatform.isx86_64 [ nasm ];
          # Enables libavformat/libswscale detection by stock x264 configure.
          # The same source also builds with only raw/Y4M input support.
          buildInputs = with pkgs; [ ffmpeg ];
          configurePhase = ''
            runHook preConfigure
            patchShebangs --build ./configure ./version.sh \
              ./config.guess ./config.sub \
              ./tools/cltostr.sh ./tools/msvsdepend.sh
            ${pkgs.lib.optionalString pkgs.stdenv.hostPlatform.isx86_64 ''
              export AS="${pkgs.nasm}/bin/nasm"
              "$AS" --version
            ''}
            ./configure --prefix=$out --enable-static --disable-opencl
            ${pkgs.lib.optionalString pkgs.stdenv.hostPlatform.isx86_64 ''
              grep -Fx "AS=$AS" config.mak
              grep -Eq '^#define HAVE_MMX 1$' config.h
            ''}
            runHook postConfigure
          '';
          buildPhase = ''
            runHook preBuild
            make -j"$NIX_BUILD_CORES" x264
            runHook postBuild
          '';
          doCheck = true;
          checkPhase = ''
            runHook preCheck
            python3 tests/test_stock_cli_mvc.py ./x264
            python3 tests/test_ntm_integration.py ./x264
            cc -std=c99 -I. tests/test_mvc_joint_rc.c mvc/joint_rc.c -lm -o mvc-joint-rc-test
            ./mvc-joint-rc-test
            cc -std=c99 -I. -include config.h tests/test_mvc_shared_vbv.c mvc/shared_vbv.c -lm -lpthread -o mvc-shared-vbv-test
            ./mvc-shared-vbv-test
            runHook postCheck
          '';
          installPhase = ''
            runHook preInstall
            make install-cli
            # Documentation filenames differ between source distributions and GitHub
            # repositories. Install any that are present, but never require one.
            for doc in README* COPYING* LICENSE* AUTHORS*; do
              if [ -f "$doc" ]; then
                install -Dm644 "$doc" "$out/share/doc/x264-mvc/$doc"
              fi
            done
            runHook postInstall
          '';
          meta = {
            description = "Stock x264 command-line encoder with experimental opt-in MVC";
            license = pkgs.lib.licenses.gpl2Plus;
            mainProgram = "x264";
            platforms = systems;
          };
        };
    in {
      packages = forAllSystems (system: {
        default = mkPackage system;
        x264-mvc = mkPackage system;
      });
      checks = forAllSystems (system: {
        x264-mvc = mkPackage system;
      });
    };
}
