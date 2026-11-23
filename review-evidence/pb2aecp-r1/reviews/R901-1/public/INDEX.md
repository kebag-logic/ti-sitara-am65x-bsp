# Round 2 receipts (BSP #29)

BSP head `cce8c9a4143548c80800b2def821431ff45e3698` (`pb2-aecp-1`); milan-fpga
head `677d605f37c3a80654df1dd2cde053b57242845b` (`pf-linuxprof-1`). Not pushed.

## The cross-built binary

| File | sha256 | Path |
| --- | --- | --- |
| `milan-ctrld` (aarch64) | `04e44ea074b28eacc31218f34964e0d6f7f35b1ab0c88466dfee53d817070cfb` | [aarch64/milan-ctrld](aarch64/milan-ctrld); built at `$VALIDATION_STORAGE/pb2aecp-a830/build-aarch64/milan-ctrld` |
| `milan-dp` (aarch64) | `8defbfe9553b096c9d9789b90f4615ff896057b231b10cfa3a9c16b49d7f9cea` | [aarch64/milan-dp](aarch64/milan-dp) |
| `milan-gptp-watch` (aarch64) | `8ea48e0503e9a29ba215c21e8e3f00fa2cd9676ec65056f7814b1376428b888c` | [aarch64/milan-gptp-watch](aarch64/milan-gptp-watch) |
| `entity.conf` | `e273ff975fc157b2f48939532c203abbd3464f476d1f02edd52e93ee5752556c` | [aarch64/entity.conf](aarch64/entity.conf) (= `milan-linux/config/entity.conf`) |

Built from the BSP head with `make CC=aarch64-linux-gnu-gcc` (GCC 16.2.0) on the
provisional tree; two clean builds gave the same `milan-ctrld`.
[aarch64/SHA256SUMS](aarch64/SHA256SUMS) lists the four.
[cross-build.log](cross-build.log) holds the compile lines.
`milan-mediad` was not cross-built: it needs the target's alsa-lib, which the
Buildroot build supplies.

## Logs

| Receipt | What it shows |
| --- | --- |
| [make-check.log](make-check.log) | `make check` at the head, rc 0: config current, unit tests, peer, aecp, talker, listener, nvm |
| [aecp-enumeration.log](aecp-enumeration.log) | the AECP and ACMP controller test's 87 checks, 0 failed |
| [provisional-pin.txt](provisional-pin.txt) | what the pin names, which archives were hash-checked, and how the provisional tree was made |
| [git-state.txt](git-state.txt) | both heads, clean trees, no push, no board access |
| [comment-bsp-29.md](comment-bsp-29.md) | the posted [A830] REVIEW READY on #29: https://github.com/kebag-logic/ti-sitara-am65x-bsp/issues/29#issuecomment-7099190088 |

milan-fpga #983's receipts are in [../linuxprof/receipts/INDEX.md](../linuxprof/receipts/INDEX.md).
