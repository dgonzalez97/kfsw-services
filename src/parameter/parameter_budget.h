#ifndef KFSW_PARAMETER_BUDGET_H
#define KFSW_PARAMETER_BUDGET_H

/*
 * How much room the parameter snapshot is allowed.
 *
 * One number, in one place, because "how much space do the parameters take" is
 * a question asked about a spacecraft and should not have to be answered by
 * reading three files.
 *
 * It bounds two things at once, and deliberately so: the snapshot is built in
 * a static RAM buffer of exactly this size and then written whole, so the
 * budget and the buffer cannot drift apart or be raised separately. Raising it
 * costs the same number of bytes of RAM on every board that carries the
 * service, which is why it is a compile-time number rather than a parameter an
 * operator can set from the ground.
 *
 * For scale: the reference composition persists 10 values in about 210 bytes,
 * and the reference board's whole storage partition is 64 KB. Free space is
 * still checked at save time, because a partition with less than this left is
 * the case the budget alone cannot see.
 *
 * kfsw_param_persist_bytes() reports what a snapshot currently occupies and
 * table 26 carries it beside this limit, so the headroom is visible from the
 * ground rather than inferred here.
 */
#define KFSW_PARAM_PERSIST_MAX_BYTES 2048U

/* The most parameters one snapshot carries. The byte budget above usually
 * binds first — every entry costs a header, a name and a value — but a count
 * is the cheaper check and it is the one that keeps a runaway table from
 * filling the buffer before the size test runs.
 */
#define KFSW_PARAM_PERSIST_MAX_ENTRY_COUNT 64U

#endif /* KFSW_PARAMETER_BUDGET_H */
