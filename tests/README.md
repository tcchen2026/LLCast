# Host robustness checks

Build and run the portable tests and the production receiver's `--no-decode`
integration tests:

```sh
cmake -S . -B build/host -DCMAKE_BUILD_TYPE=Release
cmake --build build/host -j4
ctest --test-dir build/host --output-on-failure
```

The suite includes large-gap/key-wait checks, a live UDP jump/PING test,
concurrent row reclamation, bounded statistics, sender history expiry/eviction,
and Windows stage-log tests that run without a GPU. A DROPPED output's `span` is
normally 1 and covers the whole missing range when it exceeds `max_gap`
(default 120). Only the first number gets a wire loss report; RECOVER names the
last lost number so an earlier IDR cannot suppress recovery.

Run the wall-clock memory soak separately (default: 1800 seconds, 60 fps,
24 Mbps nominal synthetic payload, with 4× key bursts at startup and recovery):

```sh
python3 tests/soak_memory.py build/host/llstream_rx build/host/llstream_send \
    build/results/h4-soak
```

This requires Linux `/proc`, uses loopback and needs no box or PC. Profiling
stays off. It records RSS/private memory every 10 seconds; after a 60-second
warmup, each process must stay within a 2 MiB range, and the receiver must
deliver at least 99% of the sent frames. Raw samples and the measured ranges
are retained beside the logs. `--seconds 10` is a quick harness check, not a
substitute for the 30-minute run. This tests transport and reassembly, not MPP,
GPU or DRM allocation.

While the soak runs, check the paced replay fixture repeatedly from another
terminal:

```sh
ctest --test-dir build/host -R '^loopback$' --repeat until-fail:20 --output-on-failure
```

The replay fixture reports `SO_RXQ_OVFL` kernel drops and the effective receive
buffer on failure. The production receiver logs its buffer at startup: it tries
`SO_RCVBUFFORCE` first, then `SO_RCVBUF` when permission is unavailable. Linux's
reported size includes its doubling for bookkeeping; it is not payload capacity.

For ASan/UBSan, configure a separate build with
`-DCMAKE_C_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer'`, the same
`CMAKE_CXX_FLAGS`, and `CMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined'`.
The concurrent `test_frame_map` and `test_rx_lifetime` also support TSan in a
separate build. Use a normal build for RSS measurements; sanitizer quarantine
and bookkeeping intentionally change allocation behavior.
