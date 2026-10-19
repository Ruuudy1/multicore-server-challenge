-- Edge-case dataset for check.py: load schema.sql from the challenge repo first, then this file (as a superuser).
-- Gaps in article ids (19999, 19996, 19993, 15000-15030), odd tag names, a 2,500-favorite article, a 185-comment thread with ties.
SELECT setseed(0.42);
INSERT INTO tags (id, name)
SELECT g, CASE WHEN g = 1 THEN 'Zeta' WHEN g = 2 THEN 'alpha' WHEN g = 3 THEN 'beta-1' WHEN g = 4 THEN 'Ångström' WHEN g = 5 THEN 'a b' ELSE 'tag' || lpad(g::text, 3, '0') END
FROM generate_series(1, 200) g;
SELECT setval(pg_get_serial_sequence('tags','id'), 200);
INSERT INTO users (id, username, email, password_hash, bio, image, created_at)
SELECT g, 'user' || g, 'user' || g || '@example.com', '$2a$10$x',
       CASE WHEN random() < 0.7 THEN 'bio "quoted" \ line' || E'\n' || g END,
       CASE WHEN random() < 0.5 THEN 'https://i.example/' || g END,
       now() - interval '800 days' + g * interval '10 minutes'
FROM generate_series(1, 3000) g;
SELECT setval(pg_get_serial_sequence('users','id'), 3000);
INSERT INTO articles (id, slug, author_id, title, description, body, created_at, updated_at)
SELECT g, 'title-number-' || g, 1 + floor(power(random(), 3) * 900)::int, 'Title Number ' || g, 'Desc ' || g,
       'Body para one ' || g || E'\n\nSecond "para"', ts, ts + CASE WHEN random() < 0.1 THEN interval '1 hour' ELSE interval '0' END
FROM (SELECT g, now() - interval '700 days' + g * interval '45 minutes' + (random() * interval '40 minutes') AS ts FROM generate_series(1, 20000) g) s;
SELECT setval(pg_get_serial_sequence('articles','id'), 20000);
INSERT INTO article_tags (article_id, tag_id)
SELECT DISTINCT a, 1 + floor(power(random(), 2) * 200)::int FROM generate_series(1, 20000) a, generate_series(1, 1 + floor(random() * 4)::int) k;
INSERT INTO favorites (user_id, article_id)
SELECT DISTINCT 1 + floor(random() * 3000)::int, 20000 - floor(power(random(), 2) * 20000)::int FROM generate_series(1, 120000);
INSERT INTO favorites (user_id, article_id) SELECT u, 19950 FROM generate_series(1, 2500) u ON CONFLICT DO NOTHING;
INSERT INTO follows (follower_id, followee_id)
SELECT DISTINCT u, 1 + floor(power(random(), 2.5) * 900)::int FROM generate_series(1, 3000) u, generate_series(1, floor(random() * 40)::int) k
WHERE true;
DELETE FROM follows WHERE follower_id = followee_id;
INSERT INTO comments (article_id, author_id, body, created_at, updated_at)
SELECT a, 1 + floor(random() * 3000)::int, 'Comment "x" ' || g, ts, ts
FROM (SELECT g, 20000 - floor(power(random(), 2) * 20000)::int AS a, now() - random() * interval '600 days' AS ts FROM generate_series(1, 60000) g) s;
INSERT INTO comments (article_id, author_id, body, created_at, updated_at)
SELECT 19990, 1 + floor(random() * 3000)::int, 'Hot comment ' || g, now() - g * interval '1 minute', now() - g * interval '1 minute' FROM generate_series(1, 180) g;
INSERT INTO comments (article_id, author_id, body, created_at, updated_at)
SELECT 19990, 7, 'Tie ' || g, now() - interval '3 minutes', now() - interval '3 minutes' FROM generate_series(1, 5) g;
DELETE FROM articles WHERE id IN (19999, 19996, 19993) OR id BETWEEN 15000 AND 15030;
VACUUM (FREEZE, ANALYZE);
