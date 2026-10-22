# cofr: co-op flight recorder (vendored)

`cofr.h`, `cofr.cpp` and `observer.h` are copied unchanged from the coop-anything hub,
[tools/flight/cpp](https://github.com/cranebeaker/coop-anything/tree/main/tools/flight/cpp) at commit 1347549, MIT
licensed (`LICENSE` here). The analyzer that reads the files (`flight.py`) stays in the hub:

```
python -I tools/flight/flight.py diverge host.cofr guest.cofr
```

cdmp's use of it: `src/mod/net/flight.h`.
