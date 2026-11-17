# Vendored third-party code

| Directory | Project | Version | License | Used for |
|---|---|---|---|---|
| `xxhash/` | [xxHash](https://github.com/Cyan4973/xxHash) | 0.8.3 (`xxhash.h` of tag `v0.8.3`, unmodified) | BSD-2-Clause (`xxhash/LICENSE`) | the `xxh3` algorithm, compiled into `checksum_hash.c` |

`xxhash.h` sha256: `17973c0dc49d9854ca26caa191f0e12f7a424b68858d9a78de3860d959d85e4b`.

Changing the vendored version must not change any `xxh3` value; the
`checksum_algorithms` regression test pins known answers.
