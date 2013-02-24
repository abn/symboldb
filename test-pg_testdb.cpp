/*
 * Copyright (C) 2013 Red Hat, Inc.
 * Written by Florian Weimer <fweimer@redhat.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "pg_testdb.hpp"
#include "pgconn_handle.hpp"
#include "pgresult_handle.hpp"
#include "pg_exception.hpp"

#include "test.hpp"

static void
test()
{
  pg_testdb db;
  pgconn_handle h(db.connect("template1"));
  h.check();
  {
    pgresult_handle r(PQexec(h.raw, "SELECT 'abc'"));
    r.check();
    CHECK(PQntuples(r.raw) == 1);
    COMPARE_STRING(PQgetvalue(r.raw, 0, 0), "abc");
  }
  {
    pgresult_handle r(PQexec(h.raw, "garbage"));
    try {
      r.check();
      CHECK(false);
    } catch (pg_exception &e) {
      COMPARE_STRING(e.severity_, "ERROR");
      COMPARE_STRING(e.primary_, "syntax error at or near \"garbage\"");
      COMPARE_STRING(e.sqlstate_, "42601");
      CHECK(e.statement_position_ == 1);
      CHECK(e.status_ == PGRES_FATAL_ERROR);
      COMPARE_STRING(PQresStatus(e.status_), "PGRES_FATAL_ERROR");
    }
  }
}

static test_register t("pg_testdb", test);