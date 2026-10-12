# The nixpkgs packaging/port.nix builds against, pinned so that CI and every machine give the same
# store paths. The same revision decanter's toolchain uses: the binaries then load the Vulkan
# drivers that toolchain sets (a Nix-built program cannot load the host's Mesa).
import (fetchTarball {
  url = "https://github.com/NixOS/nixpkgs/archive/39ad350a0602fa0a58a544344e3e9187526ea45c.tar.gz";
  sha256 = "1ri8sf7jjis1paxa61xyy3k1ylqsdy1lx1mhadlqlf4jmf1phhx3";
}) { }
