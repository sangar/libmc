# Encoding: JSON and YAML in one library

This note records why JSON and YAML stay bundled in libmc, where the real
cost of the current design is, and the order in which to change it when a
cost becomes a problem.

## Decision

JSON and YAML stay in libmc, behind one `Node` tree, with yyjson and cyaml
vendored in `deps/`. The library is not split into a JSON library and a
YAML library.

The shared `Node` tree is the valuable part. Code that reads configuration,
state or rendered documents does not care which format produced them, and
that only works because both parsers produce the same tree. Shipping two
parsers in one archive is a side effect of that, not the point.

## Why bundling costs nothing

`libmc.a` is a static archive and each parser is its own translation unit.
The linker pulls in only the object files that are referenced, so a program
that calls `json_parse` never links cyaml, and a program that calls
`yaml_parse` never links yyjson. Splitting the library would buy no binary
size and no memory.

The only bundling cost is the vendored source footprint and the time to
compile it. If a consumer ever needs a JSON-only build, a flag in
`tools/build.c` that skips `deps/cyaml` is a small change. It is not added
until someone asks.

## Where the cost actually is

Performance and memory are decided by the tree representation and by the
conversion step, not by the pairing.

1. **`Node` is wide.** Every node carries scalar, sequence and mapping fields
   at once, about 100 bytes, while yyjson's own value is 16 bytes. A union
   over the three payloads brings it to roughly 40 bytes. This matters for
   large documents only; for configuration files it is noise.
2. **Two trees exist at once.** JSON parses into a yyjson document in the
   caller's arena and then converts; the yyjson copy stays allocated because
   it shares the arena with the result. YAML builds a malloc tree in cyaml,
   copies it and frees it. Peak memory is about twice the final tree for both.
3. **YAML copies every scalar twice.** cyaml returns a malloc'd string per
   scalar, which is then copied into the arena. JSON points into the in-place
   buffer and copies nothing. This is the one place the YAML path is
   measurably slower, and it is fixable inside `src/encoding/yaml.c`.
4. **Mappings are linear.** `node_put` and `node_get` scan entries, so a
   mapping with thousands of keys is quadratic to build. Right for
   configuration, wrong for data.

## Plan

Each step is independent, keeps the public API, and is done only when a
measurement says it is needed. The order is the expected payoff.

### 1. Parse intermediates into a scratch arena

`json_parse` gives yyjson a scratch arena and builds the `Node` tree in the
caller's arena, so the yyjson document is released after conversion. The
scratch arena is created and destroyed inside `json_parse`; nothing
changes for the caller. Peak memory drops to one tree plus the in-place
buffer.

Acceptance: a test parsing a large document asserts that the caller's arena
grew by the size of the `Node` tree and not by the yyjson document as well.

### 2. Stop copying YAML scalars twice

Read plain scalars straight from the document span with `cyaml_span_ptr`
and copy into the arena once. Keep `cyaml_scalar_str` for quoted, block and
escaped scalars that need decoding.

Acceptance: the existing YAML tests pass unchanged, and a benchmark on a
multi-megabyte YAML file shows fewer allocations.

### 3. Narrow `Node`

Make the payload a union over scalar, sequence and mapping, and drop the
capacity fields from the public struct if `arena_grow` can carry them
elsewhere. This is the one step that touches the public header, so it is
done in one commit with every caller in the workspace, and before any
project depends on `Node`'s layout from another language.

Acceptance: `sizeof(Node)` is at most 48 bytes and every test in the
workspace passes.

### 4. Index large mappings

When a mapping passes a threshold of entries, build a `StrMap` from key to
entry index inside `node_put` and use it in `node_get`. The entry array
stays in document order, so encoding and iteration are unchanged.

Acceptance: a test with ten thousand keys runs in linear time.

### 5. Only if a streaming need appears: an event layer

If a project needs to decode straight into structs or handle documents
larger than memory, add an event or visitor interface under both parsers
and make the `Node` builder one consumer of it. This is the real
"unbundled" design. It is the right shape for a data library and
over-engineering for a foundation library used for configuration, so it is
not started on speculation.

## Related

dbox vendors libyaml separately while libmc already carries cyaml. That is
the duplication in the workspace worth removing: dbox should read YAML
through `mc/encoding/yaml.h` and drop `deps/libyaml`.
