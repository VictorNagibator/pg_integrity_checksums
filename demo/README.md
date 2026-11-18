# README demo

[`docs/assets/demo.gif`](../docs/assets/demo.gif) is recorded from real
commands against two PostgreSQL servers:

- **prod** holds the demo data from [`seed.sql`](seed.sql);
- **backup** gets a `pg_dump` of prod, like a backup restored on another
  server.

[`record.py`](record.py) runs the scenario with bash and writes the real
output to [`demo.cast`](demo.cast) (asciicast v2); only the typing rhythm and
pauses are synthetic.  [`render.py`](render.py) turns the cast into the GIF.

## Re-recording

Needs two servers with the extension installed, `psql` on `PATH`, and
Python with Pillow (for the GIF only).

```bash
export PROD=postgres://localhost:5432/shop BACKUP=postgres://localhost:5433/shop

createdb -h localhost -p 5432 shop
createdb -h localhost -p 5433 shop
demo/setup.sh "$PROD" "$BACKUP"

python3 demo/record.py demo/demo.cast
python3 demo/render.py demo/demo.cast docs/assets/demo.gif
```

The DSNs appear in the recording, so keep them short.  The cast can also be
played in a terminal with `asciinema play demo/demo.cast`.
