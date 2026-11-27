Run `python3 benchmarks/compare.py /path/to/baseline/atchim` after `make`.
The script uses a temporary SQLite database with 200,002 initial rows, three
warm-up iterations and twenty measured iterations per operation. It alternates
baseline/candidate order and reports median, minimum and maximum process latency.
Output is redirected to /dev/null; database preparation is outside timed regions.

Local measurement (GCC 16.2.1, both builds -O2, baseline C++20, candidate C++23;
single CLI process at a time, no CPU pinning; warm OS cache):

| Operation | Before median (range), ms | After median (range), ms |
| --- | --- | --- |
| Missing suggestion | 6.608 (6.421–7.987) | 0.715 (0.635–1.104) |
| Rare suggestion | 3.740 (3.443–4.237) | 0.659 (0.627–0.928) |
| History 5,000 rows | 1.159 (1.144–1.484) | 1.123 (1.109–1.367) |
| Log one command | 0.655 (0.641–0.854) | 0.632 (0.617–0.949) |

The suggestion gain comes from the indexed query, not C++23 itself. Logging and
history differences are small and their timing ranges overlap. This is synthetic
local evidence, not a guarantee for other storage or history distributions.
