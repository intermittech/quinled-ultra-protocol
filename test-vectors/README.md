# Test vectors

Example slots and frames generated from `include/ultra_protocol.h`:

- [`vectors.md`](vectors.md): each vector with its inputs and a hex dump.
- [`vectors.json`](vectors.json): the same bytes, machine-readable.
- [`gen_vectors.c`](gen_vectors.c): the generator. It also checks every header and slot CRC.

Regenerate after any change to the header, and commit the output:

```
cc -std=c99 -O1 -o gen_vectors gen_vectors.c && ./gen_vectors vectors.md vectors.json
```

How to use them: [Test vectors](https://github.com/intermittech/quinled-ultra-protocol/wiki/Test-Vectors) in the wiki.
