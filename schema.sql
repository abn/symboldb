-- Copyright (C) 2012, 2013 Red Hat, Inc.
-- Written by Florian Weimer <fweimer@redhat.com>
--
-- This program is free software: you can redistribute it and/or modify
-- it under the terms of the GNU General Public License as published by
-- the Free Software Foundation, either version 3 of the License, or
-- (at your option) any later version.
--
-- This program is distributed in the hope that it will be useful,
-- but WITHOUT ANY WARRANTY; without even the implied warranty of
-- MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
-- GNU General Public License for more details.
--
-- You should have received a copy of the GNU General Public License
-- along with this program.  If not, see <http://www.gnu.org/licenses/>.

BEGIN;

CREATE SCHEMA symboldb;

CREATE TYPE symboldb.arch AS ENUM
  ('noarch', 'i686', 'x86_64', 'ppc', 'ppc64', 's390', 's390x');

CREATE TYPE symboldb.elf_visibility AS ENUM
  ('default', 'internal', 'hidden', 'protected');

CREATE DOMAIN symboldb.elf_byte AS SMALLINT
  CHECK (VALUE BETWEEN 0 AND 255);
CREATE DOMAIN symboldb.elf_short AS INTEGER
  CHECK (VALUE BETWEEN 0 AND 65536);

CREATE DOMAIN symboldb.elf_symbol_type AS TEXT
  CHECK (LENGTH(VALUE) > 1);
CREATE DOMAIN symboldb.elf_binding_type AS TEXT
  CHECK (LENGTH(VALUE) > 1);

CREATE TABLE symboldb.package (
  id SERIAL NOT NULL PRIMARY KEY,
  name TEXT NOT NULL CHECK (LENGTH(name) > 0),
  epoch INTEGER CHECK (epoch >= 0),
  version TEXT NOT NULL CHECK (LENGTH(version) > 0),
  release TEXT NOT NULL CHECK (LENGTH(release) > 0),
  arch symboldb.arch NOT NULL,
  hash BYTEA NOT NULL UNIQUE CHECK (LENGTH(hash) = 20),
  source TEXT NOT NULL CHECK (LENGTH(source) > 0)
);
COMMENT ON COLUMN symboldb.package.source IS
  'file name of the source RPM package';
COMMENT ON COLUMN symboldb.package.hash IS
  'internal SHA1HEADER hash of the RPM contents';
CREATE INDEX ON symboldb.package (name, version);

CREATE FUNCTION symboldb.nvra (symboldb.package) RETURNS TEXT AS $$
  SELECT
    $1.name || '-' || $1.version || '-' || $1.release || '.' || $1.arch
  ;
$$ IMMUTABLE STRICT LANGUAGE SQL;

CREATE FUNCTION symboldb.nevra (symboldb.package) RETURNS TEXT AS $$
  SELECT
    CASE
      WHEN $1.epoch IS NULL THEN symboldb.nvra ($1)
      ELSE $1.name || '-' || $1.epoch || ':' || $1.version
        || '-' || $1.release || '.' || $1.arch
    END
  ;
$$ IMMUTABLE STRICT LANGUAGE SQL;

CREATE INDEX ON symboldb.package (symboldb.nvra(package));
CREATE INDEX ON symboldb.package (symboldb.nevra(package));

CREATE TABLE symboldb.package_sha256 (
  sha256 BYTEA NOT NULL PRIMARY KEY CHECK (LENGTH(sha256) = 32),
  package INTEGER NOT NULL
    REFERENCES symboldb.package ON DELETE CASCADE
);
CREATE INDEX ON symboldb.package_sha256 (package);
COMMENT ON table symboldb.package_sha256 IS
  'SHA-256 hashes of multiple representations of the same RPM package';

CREATE TABLE symboldb.package_set (
  id SERIAL NOT NULL PRIMARY KEY,
  name TEXT NOT NULL UNIQUE,
  arch TEXT NOT NULL CHECK (LENGTH(arch) > 0)
);
COMMENT ON COLUMN symboldb.package_set.arch IS 'main architecture';

