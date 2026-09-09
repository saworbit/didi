# Fuzz targets

Didi reads bytes it did not write. JSON-RPC arrives on stdin from whatever
client is driving the server, length-prefixed frames arrive over a local pipe,
and base64 payloads arrive inside both. These targets point libFuzzer at the
three places that happens.

## Why these three

| Target | Function | What arrives |
| --- | --- | --- |
| `fuzz_framed_message` | `didi::ipc::parseFramedMessage` | A four-byte length an attacker controls, then a payload that may not be there |
| `fuzz_jsonrpc_request` | `didi::mcp::JsonRpcRequest::parse` | Every request the server ever answers |
| `fuzz_base64_decode` | `didi::base64::decode` | Captured frames and encoded resource payloads |

The first one is the reason this exists. Reading `parseFramedMessage` closely
while deciding what to fuzz found a buffer over-read in it: the bounds check
`size < 4 + len` was evaluated in 32-bit unsigned arithmetic, so a length near
`UINT32_MAX` wrapped to a small number, the guard passed, and a four-gigabyte
`std::string` was constructed from an eight-byte buffer. Fixed in #344.

The only test that function had round-tripped a frame the same code had just
written — the one input shape guaranteed not to find it. **A decoder is defined
by what it does with input it did not write.**

## The corpus is the point

`corpus/` holds committed seeds, including
`corpus/framed_message/regression-length-wraps-to-zero`: the exact eight bytes
that used to segfault. Every fuzz job copies the seeds in before running, so
that input is re-executed on every run for as long as this target exists.

CI keeps the *accumulated* corpus in the Actions cache between runs. Starting
from empty each time would only ever find what is reachable in the first sixty
seconds; restoring what previous runs discovered is where the value compounds,
and it is most of what ClusterFuzzLite would have provided without needing its
Docker image or a separate corpus store.

## Running one locally

Needs Clang; libFuzzer ships with it and there is no portable substitute.

```bash
cmake -B build-fuzz -S . -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DDIDI_BUILD_FUZZERS=ON -DDIDI_BUILD_TESTS=OFF
cmake --build build-fuzz --parallel

# Run against the committed seeds, for a minute.
./build-fuzz/fuzz_framed_message fuzz/corpus/framed_message -max_total_time=60
```

Reproduce a specific finding by passing the file libFuzzer wrote:

```bash
./build-fuzz/fuzz_framed_message crash-a1b2c3
```

The targets build with `-fsanitize=fuzzer,address,undefined`. Both sanitizers,
deliberately: the defect this work grew out of was an integer overflow that
produced an out-of-bounds read, and each of the two sees only one half of that.

## Writing another

Assert invariants, not just absence of crashes. A target that only checks
"did not crash" finds memory errors and nothing else; the interesting failures
are usually a decoder disagreeing with its own contract. `fuzz_framed_message`
checks that a caller can never be advanced past the end of its own buffer,
which would not trip a sanitizer on its own.

**Assert the contract the code has, not the one you wish it had.** Both of
these targets failed on their first CI run, and neither had found a bug. One
claimed that returning no message implied consuming no bytes; the decoder
deliberately reports how many bytes a malformed frame occupied so a caller can
skip it, and the crashing input was a seed committed in the same change. The
other claimed a parsed request has a non-empty method; the parser requires
`method` to be a string and says nothing about its length, which matches
JSON-RPC 2.0. A fuzz target that asserts wishes reports them as bugs, and the
cost is not just noise — it is that a real finding then arrives looking exactly
like the two you already learned to dismiss.

Both are now recorded as explicit "deliberately not asserted" comments rather
than deleted, so the next person does not re-add them.

Add the target to `DIDI_FUZZ_TARGETS` in `CMakeLists.txt` and to the matrix in
`.github/workflows/fuzz.yml`. Both lists are short and explicit on purpose: a
target nobody runs is worse than no target, because the directory still looks
like coverage.
