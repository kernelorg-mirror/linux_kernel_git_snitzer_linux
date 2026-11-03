# Round 59: pages free in the region

**Planned 3 November after round 57** as a look first. Round 57's page life
found that 15% of the pages free at their put go back into the region and
wait about 1.3 ms (median 128 to 256 µs) before a ring's pool takes them
again, 377 pages of the cycle at any moment.

## What the trace says (round 58's run169)

- **Each port's free list holds 4K to 8K pages** at nearly every refill by
  its rings (`alloc_own_list_depth` in [../58-region-lock/regionlock.txt](../58-region-lock/regionlock.txt)).
  Of the region's 32768 pages about 3,600 are in use, about 6K to 7K free on
  each port's list (still mapped for that port), the rest unmapped.
- **The lists are last in, first out:** a ring takes the pages put most
  recently. The pages that go round are the top of each list, as deep as it
  swings between refills.
- **The 377 pages are round 58's round trip seen from the pages:** 19.4
  pages a WRITE go into the region and as many come out. Little's law over
  that rate gives the pages on the lists' working top. They are free pages,
  not in the region's in-use count.

## Outcome: no kernel change

- Fewer pages through the region means deeper pool stacks, which rounds 04
  and 19 turned down for the footprint, or a slower rate.
- A LIFO already hands out the hottest page first. The lists cannot be made
  "more LIFO".
- So round 59 has no change of its own. Round 58 makes the same trip cheaper
  without touching which pages make it.
- **Not measured:** how far each list's top swings, which bounds how many
  distinct pages go round. `regionlock.bt` takes the depth in powers of two
  only. If it is wanted, a linear histogram of `d->nfree` at each refill and
  put on the kernel from round 58 says it.