CREATE TABLE symboldb.package_set_member (
  set INTEGER NOT NULL
    REFERENCES symboldb.package_set ON DELETE CASCADE,
  package INTEGER NOT NULL REFERENCES symboldb.package
);
CREATE INDEX ON symboldb.package_set_member (set);
CREATE INDEX ON symboldb.package_set_member (package);

CREATE TABLE symboldb.file (
  id SERIAL NOT NULL PRIMARY KEY,
  package INTEGER NOT NULL
    REFERENCES symboldb.package (id) ON DELETE CASCADE,
  name TEXT NOT NULL CHECK (LENGTH(name) > 0),
  user_name TEXT NOT NULL CHECK (LENGTH(user_name) > 0),
  group_name TEXT NOT NULL CHECK (LENGTH(group_name) > 0),
  mtime NUMERIC NOT NULL CHECK (mtime >= 0),
  mode INTEGER NOT NULL CHECK (mode >= 0)
);
CREATE INDEX ON symboldb.file (package);
CREATE INDEX ON symboldb.file (name);

CREATE TABLE symboldb.elf_file (
  file INTEGER NOT NULL PRIMARY KEY
    REFERENCES symboldb.file (id) ON DELETE CASCADE,
  ei_class symboldb.elf_byte NOT NULL,
  ei_data symboldb.elf_byte NOT NULL,
  e_machine symboldb.elf_short NOT NULL,
  arch symboldb.arch NOT NULL,
  soname TEXT NOT NULL
);
CREATE INDEX ON symboldb.elf_file (soname);

CREATE TABLE symboldb.elf_definition (
  file INTEGER NOT NULL
    REFERENCES symboldb.file (id) ON DELETE CASCADE,
  name TEXT NOT NULL CHECK(length(name) > 0),
  version TEXT CHECK (LENGTH(version) > 0),
  primary_version BOOLEAN NOT NULL,
  symbol_type symboldb.elf_symbol_type NOT NULL,
  binding symboldb.elf_binding_type NOT NULL,
  visibility symboldb.elf_visibility NOT NULL,
  CHECK (CASE WHEN version IS NULL THEN NOT primary_version ELSE TRUE END)
);
CREATE INDEX ON symboldb.elf_definition (file);
CREATE INDEX ON symboldb.elf_definition (name, version);

CREATE TABLE symboldb.elf_reference (
  file INTEGER NOT NULL
    REFERENCES symboldb.file (id) ON DELETE CASCADE,
  name TEXT NOT NULL CHECK(length(name) > 0),
  version TEXT CHECK (LENGTH(version) > 0),
  symbol_type symboldb.elf_symbol_type NOT NULL,
  binding symboldb.elf_binding_type NOT NULL,
  visibility symboldb.elf_visibility NOT NULL
);
CREATE INDEX ON symboldb.elf_reference (file);
CREATE INDEX ON symboldb.elf_reference (name, version);

CREATE TABLE symboldb.elf_needed (
  file INTEGER NOT NULL
    REFERENCES symboldb.file (id) ON DELETE CASCADE,
  name TEXT NOT NULL,
  PRIMARY KEY (file, name)
);
CREATE INDEX ON symboldb.elf_needed (name);

CREATE TABLE symboldb.elf_rpath (
  file INTEGER NOT NULL
    REFERENCES symboldb.file (id) ON DELETE CASCADE,
  path TEXT NOT NULL,
  PRIMARY KEY (file, path)
);

CREATE TABLE symboldb.elf_runpath (
  file INTEGER NOT NULL
    REFERENCES symboldb.file (id) ON DELETE CASCADE,
  path TEXT NOT NULL,
  PRIMARY KEY (file, path)
);

CREATE TABLE symboldb.elf_error (
  file INTEGER NOT NULL
    REFERENCES symboldb.file (id) ON DELETE CASCADE,
  message TEXT
);

