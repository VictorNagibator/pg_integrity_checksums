-- Demo data for the README recording; loaded by setup.sh.
CREATE EXTENSION IF NOT EXISTS pg_integrity_checksums;
DROP TABLE IF EXISTS orders, products, customers;

SELECT setseed(0.42) \gset

CREATE TABLE customers (
    id    int PRIMARY KEY,
    name  text NOT NULL,
    email text UNIQUE NOT NULL
);
INSERT INTO customers
SELECT g, 'Customer ' || g, 'c' || g || '@example.com'
FROM generate_series(1, 20000) g;

CREATE TABLE orders (
    id          bigint PRIMARY KEY,
    customer_id int NOT NULL REFERENCES customers,
    amount      numeric(10,2) NOT NULL,
    created_at  timestamptz NOT NULL
);
INSERT INTO orders
SELECT g, 1 + g % 20000, round((random() * 500)::numeric, 2),
       '2026-01-01'::timestamptz + g * interval '1 minute'
FROM generate_series(1, 200000) g;
CREATE INDEX ON orders (customer_id);

CREATE TABLE products (
    id    int PRIMARY KEY,
    title text NOT NULL,
    price numeric(10,2) NOT NULL
);
INSERT INTO products
SELECT g, 'Product ' || g, g * 1.5
FROM generate_series(1, 2000) g;
