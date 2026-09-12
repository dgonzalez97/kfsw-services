#ifndef KFSW_PARAMETER_BUDGET_H
#define KFSW_PARAMETER_BUDGET_H

/*
 * How much room the parameter snapshot is allowed.
 *
 * One place, deliberately, because "how much space do the parameters take" is
 * a question an operator asks about a spacecraft and should not have to answer
 * by reading three files.
 *
 * Two numbers, because they bound different things and the smaller one wins:
 *
 *   MAX_BYTES   the most filesystem space a snapshot may occupy. This is the
 *               budget to raise when parameters need more room, and it is
 *               checked against the free space actually reported by the
 *               storage partition, so a board with a small partition refuses
 *               before it fills rather than while it writes.
 *
 *   STAGE_BYTES the RAM buffer the snapshot is built in before it is written.
 *               A snapshot cannot exceed this however large the budget is, so
 *               on a board this is usually the limit that really applies.
 *
 * Worth knowing before raising either: the staging buffer is static RAM on a
 * part that has 320 KB of it in total, and the reference board's whole storage
 * partition is 64 KB. A budget far above those does no harm, it simply never
 * becomes the thing that refuses.
 *
 * kfsw_param_persist_bytes() reports what a snapshot currently occupies, and
 * table 26 carries it alongside both limits, so the headroom is visible from
 * the ground rather than inferred here.
 */
#define KFSW_PARAM_PERSIST_MAX_BYTES (2U * 1024U * 1024U)

#define KFSW_PARAM_PERSIST_STAGE_BYTES 2048U

/* The most parameters one snapshot carries. Raise with the staging buffer:
 * every entry costs a header, a name and a value.
 */
#define KFSW_PARAM_PERSIST_MAX_ENTRY_COUNT 64U

#endif /* KFSW_PARAMETER_BUDGET_H */