CREATE VIEW symboldb.elf_soname_provider AS
  SELECT set AS package_set, arch, soname,
    (SELECT array_agg(id ORDER BY LENGTH(name), name)
     FROM symboldb.file WHERE id = ANY(files)) AS files
    -- heuristic: prefer files with sorter paths
   FROM (SELECT psm.set, ef.arch, ef.soname, array_agg(DISTINCT f.id) AS files
    FROM symboldb.package_set_member psm
    JOIN symboldb.file f ON psm.package = f.package
    JOIN symboldb.elf_file ef ON f.id = ef.file
    WHERE EXISTS(SELECT 1
      FROM symboldb.package_set_member psm2
      JOIN symboldb.file f2 ON psm2.package = f2.package
      JOIN symboldb.elf_file ef2 ON f2.id = ef2.file
      JOIN symboldb.elf_needed en2 ON f2.id = en2.file
      WHERE psm.set = psm2.set
      AND ef.arch = ef2.arch AND ef.soname = en2.name)
    GROUP BY psm.set, ef.arch, ef.soname
  ) x;
COMMENT ON VIEW symboldb.elf_soname_provider IS
  'files which provide a ELF soname for a specific architecture within a package set';

CREATE VIEW symboldb.elf_soname_needed_missing AS
  SELECT psm.set AS package_set, ef.arch, en.file, en.name AS soname
      FROM symboldb.package_set_member psm
      JOIN symboldb.file f ON psm.package = f.package
      JOIN symboldb.elf_file ef ON f.id = ef.file
      JOIN symboldb.elf_needed en ON f.id = en.file
    WHERE NOT EXISTS (SELECT 1
      FROM symboldb.elf_soname_provider esp
      WHERE esp.package_set = psm.set
      AND esp.arch = ef.arch AND esp.soname = en.name);
COMMENT ON VIEW symboldb.elf_soname_provider IS
  'files which need a soname which cannot be resolved within the same package set';

CREATE TABLE symboldb.elf_closure (
  package_set INTEGER NOT NULL REFERENCES symboldb.package_set
    ON DELETE CASCADE,
  file INTEGER NOT NULL REFERENCES symboldb.file,
  needed INTEGER NOT NULL REFERENCES symboldb.file
);
CREATE INDEX ON symboldb.elf_closure (file);
CREATE INDEX ON symboldb.elf_closure (needed);

CREATE FUNCTION symboldb.elf_closure_update () RETURNS VOID AS $$
BEGIN
  CREATE TEMPORARY TABLE elf_closure_tmp ON COMMIT DROP AS
    SELECT psm.set, en.file AS file, esp.files[1] AS needed
    FROM symboldb.package_set_member psm
    JOIN symboldb.file f ON psm.package = f.package
    JOIN symboldb.elf_file ef ON f.id = ef.file
    JOIN symboldb.elf_needed en ON f.id = en.file
    JOIN symboldb.elf_soname_provider esp
      ON esp.package_set = psm.set
      AND esp.arch = ef.arch AND esp.soname = en.name;
  CREATE INDEX ON elf_closure_tmp (file);
  DELETE FROM symboldb.elf_closure;
  INSERT INTO symboldb.elf_closure WITH RECURSIVE
    ec(set, file, needed) AS (
      SELECT * FROM elf_closure_tmp
      UNION (SELECT ec.set, ec.file, elf_closure_tmp.needed
             FROM ec JOIN elf_closure_tmp ON ec.needed = elf_closure_tmp.file))
    SELECT * FROM ec ORDER BY file, needed;
  DROP TABLE elf_closure_tmp;
END;
$$ LANGUAGE plpgsql;
COMMENT ON FUNCTION symboldb.elf_closure_update () IS
  'regenerate the contents of the symboldb.elf_closure table, for all package sets';
SELECT symboldb.elf_closure_update ();

