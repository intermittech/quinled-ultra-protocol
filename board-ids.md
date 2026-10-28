# Board ID allocations

Only product identity is centralized. Never infer ratings, counts, link abilities,
MCU, assembly or compatibility from this list. All facts come from the firmware's
descriptor. Product display names, including Medium/Mini/Nano, are free strings.

| ID | Product |
|---|---|
| 1 | [Dig2Analog-Plus-5ch](https://github.com/intermittech/quinled-ultra-firmware/wiki/Board-Dig2Analog-Plus-5ch) |
| 2 | [Diff-Power Ultra Receiver Analog-Medium](https://github.com/intermittech/quinled-ultra-firmware/wiki/Board-Diff-Power-UR-Analog-M) |
| 3 | [Diff-Power Ultra Receiver Digital-Medium](https://github.com/intermittech/quinled-ultra-firmware/wiki/Board-Diff-Power-UR-Digital-M) |

0 and 255 are invalid; 254 is reserved for nonproduction format-test fixtures.
4..253 are unallocated. The withdrawn illustrative 4/5 examples were never product
allocations; no prototype using them is releasable. A serialized allocation-list
change reserves a new ID, preventing collisions while retaining the existing u8
wire identity. No future product needs allocation until it exists. If this space
is exhausted, a versioned extended identity is required, not ID reuse. Assembly
IDs are opaque bytes local to a product and need no central allocation.

Dig2Analog-Plus-5ch (board 1) ALWAYS forwards: LINK.FORWARD is set on its
single-wire command link, CHAIN_END is clear. It has NO return channel:
RETURN and RETURN_RELAY are clear, reply=NONE, return timing/rate fields zero.
There is no chain/no-chain assembly bit or variant. Its only assembly variants
are 3 A and 5 A channels, represented by opaque IDs owned by board firmware.
The firmware artifact/jig supplies its descriptor to the inventory; absence of
replies is expected and never means an empty port. It cannot relay downstream
telemetry. UPDATE_FORWARD is a separate bootloader capability, not implied by
FORWARD. This allocation document is not a numeric assembly-ID registry.
