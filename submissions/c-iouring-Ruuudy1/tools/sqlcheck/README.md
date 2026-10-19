# sqlcheck

Differential and plan checks for the server's SQL. `statements.py` is the source of truth: every statement in
`MULTICORE_BUILD_SPEC.md` comes from it, and the C server copies the strings verbatim. Change SQL here first, rerun, then port.

## What it checks

1. **Lists:** v1 and v2 statements for all five list kinds (global, tag, author, favorited, feed) against the SPEC.md reference
   page plus capped count. Viewers, limits, offsets and filters are random, including:
   - windows that straddle deleted ids;
   - past-the-end offsets (small databases only);
   - unknown tags and users, and the anonymous viewer.
2. **Single article and comments** against the reference, with correct slugs, wrong slugs and missing ids.
3. **Writes,** inside rolled-back transactions:
   - favorite add/remove count correction;
   - follow/unfollow;
   - comment add/delete, including 403;
   - article create with duplicate and new tags, update with slug change, 403, delete;
   - the tags list.
4. **Slugify:** the C algorithm (Python mirror) against Postgres `SLUGIFY` on tricky titles.
5. **Plans:** flags any bitmap scan, and any sequential scan of a big table, in the read statements. This runs under the box's
   pgtune cost settings unless `--default-costs` is given.

## Run

```bash
pip install psycopg2-binary

# Dev seed (from the challenge repo: bash seed/make-seed.sh --dev)
createdb -T conduit_dev conduit_check
PGDSN="host=127.0.0.1 dbname=conduit_check user=postgres" python3 check.py

# Full seed: the reference SQL is slow at depth, so use fewer rounds
createdb -T conduit_seed conduit_check_full
PGDSN="host=127.0.0.1 dbname=conduit_check_full user=postgres" python3 check.py --rounds 20

# Edge-case dataset (gaps in ids, odd tag names, a 185-comment thread with tied timestamps)
createdb conduit_edge
psql -d conduit_edge -f <challenge repo>/schema.sql
psql -d conduit_edge -f synthetic.sql
PGDSN="host=127.0.0.1 dbname=conduit_edge user=postgres" python3 check.py
```

Flags:
- `--rounds N`: list cases per kind (default 150).
- `--seed N`: random seed.
- `--default-costs`: skip the box's cost settings, to see what plans look like without them.

The exit code is 0 only with zero mismatches. Plan warnings are printed but don't fail the run.

## Notes

- Use a disposable database copy: writes roll back, but rolled-back inserts still dirty visibility maps.
- The reference SQL decorates every row that `OFFSET` skips, so deep offsets are slow. The harness skips past-the-end offsets
  above 50k articles.
- The harness was built and run on Postgres 16. Rerun it on Postgres 18 (what the box runs) before trusting any plan.

## Results so far (PostgreSQL 16.15, 2026-10-18)

| Database | Checks | Mismatches | Plan warnings |
|---|---|---|---|
| Edge-case (`synthetic.sql`) | 1,484 | 0 | 0 |
| 200k articles, 2M favorites, 2.8M follows (12 rounds) | 404 | 0 | 0 |
| Edge-case with Postgres default costs | 344 | 0 | 2 (comments use a bitmap scan) |