CREATE FUNCTION symboldb.elf_closure_update (INTEGER) RETURNS VOID AS $$
BEGIN
  CREATE TEMPORARY TABLE elf_closure_tmp ON COMMIT DROP AS
    SELECT en.file AS file, esp.files[1] AS needed
    FROM symboldb.package_set_member psm
    JOIN symboldb.file f ON psm.package = f.package
    JOIN symboldb.elf_file ef ON f.id = ef.file
    JOIN symboldb.elf_needed en ON f.id = en.file
    JOIN symboldb.elf_soname_provider esp
      ON psm.set = $1 AND esp.package_set = $1
      AND esp.arch = ef.arch AND esp.soname = en.name;
  CREATE INDEX ON elf_closure_tmp (file);
  DELETE FROM symboldb.elf_closure WHERE package_set = $1;
  INSERT INTO symboldb.elf_closure WITH RECURSIVE
    ec(file, needed) AS (
      SELECT * FROM elf_closure_tmp
      UNION (SELECT ec.file, elf_closure_tmp.needed
             FROM ec JOIN elf_closure_tmp ON ec.needed = elf_closure_tmp.file))
    SELECT $1, * FROM ec ORDER BY file, needed;
  DROP TABLE elf_closure_tmp;
END;
$$ LANGUAGE plpgsql;
COMMENT ON FUNCTION symboldb.elf_closure_update (INTEGER) IS
  'regenerate the contents of the symboldb.elf_closure table, for a single package set';

-- URL cache (mainly for raw repository metadata).

CREATE TABLE symboldb.url_cache (
  url TEXT NOT NULL PRIMARY KEY CHECK (url LIKE '%:%'),
  http_time BIGINT NOT NULL,
  data BYTEA,
  last_change TIMESTAMP WITHOUT TIME ZONE
);
COMMENT ON TABLE symboldb.url_cache IS 'cache for URL downloads';

-- Formatting file modes.

CREATE FUNCTION symboldb.file_mode_internal
  (mode INTEGER, mask INTEGER, ch TEXT) RETURNS TEXT AS $$
  SELECT CASE WHEN (mode & mask) <> 0 THEN ch ELSE '-' END;
$$ IMMUTABLE STRICT LANGUAGE SQL;
COMMENT ON FUNCTION symboldb.file_mode_internal
  (INTEGER, INTEGER, TEXT) IS 'internal helper function';

CREATE FUNCTION symboldb.file_mode_internal
  (mode INTEGER, mask1 INTEGER, mask2 INTEGER,
   ch1 TEXT, ch2 TEXT, ch3 TEXT) RETURNS TEXT AS $$
  SELECT CASE
    WHEN (mode & mask1) <> 0 AND (mode & mask2) = 0 THEN ch1
    WHEN (mode & mask1) = 0 AND (mode & mask2) <> 0 THEN ch2
    WHEN (mode & mask1) <> 0 AND (mode & mask2) <> 0 THEN ch3
    ELSE '-'
  END;
$$ IMMUTABLE STRICT LANGUAGE SQL;
COMMENT ON FUNCTION symboldb.file_mode_internal
  (INTEGER, INTEGER, INTEGER, TEXT, TEXT, TEXT) IS
  'internal helper function';

CREATE FUNCTION symboldb.file_mode (INTEGER) RETURNS TEXT AS $$
  SELECT symboldb.file_mode_internal($1, 256, 'r')
    || symboldb.file_mode_internal($1, 128, 'w')
    || symboldb.file_mode_internal($1, 64, 2048, 'x', 'S', 's')
    || symboldb.file_mode_internal($1, 32, 'r')
    || symboldb.file_mode_internal($1, 16, 'w')
    || symboldb.file_mode_internal($1, 8, 1024, 'x', 'S', 's')
    || symboldb.file_mode_internal($1, 4, 'r')
    || symboldb.file_mode_internal($1, 2, 'w')
    || symboldb.file_mode_internal($1, 1, 512, 'x', 'T', 't');
$$ IMMUTABLE STRICT LANGUAGE SQL;
COMMENT ON FUNCTION symboldb.file_mode (INTEGER) IS
  'format the integer as a file permission string';

COMMIT;
